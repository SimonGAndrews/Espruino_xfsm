# xfsm_Test Integrity Review (Failing/Flaky Items) + Clean Delta Tests

Below is a quick integrity check of **each failing test** from your run, with a verdict (**Test OK** vs **Adjust Test**) and the **recommended engine change** to flip it to PASS. Where a test might have been tripped by formatting (Unicode) or an overly strict assumption, I’ve suggested a tiny tweak and provided a **clean “delta” mini‑suite** (ASCII‑only messages) you can paste to re‑verify just the problem areas.

---

## 1) Per‑failure integrity review

| Failure (from summary) | Req IDs | Integrity verdict | What likely happened | Engine change to pass | Test tweak (if any) |
|---|---|---|---|---|---|
| `Exception: Service.send: event must be a string` | REQ‑FSM‑06 | **Test OK** | Service only accepts string events; XState v4 accepts both string and `{type}` objects | **Normalize events** in `send`: if string → `{type}`; if object → read `.type` (string) or reject | None |
| `options.actions not used` | REQ‑FSM‑05 | **Test OK** | `options.actions` map not consulted for named actions | Look up named actions via `options.actions` before config‑local map | None |
| `object-form event not supported (count=1, caught=true)` | REQ‑FSM‑06 | **Test OK** | Same as first | Same as first | None |
| `Exception: Got '=' expected EOF` (during ordering/targetless test) | REQ‑FSM‑08 | **Adjust Test (message only)** | Espruino sometimes reports parse errors when long **Unicode** strings are printed/concatenated (especially arrows). The logic of the test is fine; the message likely triggered this | Keep test logic; **remove Unicode** glyphs from messages | Use ASCII‑only messages (see delta suite) |
| `missing changed/matches on state` | REQ‑FSM‑10/11/EXEC‑06 | **Test OK** | Engine’s `state()` result lacks `changed` and `matches()` | Add `changed` boolean and `matches(s)` predicate to returned state (unchanged: `false`) | None |
| `nested states allowed (should be rejected)` | REQ‑FSM‑12 | **Test OK** | No explicit rejection | On machine creation, if any `states[stateId].states` exists, **throw** (dev) | None |
| `subscribe not implemented` | REQ‑FSM‑13/ERR‑03/REV‑09 | **Test OK** | No `subscribe()` | Implement `subscribe(listener)` → immediate call, store, return `{unsubscribe}`; call listeners on each transition | None |
| `init event not xstate.init (saw ["NONE"])` | REQ‑REV‑02/REQ‑FSM‑14 | **Test OK** | Initial `entry` gets `undefined` or a different sentinel | Invoke initial `entry` with `{type:"xstate.init"}` | None |
| `action received unexpected parameters` | REQ‑FSM‑15 | **Test OK** | Action probably called with wrong args order (`(evt, ctx)` instead of `(ctx, evt)`) | Ensure action/guard called as `(context, event)` consistently | None |
| `order wrong or result not B:  -> A` (deterministic choice) | REQ‑FSM‑16 | **Test OK** | Transition loop **stops** on first failed candidate instead of continuing | Iterate **all** candidates until one `cond` passes; only return unchanged after exhausting all | None |
| `ordering wrong: ["afterTG:1"]` (assign‑first targeted & targetless combined) | REQ‑EXEC‑03/04/05 & REQ‑DEV‑03 | **Test OK** | Targetless branch didn’t execute (or got filtered) after the targeted one | Ensure **targetless transitions** are recognized and their actions execute without state change | None |
| `matches predicate missing` | REQ‑EXEC‑06 | **Test OK** | No `matches()` function on state | Add `matches(s)` | None |
| `exception escaped to caller` | REQ‑ERR‑04 | **Test OK** (by our spec) | Action throws bubbled up | Wrap **action execution** in try/catch; prevent service corruption and return cleanly | None |
| `status not exposed` | REQ‑REV‑01 | **Test OK** | No numeric status getter | Expose `status` (NotStarted/Running/Stopped) | None |
| `callback did not receive expected event` | REQ‑ESP‑03 | **Test OK** | Action exec probably got wrong args or unnormalized event | Fix `(context,event)` order and event normalization | None |

### Cross‑check against reference JS (v4 FSM)
- **Event normalization** `toEventObject(event)` turns `"PING"` → `{type:"PING"}` and passes event objects through. :contentReference[oaicite:0]{index=0}
- **Initial state** uses `INIT_EVENT = { type: 'xstate.init' }` for entry handling. :contentReference[oaicite:1]{index=1}
- **Unchanged state** is a real state object with `changed:false` and `matches()`. :contentReference[oaicite:2]{index=2}
- **Subscribe bug in JS ref** deletes wrong key (we’ll implement the correct version natively). :contentReference[oaicite:3]{index=3}

---

## 2) Clean delta test mini‑suite (ASCII‑only messages)

Paste this to quickly **re‑check just the failing areas** after you make fixes.  
It avoids Unicode arrows and keeps messages short to prevent Espruino parser gripes.

```javascript
  function log(s){print(s);}
  function pass(id,msg){return {id:id,ok:true,msg:msg};}
  function fail(id,msg){return {id:id,ok:false,msg:msg};}
  function runTest(id,fn){try{var r=fn();if(!r||r.ok===undefined)return fail(id,"No result");r.id=id;return r;}catch(e){return fail(id,"Exception: "+(e&&e.message?e.message:e));}}

  // A) Event normalization (string + object)
  function T_EVT_NORM(){
    var count=0;
    var m=new Machine({id:"n",initial:"A",states:{A:{on:{PING:{target:"A",actions:[function(c,e){count++;}]}}}}});
    var s=m.interpret().start();
    s.send("PING");
    var threw=false;try{s.send({type:"PING"});}catch(e){threw=true;}
    if(count===2&&!threw)return pass("REQ-FSM-06","ok");
    return fail("REQ-FSM-06","need normalization");
  }

  // B) Unchanged state shape
  function T_UNCHANGED(){
    var m=new Machine({id:"u",initial:"A",states:{A:{}}});
    var s=m.interpret().start();
    s.send("NOOP");
    var st=s.state();
    var ok = st && st.value==="A" && st.changed===false && st.matches && st.matches("A")===true;
    return ok?pass("REQ-FSM-10/11/EXEC-06","ok"):fail("REQ-FSM-10/11/EXEC-06","need changed/matches");
  }

  // C) Subscribe / unsubscribe
  function T_SUB(){
    var m=new Machine({id:"sub",initial:"A",states:{A:{on:{T:"A"}}}});
    var s=m.interpret().start();
    if(!s.subscribe)return fail("REQ-FSM-13/ERR-03/REV-09","missing");
    var hits=0;var h=s.subscribe(function(x){hits++;});
    s.send("T");
    if(!h||!h.unsubscribe)return fail("REQ-FSM-13/ERR-03/REV-09","no unsubscribe");
    h.unsubscribe();var b=hits;s.send("T");var a=hits;
    return (a===b)?pass("REQ-FSM-13/ERR-03/REV-09","ok"):fail("REQ-FSM-13/ERR-03/REV-09","unsubscribe");
  }

  // D) Init event propagation
  function T_INIT(){
    var seen=[];
    var m=new Machine({id:"init",initial:"A",states:{A:{entry:[function(c,e){seen.push(e&&e.type?e.type:"NONE");}]}}});
    var s=m.interpret();s.start();
    return (seen.length===1&&seen[0]==="xstate.init")?pass("REQ-REV-02/REQ-FSM-14","ok"):fail("REQ-REV-02/REQ-FSM-14","need xstate.init");
  }

  // E) Action args order
  function T_ARGS(){
    var got=false;
    var m=new Machine({id:"args",initial:"A",states:{A:{on:{T:{target:"A",actions:[function(ctx,evt){got=(ctx&&evt&&evt.type==="T");}]}}}}});
    var s=m.interpret().start();s.send("T");
    return got?pass("REQ-FSM-15","ok"):fail("REQ-FSM-15","arg order");
  }

  // F) Deterministic choice (check all transitions)
  function T_ORDER(){
    var order=[];
    var m=new Machine({id:"ord",initial:"A",states:{A:{on:{E:[
      {target:"A",cond:function(){order.push("first");return false;}},
      {target:"B",cond:function(){order.push("second");return true;}}
    ]}},B:{}}});
    var s=m.interpret().start();s.send("E");
    var ok=(s.state().value==="B"&&order.join(",")==="first,second");
    return ok?pass("REQ-FSM-16","ok"):fail("REQ-FSM-16","loop stops early");
  }

  // G) Targeted + Targetless ordering/assign-first
  function T_TLESS(){
    var seq=[];
    var m=new Machine({id:"tl",initial:"A",context:{c:0},states:{A:{on:{
      TG:{target:"A",actions:[
        {type:"xstate.assign",assignment:function(c,e){return {c:(c.c||0)+1};}},
        function(c,e){seq.push("afterTG:"+c.c);}
      ]},
      TL:{actions:[
        {type:"xstate.assign",assignment:function(c,e){return {c:(c.c||0)+1};}},
        function(c,e){seq.push("afterTL:"+c.c);}
      ]}
    }} }});
    var s=m.interpret().start();
    s.send("TG"); s.send("TL");
    var ok=(seq.length===2 && seq[0]==="afterTG:1" && seq[1]==="afterTL:2");
    return ok?pass("REQ-EXEC-03/04/05","ok"):fail("REQ-EXEC-03/04/05","targetless");
  }

  // H) Exceptions in actions don't break service
  function T_EXC(){
    var m=new Machine({id:"ex",initial:"A",states:{A:{on:{BOOM:{target:"A",actions:[function(){throw new Error("boom");}]}}}}});
    var s=m.interpret().start();
    var ok=true; try{s.send("BOOM");}catch(e){ok=false;}
    return ok?pass("REQ-ERR-04","ok"):fail("REQ-ERR-04","uncaught");
  }

  // I) Status exposed
  function T_STATUS(){
    var m=new Machine({id:"st",initial:"A",states:{A:{}}});
    var s=m.interpret();
    var a=typeof s.status!=="undefined"; s.start(); var b=typeof s.status!=="undefined"; s.stop(); var c=typeof s.status!=="undefined";
    return (a&&b&&c)?pass("REQ-REV-01","ok"):fail("REQ-REV-01","status");
  }

  function runDelta(){
    var tests=[T_EVT_NORM,T_UNCHANGED,T_SUB,T_INIT,T_ARGS,T_ORDER,T_TLESS,T_EXC,T_STATUS];
    var i,r;
    print("=== DELTA START ===");
    for(i=0;i<tests.length;i++){r=runTest("?",tests[i]);var tag=r.ok?"PASS":"FAIL";print(tag+" "+r.id+" : "+r.msg);}
    print("=== DELTA END ===");
  }
  runDelta();
```
---

## 3) Quick fix checklist (recap)

1) **Event normalization** in `send(event)` and in action execution path (always pass a _normalized_ event).  
2) **State object shape**: add `changed`, `matches(s)` for both unchanged and changed results.  
3) **Transition loop**: continue across all candidates (array) until one guard passes.  
4) **Targetless transitions**: execute actions only; never change `value`.  
5) **Subscribe / unsubscribe**: implement listener registry with safe removal; call all listeners on each transition.  
6) **Init event**: execute initial `entry` with `{type:"xstate.init"}`.  
7) **Action invocation**: call as `(context, event)`; wrap in try/catch to satisfy REQ‑ERR‑04.  
8) **Nested states**: reject at machine creation in dev builds.  
9) **Status**: expose numeric status getter.

With those in, the delta suite should go green — then re‑run the **full suite** for regression.

If you’d like, I can also snapshot the current **coverage table** into a Markdown CSV you can paste straight under each requirement row.
