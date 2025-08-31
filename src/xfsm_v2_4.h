// XFSM_UPLOAD_ID: 2025-08-23-14-00-native-subscribe
#ifndef CORE_XFSM_H
#define CORE_XFSM_H

#include "jsvar.h"
#include <stdbool.h>

/* ------------------------------------------------------------------------- */
/*  InterpreterStatus values (mirrors xstate-fsm.js for compatibility)       */
/* ------------------------------------------------------------------------- */
typedef enum {
  XFSM_STATUS_NOTSTARTED = 0,
  XFSM_STATUS_RUNNING    = 1,
  XFSM_STATUS_STOPPED    = 2
} XfsmStatus;

/* ------------------------------------------------------------------------- */
/*  V1 single-object FSM (kept for compatibility)                            */
/*  Prefer Service/Machine API (V2+) for new development.                    */
/* ------------------------------------------------------------------------- */
void       xfsm_init_object(JsVar *fsmObject);
XfsmStatus xfsm_start_object(JsVar *fsmObject, JsVar *initialState /*locked or 0*/);
void       xfsm_stop_object(JsVar *fsmObject);
XfsmStatus xfsm_status_object(JsVar *fsmObject);
JsVar     *xfsm_current_state_var(JsVar *fsmObject);
JsVar     *xfsm_send_object(JsVar *fsmObject, JsVar *event /*locked string*/);

/* ------------------------------------------------------------------------- */
/*  Core Machine API (V2)                                                    */
/* ------------------------------------------------------------------------- */

/* Normalize input (string or object) into event object */
JsVar *xfsm_normalize_event(JsVar *event);

/* Create initial state object from a machine */
JsVar *xfsm_machine_initial_state(JsVar *machineObj);

/* Transition with string or object event */
JsVar *xfsm_machine_transition(JsVar *machineObj, JsVar *state, JsVar *eventStr);
JsVar *xfsm_machine_transition_ex(JsVar *machineObj, JsVar *prevStateOrValue, JsVar *eventObj);

/* Run ordered actions (exit → trans.actions → entry) */
void run_actions_raw(JsVar *serviceObj, JsVar **ctx, JsVar *actions,
                     JsVar *eventObj, const char *fromStr, const char *toStr);

/* NOTE: No public xfsm_apply_assignment — internal helper is static in xfsm.c */

/* ------------------------------------------------------------------------- */
/*  Service / Interpreter API                                                */
/* ------------------------------------------------------------------------- */

/* Minimal initializers used by wrappers */
void  xfsm_machine_init(JsVar *machineObj);
void  xfsm_service_init(JsVar *serviceObj, JsVar *machineObj);

/* Start/stop/send */
JsVar *xfsm_service_start(JsVar *svc);

/* Stop the service (sets status, clears listeners) */
JsVar *xfsm_service_stop(JsVar *svc);

/* Send event to service */
JsVar *xfsm_service_send(JsVar *svc, JsVar *event);

/* Accessors */
JsVar *xfsm_service_get_state(JsVar *serviceObj);
JsVar *xfsm_service_get_status(JsVar *serviceObj);
JsVar *xfsm_service_get_status_num(JsVar *serviceObj);

/* ------------------------------------------------------------------------- */
/*  V2.1: Subscription + Validation Helpers                                  */
/* ------------------------------------------------------------------------- */

/* Call all registered listeners with latest state */
void xfsm_notify_listeners(JsVar *service);

/* Validate that config.states has no nested substates (flat only) */
bool xfsm_validate_no_nested_states(JsVar *machineConfig);

void xfsm_ensure_unsub_factory(void);
JsVar *xfsm_make_unsubscribe(JsVar *svc, int id);  // returns LOCKED function


#endif /* CORE_XFSM_H */
