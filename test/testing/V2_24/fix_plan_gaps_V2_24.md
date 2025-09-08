# XFSM V2_24 Gap Fix Plan

This plan groups changes by code area so we minimize touching the same code multiple times. Each group lists target files, core changes, and the gap tests it is intended to satisfy.

## Objectives

- Achieve parity with the v2 requirements where gaps were identified.
- Keep edits localized and incremental to ease review and reduce risk.
- Ensure updated behavior is exercised by the gap test suites.

## Summary of Groups

- Group A: Actions + Assign handling (assign-first, single-item normalization)
- Group B: Transition composition (targetless sequencing, no-match, changed semantics)
- Group C: Initial state precompute + normalization (pure path)
- Group D: JS wrapper API changes + validation (nested states, send return value)

## Group A — Actions + Assign Handling

- Files: `src/xfsm.c` (function `run_actions_raw`)
- Changes:
  - Treat a single action (non-array) as a one-element array for execution.
  - Implement assign-first per action list:
    - First pass: apply all assign-like items to `ctx` (supports `{ type:'xstate.assign' }`, `{ type:'assign' }`, and shorthand object with no `type/exec`).
    - Second pass: execute non-assign actions in original order.
  - Small helper to detect assign items to keep `run_actions_raw` readable.
- Rationale: Produces consistent context updates visible to all non-assign actions in the same step and enables single-item `entry` to run.
- Closes tests:
  - G2a/G2b (assign-first ordering)
  - G7 (single non-array entry executes on start via `run_actions_raw`)

## Group B — Transition Composition (Pure Machine)

- Files: `src/xfsm.c` (function `xfsm_machine_transition_ex`)
- Changes:
  - Targetless sequencing: compose actions as `transition.actions` only when `target` is absent; do not include `exit` or `entry`.
  - No-match unchanged: when no candidate matches, return an unchanged state object `{ value: current, context, actions: [], changed: false, matches }` instead of `0`.
  - `changed` semantics: compute as `(from !== to) || hasAnyAction`, where `hasAnyAction` is true when there are any resulting non-assign actions (covers targetless and self-target with actions).
- Rationale: Aligns with xstate-fsm semantics for targetless transitions, no-match behavior, and `changed` flag.
- Closes tests:
  - G1 (targetless must not run exit/entry)
  - G3 (no-match returns unchanged state)
  - G4 (changed=true for targetless with actions)
  - G5 (changed=true for self-target with actions)

## Group C — Initial State Precompute + Normalization

- Files: `src/xfsm.c` (function `xfsm_machine_initial_state`)
- Changes:
  - Precompute initial context by applying `assign` items from the initial state's `entry`, using a synthetic `{ type: 'xstate.init' }` event.
  - Normalize resulting `state.actions` to contain only non-assign items (preserve order) and ensure it is always an array (wrap single action).
- Rationale: Pure initial state reflects context after assigns, while execution of remaining non-assign actions occurs on `start()`.
- Closes tests:
  - G10 (initial context reflects assign before start)
  - Reinforces G7 (single non-array entry normalized to array)

## Group D — JS Wrapper API Changes + Validation

- Files: `src/jswrap_xfsm.c`
- Changes:
  - Machine constructor: reject nested states by invoking `xfsm_validate_no_nested_states(config)` and throwing via `jsExceptionHere(...)` when it returns false.
  - Service.send: return `this` consistently (chainable) regardless of whether a transition occurred; callers read `service.state.value` for the value.
- Rationale: Enforces flat machines per Phase‑1 scope; aligns JS API with expectations.
- Closes tests:
  - G6 (nested states rejected)
  - G9 (Service.send returns `this`)

## Order of Implementation

1) Group A (run_actions_raw)
2) Group B (xfsm_machine_transition_ex)
3) Group C (xfsm_machine_initial_state)
4) Group D (jswrap_xfsm constructor + send)

This sequencing flips the most tests early (A+B), then completes parity with C and D.

## Why this grouping works

- One edit to run_actions_raw covers assign-first everywhere and fixes single-item entries with no extra passes later.
- One edit to xfsm_machine_transition_ex fixes three semantics (targetless sequencing, no-match, changed flag) at once and avoids touching it again.
- One edit to xfsm_machine_initial_state makes pure initial states correct for both context and action shape.
- One edit to the wrapper handles both the constructor validation and the send return value.

## Verification

- Run current gap suite (expected FAIL before fixes):
  - `test/testing/xfsm_TestSuite_Gaps_V2_24.js`
- After each group:
  - Re-run the suite and confirm the corresponding tests pass.

- Patch checkpoints and tests to re-run

  - After Group A: G2a, G2b, G7
  - After Group B: G1, G3, G4, G5
  - After Group C: G10 (and reinforce G7)
  - After Group D: G6, G9

- Final check (expected PASS after all fixes):
  - `test/testing/xfsm_TestSuite_Gaps_V2_24_Expected.js`

## Compatibility Notes

- V1 FSM path remains unchanged (array-only `entry/exit` handling). Differences are documented and out of Phase‑1 scope.
- Listener notifications on no-match will occur once `transition_ex` returns an unchanged state and the service path persists it and calls `xfsm_notify_listeners`.

## Risk & Mitigation

- Action normalization and assign-first: Keep changes contained to `run_actions_raw` and ensure strict JsVar lock/unlock discipline.
- Transition changes: Carefully gate exit/entry inclusion based on `targetless` to avoid regressions.
- Constructor validation: Limit to dev-time exception where applicable; ensure error messages are concise and actionable.
