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

#include "xfsm.h"
#include "jsvar.h"
#include "jsutils.h"
#include "jsinteractive.h"
#include "jsparse.h"
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

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

/* ---------------- Utilities ---------------- */
static void set_status(JsVar *obj, const char *txt) {
  JsVar *v = jsvNewFromString(txt);
  jsvObjectSetChildAndUnLock(obj, K_STATUS, v);
}
static void set_str_prop(JsVar *obj, const char *k, const char *txt) {
  JsVar *v = jsvNewFromString(txt);
  jsvObjectSetChildAndUnLock(obj, k, v);
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
static JsVar *new_state_obj(const char *value, JsVar *ctx /*locked or 0*/, JsVar *acts /*locked or 0*/) {
  JsVar *st = jsvNewObject();
  if (value) {
    JsVar *sv = jsvNewFromString(value);
    jsvObjectSetChildAndUnLock(st, S_VALUE, sv);
  }
  if (ctx) jsvObjectSetChildAndUnLock(st, S_CTX, jsvLockAgain(ctx));
  if (acts) jsvObjectSetChildAndUnLock(st, S_ACTS, jsvLockAgain(acts));
  return st; // locked
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

/* Recognise assign action:
   - { type:"xstate.assign", assignment: ... }
   - { type:"assign", assignment: ... }
   - OR plain object (no 'type'): treat as shorthand assignment spec {k:fn|val,...}
*/
static bool is_assign_action(JsVar *item) {
  if (!item || !jsvIsObject(item)) return false;
  JsVar *type = jsvObjectGetChild(item, "type", 0); // locked or 0
  if (type) {
    bool ok = is_string_eq(type, "xstate.assign") || is_string_eq(type, "assign");
    jsvUnLock(type);
    if (!ok) return false;
    JsVar *ass = jsvObjectGetChild(item, "assignment", 0);
    if (ass) { jsvUnLock(ass); return true; }
    return false;
  }
  /* plain object -> shorthand assignment spec */
  return true;
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
static void apply_assignment(JsVar **pCtx, JsVar *assignment, JsVar *event, JsVar *meta) {
  if (!assignment) return;

  if (jsvIsFunction(assignment)) {
    JsVar *argv[3] = {
      *pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
      event ? jsvLockAgain(event) : jsvNewFromString(""),
      meta  ? jsvLockAgain(meta)  : jsvNewObject()
    };
    JsVar *res = xfsm_callJsFunction(assignment, 0 /* thisArg */, argv, 3);
    if (res) {
      if (jsvIsObject(res)) merge_patch_into_ctx(pCtx, res);
      jsvUnLock(res);
    }
    jsvUnLock(argv[0]); jsvUnLock(argv[1]); jsvUnLock(argv[2]);
    return;
  }

  /* Object assignment spec */

  if (jsvIsObject(assignment)) {
    JsVar *patch = jsvNewObject();
  
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, assignment);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *k    = jsvObjectIteratorGetKey(&it);     /* locked */
      JsVar *spec = jsvObjectIteratorGetValue(&it);   /* locked */
      if (k && jsvIsString(k) && spec) {
        char key[64]; size_t n = jsvGetString(k, key, sizeof(key)-1); key[n]=0;
        JsVar *out = 0;
        if (jsvIsFunction(spec)) {
          JsVar *argv[3] = {
            *pCtx ? jsvLockAgain(*pCtx) : jsvNewObject(),
            event ? jsvLockAgain(event) : jsvNewFromString(""),
            meta  ? jsvLockAgain(meta)  : jsvNewObject()
          };
          out = xfsm_callJsFunction(spec, 0 /* thisArg */, argv, 3);
          jsvUnLock(argv[0]); jsvUnLock(argv[1]); jsvUnLock(argv[2]);
        } else {
          out = jsvLockAgain(spec); /* value as-is */
        }
        if (out) { jsvObjectSetChildAndUnLock(patch, key, jsvLockAgain(out)); jsvUnLock(out); }
      }
      if (spec) jsvUnLock(spec);
      if (k)    jsvUnLock(k);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
  
    merge_patch_into_ctx(pCtx, patch);
    jsvUnLock(patch);
    return;
  }
  
  /* otherwise ignore */
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

/* ---------------- Execute actions (raw) ----------------
   - items may be: assign-object, function, string
   - owner: used to resolve named actions via config.actions/global
   - mutates *pCtx (merging patches); DOES NOT persist to container
*/
static void run_actions_raw(JsVar *owner,
                            JsVar **pCtx,
                            JsVar *actions,   /* array OR single item OR 0 */
                            JsVar *event,     /* string or 0 */
                            const char *fromState,
                            const char *toState) {
  if (!actions) return;

  /* shared meta */
  JsVar *meta = jsvNewObject();
  if (fromState && fromState[0]) { JsVar *fs=jsvNewFromString(fromState); jsvObjectSetChildAndUnLock(meta,"state",fs); }
  if (toState   && toState[0])   { JsVar *ts=jsvNewFromString(toState);   jsvObjectSetChildAndUnLock(meta,"target",ts); }
  if (event) { JsVar *evn = jsvLockAgain(event); jsvObjectSetChildAndUnLock(meta, "event", evn); }

  bool isArray = jsvIsArray(actions);
  unsigned int len = isArray ? jsvGetArrayLength(actions) : 1;

  for (unsigned int i=0;i<len;i++) {
    JsVar *item = isArray ? jsvGetArrayItem(actions, i) : jsvLockAgain(actions);
    if (!item) continue;

    if (is_assign_action(item)) {
      /* Determine assignment spec */
      JsVar *assignment = jsvObjectGetChild(item, "assignment", 0); // may be 0
      if (assignment) {
        apply_assignment(pCtx, assignment, event, meta);
        jsvUnLock(assignment);
      } else {
        /* shorthand: the object itself */
        apply_assignment(pCtx, item, event, meta);
      }
      jsvUnLock(item);
      continue;
    }

    /* Non-assign: function or named string */
    JsVar *fn = 0;
    if (jsvIsFunction(item)) fn = jsvLockAgain(item);
    else if (jsvIsString(item)) fn = resolveFunc(owner, item);

    if (fn) {
      if (!*pCtx || !jsvIsObject(*pCtx)) { if (*pCtx) jsvUnLock(*pCtx); *pCtx = jsvNewObject(); }
      JsVar *ev = event ? jsvLockAgain(event) : jsvNewFromString("");
      JsVar *argv[3] = { jsvLockAgain(*pCtx), ev, jsvLockAgain(meta) };
      JsVar *res = xfsm_callJsFunction(fn, 0, argv, 3);
      jsvUnLock(argv[0]); jsvUnLock(ev); jsvUnLock(argv[2]);
      if (res) {
        /* allow functions to return patch objects (merged) */
        if (jsvIsObject(res)) merge_patch_into_ctx(pCtx, res);
        jsvUnLock(res);
      }
      jsvUnLock(fn);
    }

    jsvUnLock(item);
  }

  jsvUnLock(meta);
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
  if (!fsmObject) return XFSM_STATUS_NOT_STARTED;

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
  if (!fsmObject) return XFSM_STATUS_NOT_STARTED;
  JsVar *v = jsvObjectGetChild(fsmObject, K_STATUS, 0);
  if (!v) return XFSM_STATUS_NOT_STARTED;
  XfsmStatus st = XFSM_STATUS_NOT_STARTED;
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

JsVar *xfsm_machine_initial_state(JsVar *m) {
  if (!m) return 0;
  JsVar *cfg = getChildObj(m, K_CFG); if (!cfg) return 0;

  char val[64]="idle";
  JsVar *init = jsvObjectGetChild(cfg, "initial", 0);
  if (init && jsvIsString(init)) str_from_jsv(init,val,sizeof(val));
  if (init) jsvUnLock(init);

  JsVar *ctx = jsvObjectGetChild(cfg, K_CONTEXT, 0); // may be 0
  JsVar *states = getChildObj(cfg, K_STATES);
  JsVar *node = (states) ? jsvObjectGetChild(states, val, 0) : 0;
  JsVar *entryActs = (node && jsvIsObject(node)) ? getActionListRaw(node, K_ENTRY) : 0;

  if (node) jsvUnLock(node);
  if (states) jsvUnLock(states);

  JsVar *st = new_state_obj(val, ctx, entryActs);
  if (entryActs) jsvUnLock(entryActs);
  if (ctx) jsvUnLock(ctx);
  jsvUnLock(cfg);
  return st; // locked
}

JsVar *xfsm_machine_transition(JsVar *m, JsVar *stateOrValue, JsVar *eventStr) {
  if (!m || !eventStr || !jsvIsString(eventStr)) return 0;

  char fromBuf[64]="";
  JsVar *ctx = 0;
  if (stateOrValue && jsvIsObject(stateOrValue)) {
    JsVar *sv = jsvObjectGetChild(stateOrValue, S_VALUE, 0);
    if (sv && jsvIsString(sv)) str_from_jsv(sv,fromBuf,sizeof(fromBuf));
    if (sv) jsvUnLock(sv);
    ctx = jsvObjectGetChild(stateOrValue, S_CTX, 0); // may be 0
  } else if (stateOrValue && jsvIsString(stateOrValue)) {
    str_from_jsv(stateOrValue, fromBuf, sizeof(fromBuf));
  }

  JsVar *cfg = getChildObj(m, K_CFG); if (!cfg) { if (ctx) jsvUnLock(ctx); return 0; }
  if (!fromBuf[0]) {
    JsVar *init = jsvObjectGetChild(cfg, "initial", 0);
    if (init && jsvIsString(init)) str_from_jsv(init,fromBuf,sizeof(fromBuf));
    if (init) jsvUnLock(init);
    if (!fromBuf[0]) strncpy(fromBuf,"idle",sizeof(fromBuf));
  }
  if (!ctx) ctx = jsvObjectGetChild(cfg, K_CONTEXT, 0); // may be 0

  JsVar *states = getChildObj(cfg, K_STATES);
  JsVar *srcNode = (states) ? jsvObjectGetChild(states, fromBuf, 0) : 0;
  if (!srcNode || !jsvIsObject(srcNode)) { if(srcNode) jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }

  JsVar *onObj = getChildObj(srcNode, K_ON);
  if (!onObj) { jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }

  char evKey[64]; str_from_jsv(eventStr, evKey, sizeof(evKey));
  JsVar *trans = jsvObjectGetChild(onObj, evKey, 0);
  if (!trans) { jsvUnLock(onObj); jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }

  /* guard */
  if (jsvIsObject(trans)) {
    JsVar *cond = jsvObjectGetChild(trans, K_COND, 0);
    if (cond) {
      JsVar *fn = resolveFunc(m, cond); jsvUnLock(cond);
      if (fn) {
        JsVar *meta = jsvNewObject();
        if (fromBuf[0]) { JsVar *fs=jsvNewFromString(fromBuf); jsvObjectSetChildAndUnLock(meta,"state",fs); }
        JsVar *argv[3] = { ctx?jsvLockAgain(ctx):jsvNewObject(), jsvLockAgain(eventStr), meta };
        JsVar *res = xfsm_callJsFunction(fn,0,argv,3);
        jsvUnLock(argv[0]); jsvUnLock(argv[1]); jsvUnLock(meta);
        if (res) { bool truthy = jsvGetBool(res); jsvUnLock(res);
          if (!truthy){ jsvUnLock(fn); jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }
        } else { jsvUnLock(fn); jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }
        jsvUnLock(fn);
      }
    }
  }

  char toBuf[64]={0};
  JsVar *exitActs=0,*transActs=0,*entryActs=0;

  if (jsvIsString(trans)) {
    str_from_jsv(trans,toBuf,sizeof(toBuf));
  } else if (jsvIsObject(trans)) {
    JsVar *tgt = jsvObjectGetChild(trans, K_TARGET, 0);
    if (tgt && jsvIsString(tgt)) str_from_jsv(tgt,toBuf,sizeof(toBuf));
    if (tgt) jsvUnLock(tgt);
    transActs = getTransitionActionsRaw(trans); // may be array or single
  }

  if (!toBuf[0]){ jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); if(states) jsvUnLock(states); if(ctx) jsvUnLock(ctx); jsvUnLock(cfg); return 0; }

  exitActs = getActionListRaw(srcNode, K_EXIT);
  JsVar *dstNode = (states)? jsvObjectGetChild(states, toBuf, 0) : 0;
  if (dstNode && jsvIsObject(dstNode)) entryActs = getActionListRaw(dstNode, K_ENTRY);
  if (dstNode) jsvUnLock(dstNode);

  /* Build combined RAW actions array: exit, trans, entry */
  JsVar *all = jsvNewArray(NULL,0);
  if (exitActs)  { unsigned int n=jsvGetArrayLength(exitActs); for (unsigned int i=0;i<n;i++){ JsVar *v=jsvGetArrayItem(exitActs,i); if(v){ jsvArrayPush(all,v); jsvUnLock(v);} } jsvUnLock(exitActs); }
  if (transActs) {
    if (jsvIsArray(transActs)) {
      unsigned int n=jsvGetArrayLength(transActs);
      for (unsigned int i=0;i<n;i++){ JsVar *v=jsvGetArrayItem(transActs,i); if(v){ jsvArrayPush(all,v); jsvUnLock(v);} }
    } else {
      jsvArrayPush(all, transActs);
    }
    jsvUnLock(transActs);
  }
  if (entryActs) { unsigned int n=jsvGetArrayLength(entryActs); for (unsigned int i=0;i<n;i++){ JsVar *v=jsvGetArrayItem(entryActs,i); if(v){ jsvArrayPush(all,v); jsvUnLock(v);} } jsvUnLock(entryActs); }

  JsVar *st = new_state_obj(toBuf, ctx, all);
  jsvUnLock(all);
  if (ctx) jsvUnLock(ctx);
  if (states) jsvUnLock(states);
  jsvUnLock(trans); jsvUnLock(onObj); jsvUnLock(srcNode); jsvUnLock(cfg);
  return st; // locked
}

/* ========================================================================== */
/*                           Service / Interpreter                             */
/* ========================================================================== */
// Initialize a Service object with an owned copy of its context
// svc: the Service JsVar (object) that already has K_CONFIG set
void xfsm_service_init(JsVar *svc, JsVar *machine) {
  if (!svc || !machine) return;

  jsvObjectSetChildAndUnLock(svc, K_MACHINE, jsvLockAgain(machine));
  set_str_prop(svc, K_SSTATUS, "NotStarted");

  JsVar *cfg = getChildObj(machine, K_CFG);
  JsVar *ctx = cfg ? jsvObjectGetChild(cfg, K_CONTEXT, 0) : 0;

  // Own a fresh handle for the service's context (defensive)
  JsVar *ctxOwned = ctx ? jsvLockAgain(ctx) : jsvNewObject();

  if (ctx) jsvUnLock(ctx);
  if (cfg) jsvUnLock(cfg);

  jsvObjectSetChildAndUnLock(svc, K_SCTX, ctxOwned);

  // jsDebug(DBG_INFO,"XFSM service init: context prepared \n");
}




void xfsm_service_start(JsVar *svc, JsVar *initialValue /*string or 0*/) {
  if (!svc) return;

  set_str_prop(svc, K_SSTATUS, "Running");

  JsVar *m = jsvObjectGetChild(svc, K_MACHINE, 0);
  if (!m) return;

  JsVar *st = xfsm_machine_initial_state(m);
  if (!st) { jsvUnLock(m); return; }

  /* Read raw actions and value BEFORE storing st on service */
  JsVar *val  = jsvObjectGetChild(st, S_VALUE, 0);
  JsVar *acts = jsvObjectGetChild(st, S_ACTS, 0);
  char toBuf[64]={0}; if (val && jsvIsString(val)) str_from_jsv(val,toBuf,sizeof(toBuf));

  JsVar *ctx = jsvObjectGetChild(svc, K_SCTX, 0); // locked or 0
  run_actions_raw(svc, &ctx, acts, /*event*/0, /*from*/0, /*to*/toBuf);

  /* Persist context ONCE */
  if (ctx) { jsvObjectSetChildAndUnLock(svc, K_SCTX, jsvLockAgain(ctx)); jsvUnLock(ctx); }

  /* Store state object */
  jsvObjectSetChildAndUnLock(svc, K_SSTATE, st); // consumes st

  if (acts) jsvUnLock(acts);
  if (val)  jsvUnLock(val);
  jsvUnLock(m);
}

void xfsm_service_stop(JsVar *svc) { if (!svc) return; set_str_prop(svc, K_SSTATUS, "Stopped"); }

JsVar *xfsm_service_send(JsVar *svc, JsVar *eventStr /*string*/) {
  if (!svc || !eventStr || !jsvIsString(eventStr)) return 0;

  /* must be running */
  JsVar *st = jsvObjectGetChild(svc, K_SSTATUS, 0);
  bool running = false;
  if (st && jsvIsString(st)) { char s[16]; str_from_jsv(st,s,sizeof(s)); running = (0==strcmp(s,"Running")); }
  if (st) jsvUnLock(st);
  if (!running) return 0;

  JsVar *m   = jsvObjectGetChild(svc, K_MACHINE, 0); if (!m) return 0;
  JsVar *cur = jsvObjectGetChild(svc, K_SSTATE, 0);  // locked or 0
  JsVar *next= xfsm_machine_transition(m, cur?cur:0, eventStr); // locked or 0
  if (cur) jsvUnLock(cur);
  if (!next){ jsvUnLock(m); return 0; }

  /* Execute raw actions over service _context */
  JsVar *ctx  = jsvObjectGetChild(svc, K_SCTX, 0);
  JsVar *acts = jsvObjectGetChild(next, S_ACTS, 0);
  JsVar *val  = jsvObjectGetChild(next, S_VALUE, 0);
  char toBuf[64]=""; if (val && jsvIsString(val)) str_from_jsv(val,toBuf,sizeof(toBuf));

  run_actions_raw(svc, &ctx, acts, eventStr, /*from*/0, /*to*/toBuf);

  if (acts) jsvUnLock(acts);
  if (val)  jsvUnLock(val);

  /* persist updated context */
  if (ctx) { jsvObjectSetChildAndUnLock(svc, K_SCTX, jsvLockAgain(ctx)); jsvUnLock(ctx); }

  /* commit service state */
  jsvObjectSetChildAndUnLock(svc, K_SSTATE, jsvLockAgain(next));

  /* return new state's value */
  JsVar *retVal = jsvObjectGetChild(next, S_VALUE, 0); // locked or 0
  jsvUnLock(next);
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
