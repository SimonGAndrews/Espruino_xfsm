# XFSM v2 Requirements Compliance Analysis (V2_24)

This document reviews the implemented functionality in the Espruino XFSM library against the requirements in `docs/xfsm_Requirements_v2.md` and identifies gaps, with concrete code references.


## Summary

- Overall the Machine/Service core matches much of the `xstate-fsm` subset: event normalization, transition selection (including arrays and guards), action execution (functions, objects, named actions), built‑in `assign`, initial actions executed on `start()`, interpreter lifecycle, and subscription semantics.
- Key gaps remain around targetless transition sequencing, assign‑first semantics, unchanged/no‑match return behavior, the `changed` flag, strict rejection of nested states, initial entry action normalization, and the JS `Service.send()` return type.


## Detailed Assessment by Requirement

Below, each requirement is marked as Met / Partial / Gap with supporting notes.

- REQ‑FSM‑01 Machine creation via `createMachine(config, options?)`: Met
  - Exposed as `new Machine(config, options?)`. References: `src/jswrap_xfsm.c:146`.

- REQ‑FSM‑02 Config supports `id`, `initial`, `context`, `states`: Met
  - `initial`, `context`, `states` are consumed by the core. `id` is accepted (not used by core). References: `src/xfsm.c:801`.

- REQ‑FSM‑03 StateConfig supports `entry`, `exit`, `on`: Met
  - Machine path handles both single action and array forms when composing actions (exit/entry). References: `src/xfsm.c:1038` and `src/xfsm.c:1042`.
  - Note: V1 FSM helpers use `getActionListRaw` that requires arrays (limitation only in legacy API). References: `src/xfsm.c:431`.

- REQ‑FSM‑04 Transition supports string or object `{ target?, actions?, cond? }`: Partial
  - Supported forms and cond evaluation are implemented. References: `src/xfsm.c:861`.
  - Gap: For targetless transitions, exit/entry should not run. Current implementation always queues `exit[]` first. References: `src/xfsm.c:858` (composition), `src/xfsm.c:1038` (entry composition), `src/xfsm.c:1051` (changed calc).

- REQ‑FSM‑05 `options.actions` mapping for named actions: Met
  - Lookup order: `service._options.actions` → `machine._options.actions` → `machine.config.options.actions` → `machine.config.actions`. References: `src/xfsm.c:472`.

- REQ‑FSM‑06 Event normalization: Met
  - Strings become `{ type }`. References: `src/xfsm.c:64`, `src/xfsm.c:841`.

- REQ‑FSM‑07 Initial state prep: Partial
  - Initial state value honored; initial `entry` actions are prepared for execution on `start()`. References: `src/xfsm.c:789`, `src/xfsm.c:1123`.
  - Gaps:
    - `initialState()` does not precompute context changes from `assign` in initial `entry`. References: `src/xfsm.c:825`.
    - If `entry` is a single item (not array), `Service.start()` will skip execution because `run_actions_raw` requires an array. References: `src/xfsm.c:821`, `src/xfsm.c:1125`, `src/xfsm.c:463`.

- REQ‑FSM‑08 Transition semantics (ordering): Partial
  - Targeted: exit → transition.actions → entry is implemented. References: `src/xfsm.c:858`.
  - Targetless: Should execute only `transition.actions` (no exit/entry); currently exit/entry may still be included (see REQ‑FSM‑04 gap).

- REQ‑FSM‑09 `assign` semantics (assign‑first): Gap
  - `assign` is supported in object and shorthand forms, mutating context. References: `src/xfsm.c:556`, `src/xfsm.c:585`, `src/xfsm.c:345`.
  - Gap: There is no pre‑pass to apply all `assign` before non‑assign actions in the same step; execution follows listed order only. References: `src/xfsm.c:513`.

- REQ‑FSM‑10 Transition result shape and `changed`: Gap
  - Result shape `{ value, context, actions, changed, matches }` is implemented. References: `src/xfsm.c:275`.
  - Gap: `changed` is computed only when targeted and `from !== to`, not when actions run or `assign` occurs (including targetless). References: `src/xfsm.c:1051`.

- REQ‑FSM‑11 No match returns unchanged state: Gap
  - Current: returns `0` (no state) on no match. Expected: `{ value: current, context, actions: [], changed: false }`. References: `src/xfsm.c:899`.

- REQ‑FSM‑12 Nested states must be rejected: Gap
  - Current: detected and logged via `jsDebug`, but not rejected. References: `src/xfsm.c:231`.

- REQ‑FSM‑13 Interpreter API: Partial
  - `interpret(machine)` → Service implemented. References: `src/jswrap_xfsm.c:167`.
  - `start()`: sets Running, executes initial actions with `{ type: 'xstate.init' }`, notifies subscribers. References: `src/xfsm.c:1130`, `src/xfsm.c:1147`.
  - `stop()`: sets Stopped and clears listeners. References: `src/xfsm.c:1160`.
  - `send()`: ignored unless Running; executes transition; notifies subscribers. References: `src/xfsm.c:1182`, `src/xfsm.c:1227`.
  - `subscribe(listener)`: queues immediate call with current state (next tick) and returns an unsubscribe function. References: `src/jswrap_xfsm.c:213`.
  - Gaps:
    - Return type: `Service.send()` returns new state value string on success, `this` otherwise. Requirement expects chainable `service` always. References: `src/jswrap_xfsm.c:184`.
    - Notifications on unchanged/no‑match: due to REQ‑FSM‑11/10 gaps, no‑match returns 0 and subscribers are not notified even though requirement expects an emission including when `changed === false`.

- REQ‑FSM‑14 Initial actions executed on `start()`: Met
  - Initial actions are not executed during construction; they execute on `start()` with `xstate.init`. References: `src/xfsm.c:1123`.

- REQ‑FSM‑15 Action function shape: Met
  - Function actions `(ctx, evt)` and object `{ exec: fn }` both called with `(ctx, evt)`. References: `src/xfsm.c:513`, `src/xfsm.c:530`.

- REQ‑ESP‑04 Logging/diagnostics: Partial
  - Has `jsDebug` for nested state detection; broader validation/logging not enforced. References: `src/xfsm.c:231`.

- REQ‑ESP‑05/06 Memory/Performance: Not assessed here
  - Code follows lock/unlock discipline; needs long‑run tests to verify targets.

- REQ‑REV‑02 INIT event tag: Met
  - Uses `{ type: 'xstate.init' }`. References: `src/xfsm.c:1130`.

- REQ‑REV‑03 Assign action tag: Met
  - Recognizes `'xstate.assign'` and `'assign'` aliases; shorthand map supported. References: `src/xfsm.c:556`, `src/xfsm.c:585`.

- REQ‑REV‑04 Helper parity: Partial
  - Internal equivalents exist; not all helper names are exposed (non‑blocking for behavior).

- REQ‑REV‑05 Unchanged state helper: Gap
  - No `createUnchangedState`; ties to REQ‑FSM‑11 gap.

- REQ‑REV‑06 Initial state context preparation: Gap
  - Does not precompute initial context by applying `assign` in `entry`. References: `src/xfsm.c:825`.

- REQ‑REV‑07 Transition loop and `changed`: Partial
  - Loop behavior present; `changed` semantics don’t include assigns/non‑assign actions or targetless. References: `src/xfsm.c:1051`.

- REQ‑REV‑08 Action execution via `exec(context, event)`: Met
  - References: `src/xfsm.c:530`.

- REQ‑REV‑09 Service semantics (subscribe, stop, send): Partial
  - Subscribe uses queued immediate callback and returns a function; stop clears listeners; send ignored unless Running. References: `src/jswrap_xfsm.c:213`, `src/xfsm.c:1162`, `src/xfsm.c:1182`.
  - Gap: `send()` return type (see REQ‑FSM‑13) and unchanged/no‑match notification (see REQ‑FSM‑11/10).


## Additional Notes (Legacy V1 FSM)

- V1 `xfsm_send_object` rejects targetless transitions (returns 0). References: `src/xfsm.c:754`.
- V1 `entry`/`exit` must be arrays due to `getActionListRaw`. References: `src/xfsm.c:431`, `src/xfsm.c:659`.

These do not block Phase‑1 parity (focus is Machine/Service) but are worth documenting.


## Recommended Changes (Prioritized)

1) Targetless sequencing
- Only include `transition.actions` when `target` is absent; skip `exit[]` and `entry[]`. References: `src/xfsm.c:858` (composition) and where `exitArr`/`entryArr` are pushed.

2) Assign‑first execution
- In `run_actions_raw`, split the pass: first apply all `assign` actions to `ctx`, then execute remaining non‑assign actions in listed order. Apply the same policy for initial `entry` actions.

3) Initial `entry` normalization
- Ensure `initialState().actions` is an array. If the config’s `entry` is a single action, wrap it into a single‑element array so `Service.start()` executes it. References: `src/xfsm.c:821`, `src/xfsm.c:1125`.

4) Unchanged/no‑match behavior
- When no transition matches, return an unchanged state `{ value: current, context, actions: [], changed: false }` (instead of `0`) and notify subscribers to maintain observable behavior. References: `src/xfsm.c:899`.

5) `changed` semantics
- Compute `changed = true` when:
  - `target !== current`, or
  - any non‑assign actions are present, or
  - any `assign` occurred (even targetless).

6) Reject nested states
- Convert `xfsm_validate_no_nested_states` into an enforced validation for machine initialization (throw/abort in dev builds) rather than logging only. References: `src/xfsm.c:231`.

7) JS API consistency
- Make `Service.send()` return `this` (chainable) consistently. Expose `new state value` via `service.state.value` for callers that need it. References: `src/jswrap_xfsm.c:184`.

8) Optional: expose a helper (pure path)
- Add a `createUnchangedState(value, context)` equivalent or implement the behavior inline where no match occurs.


## Espruino‑Specific Considerations

- Diagnostics: Consider adding more `jsDebug(DBG_INFO, ...)` messages for invalid configs (`initial` missing, unknown `target`) to aid users.
- Memory/perf: Add a burn‑in test harness to run `send()` loops and watch heap usage and timing.


## References (Files/Lines)

- Machine/Service core and helpers: `src/xfsm.c:861`, `src/xfsm.c:513`, `src/xfsm.c:556`, `src/xfsm.c:585`, `src/xfsm.c:1038`, `src/xfsm.c:1051`, `src/xfsm.c:1147`, `src/xfsm.c:1182`, `src/xfsm.c:1227`.
- Initial state handling: `src/xfsm.c:789`, `src/xfsm.c:821`, `src/xfsm.c:825`, `src/xfsm.c:1125`.
- Event normalization: `src/xfsm.c:64`, `src/xfsm.c:841`.
- Nested state validation: `src/xfsm.c:231`.
- JS wrappers: `src/jswrap_xfsm.c:146`, `src/jswrap_xfsm.c:167`, `src/jswrap_xfsm.c:184`, `src/jswrap_xfsm.c:213`, `src/jswrap_xfsm.c:260`.
- Legacy FSM V1 specifics: `src/xfsm.c:431`, `src/xfsm.c:659`, `src/xfsm.c:754`.


## Conclusion

Core behavior aligns with much of the `xstate-fsm` subset, but a few semantic differences must be addressed to claim full parity for Phase‑1. The most impactful changes are: fix targetless sequencing, enforce assign‑first semantics, normalize initial `entry` to arrays, return unchanged state on no match (and notify), compute `changed` per spec, and enforce flat machines. After these, revisit `Service.send()`’s return type for API consistency.

