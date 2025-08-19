# Espruino FSM: Executing JavaScript from C (Team Share Pack)

## 1) What this note contains
- The agreed backend approach to execute JS from C inside the FSM.
- Drop-in helper prototypes to keep usage consistent.
- A tiny self-test you can run from the Espruino REPL.
- Exact files/places to touch so other threads know where to look/change.
- A PR/checklist workflow so we don’t diverge.

---

## 2) One-paragraph summary (copy/paste into PRs/issues)
The FSM’s action execution uses Espruino’s internal function-call API to invoke a `JsVar*` function with an optional `this` and an argv list, avoiding per-transition `eval`. Named actions are resolved from `execInfo.root` (global) once and reused; string actions are compiled **once** at `.start()` using the `Function` constructor or `jspEvaluateVar`, then called via the same path. This keeps transitions fast, safe, and consistent with Espruino’s locking rules.

---

## 3) References (where things live)
- Global object: `execInfo.root` (in the interactive subsystem).
- Evaluate JS string: `jspEvaluateVar(...)` (parse/eval path).
- Construct a Function from strings: Function constructor wrapper in `jswrap_functions.c`.
- **Direct function call API**: declared in the interactive subsystem header (verify signature in your `jsinteractive.h` and use that exact name/signature).

> Team rule: **Do not rename the backend call API** in this doc. Always use the exact symbol/signature from `jsinteractive.h` in your branch and paste it below (Section 4.1) before merging.

---

## 4) Standard helpers (put in `jswrap_xfsm.c`)
Paste these **as-is**, but replace the call to the function exec primitive with the exact symbol/signature from your `jsinteractive.h` (see 4.1). Use two-space indents.

### 4.1 Confirmed function call primitive (paste from `jsinteractive.h`)
(Team member: copy the exact prototype here)
    
    // Example (replace with your exact signature before merge)
    JsVar *jsiExecuteFunction(JsVar *func, JsVar *thisArg, int argc, JsVar **argv);

### 4.2 Call a JsVar* function

    static JsVar *xfsm_callJsFunction(JsVar *fn, JsVar *thisVar, JsVar **argv, int argc) {
      if (!fn || !jsvIsFunction(fn)) {
        jsError("FSM: attempted to call a non-function");
        return 0;
      }
      JsVar *thisObj = thisVar ? thisVar : jsvGetNull();
      JsVar *res = jsiExecuteFunction(fn, thisObj, argc, argv); // <-- keep signature synced
      jsvUnLock(thisObj);
      return res; // caller must jsvUnLock(res) if non-null
    }

### 4.3 Call a named global function (resolve once if possible)

    static JsVar *xfsm_callNamedJsFunction(const char *name, JsVar *thisVar, JsVar **argv, int argc) {
      JsVar *root = jsvLockAgain(execInfo.root);
      if (!root) {
        jsError("FSM: no global root");
        return 0;
      }
      JsVar *fn = jsvObjectGetChild(root, name, 0);
      JsVar *res = 0;
      if (fn && jsvIsFunction(fn)) {
        JsVar *thisObj = thisVar ? thisVar : jsvGetNull();
        res = jsiExecuteFunction(fn, thisObj, argc, argv);
        jsvUnLock(thisObj);
      } else {
        jsError("FSM: function '%s' not found/not callable", name);
      }
      jsvUnLock(fn);
      jsvUnLock(root);
      return res;
    }

### 4.4 Optional: compile string → function once (at `.start()`)

    static JsVar *xfsm_compileFunctionFromString(const char *bodySrc) {
      // Wrap as classic Function(): (function(){ <body> })
      char buf[512];
      if (!bodySrc) return 0;
      if (strlen(bodySrc) + 32 >= sizeof(buf)) {
        jsError("FSM: action code too long for static buffer");
        return 0;
      }
      // Use classic function wrapper for older parsers
      // NOTE: You can switch to Function constructor path from jswrap_functions.c if preferred.
      sprintf(buf, "(function(){ %s })", bodySrc);

      // Evaluate once and verify it produced a function
      JsVar *fn = jspEvaluateVar(buf, 0, "xfsm_compileFunctionFromString");
      if (!fn || !jsvIsFunction(fn)) {
        jsError("FSM: eval did not produce a function");
        jsvUnLock(fn);
        return 0;
      }
      return fn; // caller owns lock
    }

---

## 5) Locking & ownership (quick rules)
- If a helper returns `JsVar *res`, the **caller must `jsvUnLock(res)`** when finished.
- Pass `thisVar == 0` to use `null` (`jsvGetNull()`), which you must `jsvUnLock`.
- Build args as locked `JsVar*`, pass them in `argv`, and **unlock them after the call** (the callee does not take ownership).
- Store compiled/inline action functions **locked** in your FSM instance; unlock them at destroy.

---

## 6) Tiny REPL self-test (put under `#ifdef FSM_SELFTEST` in `jswrap_xfsm.c`)
Run from Espruino REPL to sanity-check the call path.

    static void xfsm_selftest(void) {
      // Build args
      JsVar *a = jsvNewFromInteger(2);
      JsVar *b = jsvNewFromInteger(3);

      // Make an inline function: function(a,b){return a+b;}
      JsVar *fn = jspEvaluateVar("(function(a,b){return a+b;})", 0, "xfsm_selftest");
      if (!fn || !jsvIsFunction(fn)) {
        jsError("FSM selftest: failed to create function");
        jsvUnLock(fn);
        jsvUnLock(a); jsvUnLock(b);
        return;
      }

      JsVar *argv[2] = { a, b };
      JsVar *res = xfsm_callJsFunction(fn, 0, argv, 2);
      if (res) {
        int v = jsvGetInteger(res);
        jsDebug(DBG_INFO, "FSM selftest result: %d\n", v);
        jsvUnLock(res);
      }

      jsvUnLock(fn);
      jsvUnLock(a); jsvUnLock(b);
    }

> Enable with a temporary `#define FSM_SELFTEST 1` and call `xfsm_selftest()` from your wrapper’s init path (then remove before merge).

---

## 7) Where to place things
- Helpers (4.2–4.4): `libs/XFSM/jswrap_xfsm.c` (or your current wrapper path).
- Declarations (if you want to expose internally): `libs/XFSM/jswrap_xfsm.h`.
- Notes for reviewers: add a short comment in `jswrap_xfsm.c` above the helpers pointing to this doc.

---