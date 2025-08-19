# Espruino Native FSM API (Phase 1)

This document describes the API of the native Espruino FSM engine implemented in C (`xfsm.c/.h` + `jswrap_xfsm.c/.h`), exposed to JavaScript through Espruino’s class system. The API design follows the conventions of **XState-FSM** while being optimized for **embedded microcontrollers**.

---

## Overview

- **Two core classes** are exposed:  
  - **`Machine`**: Represents a finite state machine configuration.  
  - **`Interpreter`**: Represents a running instance (service) of a machine.  

- **Actions**: Support for user-defined functions and built-in actions (e.g. `assign`).  
- **Context**: Mutable key/value object tied to the machine. Updated via `assign` actions.  
- **Events**: Trigger transitions using `interpreter.send(event)`.

---

## Machine Class

**Constructor**

```javascript
var m = new Machine(config);
```

**Parameters**:

- config (object) – machine definition:
- id (string) – unique identifier.
- initial (string) – initial state key.
- context (object) – initial context (optional).
- states (object) – state definitions.
- actions (object) – named reusable actions (optional).

**Returns**: a Machine object.

Notes:

- The configuration is stored as a JsVar tree (not copied).
- Validation occurs during construction.

## Interpreter Class

**Creation**:
```javascript
var service = m.interpret();
```

**Returns**: an Interpreter object bound to the Machine.

## Methods / Properties

`start()`

```javascript
service.start();
```

- Starts the interpreter.
- Enters the initial state and executes entry actions.
- Returns the Interpreter instance (chainable).

##
 `stop()`

```javascript
service.stop();
```

- Stops the interpreter.
- Clears listeners and halts event processing.

**Returns** the Interpreter.

## 
`send(event)`

```javascript
service.send("EVENT_NAME");
```

- Sends an event to the machine.
- Evaluates transitions from the current state.
- Executes:
  - exit actions for the current state.
  - Transition actions.
  - entry actions for the new state.
  - assign actions update the context.

**Returns** the new state object.

## 
`state`

```javascript
service.state
```

- Current state object:
  - `value` (string) – current state name.
  - `context` (object) – current context snapshot.

##
`subscribe(listener)`

```javascript
service.subscribe(function (state) {
  console.log(state.value, state.context);
});
```

- Registers a callback for state changes.
- Listener is invoked after every successful transition.

**Returns** an unsubscribe function (planned for later phase).


## Actions

1. User-Defined Actions
- Functions provided in entry, exit, or on.transition.actions.
- Invoked with (context, event, meta).

2. Built-In Actions

- `assign`: the only built-in action implemented in Phase 1.

```javascript
entry: [
  { type: "xstate.assign", assignment: { count: (ctx) => ctx.count + 1 } }
]
```

- Updates context immutably.
- Evaluated before other actions in the same list.
- Implements the same rules as XState (pure function, no side effects).

Future Phases: log and other standard built-ins.

## Example

```javascript
var m = new Machine({
  id: "counter",
  initial: "idle",
  context: { count: 0 },
  states: {
    idle: {
      on: {
        INC: {
          target: "idle",
          actions: [
            { type: "xstate.assign", assignment: { count: (ctx) => ctx.count + 1 } }
          ]
        }
      }
    }
  }
});

var service = m.interpret();

service.subscribe((state) => {
  console.log("Now in state", state.value, "with count", state.context.count);
});

service.start();
service.send("INC"); // count = 1
service.send("INC"); // count = 2
```

## Implementation Notes

- Naming alignment: Machine and Interpreter follow XState-FSM naming directly; no aliases.
- Minimal wrapper layers: Context updates and action resolution occur in the interpreter core, not through extra indirection.
- Pure functions: Built-in actions like assign are pure and do not perform side effects directly.
- Memory safety: Care taken to lock/unlock JsVar objects correctly.
- Burn-in testing: Example REPL scripts verify correctness across repeated .start(), .send(), and .stop() calls.

## Flow Summary

### 1) Runtime flow: Interpreter.send(event)

```mermaid
flowchart TD
  A["send(event)"] --> B{Status == Running?}
  B -- no --> Z[Return undefined]
  B -- yes --> C[Read current state value]
  C --> D["Lookup transitions: states[current].on[event]"]
  D -- none --> Z
  D -- found --> E{Guard cond?}
  E -- no cond --> G[Resolve target + actions]
  E -- cond present --> F["Call cond(ctx, evt, meta)"]
  F -- falsy --> Z
  F -- truthy --> G
  G --> H["Build meta {state, target, event}"]
  H --> I[exit actions of source]
  I --> J[transition actions]
  J --> K[entry actions of target]
  K --> L["Merge all assign patches into ctx (in-memory only)"]
  L --> M[Persist ctx ONCE to _context]
  M --> N[Set _state = target]
  N --> O[Notify subscribers with StateObject]
  O --> P[Return new state value]
```
 ## Notes

- Guard truthiness uses Espruino’s boolean coercion (jsvGetBool).
- Actions order is exit → transition → entry.
- Context is updated in memory during the loop; it is persisted once after all actions finish (avoids lock/unlock hazards).
- Assign handling:
- { type: "xstate.assign", assignment: fn|object }, or shorthand object.
- Produces a patch that is shallow-merged into context.

---

### 2) Startup flow: Interpreter.start([initialValue])

```mermaid
flowchart TD
  A["start(initialValue?)"] --> B["Set status = Running"]
  B --> C{initialValue provided?}
  C -- yes --> D["st = machine.stateForValue(initialValue)"]
  C -- no --> E["st = machine.initialState()"]
  D --> F
  E --> F
  F["Read st.value & st.actions"] --> G["Build meta {target:value}"]
  G --> H[Run entry actions of value]
  H --> I["Merge assign patches into ctx (in-memory)"]
  I --> J[Persist ctx ONCE to _context]
  J --> K[Set _state = st]
  K --> L[Notify subscribers with StateObject]
  L --> M[Return this]
```

---

```mermaid
classDiagram
  class Machine {
    +config: object
    +initialState(): State
    +transition(stateOrValue, event): State
  }

  class Interpreter {
    +_machine: Machine
    +_state: State
    +_context: object
    +_status: "NotStarted" | "Running" | "Stopped"
    +_subs: Function[]
    +start(initial?): Interpreter
    +stop(): Interpreter
    +send(event): string | undefined
    +state(): State
    +statusText(): string
    +subscribe(fn): token
    +unsubscribe(token): void
  }

  class State {
    +value: string
    +context: object
    +actions: Action[]
  }

  class Action {
    <<interface>>
  }

  class FunctionAction {
    +invoke(ctx, evt, meta): (void | objectPatch)
  }

  class NamedAction {
    +name: string
  }

  class AssignAction {
    +assignment: (fn or objectSpec)
  }

  class ShorthandAssignAction {
    +objectSpec: key -> (value | fn)
  }

  Machine --> State : "produces"
  Interpreter --> Machine : "uses"
  Interpreter --> State : "holds current"
  State --> Action : "actions Action[]"
  Action <|-- FunctionAction : "implements"
  Action <|-- NamedAction : "implements"
  Action <|-- AssignAction : "implements"
  Action <|-- ShorthandAssignAction : "implements"
```

  Note for AssignAction:

  "Explicit built‑in: type=\"xstate.assign\" (alias: \"assign\").
  - If assignment is a function: call (ctx, evt, meta) -> patch (object)
  - If assignment is an object: evaluate function values, use raw values as‑is
  - Shallow‑merge resulting patch into context after action group"

  Note for ShorthandAssignAction:
  
  "Shorthand built‑in: plain object used in action list.
  - Equivalent to assign with objectSpec
  - Shallow‑merged into context after action group"

