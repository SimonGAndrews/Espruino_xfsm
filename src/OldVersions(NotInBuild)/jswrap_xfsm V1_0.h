// jswrap_xfsm.h — Unified wrapper header for FSM + Machine + Service

#ifndef JSWRAP_XFSM_H
#define JSWRAP_XFSM_H

#include "jsvar.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------- FSM (V1 compatibility class) -------- */
JsVar *jswrap_xfsm_constructor(JsVar *config);
JsVar *jswrap_xfsm_start(JsVar *parent, JsVar *initialState);
JsVar *jswrap_xfsm_stop(JsVar *parent);
JsVar *jswrap_xfsm_statusText(JsVar *parent);
JsVar *jswrap_xfsm_current(JsVar *parent);
JsVar *jswrap_xfsm_send(JsVar *parent, JsVar *event);

/* -------- Machine (pure) -------- */
JsVar *jswrap_machine_constructor(JsVar *config, JsVar *options);
JsVar *jswrap_machine_initialState(JsVar *parent);
JsVar *jswrap_machine_transition(JsVar *parent, JsVar *stateOrValue, JsVar *eventStr);
JsVar *jswrap_machine_interpret(JsVar *parent);

/* -------- Service (interpreter) -------- */
JsVar *jswrap_service_start(JsVar *parent, JsVar *initialValue);
JsVar *jswrap_service_stop(JsVar *parent);
JsVar *jswrap_service_send(JsVar *parent, JsVar *eventStr);
JsVar *jswrap_service_state(JsVar *parent);
JsVar *jswrap_service_statusText(JsVar *parent);

#ifdef __cplusplus
}
#endif

#endif // JSWRAP_XFSM_H
