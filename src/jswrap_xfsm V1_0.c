// jswrap_xfsm.c — Unified JavaScript wrappers for Espruino
// Exposes 3 classes to JS:
//   - FSM      (V1 compatibility, state stored on the instance)
//   - Machine  (pure, creates state objects and Services)
//   - Service  (interpreter; runs actions/guards; maintains its own status/context)

#include "jswrap_xfsm.h"
#include "xfsm.h"
#include "jsvar.h"
#include "jsutils.h"
#include "jsinteractive.h"
#include "jsparse.h"
#include <string.h>

/* ========================================================================== */
/*                              FSM (V1)                                      */
/* ========================================================================== */

/*JSON{
  "type"  : "class",
  "class" : "FSM",
  "name"  : "FSM"
}*/

/*JSON{
  "type"     : "constructor",
  "class"    : "FSM",
  "name"     : "FSM",
  "generate" : "jswrap_xfsm_constructor",
  "params"   : [["config", "JsVar", "FSM configuration object"]],
  "return"   : ["JsVar", "A new FSM instance"]
}*/
JsVar *jswrap_xfsm_constructor(JsVar *config) {
  JsVar *obj = jspNewObject(0, "FSM");
  if (!obj) return 0;

  // Store config (copy or empty object)
  JsVar *cfg = (config && jsvIsObject(config)) ? jsvLockAgain(config) : jsvNewObject();
  jsvObjectSetChildAndUnLock(obj, "config", cfg);

  // Initialise defaults
  xfsm_init_object(obj);
  return obj;
}

/*JSON{
  "type"     : "method",
  "class"    : "FSM",
  "name"     : "start",
  "generate" : "jswrap_xfsm_start",
  "params"   : [["initialState", "JsVar", "[optional] initial state string"]],
  "return"   : ["JsVar", "Current FSM status string"]
}*/
JsVar *jswrap_xfsm_start(JsVar *parent, JsVar *initialState) {
  if (!jsvIsObject(parent)) return jsvNewFromString("NotStarted");

  JsVar *stateToSet = 0;
  if (initialState && !jsvIsUndefined(initialState) && !jsvIsNull(initialState)) {
    if (!jsvIsString(initialState)) {
      jsExceptionHere(JSET_ERROR, "FSM.start: initialState must be a string");
      return jsvNewFromString("NotStarted");
    }
    stateToSet = jsvLockAgain(initialState);
  }

  XfsmStatus st = xfsm_start_object(parent, stateToSet);
  if (stateToSet) jsvUnLock(stateToSet);

  return (st == XFSM_STATUS_RUNNING)
           ? jsvNewFromString("Running")
           : (st == XFSM_STATUS_STOPPED ? jsvNewFromString("Stopped")
                                        : jsvNewFromString("NotStarted"));
}

/*JSON{
  "type"     : "method",
  "class"    : "FSM",
  "name"     : "stop",
  "generate" : "jswrap_xfsm_stop",
  "return"   : ["JsVar", "undefined"]
}*/
JsVar *jswrap_xfsm_stop(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  xfsm_stop_object(parent);
  return 0; // undefined
}

/*JSON{
  "type"     : "method",
  "class"    : "FSM",
  "name"     : "statusText",
  "generate" : "jswrap_xfsm_statusText",
  "return"   : ["JsVar", "Current FSM status string"]
}*/
JsVar *jswrap_xfsm_statusText(JsVar *parent) {
  if (!jsvIsObject(parent)) return jsvNewFromString("NotStarted");
  XfsmStatus st = xfsm_status_object(parent);
  return (st == XFSM_STATUS_RUNNING)
           ? jsvNewFromString("Running")
           : (st == XFSM_STATUS_STOPPED ? jsvNewFromString("Stopped")
                                        : jsvNewFromString("NotStarted"));
}

/*JSON{
  "type"     : "method",
  "class"    : "FSM",
  "name"     : "current",
  "generate" : "jswrap_xfsm_current",
  "return"   : ["JsVar", "Current FSM state string or undefined"]
}*/
JsVar *jswrap_xfsm_current(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  return xfsm_current_state_var(parent); // locked or 0
}

/*JSON{
  "type"     : "method",
  "class"    : "FSM",
  "name"     : "send",
  "generate" : "jswrap_xfsm_send",
  "params"   : [["event", "JsVar", "Event string"]],
  "return"   : ["JsVar", "New state string or undefined if no transition"]
}*/
JsVar *jswrap_xfsm_send(JsVar *parent, JsVar *event) {
  if (!jsvIsObject(parent)) return 0;
  if (!event || !jsvIsString(event)) {
    jsExceptionHere(JSET_ERROR, "FSM.send: event must be a string");
    return 0;
  }
  return xfsm_send_object(parent, event); // locked or 0
}

/* ========================================================================== */
/*                              Machine (pure)                                */
/* ========================================================================== */

/*JSON{
  "type":"class", "class":"Machine", "name":"Machine"
}*/

/*JSON{
  "type":"constructor","class":"Machine","name":"Machine",
  "generate":"jswrap_machine_constructor",
  "params":[["config","JsVar","FSM config object"],["options","JsVar","[optional] options (unused yet)"]],
  "return":["JsVar","Machine instance"]
}*/
JsVar *jswrap_machine_constructor(JsVar *config, JsVar *options) {
  JsVar *obj = jspNewObject(0, "Machine");
  if (!obj) return 0;
  jsvObjectSetChildAndUnLock(obj, "config", (config && jsvIsObject(config)) ? jsvLockAgain(config) : jsvNewObject());
  if (options && jsvIsObject(options))
    jsvObjectSetChildAndUnLock(obj, "_options", jsvLockAgain(options));
  else
    jsvObjectSetChildAndUnLock(obj, "_options", jsvNewObject());
  xfsm_machine_init(obj);
  return obj;
}

/*JSON{
  "type":"method","class":"Machine","name":"initialState",
  "generate":"jswrap_machine_initialState",
  "return":["JsVar","State object {value,context,actions}"]
}*/
JsVar *jswrap_machine_initialState(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  return xfsm_machine_initial_state(parent);
}

/*JSON{
  "type":"method","class":"Machine","name":"transition",
  "generate":"jswrap_machine_transition",
  "params":[["stateOrValue","JsVar","Current state object or value string"],["event","JsVar","Event string"]],
  "return":["JsVar","Next state object or undefined"]
}*/
JsVar *jswrap_machine_transition(JsVar *parent, JsVar *stateOrValue, JsVar *eventStr) {
  if (!jsvIsObject(parent) || !eventStr || !jsvIsString(eventStr)) return 0;
  return xfsm_machine_transition(parent, stateOrValue, eventStr);
}

/*JSON{
  "type":"method","class":"Machine","name":"interpret",
  "generate":"jswrap_machine_interpret",
  "return":["JsVar","A new Service interpreter"]
}*/
JsVar *jswrap_machine_interpret(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  JsVar *svc = jspNewObject(0, "Service");
  if (!svc) return 0;
  xfsm_service_init(svc, parent);
  return svc;
}

/* ========================================================================== */
/*                              Service (interpreter)                          */
/* ========================================================================== */

/*JSON{
  "type":"class", "class":"Service", "name":"Service"
}*/

/*JSON{
  "type":"method","class":"Service","name":"start",
  "generate":"jswrap_service_start",
  "params":[["initialValue","JsVar","[optional] starting state value string"]],
  "return":["JsVar","this"]
}*/
JsVar *jswrap_service_start(JsVar *parent, JsVar *initialValue) {
  if (!jsvIsObject(parent)) return 0;
  if (initialValue && !jsvIsString(initialValue)) {
    jsExceptionHere(JSET_ERROR, "Service.start: initialValue must be a string");
    return jsvLockAgain(parent); // return locked 'this' on error
  }
  xfsm_service_start(parent, initialValue ? initialValue : 0);
  return jsvLockAgain(parent); // IMPORTANT: return a locked 'this'
}

/*JSON{
  "type":"method","class":"Service","name":"stop",
  "generate":"jswrap_service_stop",
  "return":["JsVar","this"]
}*/
JsVar *jswrap_service_stop(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  xfsm_service_stop(parent);
  return jsvLockAgain(parent); // return a locked 'this'
}

/*JSON{
  "type":"method","class":"Service","name":"send",
  "generate":"jswrap_service_send",
  "params":[["event","JsVar","Event string"]],
  "return":["JsVar","New state value string or this (chainable)"]
}*/
JsVar *jswrap_service_send(JsVar *parent, JsVar *event) {
  if (!jsvIsObject(parent)) return 0;
  if (!event || !jsvIsString(event)) {
    jsExceptionHere(JSET_ERROR, "Service.send: event must be a string");
    return jsvLockAgain(parent); // chainable on error
  }
  JsVar *st = xfsm_service_send(parent, event); // returns 0 or a *locked* string
  if (st) return st;             // return newly created locked value
  return jsvLockAgain(parent);    // chainable when no transition
}


/*JSON{
  "type":"method","class":"Service","name":"state",
  "generate":"jswrap_service_state",
  "return":["JsVar","Current state object"]
}*/
JsVar *jswrap_service_state(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  return xfsm_service_get_state(parent);
}

/*JSON{
  "type":"method","class":"Service","name":"statusText",
  "generate":"jswrap_service_statusText",
  "return":["JsVar","Current status string"]
}*/
JsVar *jswrap_service_statusText(JsVar *parent) {
  if (!jsvIsObject(parent)) return 0;
  return xfsm_service_get_status(parent);
}
