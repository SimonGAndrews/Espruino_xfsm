# API alignment with XState-FSM 



## typical format

```javascript
const { createMachine, interpret, assign } = require('xstate-fsm');

const lightMachine = createMachine({...});
const lightService = interpret(lightMachine);

lightService.subscribe(fn);
lightService.start();
lightService.send("BUTTON");
```

We can expose the same ergonomics on Espruino with thin wrappers:

`createMachine(config[, options])` → returns a `Machine` instance (our native class).

`interpret(machine)` → returns a `Service` bound to that machine.

`assign(updater)`:

In XState-FSM, `assign` returns an action function that merges changes into context.

Our native engine already supports “assign-like” by letting any action return an object to replace context. To match merge semantics, we can implement assign in JS as a helper that builds a function which returns a new object with merged fields.

## Shims

Tiny JS shim you can drop into your app (or as a module)

```javascript
function createMachine(config, options) {
  return new Machine(config, options);
}

function interpret(machine) {
  return machine.interpret();
}


// Minimal assign() compatible with xstate-fsm style:
// - usage: actions: [ assign({count: (ctx)=>ctx.count+1}) ]
// - returns a function (ctx, evt, meta) => newCtxObject
function assign(updater) {
  return function(ctx, evt, meta) {
    var base = {};
    // shallow clone ctx into base
    if (ctx) {
      for (var k in ctx) if (ctx.hasOwnProperty(k)) base[k] = ctx[k];
    }
    // apply updater: object of reducers or a function returning partials
    var patch = {};
    if (typeof updater === "function") {
      patch = updater(ctx, evt, meta) || {};
    } else {
      for (var key in updater) if (updater.hasOwnProperty(key)) {
        var v = updater[key];
        patch[key] = (typeof v === "function") ? v(ctx, evt, meta) : v;
      }
    }
    // merge
    for (var p in patch) if (patch.hasOwnProperty(p)) base[p] = patch[p];
    return base; // our native will replace context with this object
  };
}

global.createMachine = createMachine;
global.interpret = interpret;
global.assign = assign;

```

This gives you the same authoring style as your example. Your full snippet should run unchanged apart from the require(...) lines (Espruino doesn’t use Node’s require by default). With the globals above, you can just write:

```javascript
var lightMachine = createMachine({ /* your config */ });
var lightService = interpret(lightMachine);

lightService.start();
lightService.send("BUTTON");
```