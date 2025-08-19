# xfsm_Requirements_Coverage_V1.md
Coverage Matrix for Espruino FSM Library — Version V1.0 (C Native)

Notes:
- Source reviewed: 
  - /mnt/data/xfsm V1_0.c, /mnt/data/xfsm V1_0.h
  - /mnt/data/jswrap_xfsm V1_0.c, /mnt/data/jswrap_xfsm V1_0.h
  - Reference JS: /mnt/data/xstate-fsm.js (for parity targets)
- Status values: Yes / Partial / No.
- “Notes” explain the decision and point to observed code patterns.
- “Test Result” column is left blank for you to fill during execution; I can help produce concrete test scripts next.

## Coverage Table

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-FSM-01 | Machine creation (`createMachine` equivalent) | Yes | `Machine` class exposed via jswrap; ctor stores `config`, `_options`, calls `xfsm_machine_init`. |
| REQ-FSM-02 | Config supports `id?`, `initial`, `context?`, `states` | Yes | Code reads `config.states`, `config.initial`, `config.context`. |
| REQ-FSM-03 | StateConfig `entry?`, `exit?`, `on?` | Yes | `entry`/`exit` fetched; transitions read from `on[event]`. |
| REQ-FSM-04 | Transition: string or object (`target?`, `actions?`, `cond?`) | Partial | String/object supported; `cond` functions resolved/executed; **targetless not supported** (returns early if no target). |
| REQ-FSM-05 | `options.actions` map | Partial | Named action resolution uses `config.actions` (and global), `_options` stored but not used. |
| REQ-FSM-06 | Event normalization (string → `{type}`) | No | V1 expects **string events** only (`JsVar` string checks); no `{type}` handling. |
| REQ-FSM-07 | Initial state prep (actions prepared, context computed; exec on `start()`) | Yes | `xfsm_machine_initial_state` produces `{value, actions, context?}`; service `start()` executes entry actions once. |
| REQ-FSM-08 | Transition semantics: targeted vs targetless | Partial | Targeted transitions supported with `exit → trans → entry`; **targetless transitions not supported**. |
| REQ-FSM-09 | `assign` action semantics & ordering | Yes | Built-in `assign` (also alias `assign` and shorthand object). Assign produces context patch merged **before** non-assign action execution. |
| REQ-FSM-10 | Transition result shape: `{ value, context, actions, changed, matches }` | Partial | Machine returns state object with `{ value, actions, context }`. `changed`/`matches` not provided. |
| REQ-FSM-11 | No-match behavior (unchanged state) | Partial | Functions **return 0/undefined** when no match; no unchanged-state object with `changed:false`. |
| REQ-FSM-12 | Nested states rejected (Phase 1) | No | No explicit nested-state validation path observed. |
| REQ-FSM-13 | Interpreter API: `interpret`, `start/stop/send/subscribe`, getters | Partial | `Service` has `start/stop/send/state/statusText`; **no `subscribe`** implementation. |
| REQ-FSM-14 | Initial actions executed on `start()` with INIT event | Partial | Entry actions executed on `start()`; **no explicit `xstate.init` event object** passed. |
| REQ-FSM-15 | Action function shape `(context, event)` | Yes | Actions invoked with `(context, event, meta)`; return object patches merged. |
| REQ-FSM-16 | Determinism (transition order; assign first) | Yes | Transition/action order preserved; assign handled in first pass. |
| REQ-FSM-17 | Dev/production checks (invalid state/target) | Partial | Some checks; generally returns 0 on issues; **no strict dev asserts** observed. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-EXEC-01 | `start()` sets Running and executes initial actions | Yes | `xfsm_service_start` executes entry actions then persists context/state. |
| REQ-EXEC-02 | `send(event)` when Running → transition + exec actions | Yes | `xfsm_service_send` enforces Running; executes actions and persists state/context. |
| REQ-EXEC-03 | Assign partitioning: compute `nextContext` first | Yes | `run_actions_raw` applies assign/object patches before non-assign exec. |
| REQ-EXEC-04 | Targeted ordering `exit → transition.actions → entry` | Yes | Actions array built in that exact order. |
| REQ-EXEC-05 | Targetless transition executes only `transition.actions` | No | Targetless not supported. |
| REQ-EXEC-06 | `matches(s)` predicate | No | Not exposed on returned state objects. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-ERR-01 | Creation checks: invalid `initial`, nested states | Partial | Fallbacks exist (`idle` default); no explicit nested-state rejection. |
| REQ-ERR-02 | Transition checks: missing current/target, invalid entries | Partial | Typically returns 0; no explicit dev error throws. |
| REQ-ERR-03 | `subscribe/unsubscribe` correctness | No | No subscription API present in V1 C. |
| REQ-ERR-04 | Guard/action exceptions don’t corrupt service | Partial | Invocation via helper with basic safety; needs stress tests to confirm. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-ESP-01 | JS API surface (constructor, start, stop, send, state, status, subscribe) | Partial | Methods implemented except `subscribe`; `statusText()` provided (string). |
| REQ-ESP-02 | Use `JsVar` for all JS-facing data | Yes | All config, context, events, state/actions via `JsVar`. |
| REQ-ESP-03 | JS callbacks `(context, event)` invocation | Yes | Via `xfsm_callJsFunction(...)`; also passes `meta`. |
| REQ-ESP-04 | Logging via `jsDebug(DBG_INFO, ...)` | Partial | Logging present but sparse/disabled in places; policy-compliant usage can be expanded. |
| REQ-ESP-05 | Memory discipline / no leaks under burn-in | Partial | Design follows lock/unlock rules; requires runtime validation. |
| REQ-ESP-06 | Performance targets | Partial | Engine is native/JsVar-based; needs measurement on target HW. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-SCXML-01 | Prefer SCXML-aligned semantics where possible | Partial | Entry/exit ordering aligns; other aspects pending. |
| REQ-SCXML-02 | Document differences | Partial | Doc-side task; engine not blocking. |
| REQ-SCXML-03 | LCCA sequencing for hierarchy | No | Hierarchy not in V1. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-RM-01 | Hierarchical (compound) states | No | Roadmap. |
| REQ-RM-02 | `always`, delays, activities | No | Roadmap. |
| REQ-RM-03 | History states | No | Roadmap. |
| REQ-RM-04 | SCXML parity/import | No | Roadmap. |
| REQ-RM-05 | XState v5 actor model | No | Out of scope. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-REV-01 | `InterpreterStatus` enum parity | Partial | C enum exists (`XFSM_STATUS_*`); JS-side exposes `statusText()` string. |
| REQ-REV-02 | `INIT_EVENT = { type:'xstate.init' }` use | No | Initial actions run without explicit init event object. |
| REQ-REV-03 | `ASSIGN_ACTION = 'xstate.assign'` | Yes | Supports `xstate.assign`, `assign`, and shorthand object. |
| REQ-REV-04 | Normalization helpers (`toArray`, etc.) | N/A | Implemented differently in C; behavior covered. |
| REQ-REV-05 | `createUnchangedState` semantics | No | V1 returns 0/undefined on no-match; no unchanged-state object. |
| REQ-REV-06 | Initial state prep & deferred exec on `start()` | Yes | Matches behavior (exec on start). |
| REQ-REV-07 | Transition loop semantics | Partial | Matches targeted flow; **no targetless**, returns 0 on miss/guard-fail. |
| REQ-REV-08 | Action execution contract | Yes | `(context, event, meta)`, patch merge if object returned. |
| REQ-REV-09 | Service semantics: send/subscribe/stop | Partial | `send/stop` present; **no subscribe**. |

| Requirement ID | Summary | Implemented (V1) | Notes | Test Result |
|---|---|---|---|---|
| REQ-DEV-01 | Preprocessed fast lookups | Partial | Some direct lookups; no explicit precompute tables found. |
| REQ-DEV-02 | Store `context` as `JsVar` object | Yes | Context maintained as `JsVar` and persisted once per step. |
| REQ-DEV-03 | Two-pass action queue (assign then non-assign) | Yes | Implemented in `run_actions_raw`. |
| REQ-DEV-04 | Reentrancy (`send` inside action) behavior | Partial | Not explicitly guarded; needs behavioral decision/test. |
| REQ-DEV-05 | Debug traces (event, transition, actions) | Partial | Minimal; can be expanded with `jsDebug(DBG_INFO, ...)`. |

## Summary of Gaps to Reach XState v4 FSM Parity
1. Add **targetless transitions** (execute only transition.actions; don’t change state).  
2. Implement **event normalization** (accept `{type}` objects; allow string shorthand).  
3. Provide **unchanged state** return object for no-match (`changed:false`, empty `actions`).  
4. Add **`matches` predicate** and **`changed`** flag on returned state objects.  
5. Implement **`subscribe`** on `Service` with proper `unsubscribe`.  
6. Enforce **nested state rejection** (dev mode) and clearer error paths.  
7. Optionally route named actions via **`options.actions`** map (in addition to `config.actions`).  
8. Pass/initiate a synthetic **init event** (e.g., `{type:'xstate.init'}`) to initial actions for closer parity.

If you’d like, I can now generate a small on-device test suite that walks each “Partial/No” above and records pass/fail in the “Test Result” column.
