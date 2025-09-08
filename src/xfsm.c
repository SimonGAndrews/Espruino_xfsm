// XFSM_UPLOAD_ID: 2025-08-23-14-00-native-subscribe
// xfsm.c — Espruino JsVar-only FSM core with xstate-fsm alignment
//
// Highlights
// - Built-in action: `assign` (also accepts shorthand object without `type`)
//   * { type:"xstate.assign", assignment: fn|object }  // preferred
//   * { type:"assign", assignment: fn|object }         // alias
//   * shorthand: { key: valueOrFn, ... }               // treated as assignment spec
//   Semantics: produces a patch (object) which is shallow-merged into context.
// - Actions list items may be: function, string (resolved via config.actions then global), or assign object.
// - Guards (cond) are functions or strings; truthiness via jsvGetBool.
// - Context persistence happens ONCE after executing a group of actions.
// - No C++ features; strict JsVar lock/unlock discipline.
//
// Public API (declared in xfsm.h):
//   V1 FSM (single-object): xfsm_init_object, xfsm_start_object, xfsm_stop_object,
//                           xfsm_status_object, xfsm_current_state_var, xfsm_send_object
//   Machine (pure): xfsm_machine_init, xfsm_machine_initial_state,
//                   xfsm_machine_transition
//   Service/Interpreter (stateful): xfsm_service_init, xfsm_service_start, xfsm_service_stop,
//                                   xfsm_service_send, xfsm_service_get_state, xfsm_service_get_status
//
// NOTE: Keep 'persist once' rule to avoid jsvUnLockInline asserts.


#include "jsutils.h"
#include "jsinteractive.h"
#include "jsparse.h"  // jspEvaluateVar
#include "jsvar.h"

#include "xfsm.h"
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <jswrapper.h>


// --- One-time unsubscribe factory (closure-based, strong delete) ---

static JsVar *xfsm_unsub_factory = 0;

void xfsm_ensure_unsub_factory(void) {
  if (xfsm_unsub_factory && jsvIsFunction(xfsm_unsub_factory)) return;

  // (function(m,i){ i=''+i; return function(){ try{ return m._unsubById(i); }catch(e){ return false; } }; })
  JsVar *src = jsvNewFromString(
    "(function(m,i){"
      "i = ''+i;"
      "return function(){"
        "try { return m._unsubById(i); }"
        "catch(e){ return false; }"
      "};"
    "})"
  );
  if (!src) return;

  JsVar *fn = jspEvaluateVar(src, 0 /*global*/, "xfsm.unsubFactory");
  jsvUnLock(src);

  if (fn && jsvIsFunction(fn)) xfsm_unsub_factory = fn;
  else { if (fn) jsvUnLock(fn); xfsm_unsub_factory = 0; }
}


// Create a fresh unsubscribe() that closes over svc + id (stringified inside the factory).
// Returns a LOCKED function.
JsVar *xfsm_make_unsubscribe(JsVar *svc, int id) {
  if (!(xfsm_unsub_factory && jsvIsFunction(xfsm_unsub_factory)))
    return jsvNewNativeFunction((void (*)(void))0, JSWAT_VOID);

  JsVar *factory = jsvLockAgain(xfsm_unsub_factory);
  JsVar *a0 = jsvLockAgain(svc);
  JsVar *a1 = jsvNewFromInteger(id);
  JsVar *argv[2] = { a0, a1 };

  JsVar *fn = jspExecuteFunction(factory, 0 /*this*/, 2, argv);

  jsvUnLock(a0);
  jsvUnLock(a1);
  jsvUnLock(factory);

  if (fn && jsvIsFunction(fn)) return fn; // LOCKED
  if (fn) jsvUnLock(fn);
  return jsvNewNativeFunction((void (*)(void))0, JSWAT_VOID);
}



/* ---------------- Event normalization ---------------- */
// Enable events to be recieved as strings or objects.  

JsVar *xfsm_normalize_event(JsVar *event) {
  if (!event) return 0;

  if (jsvIsString(event)) {
    JsVar *obj = jsvNewObject();
    if (!obj) return 0;
    /* jsvAsString takes a single argument in this tree */
    JsVar *t = jsvAsString(event);          /* LOCKED string */
    if (!t) t = jsvNewFromString("");       /* fallback */
    jsvObjectSetChildAndUnLock(obj, "type", t);
    return obj;                              /* LOCKED */
  }

  if (jsvIsObject(event)) {
    JsVar *t = jsvObjectGetChild(event, "type", 0);
    if (!t || !jsvIsString(t)) {
      if (t) jsvUnLock(t);
      jsvObjectSetChildAndUnLock(event, "type", jsvNewFromString(""));
    } else {
      jsvUnLock(t);
    }
    return jsvLockAgain(event);              /* LOCKED */
  }

  return 0;
}

/* ---------------- Key strings ---------------- */
static const char * const K_STATUS  = "status";
static const char * const K_STATE   = "state";
static const char * const K_CFG     = "config";

static const char * const K_STATES  = "states";
static const char * const K_ON      = "on";
static const char * const K_ENTRY   = "entry";
static const char * const K_EXIT    = "exit";
static const char * const K_TARGET  = "target";
static const char * const K_ACTIONS = "actions";
static const char * const K_CONTEXT = "context";
static const char * const K_COND    = "cond";

/* Machine state object fields */
static const char * const S_VALUE   = "value";
static const char * const S_CTX     = "context";
static const char * const S_ACTS    = "actions";

/* Service fields */
static const char * const K_MACHINE = "_machine";
static const char * const K_SSTATE  = "_state";
static const char * const K_SCTX    = "_context";
static const char * const K_SSTATUS = "_status";

/* ---------------- Function invocation helper ---------------- */
static JsVar *xfsm_callJsFunction(JsVar *fn, JsVar *thisArg, JsVar **argv, int argc) {
  if (!fn || !jsvIsFunction(fn)) return 0;
  JsVar *thisObj = thisArg ? jsvLockAgain(thisArg) : jsvNewNull();
  JsVar *res = jspExecuteFunction(fn, thisObj, argc, argv); // locked or 0
  jsvUnLock(thisObj);
  return res;
}

/** Notify all registered listeners with the current state (argument 0). */
#include "jsutils.h"
#include "jsinteractive.h"
#include "jsparse.h"
#include "jsvar.h"
#include "xfsm.h"

/** Notify all registered listeners with the current state (argument 0). */
void xfsm_notify_listeners(JsVar *service) {
  if (!service || !jsvIsObject(service)) return;

  JsVar *listeners = jsvObjectGetChild(service, "_listeners", 0);
  if (!listeners || !jsvIsObject(listeners)) { if (listeners) jsvUnLock(listeners); return; }

  // Prefer backing state; fall back to getter if missing
  JsVar *st = jsvObjectGetChild(service, K_SSTATE, 0);
  if (!st) st = xfsm_service_get_state(service); // LOCKED or 0

  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, listeners);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *k  = jsvObjectIteratorGetKey(&it);
    JsVar *fn = jsvObjectIteratorGetValue(&it);
    jsvUnLock(k);
    if (fn && jsvIsFunction(fn) && st) {
      JsVar *argv[1] = { st };
      JsVar *res = jspExecuteFunction(fn, service, 1, argv);
      if (res) jsvUnLock(res);
    }
    if (fn) jsvUnLock(fn);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  if (st) jsvUnLock(st);
  jsvUnLock(listeners);
}


/* ---------------- Flat machine validation (reject nested states) ---------- */
bool xfsm_validate_no_nested_states(JsVar *machineConfig) {
  if (!machineConfig || !jsvIsObject(machineConfig)) return true;

  JsVar *src = jsvNewFromString(
    "(function(cfg){"
      "if (!cfg || !cfg.states) return '';"
      "var s = cfg.states, k;"
      "for (k in s) { if (s[k] && s[k].states) return k || '<unknown>'; }"
      "return '';"
    "})"
  );
  if (!src) return true;

  JsVar *fn = jspEvaluateVar(src, 0, "xfsm.validate.flat");
  jsvUnLock(src);
  if (!fn) return true;

  bool ok = true;
  if (jsvIsFunction(fn)) {
    JsVar *argv[1] = { machineConfig };
    JsVar *res = jspExecuteFunction(fn, 0 /*this*/, 1, argv);
    if (res) {
      if (jsvIsString(res) && jsvGetStringLength(res) > 0) {
        /* Convert to a plain string var */
        JsVar *s = jsvAsString(res);
        char buf[64];
        size_t i = 0;

        JsvStringIterator it;
        jsvStringIteratorNew(&it, s, 0);
        while (jsvStringIteratorHasChar(&it) && i < sizeof(buf) - 1) {
          buf[i++] = (char)jsvStringIteratorGetChar(&it);
          jsvStringIteratorNext(&it);
        }
        buf[i] = 0;
        jsvStringIteratorFree(&it);
        jsvUnLock(s);

        if (buf[0]) {
          jsDebug(DBG_INFO,
                  "XFSM: Nested states not supported (found nested under state \"%s\").\n",
                  buf);
          ok = false;
        }
      }
      jsvUnLock(res);
    }
  }
  jsvUnLock(fn);
  return ok;
}


/* ---------------- Utilities ---------------- */
static void set_status(JsVar *obj, const char *txt) {
  JsVar *v = jsvNewFromString(txt);
  jsvObjectSetChildAndUnLock(obj, K_STATUS, v);
}
static int str_from_jsv(JsVar *s, char *buf, size_t bufSize) {
  if (!s || !jsvIsString(s) || bufSize == 0) return 0;
  size_t n = jsvGetString(s, buf, bufSize-1);
  buf[n] = 0;
  return (int)n;
}
static JsVar *getChildObj(JsVar *o, const char *k) {
  JsVar *v = jsvObjectGetChild(o, k, 0);
  if (!v) return 0;
  if (!jsvIsObject(v)) { jsvUnLock(v); return 0; }
  return v;
}

/**
 * new_state_obj
 * Construct a state object for return from machine/transition
 *
 * Fields:
 *   value   : string (state name)
 *   context : copy/lock of the context object (if provided)
 *   actions : array of actions to execute (if provided)
 *   changed : bool (true if state value changed, false otherwise)
 */
/* Build state object { value, context, actions, changed, matches } */
static JsVar *new_state_obj(const char *value, JsVar *ctx /*locked or 0*/, JsVar *acts /*locked or 0*/, bool changed) {
  JsVar *st = jsvNewObject();
  if (!st) return 0;

  if (value && value[0]) {
    JsVar *sv = jsvNewFromString(value);
    if (sv) jsvObjectSetChildAndUnLock(st, S_VALUE, sv);
  }
  if (ctx) jsvObjectSetChildAndUnLock(st, S_CTX, jsvLockAgain(ctx));
  if (acts) jsvObjectSetChildAndUnLock(st, S_ACTS, jsvLockAgain(acts));
  jsvObjectSetChildAndUnLock(st, "changed", jsvNewFromBool(changed));

/* Attach JS matches(stateName) per state (avoid shared instance issues) */
{
  JsVar *src = jsvNewFromString("(function(s){return this.value===s;})");
  if (src) {
    JsVar *fn = jspEvaluateVar(src, 0 /*global scope*/, "xfsm.matches");
    jsvUnLock(src);
    if (fn && jsvIsFunction(fn)) {
      jsvObjectSetChildAndUnLock(st, "matches", fn); // fn still locked
    } else if (fn) {
      jsvUnLock(fn);
    }
  }
}

  return st; /* LOCKED */
}


/* ---------------- Named function resolution ---------------- */
static JsVar *resolveNamedFromConfig(JsVar *owner, const char *name) {
  JsVar *cfg = jsvObjectGetChild(owner, K_CFG, 0);
  if (!cfg || !jsvIsObject(cfg)) { if (cfg) jsvUnLock(cfg); return 0; }
  JsVar *cfgActs = getChildObj(cfg, K_ACTIONS);
  jsvUnLock(cfg);
  if (!cfgActs) return 0;
  JsVar *fn = jsvObjectGetChild(cfgActs, name, 0); // locked or 0
  jsvUnLock(cfgActs);
  if (fn && !jsvIsFunction(fn)) { jsvUnLock(fn); fn = 0; }
  return fn;
}
static JsVar *resolveNamedFromGlobal(const char *name) {
  JsVar *root = jsvLockAgain(execInfo.root);
  if (!root) return 0;
  JsVar *fn = jsvObjectGetChild(root, name, 0);
  jsvUnLock(root);
  if (fn && !jsvIsFunction(fn)) { jsvUnLock(fn); fn = 0; }
  return fn; // locked or 0
}
static JsVar *resolveFunc(JsVar *owner, JsVar *item /*locked*/) {
  if (!item) return 0;
  if (jsvIsFunction(item)) return jsvLockAgain(item);
  if (jsvIsString(item)) {
    char name[48]; str_from_jsv(item, name, sizeof(name));
    if (!name[0]) return 0;
    JsVar *fn = resolveNamedFromConfig(owner, name);
    if (!fn) fn = resolveNamedFromGlobal(name);
    return fn; // locked or 0
  }
  return 0;
}

/* ---------------- Built-in 'assign' support ---------------- */
static bool is_string_eq(JsVar *v, const char *s) {
  if (!v || !jsvIsString(v)) return false;
  char buf[32]; size_t n = jsvGetString(v, buf, sizeof(buf)-1); buf[n]=0;
  return 0 == strcmp(buf, s);
}

/* Shallow-merge 'patch' object into *pCtx (no jsvGetKeys; use iterator) */
static void merge_patch_into_ctx(JsVar **pCtx, JsVar *patch) {
  if (!*pCtx || !jsvIsObject(*pCtx)) { if (*pCtx) jsvUnLock(*pCtx); *pCtx = jsvNewObject(); }

  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, patch);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *k = jsvObjectIteratorGetKey(&it);     /* locked */
    JsVar *v = jsvObjectIteratorGetValue(&it);   /* locked */
    if (k && jsvIsString(k) && v) {
      char key[64]; size_t n = jsvGetString(k, key, sizeof(key)-1); key[n]=0;
      jsvObjectSetChildAndUnLock(*pCtx, key, jsvLockAgain(v));
    }
    if (v) jsvUnLock(v);
    if (k) jsvUnLock(k);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
}


/* Apply an 'assignment' spec (function or object) to produce a patch and merge */
static void apply_assignment(JsVar *svc, JsVar **pCtx, JsVar *assignAction, JsVar *eventObj) {
  if (!pCtx) return;

  /* Ensure we have a context object to write to */
  if (!*pCtx || !jsvIsObject(*pCtx)) {
    if (*pCtx) jsvUnLock(*pCtx);
    *pCtx = jsvNewObject(); /* LOCKED */
  }

  /* Unwrap object-form { type:'xstate.assign', assignment: ... } or accept function/map directly */
  JsVar *payload = 0;
  if (jsvIsObject(assignAction)) {
    payload = jsvObjectGetChild(assignAction, "assignment", 0);
    if (!payload) payload = jsvLockAgain(assignAction);
  } else if (jsvIsFunction(assignAction)) {
    payload = jsvLockAgain(assignAction);
  }
  if (!payload) return;

  /* Case 1: function (ctx, evt) -> object to merge */
  if (jsvIsFunction(payload)) {
    JsVar *args[2] = { jsvLockAgain(*pCtx), eventObj ? jsvLockAgain(eventObj) : jsvNewObject() };
    JsVar *res = xfsm_callJsFunction(payload, 0, args, 2);
    if (args[0]) jsvUnLock(args[0]);
    if (args[1]) jsvUnLock(args[1]);

    if (res && jsvIsObject(res)) {
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, res);
      while (jsvObjectIteratorHasValue(&it)) {
        JsVar *k = jsvObjectIteratorGetKey(&it);
        JsVar *v = jsvObjectIteratorGetValue(&it);
        if (k && v) {
          /* Coerce key safely */
          JsVar *kstr = jsvAsString(k);
          char keybuf[64] = "";
          if (kstr) { jsvGetString(kstr, keybuf, sizeof(keybuf)); jsvUnLock(kstr); }
          if (keybuf[0]) jsvObjectSetChildAndUnLock(*pCtx, keybuf, jsvLockAgain(v));
        }
        if (k) jsvUnLock(k);
        if (v) jsvUnLock(v);
        jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
    }
    if (res) jsvUnLock(res);
    jsvUnLock(payload);
    return;
  }

  /* Case 2: object map { key: const | fn(ctx,evt) } */
  if (jsvIsObject(payload)) {
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, payload);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *k = jsvObjectIteratorGetKey(&it);
      JsVar *v = jsvObjectIteratorGetValue(&it);
      if (k) {
        JsVar *kstr = jsvAsString(k);
        char keybuf[64] = "";
        if (kstr) { jsvGetString(kstr, keybuf, sizeof(keybuf)); jsvUnLock(kstr); }
        if (keybuf[0]) {
          JsVar *out = 0;
          if (v && jsvIsFunction(v)) {
            JsVar *args[2] = { jsvLockAgain(*pCtx), eventObj ? jsvLockAgain(eventObj) : jsvNewObject() };
            JsVar *res = xfsm_callJsFunction(v, 0, args, 2);
            if (args[0]) jsvUnLock(args[0]);
            if (args[1]) jsvUnLock(args[1]);
            if (res) { out = jsvLockAgain(res); jsvUnLock(res); }
          } else if (v) {
            out = jsvLockAgain(v);
          }
          if (out) jsvObjectSetChildAndUnLock(*pCtx, keybuf, out);
        }
      }
      if (k) jsvUnLock(k);
      if (v) jsvUnLock(v);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
  }
  jsvUnLock(payload);
}

/* ---------------- Raw actions accessors ---------------- */
static JsVar *getActionListRaw(JsVar *node, const char *key) {
  JsVar *v = jsvObjectGetChild(node, key, 0); // locked or 0
  if (!v) return 0;
  if (!jsvIsArray(v)) { jsvUnLock(v); return 0; }
  return v; // locked
}
static JsVar *getTransitionActionsRaw(JsVar *transitionObj) {
  if (!transitionObj || !jsvIsObject(transitionObj)) return 0;
  JsVar *acts = jsvObjectGetChild(transitionObj, K_ACTIONS, 0); // locked or 0
  if (!acts) return 0;
  /* may be array or single item; caller handles both forms */
  return acts; // locked
}

/* Execute an array of actions against (ctx,event).
 * Call sites (kept compatible):
 *   run_actions_raw(service, &ctx, exitActs,  event, fromName, toName);
 *   run_actions_raw(service, &ctx, transActs, event, fromName, toName);
 *   run_actions_raw(service, &ctx, entryActs, event, fromName, toName);
 *
 * Supports:
 *   - function(ctx,evt)
 *   - { exec:function(ctx,evt) }
 *   - "name"                -> lookup in actions map(s)
 *   - { type:"name" }       -> lookup in actions map(s)
 *   - xstate.assign family  -> apply_assignment(...) (updates *pCtx)
 */
void run_actions_raw(JsVar *service, JsVar **pCtx, JsVar *actionsArr,
                            JsVar *eventObj, const char *fromName,
                            const char *toName) {
  (void)fromName;
  (void)toName; // currently unused (kept for signature compatibility)
  if (!actionsArr || !jsvIsArray(actionsArr))
    return;

  /* --- Resolve actions map (preferred -> fallbacks) ---
   * 1) service._options.actions
   * 2) service._machine._options.actions
   * 3) service._machine.config.options.actions
   * 4) service._machine.config.actions
   */
  JsVar *actsMap = 0;
  {
    JsVar *opts = jsvObjectGetChild(service, "_options", 0);
    if (opts) {
      actsMap = jsvObjectGetChild(opts, "actions", 0);
      jsvUnLock(opts);
    }
    if (!actsMap) {
      JsVar *mach = jsvObjectGetChild(service, "_machine", 0);
      if (mach) {
        JsVar *mopts = jsvObjectGetChild(mach, "_options", 0);
        if (mopts) {
          actsMap = jsvObjectGetChild(mopts, "actions", 0);
          jsvUnLock(mopts);
        }
        if (!actsMap) {
          JsVar *cfg = jsvObjectGetChild(mach, "config", 0);
          if (cfg) {
            JsVar *copt = jsvObjectGetChild(cfg, "options", 0);
            if (copt) {
              actsMap = jsvObjectGetChild(copt, "actions", 0);
              jsvUnLock(copt);
            }
            if (!actsMap)
              actsMap = jsvObjectGetChild(cfg, "actions", 0);
            jsvUnLock(cfg);
          }
        }
        jsvUnLock(mach);
      }
    }
  }

  unsigned int len = (unsigned int)jsvGetArrayLength(actionsArr);
  for (unsigned int i = 0; i < len; i++) {
    JsVar *item = jsvGetArrayItem(actionsArr, i); // LOCKED (or 0)
    if (!item)
      continue;

    bool handled = false;

    /* A) direct function */
    if (!handled && jsvIsFunction(item)) {
      JsVar *argv[2] = {*pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
                        eventObj ? jsvLockAgain(eventObj) : jsvNewObject()};
      JsVar *res = jspExecuteFunction(item, service, 2, argv);
      if (res)
        jsvUnLock(res);
      if (argv[0])
        jsvUnLock(argv[0]);
      if (argv[1])
        jsvUnLock(argv[1]);
      handled = true;
    }

    /* B) object action */
    if (!handled && jsvIsObject(item)) {
      /* B1) { exec:function } */
      JsVar *exec = jsvObjectGetChild(item, "exec", 0);
      if (exec && jsvIsFunction(exec)) {
        JsVar *argv[2] = {*pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
                          eventObj ? jsvLockAgain(eventObj) : jsvNewObject()};
        JsVar *res = jspExecuteFunction(exec, service, 2, argv);
        if (res)
          jsvUnLock(res);
        if (argv[0])
          jsvUnLock(argv[0]);
        if (argv[1])
          jsvUnLock(argv[1]);
        handled = true;
      }
      if (exec)
        jsvUnLock(exec);

      /* B2) { type:"..." } */
      if (!handled) {
        JsVar *typ = jsvObjectGetChild(item, "type", 0);
        if (typ && jsvIsString(typ)) {
          char tbuf[32];
          int tlen = jsvGetString(typ, tbuf, sizeof(tbuf) - 1);
          if (tlen < 0)
            tlen = 0;
          tbuf[tlen] = 0;

          if (!strcmp(tbuf, "xstate.assign") || !strcmp(tbuf, "assign")) {
            /* assign family -> updates *pCtx */
            apply_assignment(service, pCtx, item, eventObj);
            handled = true;
          } else if (actsMap && jsvIsObject(actsMap)) {
            /* NEW: object-form named action via actions map(s) */
            JsVar *fn = jsvObjectGetChild(actsMap, tbuf, 0);
            if (fn && jsvIsFunction(fn)) {
              JsVar *argv[2] = {*pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
                                eventObj ? jsvLockAgain(eventObj)
                                         : jsvNewObject()};
              JsVar *res = jspExecuteFunction(fn, service, 2, argv);
              if (res)
                jsvUnLock(res);
              if (argv[0])
                jsvUnLock(argv[0]);
              if (argv[1])
                jsvUnLock(argv[1]);
              handled = true;
            }
            if (fn)
              jsvUnLock(fn);
          }
        }
        if (typ)
          jsvUnLock(typ);
      }

      /* B3) shorthand assign object (no type/exec) */
      if (!handled) {
        apply_assignment(service, pCtx, item, eventObj);
        handled = true;
      }
    }

    /* C) "name" -> resolve via actions map(s) */
    if (!handled && jsvIsString(item) && actsMap && jsvIsObject(actsMap)) {
      char nbuf[32];
      int nlen = jsvGetString(item, nbuf, sizeof(nbuf) - 1);
      if (nlen < 0)
        nlen = 0;
      nbuf[nlen] = 0;
      JsVar *fn = jsvObjectGetChild(actsMap, nbuf, 0);
      if (fn && jsvIsFunction(fn)) {
        JsVar *argv[2] = {*pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
                          eventObj ? jsvLockAgain(eventObj) : jsvNewObject()};
        JsVar *res = jspExecuteFunction(fn, service, 2, argv);
        if (res)
          jsvUnLock(res);
        if (argv[0])
          jsvUnLock(argv[0]);
        if (argv[1])
          jsvUnLock(argv[1]);
        handled = true;
      }
      if (fn)
        jsvUnLock(fn);
    }

    jsvUnLock(item);
  }

  if (actsMap)
    jsvUnLock(actsMap);
}

/* ========================================================================== */
/*                         V1: Single-object FSM                              */
/* ========================================================================== */
void xfsm_init_object(JsVar *fsmObject) {
  if (!fsmObject) return;
  JsVar *v = jsvObjectGetChild(fsmObject, K_STATUS, 0);
  if (!v) set_status(fsmObject, "NotStarted"); else jsvUnLock(v);
}

XfsmStatus xfsm_start_object(JsVar *fsmObject, JsVar *initialState /*locked or 0*/) {
  if (!fsmObject) return XFSM_STATUS_NOTSTARTED; 

  /* choose state */
  JsVar *chosen = 0;
  if (initialState && jsvIsString(initialState)) chosen = jsvLockAgain(initialState);
  else {
    JsVar *cfg0 = jsvObjectGetChild(fsmObject, K_CFG, 0);
    if (cfg0 && jsvIsObject(cfg0)) {
      JsVar *init = jsvObjectGetChild(cfg0, "initial", 0);
      if (init && jsvIsString(init)) chosen = init; else if (init) jsvUnLock(init);
    }
    if (cfg0) jsvUnLock(cfg0);
    if (!chosen) chosen = jsvNewFromString("idle");
  }

  jsvObjectSetChildAndUnLock(fsmObject, K_STATE, chosen);
  set_status(fsmObject, "Running");

  /* entry actions */
  JsVar *cfg   = getChildObj(fsmObject, K_CFG);
  JsVar *ctx   = cfg ? jsvObjectGetChild(cfg, K_CONTEXT, 0) : 0;
  JsVar *states= cfg ? getChildObj(cfg, K_STATES) : 0;

  char toBuf[64]={0}; JsVar *stateVal = jsvObjectGetChild(fsmObject, K_STATE, 0);
  if (stateVal){ str_from_jsv(stateVal,toBuf,sizeof(toBuf)); jsvUnLock(stateVal); }

  JsVar *node = (states && toBuf[0]) ? jsvObjectGetChild(states, toBuf, 0) : 0;
  JsVar *entryActs = (node && jsvIsObject(node)) ? getActionListRaw(node, K_ENTRY) : 0;

  run_actions_raw(fsmObject, &ctx, entryActs, 0, 0, toBuf);

  if (entryActs) jsvUnLock(entryActs);
  if (node) jsvUnLock(node);
  if (states) jsvUnLock(states);

  if (cfg) {
    if (ctx) jsvObjectSetChildAndUnLock(cfg, K_CONTEXT, jsvLockAgain(ctx));
    jsvUnLock(cfg);
  }
  if (ctx) jsvUnLock(ctx);

  return XFSM_STATUS_RUNNING;
}

void xfsm_stop_object(JsVar *fsmObject) { if (!fsmObject) return; set_status(fsmObject,"Stopped"); }

XfsmStatus xfsm_status_object(JsVar *fsmObject) {
  if (!fsmObject) return XFSM_STATUS_NOTSTARTED;
  JsVar *v = jsvObjectGetChild(fsmObject, K_STATUS, 0);
  if (!v) return XFSM_STATUS_NOTSTARTED;
  XfsmStatus st = XFSM_STATUS_NOTSTARTED;
  if (jsvIsString(v)) {
    char sbuf[16]; str_from_jsv(v,sbuf,sizeof(sbuf));
    if (!strcmp(sbuf,"Running")) st=XFSM_STATUS_RUNNING;
    else if (!strcmp(sbuf,"Stopped")) st=XFSM_STATUS_STOPPED;
  }
  jsvUnLock(v);
  return st;
}

JsVar *xfsm_current_state_var(JsVar *fsmObject) {
  if (!fsmObject) return 0;
  JsVar *v = jsvObjectGetChild(fsmObject, K_STATE, 0);
  if (!v || !jsvIsString(v)) { if (v) jsvUnLock(v); return 0; }
  return v; // locked
}

/* send(): supports string target or object { target, actions, cond } */
JsVar *xfsm_send_object(JsVar *fsmObject, JsVar *event /* string */) {
  if (!fsmObject || !jsvIsObject(fsmObject)) return 0;

  JsVar *cur = jsvObjectGetChild(fsmObject, K_STATE, 0);
  if (!cur || !jsvIsString(cur)) { if (cur) jsvUnLock(cur); return 0; }
  char fromBuf[64]; str_from_jsv(cur, fromBuf, sizeof(fromBuf));

  JsVar *cfg    = getChildObj(fsmObject, K_CFG);
  JsVar *states = cfg ? getChildObj(cfg, K_STATES) : 0;
  if (!cfg || !states) { if(states) jsvUnLock(states); if(cfg) jsvUnLock(cfg); jsvUnLock(cur); return 0; }

  JsVar *srcNode = jsvObjectGetChild(states, fromBuf, 0);
  if (!srcNode || !jsvIsObject(srcNode)) { if(srcNode) jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }

  JsVar *onObj = getChildObj(srcNode, K_ON);
  if (!onObj) { jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }

  char evKey[64]; str_from_jsv(event, evKey, sizeof(evKey));
  JsVar *trans = jsvObjectGetChild(onObj, evKey, 0);
  if (!trans){ jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }

  /* guard */
  if (jsvIsObject(trans)) {
    JsVar *cond = jsvObjectGetChild(trans, K_COND, 0);
    if (cond) {
      JsVar *fn = resolveFunc(fsmObject, cond); jsvUnLock(cond);
      if (fn) {
        JsVar *ctxg = jsvObjectGetChild(cfg, K_CONTEXT, 0); if (!ctxg||!jsvIsObject(ctxg)){ if(ctxg)jsvUnLock(ctxg); ctxg=jsvNewObject(); }
        JsVar *meta = jsvNewObject();
        if (fromBuf[0]) { JsVar *fs=jsvNewFromString(fromBuf); jsvObjectSetChildAndUnLock(meta,"state",fs); }
        JsVar *argv[3] = { jsvLockAgain(ctxg), jsvLockAgain(event), meta };
        JsVar *res = xfsm_callJsFunction(fn,0,argv,3);
        jsvUnLock(argv[0]); jsvUnLock(argv[1]); jsvUnLock(meta);
        if (res) { bool truthy = jsvGetBool(res); jsvUnLock(res);
          if (!truthy){ jsvUnLock(fn); jsvUnLock(ctxg); jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }
        } else { jsvUnLock(fn); jsvUnLock(ctxg); jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }
        jsvUnLock(fn); jsvUnLock(ctxg);
      }
    }
  }

  /* resolve target + raw actions */
  char toBuf[64]; toBuf[0]=0;
  JsVar *exitActs = 0, *transActs = 0, *entryActs = 0;

  if (jsvIsString(trans)) {
    str_from_jsv(trans, toBuf, sizeof(toBuf));
  } else if (jsvIsObject(trans)) {
    JsVar *tgt = jsvObjectGetChild(trans, K_TARGET, 0);
    if (tgt && jsvIsString(tgt)) str_from_jsv(tgt, toBuf, sizeof(toBuf));
    if (tgt) jsvUnLock(tgt);
    transActs = getTransitionActionsRaw(trans);
  }

  if (!toBuf[0]) { jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur); return 0; }

  exitActs = getActionListRaw(srcNode, K_EXIT);
  JsVar *dstNode = jsvObjectGetChild(states, toBuf, 0);
  if (dstNode && jsvIsObject(dstNode)) entryActs = getActionListRaw(dstNode, K_ENTRY);
  if (dstNode) jsvUnLock(dstNode);

  JsVar *ctx = jsvObjectGetChild(cfg, K_CONTEXT, 0);

  run_actions_raw(fsmObject, &ctx, exitActs,  event, fromBuf, toBuf);
  run_actions_raw(fsmObject, &ctx, transActs, event, fromBuf, toBuf);
  run_actions_raw(fsmObject, &ctx, entryActs, event, fromBuf, toBuf);

  if (exitActs) jsvUnLock(exitActs);
  if (transActs) jsvUnLock(transActs);
  if (entryActs) jsvUnLock(entryActs);

  jsvObjectSetChildAndUnLock(fsmObject, K_STATE, jsvNewFromString(toBuf));
  if (ctx) { jsvObjectSetChildAndUnLock(cfg, K_CONTEXT, jsvLockAgain(ctx)); jsvUnLock(ctx); }

  jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(states); jsvUnLock(cfg); jsvUnLock(cur);

  return jsvNewFromString(toBuf);
}

/* ========================================================================== */
/*                                Machine                                     */
/* ========================================================================== */
void xfsm_machine_init(JsVar *m) { (void)m; }

/**
 * xfsm_machine_initial_state
 * Build the initial state object: { value, context, actions, changed:false }
 * Note: returns actions = state's entry[] (if any). Service.start() will execute them.
 */
JsVar *xfsm_machine_initial_state(JsVar *machine) {
  if (!machine || !jsvIsObject(machine)) return 0;

  JsVar *cfg = jsvObjectGetChild(machine, K_CFG, 0);
  if (!cfg) return 0;

  /* Flat-only validation (dev-time aid).
   * This logs if nested states are found; we continue safely.
   * If you ever want to hard-fail, check the boolean and return/throw here.
   */
  (void)xfsm_validate_no_nested_states(cfg);

  char initBuf[64] = "";
  JsVar *initial = jsvObjectGetChild(cfg, "initial", 0);
  if (initial && jsvIsString(initial)) str_from_jsv(initial, initBuf, sizeof(initBuf));
  if (initial) jsvUnLock(initial);

  if (!initBuf[0]) {
    jsvUnLock(cfg);
    return 0;
  }

  JsVar *states = jsvObjectGetChild(cfg, K_STATES, 0);
  if (!states || !jsvIsObject(states)) {
    if (states) jsvUnLock(states);
    jsvUnLock(cfg);
    return 0;
  }

  JsVar *node = jsvObjectGetChild(states, initBuf, 0);
  JsVar *entryArr = 0;
  if (node && jsvIsObject(node)) {
    entryArr = jsvObjectGetChild(node, K_ENTRY, 0);
  }

  /* Context for machine path is the config.context (service owns its own copy) */
  JsVar *ctx = jsvObjectGetChild(cfg, K_CONTEXT, 0);

  JsVar *st = new_state_obj(initBuf, ctx, entryArr, false /*changed*/);

  if (ctx) jsvUnLock(ctx);
  if (entryArr) jsvUnLock(entryArr);
  if (node) jsvUnLock(node);
  jsvUnLock(states);
  jsvUnLock(cfg);
  return st; /* locked */
}

/**
 * xfsm_machine_transition (shim)
 * Keep backward-compat signature using string event.
 */
JsVar *xfsm_machine_transition(JsVar *machine, JsVar *stateOrValue, JsVar *eventStr /*string*/) {
  if (!machine || !jsvIsObject(machine) || !eventStr || !jsvIsString(eventStr))
    return 0;
  JsVar *evtObj = jsvNewObject();
  if (!evtObj) return 0;
  jsvObjectSetChildAndUnLock(evtObj, "type", jsvLockAgain(eventStr));
  JsVar *res = xfsm_machine_transition_ex(machine, stateOrValue, evtObj);
  jsvUnLock(evtObj);
  return res; /* LOCKED or 0 */
}

/**
 * xfsm_machine_transition_ex
 * Compute next state object given machine, prev state/value, and OBJECT-form event.
 * - Supports shorthand: on[event] = "B"
 * - Supports arrays with cond(ctx, evt) (first truthy wins)
 * - Supports targetless (actions only, keep value, changed=false)
 * - Builds actions in order: exit[], transition.actions[], entry[]
 * Returns LOCKED state object or 0 if no transition.
 */
JsVar *xfsm_machine_transition_ex(JsVar *machine, JsVar *stateOrValue, JsVar *eventObj /*object*/) {
  if (!machine || !jsvIsObject(machine) || !eventObj || !jsvIsObject(eventObj))
    return 0;

  /* config + states */
  JsVar *cfg = jsvObjectGetChild(machine, K_CFG, 0);
  if (!cfg) return 0;
  JsVar *states = jsvObjectGetChild(cfg, K_STATES, 0);
  if (!states || !jsvIsObject(states)) {
    if (states) jsvUnLock(states);
    jsvUnLock(cfg);
    return 0;
  }

  /* event.type => evBuf */
  char evBuf[64] = "";
  JsVar *etype = jsvObjectGetChild(eventObj, "type", 0);
  if (etype && jsvIsString(etype)) str_from_jsv(etype, evBuf, sizeof(evBuf));
  if (etype) jsvUnLock(etype);
  if (!evBuf[0]) { jsvUnLock(states); jsvUnLock(cfg); return 0; }

  /* determine fromBuf and guard context (prefer prev state's context if provided) */
  char fromBuf[64] = "";
  JsVar *guardCtx = 0; /* locked or 0 */

  if (stateOrValue) {
    if (jsvIsObject(stateOrValue)) {
      JsVar *sv = jsvObjectGetChild(stateOrValue, S_VALUE, 0);
      if (sv && jsvIsString(sv)) str_from_jsv(sv, fromBuf, sizeof(fromBuf));
      if (sv) jsvUnLock(sv);
      guardCtx = jsvObjectGetChild(stateOrValue, S_CTX, 0); /* may be 0 */
    } else if (jsvIsString(stateOrValue)) {
      str_from_jsv(stateOrValue, fromBuf, sizeof(fromBuf));
    }
  }
  if (!fromBuf[0]) {
    JsVar *initial = jsvObjectGetChild(cfg, "initial", 0);
    if (initial && jsvIsString(initial)) str_from_jsv(initial, fromBuf, sizeof(fromBuf));
    if (initial) jsvUnLock(initial);
  }
  if (!guardCtx) {
    /* fall back to machine.config.context */
    guardCtx = jsvObjectGetChild(cfg, K_CONTEXT, 0); /* may be 0 */
  }
  if (!fromBuf[0]) {
    if (guardCtx) jsvUnLock(guardCtx);
    jsvUnLock(states); jsvUnLock(cfg);
    return 0;
  }

  /* source node */
  JsVar *srcNode = jsvObjectGetChild(states, fromBuf, 0);
  if (!srcNode || !jsvIsObject(srcNode)) {
    if (srcNode) jsvUnLock(srcNode);
    if (guardCtx) jsvUnLock(guardCtx);
    jsvUnLock(states); jsvUnLock(cfg);
    return 0;
  }

  JsVar *onObj   = jsvObjectGetChild(srcNode, K_ON, 0);
  JsVar *exitArr = jsvObjectGetChild(srcNode, K_EXIT, 0);

  /* on[event] candidates: string | object | array */
  JsVar *cands = 0;
  if (onObj && jsvIsObject(onObj)) cands = jsvObjectGetChild(onObj, evBuf, 0);

  /* select candidate (shorthand, object, or first array element whose cond(ctx,evt) passes) */
  JsVar *candSel = 0;

  if (cands) {
    if (jsvIsString(cands)) {
      /* shorthand "B" -> { target:"B" } */
      char toTmp[64]=""; str_from_jsv(cands, toTmp, sizeof(toTmp));
      JsVar *obj = jsvNewObject();
      if (obj) jsvObjectSetChildAndUnLock(obj, K_TARGET, jsvNewFromString(toTmp));
      candSel = obj;
    } else if (jsvIsObject(cands)) {
      JsVar *c = jsvLockAgain(cands);
      bool pass = true;
      JsVar *cond = jsvObjectGetChild(c, K_COND, 0);
      if (cond) {
        if (jsvIsFunction(cond)) {
          JsVar *args[2] = { guardCtx ? jsvLockAgain(guardCtx) : jsvNewObject(), jsvLockAgain(eventObj) };
          JsVar *res = xfsm_callJsFunction(cond, 0, args, 2);
          if (args[0]) jsvUnLock(args[0]);
          if (args[1]) jsvUnLock(args[1]);
          pass = res ? jsvGetBool(res) : false;  /* ✔ truthiness via jsvGetBool */
          if (res) jsvUnLock(res);
        }
        jsvUnLock(cond);
      }
      candSel = pass ? c : 0;
      if (!pass) jsvUnLock(c);
    } else if (jsvIsArray(cands)) {
      JsVarInt len = jsvGetArrayLength(cands);
      for (JsVarInt i=0;i<len;i++) {
        JsVar *el = jsvGetArrayItem(cands, i);
        if (!el) continue;

        JsVar *c = 0;
        if (jsvIsString(el)) {
          char toTmp[64]=""; str_from_jsv(el, toTmp, sizeof(toTmp));
          c = jsvNewObject();
          if (c) jsvObjectSetChildAndUnLock(c, K_TARGET, jsvNewFromString(toTmp));
        } else if (jsvIsObject(el)) {
          c = jsvLockAgain(el);
        }
        jsvUnLock(el);
        if (!c) continue;

        bool pass = true;
        JsVar *cond = jsvObjectGetChild(c, K_COND, 0);
        if (cond) {
          if (jsvIsFunction(cond)) {
            JsVar *args[2] = { guardCtx ? jsvLockAgain(guardCtx) : jsvNewObject(), jsvLockAgain(eventObj) };
            JsVar *res = xfsm_callJsFunction(cond, 0, args, 2);
            if (args[0]) jsvUnLock(args[0]);
            if (args[1]) jsvUnLock(args[1]);
            pass = res ? jsvGetBool(res) : false;  /* ✔ truthiness via jsvGetBool */
            if (res) jsvUnLock(res);
          }
          jsvUnLock(cond);
        }

        if (pass) { candSel = c; break; }
        jsvUnLock(c);
      }
    }
  }

  if (!candSel) {
    if (cands) jsvUnLock(cands);
    if (exitArr) jsvUnLock(exitArr);
    if (onObj) jsvUnLock(onObj);
    jsvUnLock(srcNode);
    if (guardCtx) jsvUnLock(guardCtx);
    jsvUnLock(states); jsvUnLock(cfg);
    return 0;
  }

  /* Build actions = exit[] + transition.actions[] + (entry[] if targeted) */
  JsVar *allActs = jsvNewArray(NULL, 0);

  /* exit[] */
  if (exitArr) {
    if (jsvIsArray(exitArr)) {
      JsVarInt xlen = jsvGetArrayLength(exitArr);
      for (JsVarInt i=0;i<xlen;i++) { JsVar *a = jsvGetArrayItem(exitArr, i); if (a){ jsvArrayPush(allActs, a); jsvUnLock(a);} }
    } else {
      jsvArrayPush(allActs, exitArr);
    }
  }

  /* transition.actions */
  JsVar *transActs = jsvObjectGetChild(candSel, K_ACTIONS, 0);
  if (transActs) {
    if (jsvIsArray(transActs)) {
      JsVarInt tlen = jsvGetArrayLength(transActs);
      for (JsVarInt i=0;i<tlen;i++) { JsVar *a = jsvGetArrayItem(transActs, i); if (a){ jsvArrayPush(allActs, a); jsvUnLock(a);} }
    } else {
      jsvArrayPush(allActs, transActs);
    }
  }

  /* target? */
  char toBuf[64] = "";
  JsVar *t = jsvObjectGetChild(candSel, K_TARGET, 0);
  if (t && jsvIsString(t)) str_from_jsv(t, toBuf, sizeof(toBuf));
  if (t) jsvUnLock(t);
  bool targetless = (toBuf[0] == 0);

  /* entry[] if targeted */
  if (!targetless) {
    JsVar *dstNode = jsvObjectGetChild(states, toBuf, 0);
    if (dstNode && jsvIsObject(dstNode)) {
      JsVar *entryArr = jsvObjectGetChild(dstNode, K_ENTRY, 0);
      if (entryArr) {
        if (jsvIsArray(entryArr)) {
          JsVarInt elen = jsvGetArrayLength(entryArr);
          for (JsVarInt i=0;i<elen;i++) { JsVar *a = jsvGetArrayItem(entryArr, i); if (a){ jsvArrayPush(allActs, a); jsvUnLock(a);} }
        } else {
          jsvArrayPush(allActs, entryArr);
        }
        jsvUnLock(entryArr);
      }
    }
    if (dstNode) jsvUnLock(dstNode);
  }

  /* create next state object (machine path: context is the guardCtx snapshot) */
  bool changed = (!targetless) && (0 != strcmp(fromBuf, toBuf));
  JsVar *st = new_state_obj(targetless ? fromBuf : toBuf, guardCtx /*locked or 0*/, allActs, changed);

  if (allActs) jsvUnLock(allActs);
  if (transActs) jsvUnLock(transActs);
  if (candSel) jsvUnLock(candSel);
  if (cands) jsvUnLock(cands);
  if (exitArr) jsvUnLock(exitArr);
  if (onObj) jsvUnLock(onObj);
  jsvUnLock(srcNode);
  if (guardCtx) jsvUnLock(guardCtx);
  jsvUnLock(states); jsvUnLock(cfg);

  return st; /* LOCKED */
}

/* ========================================================================== */
/*                           Service / Interpreter                             */
/* ========================================================================== */
// Initialize a Service object with an owned copy of its context
// svc: the Service JsVar (object) that already has K_CONFIG set
void xfsm_service_init(JsVar *serviceObj, JsVar *machineObj) {
  if (!serviceObj || !jsvIsObject(serviceObj)) return;
  if (!machineObj || !jsvIsObject(machineObj)) return;

  /* Bind machine to service */
  jsvObjectSetChildAndUnLock(serviceObj, K_MACHINE, jsvLockAgain(machineObj));

  /* Initialize service context from machine.config.context (if present) */
  JsVar *cfg = jsvObjectGetChild(machineObj, K_CFG, 0);
  if (cfg) {
    JsVar *ctx = jsvObjectGetChild(cfg, K_CONTEXT, 0);
    if (ctx) {
      jsvObjectSetChildAndUnLock(serviceObj, K_SCTX, jsvLockAgain(ctx));
      jsvUnLock(ctx);
    }
    jsvUnLock(cfg);
  }

  /* Seed _state with machine.initialState (PURE; entry actions NOT executed here) */
  JsVar *st = xfsm_machine_initial_state(machineObj);
  if (st) {
    jsvObjectSetChildAndUnLock(serviceObj, K_SSTATE, jsvLockAgain(st));
    jsvUnLock(st);
  }

  /* Unsubscribe factory is ready before any wrapper call */
  xfsm_ensure_unsub_factory();

  /* Set status to NotStarted */
  jsvObjectSetChildAndUnLock(serviceObj, K_SSTATUS, jsvNewFromString("NotStarted"));

}


JsVar *xfsm_service_start(JsVar *svc) {
  if (!svc || !jsvIsObject(svc)) return 0;

  /* already running? */
  JsVar *status = jsvObjectGetChild(svc, K_SSTATUS, 0);
  if (status && jsvIsString(status)) {
    char s[16]; str_from_jsv(status, s, sizeof(s));
    jsvUnLock(status);
    if (!strcmp(s, "Running")) return jsvLockAgain(svc);
  } else if (status) jsvUnLock(status);

  JsVar *m = jsvObjectGetChild(svc, K_MACHINE, 0);
  if (!m) return 0;

  JsVar *st = xfsm_machine_initial_state(m);
  if (!st) { jsvUnLock(m); return 0; }

  /* run entry actions with xstate.init */
  JsVar *ctx = jsvObjectGetChild(svc, K_SCTX, 0);
  JsVar *acts = jsvObjectGetChild(st, S_ACTS, 0);
  JsVar *val  = jsvObjectGetChild(st, S_VALUE, 0);
  char toBuf[64] = "";
  if (val && jsvIsString(val)) str_from_jsv(val, toBuf, sizeof(toBuf));

  JsVar *evtInit = jsvNewObject();
  if (evtInit) jsvObjectSetChildAndUnLock(evtInit, "type", jsvNewFromString("xstate.init"));
  run_actions_raw(svc, &ctx, acts, evtInit, 0, toBuf);
  if (evtInit) jsvUnLock(evtInit);

  /* persist updated context back to service */
  if (ctx) {
    jsvObjectSetChildAndUnLock(svc, K_SCTX, jsvLockAgain(ctx));
    /* IMPORTANT: also reflect into stored state object */
    jsvObjectSetChildAndUnLock(st, S_CTX, jsvLockAgain(ctx));
    jsvUnLock(ctx);
  }

  /* commit state + status */
  jsvObjectSetChildAndUnLock(svc, K_SSTATE, jsvLockAgain(st));
  jsvObjectSetChildAndUnLock(svc, K_SSTATUS, jsvNewFromString("Running"));

  xfsm_notify_listeners(svc);

  if (val) jsvUnLock(val);
  if (acts) jsvUnLock(acts);
  jsvUnLock(st);
  jsvUnLock(m);
  return jsvLockAgain(svc);
}

JsVar *xfsm_service_stop(JsVar *svc) {
  if (!svc) return 0;

  // Status -> "Stopped"
  jsvObjectSetChildAndUnLock(svc, K_SSTATUS, jsvNewFromString("Stopped"));

  // Clear all listeners
  JsVar *empty = jsvNewObject();
  if (empty) jsvObjectSetChildAndUnLock(svc, "_listeners", empty);

  // Return locked svc so the wrapper can return `this`
  return jsvLockAgain(svc);
}



/**
 * xfsm_service_send
 * Apply a transition to a running service.
 * Accepts event as string OR object; normalizes to {type:string,...}.
 * Executes actions with the **object** event; returns next state's value (locked string) or 0.
 */
JsVar *xfsm_service_send(JsVar *svc, JsVar *event /*string or object*/) {
  if (!svc || !event) return 0;

  /* must be running */
  JsVar *st = jsvObjectGetChild(svc, K_SSTATUS, 0);
  bool running = false;
  if (st && jsvIsString(st)) { char s[16]; str_from_jsv(st, s, sizeof(s)); running = (0==strcmp(s,"Running")); }
  if (st) jsvUnLock(st);
  if (!running) return 0;

  JsVar *m = jsvObjectGetChild(svc, K_MACHINE, 0); if (!m) return 0;

  /* normalize event */
  JsVar *evtObj = xfsm_normalize_event(event);
  if (!evtObj) { jsvUnLock(m); return 0; }

  /* previous service state (for fromStr) */
  char fromBuf[64] = "";
  JsVar *prev = jsvObjectGetChild(svc, K_SSTATE, 0);
  if (prev) {
    JsVar *pv = jsvObjectGetChild(prev, S_VALUE, 0);
    if (pv && jsvIsString(pv)) str_from_jsv(pv, fromBuf, sizeof(fromBuf));
    if (pv) jsvUnLock(pv);
    jsvUnLock(prev);
  }

  /* compute next pure state */
  JsVar *next = xfsm_machine_transition_ex(m, 0 /*use svc state via fromBuf if needed in ex*/, evtObj);
  if (!next) { jsvUnLock(evtObj); jsvUnLock(m); return 0; }

  /* execute actions */
  JsVar *acts = jsvObjectGetChild(next, S_ACTS, 0);
  JsVar *val  = jsvObjectGetChild(next, S_VALUE, 0);
  char toBuf[64] = "";
  if (val && jsvIsString(val)) str_from_jsv(val, toBuf, sizeof(toBuf));

  JsVar *ctx = jsvObjectGetChild(svc, K_SCTX, 0);
  run_actions_raw(svc, &ctx, acts, evtObj, fromBuf, toBuf);

  /* reflect updated ctx both into service and into state object */
  if (ctx) {
    jsvObjectSetChildAndUnLock(svc, K_SCTX, jsvLockAgain(ctx));
    jsvObjectSetChildAndUnLock(next, S_CTX, jsvLockAgain(ctx));
    jsvUnLock(ctx);
  }

  /* store next state object on service */
  jsvObjectSetChildAndUnLock(svc, K_SSTATE, jsvLockAgain(next));

  /* V2.1 addition: notify listeners after a successful transition */
  xfsm_notify_listeners(svc);

  /* return the new value */
  JsVar *retVal = jsvObjectGetChild(next, S_VALUE, 0);
  if (acts) jsvUnLock(acts);
  if (val)  jsvUnLock(val);
  jsvUnLock(next);
  jsvUnLock(evtObj);
  jsvUnLock(m);
  return retVal;
}

JsVar *xfsm_service_get_state(JsVar *svc) {
  if (!svc) return 0;
  JsVar *st = jsvObjectGetChild(svc, K_SSTATE, 0);
  if (!st) return 0;
  return st; // locked
}
JsVar *xfsm_service_get_status(JsVar *svc) {
  if (!svc) return 0;
  JsVar *v = jsvObjectGetChild(svc, K_SSTATUS, 0);
  if (!v) return jsvNewFromString("NotStarted");
  return v; // locked
}

/**
 * xfsm_service_get_status_num
 * Map _status string to numeric: NotStarted=0, Running=1, Stopped=2
 */

JsVar *xfsm_service_get_status_num(JsVar *svc) {
  if (!svc) return jsvNewFromInteger(0);
  JsVar *v = jsvObjectGetChild(svc, K_SSTATUS, 0);
  int num = 0;
  if (v && jsvIsString(v)) {
    char s[16]; str_from_jsv(v, s, sizeof(s));
    if (0 == strcmp(s, "Running")) num = 1;
    else if (0 == strcmp(s, "Stopped")) num = 2;
  }
  if (v) jsvUnLock(v);
  return jsvNewFromInteger(num);
}
