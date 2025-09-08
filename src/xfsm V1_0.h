// xfsm.h — Plan A: Machine/Service + V1 (FSM) compatibility
#ifndef XFSM_H
#define XFSM_H

#include "jsvar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  XFSM_STATUS_NOT_STARTED = 0,
  XFSM_STATUS_RUNNING     = 1,
  XFSM_STATUS_STOPPED     = 2
} XfsmStatus;

/* ---------------- V1 single-object FSM (kept for compatibility) ---------------- */
void       xfsm_init_object(JsVar *fsmObject);
XfsmStatus xfsm_start_object(JsVar *fsmObject, JsVar *initialState /*locked or 0*/);
void       xfsm_stop_object(JsVar *fsmObject);
XfsmStatus xfsm_status_object(JsVar *fsmObject);
JsVar     *xfsm_current_state_var(JsVar *fsmObject);
JsVar     *xfsm_send_object(JsVar *fsmObject, JsVar *event /*locked string*/);

/* ---------------- Machine (pure) ----------------
   State object shape we return:
     { value: <string>, context: <object>, actions: <array-of-functions> }
*/
void   xfsm_machine_init(JsVar *machineObj); // ensures structure; does not compute/store initial
JsVar *xfsm_machine_initial_state(JsVar *machineObj); // locked state obj
JsVar *xfsm_machine_transition(JsVar *machineObj, JsVar *stateOrValue /*state obj or string*/, JsVar *eventStr /*string*/); // locked state obj

/* ---------------- Service (stateful interpreter) ----------------
   Keeps independent _context (object), _state (state object), _status (string)
*/
void   xfsm_service_init(JsVar *serviceObj, JsVar *machineObj);
void   xfsm_service_start(JsVar *serviceObj, JsVar *initialValue /*locked string or 0*/);
void   xfsm_service_stop(JsVar *serviceObj);
JsVar *xfsm_service_send(JsVar *serviceObj, JsVar *eventStr /*locked string*/); // returns new state.value (locked) or 0
JsVar *xfsm_service_get_state(JsVar *serviceObj);   // locked state object or 0
JsVar *xfsm_service_get_status(JsVar *serviceObj);  // locked string

#ifdef __cplusplus
}
#endif

#endif // XFSM_H
