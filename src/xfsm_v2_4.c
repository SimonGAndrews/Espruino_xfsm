// XFSM_UPLOAD_ID: 2025-08-23-14-00-native-subscribe
// xfsm.c — Espruino JsVar-only FSM core with xstate-fsm alignment

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

// Public API (declared in xfsm.h):
//   V1 FSM (single-object): xfsm_init_object, xfsm_start_object, xfsm_stop_object,
//                           xfsm_status_object, xfsm_current_state_var, xfsm_send_object
//   Machine (pure): xfsm_machine_init, xfsm_machine_initial_state,
//                   xfsm_machine_state_for_value, xfsm_machine_transition
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