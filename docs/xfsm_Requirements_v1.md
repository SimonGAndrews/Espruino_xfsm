# xfsm_Requirements_v1.md
Developer-Focused, Testable Requirements for the Espruino FSM Library (Native C)

## 1. Scope and Goals
The Espruino FSM Library provides an **XState v4 FSM–equivalent** state machine engine implemented in **C**, exposed to Espruino JavaScript via the standard library mechanism (jswrap). The goal is **full feature parity with the XState v4 `xstate-fsm` package** (minimal ~1 kB FSM), while aligning with XState semantics to enable tooling, visualization, and future AI-assisted workflows. A future roadmap includes **hierarchical (nested) states** and deeper **SCXML alignment**.

## 2. Terminology
- Machine: A configured finite state machine instance.
- State Value: Current state identifier (string).
- Context: A mutable, user-defined data object carried by the machine.
- Event: An object `{ type: string, ... }` (string shorthand expands to `{ type }`).
- Action: A side-effect to perform; may be a string key, function, or action object.
- Guard (`cond`): A predicate `(context, event) => boolean` determining transition viability.
- Assign: Special action `xstate.assign` (or equivalent constant) that updates context.

## 3. Parity Targets (XState v4 FSM)
The following features must be implemented in Phase 1 to achieve **100% parity** with the `xstate-fsm` package:

- REQ-FSM-01  Machine creation via `createMachine(config, options?)`.
- REQ-FSM-02  Config supports:
  - `id?: string`
  - `initial: string`
  - `context?: object`
  - `states: { [stateId: string]: StateConfig }`
- REQ-FSM-03  Each `StateConfig` supports:
  - `entry?: Action | Action[]`
  - `exit?: Action | Action[]`
  - `on?: { [eventType: string]: Transition | Transition[] }`
- REQ-FSM-04  `Transition` supports:
  - String target shorthand (e.g., `"next"`) or object form:
    - `target?: string` (targetless when omitted)
    - `actions?: Action | Action[]`
    - `cond?: (context, event) => boolean`
- REQ-FSM-05  `options.actions?: { [key: string]: ActionFunction }` maps string action names to functions.
- REQ-FSM-06  Event normalization: strings become `{ type: string }`.
- REQ-FSM-07  Initial state:
  - Respects `config.initial`.
  - Prepares `entry` actions for the initial state.
  - Computes initial `context` by running `assign` actions queued in the initial state’s `entry` (but see Execution Model below—actions are executed on `start()`).
- REQ-FSM-08  Transition semantics:
  - Look up transitions from `states[current].on[event.type]` (array or single).
  - For each candidate:
    - Normalize to object shape.
    - Default `cond` is `() => true`.
    - If `cond(context, event)` is true, select and resolve it.
  - **Targeted transition**:
    - Queue actions: `exit(current) → transition.actions → entry(target)`.
  - **Targetless transition**:
    - Queue actions: `transition.actions` only (no exit/entry).
- REQ-FSM-09  `assign` action:
  - Action object form: `{ type: 'xstate.assign', assignment }`.
  - `assignment` may be:
    - a function `(ctx, evt) => nextContext`
    - an object mapping `{ key: valueOrFn }`, where each `valueOrFn` is either value or `(ctx, evt) => value`.
  - **Order**: `assign` mutates/produces `context` **before** other non-assign actions execute.
- REQ-FSM-10  Transition result:
  - Returns `{ value, context, actions, changed, matches }`.
  - `changed === true` when:
    - `target !== current`, or
    - any non-assign actions produced, or
    - any `assign` occurred.
  - `matches` is a predicate `matches(s: string): boolean` that compares equality with `value`.
- REQ-FSM-11  No match behavior:
  - Returns unchanged state with `actions: []`, `changed: false`.
- REQ-FSM-12  Nested states: **must be rejected** (Phase 1 parity with `xstate-fsm` which disallows nesting).
- REQ-FSM-13  Interpreter (service) API via `interpret(machine)`:
  - `start(initialStateOrValue?)` → service (sets status to Running; executes pending actions for the current state with `INIT_EVENT`).
  - `stop()` → service (sets status to Stopped; clears listeners).
  - `send(event)` (no-op if not Running; otherwise transitions and executes actions).
  - `subscribe(listener)` returns `{ unsubscribe() }` and immediately calls listener with current state.
  - `state` getter returns current state object.
  - `status` getter returns status enum with values `NotStarted`, `Running`, `Stopped`.
- REQ-FSM-14  Initial actions execution timing:
  - `initialState.actions` must be **executed when `start()` is called**, not at machine creation.
- REQ-FSM-15  Action function shape:
  - For function-based actions, execute as `exec(context, event)`.
  - For string or object actions, resolve to `{ type, exec? }` using `options.actions` then execute `exec` if present.
- REQ-FSM-16  Determinism:
  - Transition selection order follows definition order (array order).
  - Action queues maintain program order with `assign` extraction executed first in-context-update pass.
- REQ-FSM-17  Production checks:
  - In production mode, avoid assertion-heavy checks; in dev builds, error when state/target missing or nested states detected.

## 4. Execution Model and Ordering
- REQ-EXEC-01  On `start()`:
  - Service status becomes Running.
  - Execute queued `initialState.actions` with the synthetic `INIT_EVENT = { type: 'xstate.init' }`.
- REQ-EXEC-02  On `send(event)`:
  - If service not Running, ignore.
  - Compute next state via `machine.transition(current, event)`.
  - Execute resulting `state.actions` with the dispatched `event`.
- REQ-EXEC-03  Assign handling:
  - When computing transition, **partition** actions into:
    - assign actions → applied immediately to derive `nextContext`
    - non-assign actions → emitted to be executed post-transition
- REQ-EXEC-04  Entry/Exit ordering (targeted only):
  - `exit(current)` → `transition.actions` (non-assign) → `entry(target)`.
- REQ-EXEC-05  Targetless transition:
  - Only `transition.actions` (assign + non-assign as applicable).
- REQ-EXEC-06  `matches`:
  - Simple equality predicate against flat state id strings.

## 5. Error Handling and Validation
- REQ-ERR-01  Machine creation:
  - If `initial` not found in `states`, throw error (dev).
  - If any state contains `states` (nested), throw error (dev).
- REQ-ERR-02  Transition:
  - If `current` or `target` not found, throw error (dev).
  - If `on[event.type]` exists but contains invalid entries (neither string nor object), treat as undefined (unchanged state).
- REQ-ERR-03  Subscribe:
  - `subscribe` must return an object with a working `unsubscribe()` that stops further notifications for that listener.
- REQ-ERR-04  Action execution:
  - Exceptions thrown inside action/guard functions must not corrupt service state; report/log per Espruino policy and continue safely where possible.

## 6. Espruino Integration Requirements
- REQ-ESP-01  Bindings:
  - Expose a JS-facing class (e.g., `FSM` via `jswrap_xfsm.c/.h`) with methods equivalent to:
    - `constructor(config, options?)`
    - `.start(initialStateOrValue?)`
    - `.stop()`
    - `.send(event)`
    - `.state()` (returns object `{ value, context, changed }`)
    - `.status()` (returns numeric enum compatible with `NotStarted/Running/Stopped`)
    - `.subscribe(fn)` (returns `{ unsubscribe: function }`)
- REQ-ESP-02  Data exchange:
  - All JS-facing data (`config`, `context`, `event`, `state`, `actions`) must be modeled with **Espruino `JsVar`** types, not raw C-only structures, to simplify lifetime and GC across the JS/C boundary.
- REQ-ESP-03  Action/Guard invocation:
  - Support function-based actions/guards passed from JS (stored as `JsVar` references) and invoked with `(context, event)` semantics.
  - Ensure strict ref-counting and safe exception propagation.
- REQ-ESP-04  Logging and diagnostics:
  - Use `jsDebug(DBG_INFO, "...")` for all diagnostic messages per project policy.
  - Provide build-time guard to mute logs for production builds.
- REQ-ESP-05  Memory discipline:
  - Clearly define ownership of all `JsVar` references (config, options.actions map, subscribers, current state, queued actions).
  - No leaks under continuous `send()` burn-in for ≥ 24 hours.
- REQ-ESP-06  Performance targets:
  - Single transition (no user action cost) target: ≤ 100 μs on ESP32 @ 240 MHz (guideline; tune with profiling).
  - Steady memory usage with bounded transient allocations.

## 7. Conformance Notes and SCXML Alignment
- REQ-SCXML-01  Favor SCXML semantics where they align with XState v4 FSM subset (e.g., event object shape, guard evaluation, entry/exit ordering).
- REQ-SCXML-02  Document differences explicitly (e.g., no history or parallel regions in Phase 1).
- REQ-SCXML-03  For future hierarchical states, specify **LCCA (Least Common Compound Ancestor)** exit/entry sequencing as per SCXML §3.1.5.

## 8. Roadmap (Non-Phase-1)
- REQ-RM-01  Hierarchical states (compound states) with correct exit/entry ordering and parent fallback transitions.
- REQ-RM-02  `always` transitions, delayed transitions (timers), and activities (as feasible).
- REQ-RM-03  History states (shallow/deep).
- REQ-RM-04  Closer SCXML feature parity and import guidance.
- REQ-RM-05  XState v5 features (e.g., actor model) are **out of scope** for initial delivery.

## 9. Reverse Read of Attached Reference (`xstate-fsm.js`)
The attached JS baseline (Espruino-adapted) indicates the following behaviors that must be preserved:

- REQ-REV-01  `InterpreterStatus` enum with `NotStarted`, `Running`, `Stopped`.
- REQ-REV-02  `INIT_EVENT = { type: 'xstate.init' }` must be used for initial action execution on `start()`.
- REQ-REV-03  `ASSIGN_ACTION = 'xstate.assign'` action tag; `exports.assign(assignment)` helper.
- REQ-REV-04  Normalization helpers: `toArray`, `toActionObject`, `toEventObject`, `createMatcher`.
- REQ-REV-05  `createUnchangedState(value, context)` returns `{ value, context, actions: [], changed: false, matches }`.
- REQ-REV-06  Initial state preparation:
  - Initial state’s `entry` actions are analyzed to compute `initialContext` (applying `assign`) and produce `initialActions` (non-assign), then **execution** is deferred until `start()`.
- REQ-REV-07  Transition loop:
  - Reads `stateConfig.on[event.type]` array.
  - Each transition supports string target shorthand or object shape, optional `cond`, optional `actions`.
  - Targetless transitions only perform `actions`.
  - Targeted transitions compose `exit + actions + entry`.
  - `changed` is true if target differs OR any non-assign actions exist OR any `assign` occurred.
- REQ-REV-08  Action execution:
  - Each action object with `exec` is invoked as `exec(context, event)`.
- REQ-REV-09  Service semantics:
  - `send()` ignored unless Running.
  - `subscribe(listener)` immediately invokes the listener with current state and returns `{ unsubscribe }`.
  - `stop()` clears listeners and sets `Stopped`.

Note: The uploaded JS shows a subscription `unsubscribe` bug (`delete listeners(listener)` instead of deleting by key). The **C implementation must implement a correct unsubscribe** (REQ-ERR-03).

## 10. Test Plan (Acceptance & Regression)
For each requirement above, derive at least one test. Minimal required suites:

1) Initialization & Start
  - Creating a machine with `initial`, `context`, and `entry` that includes `assign` to mutate context; verify:
    - Before `start()`: no actions executed.
    - After `start()`: initial `entry` actions executed with `INIT_EVENT`, context updated, subscribers notified.

2) Transition Selection & Guards
  - Multiple transitions for same event with differing `cond`; ensure first passing guard is selected, others ignored.

3) Targeted vs Targetless Ordering
  - Verify `exit → actions → entry` for targeted transitions.
  - Verify only `actions` for targetless transitions.

4) Assign Ordering
  - Transition with `assign` and other actions; ensure context reflects `assign` results before non-assign actions execute.

5) Unchanged State
  - No matching transition or failing guards → unchanged state with `changed: false`, empty actions.

6) Error Conditions (Dev Mode)
  - Invalid `initial`, unknown `target`, nested states → throw errors.

7) Interpreter Lifecycle
  - `send()` ignored when not Running.
  - `stop()` clears listeners; `subscribe()` immediate call + `unsubscribe()` works (no further notifications).

8) Event Normalization
  - `send('PING')` equals `send({ type: 'PING' })`.

9) Performance & Memory
  - Burn-in test: repeated `send()` at steady rate; confirm no leaks (constant allocations), stable latency.

10) Espruino Integration
  - JS/C exchange of `context` and `event` via `JsVar`; verify ref counts and absence of leaked references.
  - Action/guard function invocation from JS with correct `(context, event)` values.

## 11. Developer Notes (Binding & Internals)
- REQ-DEV-01  Internal C structures should maintain preprocessed maps for fast lookup of:
  - current state id → state config
  - event type → transition array (pre-normalized)
  - action resolution (string → function map from `options.actions`)
- REQ-DEV-02  Store `context` as a `JsVar` object; copy-on-write where feasible to avoid accidental aliasing.
- REQ-DEV-03  Queue model:
  - During transition: build an ordered list of action objects.
  - First pass: apply `assign` to compute `nextContext`.
  - Second pass: emit non-assign actions for execution post-state update.
- REQ-DEV-04  Reentrancy:
  - `send()` inside an action is permitted but must operate on the updated state after the current transition completes (document chosen behavior; default is synchronous completion before nested `send`).
- REQ-DEV-05  Debug:
  - Optional lightweight traces: event received, chosen transition, actions queued, context delta, final state.

## 12. Out of Scope (Phase 1)
- Hierarchical/compound states, history states, parallel regions.
- Actor model / XState v5 features.
- Delayed transitions and activities (timers/sidecar scheduling).

## 13. Traceability Matrix (Example)
  - REQ-FSM-01..17 → Unit tests `t_fsm_*.js` (Espruino) and C harness tests.
  - REQ-EXEC-01..06 → Lifecycle tests.
  - REQ-ERR-01..04 → Negative tests.
  - REQ-ESP-01..06 → Binding & memory tests.
  - REQ-REV-01..09 → Parity tests with canonical `xstate-fsm` examples.

## 14. Reference Diagram

```mermaid
flowchart TD
  A["createMachine(config, options)"]
    --> B["machine.initialState  (value=initial, actions=entry(initial), context=apply(assign))"]
  B --> C["interpret(machine).start()  (exec initial actions with INIT_EVENT)"]
  C --> D["service.send(event)"]
  D --> E{"transitions for event"}
  E -->|guard fails or not found| F["unchanged state (actions=[])"]
  E -->|guard passes, targetless| G["actions = transition.actions"]
  E -->|guard passes, targeted| H["actions = exit(current) → transition.actions → entry(target)"]
  G --> I["apply assign → execute non-assign → notify"]
  H --> I
  F --> J["notify subscribers"]
  I --> J
  ```

## 15. Deliverables
- C engine (`xfsm.c/.h`) and Espruino bindings (`jswrap_xfsm.c/.h`).
- Unit and device tests matching REQ IDs.
- Developer docs summarizing the above requirements and test plan.
- Conformance note documenting equivalencies and differences vs `xstate-fsm`.

