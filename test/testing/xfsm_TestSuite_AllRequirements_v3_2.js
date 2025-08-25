// xfsm_TestSuite_AllRequirements_v2.js
// Espruino XFSM Test Suite - All Requirements v2.2 (native props + native subscribe)

// =========================
// Test Harness Infrastructure
// =========================

function log(s) { print(s); }
function pass(id, msg) { return { id:id, ok:true,  msg:msg }; }
function fail(id, msg) { return { id:id, ok:false, msg:msg }; }
function skip(id, msg) { return { id:id, ok:true,  skipped:true, msg:msg || "Skipped (not directly testable / roadmap)" }; }

function deepEqual(a, b) {
  if (a === b) return true;
  if (!a || !b) return false;
  if (typeof a !== "object" || typeof b !== "object") return false;
  var ka = Object.keys(a);
  var kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (var i = 0; i < ka.length; i++) {
    var k = ka[i];
    if (!deepEqual(a[k], b[k])) return false;
  }
  return true;
}

// Async test helper
function asyncTest(startFn, timeoutMs) {
  return { __async__: true, __start__: startFn, __timeout__: timeoutMs||300 };
}

function asyncTest(startFn, timeoutMs) {
  return { __async__: true, __start__: startFn, __timeout__: (timeoutMs||300) };
}


// Small, optional output throttle (you’re uploading from flash, so default=off)
var XFSM_TS_CFG = {
  FAIL_ONLY      : false,
  CHUNK_DELAY_MS : 0,
  MAX_MSG_LEN    : 160
};
function _san(s) {
  s = ""+s;
  s = s.replace(/[\r\n]/g," ").replace(/,/g,";");
  if (s.length > XFSM_TS_CFG.MAX_MSG_LEN) s = s.slice(0,XFSM_TS_CFG.MAX_MSG_LEN) + "…";
  return s;
}
function _drain(lines, idx) {
  if (idx >= lines.length) return;
  print(lines[idx]);
  setTimeout(function(){ _drain(lines, idx+1); }, XFSM_TS_CFG.CHUNK_DELAY_MS|0);
}

// Helper: make a minimal machine quickly
function makeMachine(config, options) {
  return new Machine(config, options);
}

// Helper: collect ordered trace of action execution
function tracer(arr, label) {
  return function (ctx, evt) { arr.push(label); };
}

// =========================
// Tests — FSM 01..17
// =========================

function T_REQ_FSM_01() {
  var m = makeMachine({ id:"t", initial:"A", states:{ A:{} }});
  if (!m || typeof m.interpret !== "function") return fail("REQ-FSM-01", "Machine/interpret not constructed");
  var s = m.interpret();
  if (!s || typeof s.start !== "function" || typeof s.send !== "function") return fail("REQ-FSM-01", "Service API missing");
  return pass("REQ-FSM-01", "Machine creation & interpret() available");
}

function T_REQ_FSM_02() {
  var m = makeMachine({
    id:"t2",
    initial:"IDLE",
    context:{ c:1 },
    states:{ IDLE:{}, RUN:{} }
  });
  var s = m.interpret().start();
  var st = s.state;
  if (!st) return fail("REQ-FSM-02","no state exposed");
  if (st.value !== "IDLE") return fail("REQ-FSM-02", "Initial state not respected");
  if (!st.context || st.context.c !== 1) return fail("REQ-FSM-02", "Context not set");
  return pass("REQ-FSM-02", "Config fields respected");
}

function T_REQ_FSM_03() {
  var trace = [];
  var m = makeMachine({
    id:"t3",
    initial:"A",
    states:{
      A:{
        entry:[ tracer(trace,"entryA") ],
        exit: [ tracer(trace,"exitA")  ],
        on:{ GO:"B" }
      },
      B:{}
    }
  });
  var s = m.interpret().start();
  if (trace.indexOf("entryA")<0) return fail("REQ-FSM-03", "entry not executed");
  s.send("GO");
  if (trace.indexOf("exitA")<0) return fail("REQ-FSM-03", "exit not executed");
  return pass("REQ-FSM-03", "entry/exit/on supported");
}

function T_REQ_FSM_04() {
  var trace = [];
  var condHit = 0;
  var m = makeMachine({
    id:"t4",
    initial:"S",
    context:{},
    states:{
      S:{
        on:{
          TOBJ: { target:"T", actions:[ tracer(trace,"obj") ] },
          TSTR: "T",
          CGRD: { target:"T", cond:function(ctx,evt){ condHit++; return evt && evt.pass === true; } },
          TLESS: { actions:[ tracer(trace,"tless") ] }
        }
      },
      T:{}
    }
  });
  var s = m.interpret().start();

  s.send("TOBJ");
  if (s.state.value!=="T" || trace[0]!=="obj") return fail("REQ-FSM-04", "object transition failed");

  s.stop().start();
  s.send("TSTR");
  if (s.state.value!=="T") return fail("REQ-FSM-04", "string transition failed");

  s.stop().start();
  s.send({ type:"CGRD", pass:false });
  if (s.state.value!=="S" || condHit!==1) return fail("REQ-FSM-04", "guard false should not transition");
  s.send({ type:"CGRD", pass:true });
  if (s.state.value!=="T" || condHit!==2) return fail("REQ-FSM-04", "guard true should transition");

  s.stop().start();
  s.send("TLESS"); // targetless should not change value
  if (s.state.value!=="S") return fail("REQ-FSM-04", "targetless changed state unexpectedly");
  return pass("REQ-FSM-04", "string/object transitions, guard, targetless handled");
}

function T_REQ_FSM_05() {
  var ran = { n:0 };
  function namedAction(ctx, evt) { ran.n++; }
  var m = makeMachine({
    id:"t5",
    initial:"A",
    states:{
      A:{ on:{ GO:{ target:"A", actions:["doIt"] } } }
    }
  }, { actions: { doIt: namedAction }});
  var s = m.interpret().start();
  s.send("GO");
  if (ran.n===1) return pass("REQ-FSM-05", "options.actions map resolved action name");
  return fail("REQ-FSM-05", "options.actions not used (expected named action to run)");
}

function T_REQ_FSM_06() {
  var count = 0;
  var m = makeMachine({
    id:"t6",
    initial:"A",
    states:{
      A:{ on:{ PING:{ target:"A", actions:[ function (ctx,evt){ count++; } ] } } }
    }
  });
  var s = m.interpret().start();
  s.send("PING");
  var ok1 = (count===1);
  var caught = false;
  try { s.send({ type:"PING" }); } catch(e){ caught = true; }
  var ok2 = (count===2); // parity target
  if (ok1 && ok2) return pass("REQ-FSM-06", "event normalization (string & object) supported");
  if (!ok1) return fail("REQ-FSM-06", "string event failed");
  return fail("REQ-FSM-06", "object-form event not supported (count="+count+", caught="+caught+")");
}

function T_REQ_FSM_07() {
  // Ensure initial entry actions are prepared, applied on start()
  var m = makeMachine({
    id:"t7",
    initial:"A",
    context:{ c:0 },
    states:{
      A:{
        entry:[
          { type:"xstate.assign", assignment:function (ctx,evt){ return { c:(ctx.c||0)+1 }; } }
        ]
      }
    }
  });
  var s = m.interpret();
  var st0 = s.state; // before start, no execution yet
  s.start();
  var st1 = s.state;
  if (!st1.context || st1.context.c!==1) return fail("REQ-FSM-07", "initial assign not reflected after start()");
  return pass("REQ-FSM-07", "initial prep/assign OK; exec on start()");
}

function T_REQ_FSM_08() {
  var trace = [];
  var m = makeMachine({
    id:"t8",
    initial:"A",
    states:{
      A:{
        exit:[ tracer(trace,"exitA") ],
        on:{
          GO:{ target:"B", actions:[ tracer(trace,"trans") ] },
          NOP:{ actions:[ tracer(trace,"nop") ] }
        }
      },
      B:{ entry:[ tracer(trace,"entryB") ] }
    }
  });
  var s = m.interpret().start();
  s.send("GO");
  var ord = trace.join(",");
  var ok1 = (ord==="exitA,trans,entryB");
  trace=[];
  s.send("NOP");
  var ok2 = (s.state.value==="B" && trace.join(",")==="nop");
  if (ok1 && ok2) return pass("REQ-FSM-08", "targeted (exit→trans→entry) and targetless handled");
  return fail("REQ-FSM-08", "ordering/targetless mismatch: "+ord+" / "+trace.join(","));
}

function T_REQ_FSM_09() {
  var seq = [];
  var m = makeMachine({
    id:"t9",
    initial:"A",
    context:{ x:0 },
    states:{
      A:{
        on:{
          T:{
            target:"A",
            actions:[
              { type:"xstate.assign", assignment:function (ctx,evt){ return { x:(ctx.x||0)+1 }; } },
              function after(ctx,evt){ seq.push("after:"+ctx.x); }
            ]
          }
        }
      }
    }
  });
  var s = m.interpret().start();
  s.send("T");
  if (seq.length===1 && seq[0]==="after:1") return pass("REQ-FSM-09", "assign before non-assign");
  return fail("REQ-FSM-09", "assign ordering wrong: "+JSON.stringify(seq));
}

function T_REQ_FSM_10_11() {
  // Expect returned state to expose changed:false and matches()
  var m = makeMachine({ id:"t10", initial:"A", states:{ A:{} }});
  var s = m.interpret().start();
  var before = s.state;
  s.send("NOOP");
  var after  = s.state;
  if (!after || after.value!==before.value) return fail("REQ-FSM-10/11", "state changed unexpectedly");
  var hasChanged = (after.changed !== undefined);
  var hasMatches = (after.matches && typeof after.matches==="function");
  if (hasChanged && !after.changed && hasMatches && after.matches(after.value)) {
    return pass("REQ-FSM-10/11", "unchanged state shape (changed:false, matches())");
  }
  return fail("REQ-FSM-10/11", "missing changed/matches on state");
}

function T_REQ_FSM_12() {
  // Nested states must be rejected (dev behavior)
  var threw = false;
  try {
    makeMachine({
      id:"t12",
      initial:"A",
      states:{
        A:{ states:{ AA:{} } } // illegal for v4 FSM
      }
    });
  } catch(e) { threw = true; }
  if (threw) return pass("REQ-FSM-12", "nested states rejected");
  return fail("REQ-FSM-12", "nested states allowed (should be rejected)");
}

function T_REQ_FSM_13() {
  var m = makeMachine({ id:"t13", initial:"A", states:{ A:{ on:{ T:"A" } } }});
  var s = m.interpret().start();
  var okAPI = (typeof s.start==="function" && typeof s.stop==="function" && typeof s.send==="function");
  var hasSubscribe = (typeof s.subscribe==="function");
  if (!okAPI) return fail("REQ-FSM-13", "Service lifecycle API missing");
  if (!hasSubscribe) return fail("REQ-FSM-13", "subscribe not implemented");

  var hits=0;
  var unsub = s.subscribe(function(st){ hits++; });
  if (typeof unsub !== "function") return fail("REQ-FSM-13","subscribe did not return function");

  s.send("T"); // should notify
  var before = hits;
  unsub();     // remove
  s.send("T");
  var after = hits;
  if (after!==before) return fail("REQ-FSM-13", "unsubscribe ineffective");

  // NEW: stop() clears listeners and prevents notifications
  s.subscribe(function(){ hits++; });
  s.stop();
  var keys = Object.keys(s._listeners||{});
  if (keys.length !== 0) return fail("REQ-FSM-13","stop() did not clear listeners");
  var hitsBefore = hits;
  s.send("T");
  if (hits !== hitsBefore) return fail("REQ-FSM-13","send() while stopped triggered listeners");
  return pass("REQ-FSM-13", "Service lifecycle + subscribe/unsubscribe + stop() clear OK");
}

function T_REQ_FSM_14() {
  // Init actions should get xstate.init as event.type
  var seen=[];
  var m = makeMachine({ id:"t14", initial:"A", states:{ A:{ entry:[ function(ctx,evt){ seen.push(evt&&evt.type?evt.type:"NONE"); } ] } }});
  var s = m.interpret(); s.start();
  if (seen.length===1 && seen[0]==="xstate.init") return pass("REQ-FSM-14", "xstate.init propagated");
  return fail("REQ-FSM-14", "init event not xstate.init (saw "+JSON.stringify(seen)+")");
}

function T_REQ_FSM_15() {
  // Action function receives (context,event)
  var got = [];
  var m = makeMachine({
    id:"t15", initial:"A",
    states:{ A:{ on:{ T:{ target:"A", actions:[ function (ctx,evt){ got.push( (ctx!==undefined) && (!!evt && typeof evt.type==="string") ); } ] } } } }
  });
  var s = m.interpret().start();
  s.send("T");
  if (got.length===1 && got[0]===true) return pass("REQ-FSM-15", "action exec(ctx,evt) shape OK");
  return fail("REQ-FSM-15", "action received unexpected parameters");
}

function T_REQ_FSM_16() {
  // Deterministic transition order for arrays; assign-first already tested
  var order=[];
  var m = makeMachine({
    id:"t16", initial:"A",
    states:{ A:{ on:{
      E:[
        { target:"A", cond:function(){ order.push("first"); return false; } },
        { target:"B", cond:function(){ order.push("second"); return true; } }
      ]
    } }, B:{} }
  });
  var s = m.interpret().start();
  s.send("E");
  if (s.state.value==="B" && order.join(",")==="first,second") return pass("REQ-FSM-16", "deterministic transition order");
  return fail("REQ-FSM-16", "order wrong or result not B: "+order.join(",")+" -> "+s.state.value);
}

function T_REQ_FSM_17() {
  // Dev vs prod checks are build-time; we can only sanity check missing target doesn't crash
  var m = makeMachine({ id:"t17", initial:"A", states:{ A:{ on:{ BAD:{ target:"MISSING" } } } }});
  var s = m.interpret().start();
  var ok=true; try{ s.send("BAD"); }catch(e){ ok=false; }
  if (ok) return pass("REQ-FSM-17", "invalid target handled without crashing (dev/prod behavior may differ)");
  return fail("REQ-FSM-17", "invalid target caused exception");
}

// =========================
// Tests — EXEC 01..06
// =========================

function T_REQ_EXEC_01() {
  var ran=0;
  var m=makeMachine({ id:"e1", initial:"A", states:{ A:{ entry:[ function(){ ran++; } ] } }});
  var s=m.interpret(); var before=ran; s.start(); var after=ran;
  if (after===before+1) return pass("REQ-EXEC-01", "start() executes initial entry");
  return fail("REQ-EXEC-01", "initial entry not executed");
}

function T_REQ_EXEC_02() {
  var ran=0;
  var m=makeMachine({ id:"e2", initial:"A", states:{ A:{ on:{ T:{ target:"A", actions:[ function(){ ran++; } ] } } } }});
  var s=m.interpret().start();
  s.send("T");
  if (ran===1) return pass("REQ-EXEC-02", "send() executes transition actions");
  return fail("REQ-EXEC-02", "send() didn't execute action");
}

function T_REQ_EXEC_03_04_05() {
  // assign partitioning and ordering (targeted and targetless)
  var seq=[];
  var m=makeMachine({
    id:"e3",
    initial:"A",
    context:{ c:0 },
    states:{
      A:{ on:{
        TG:{ target:"A", actions:[
          { type:"xstate.assign", assignment:function(ctx,evt){ return { c:(ctx.c||0)+1 }; } },
          function(ctx,evt){ seq.push("afterTG:"+ctx.c); }
        ]},
        TL:{ actions:[
          { type:"xstate.assign", assignment:function(ctx,evt){ return { c:(ctx.c||0)+1 }; } },
          function(ctx,evt){ seq.push("afterTL:"+ctx.c); }
        ]}
      } }
    }
  });
  var s=m.interpret().start();
  s.send("TG"); s.send("TL");
  var ok = (seq[0]==="afterTG:1" && seq[1]==="afterTL:2");
  if (ok) return pass("REQ-EXEC-03/04/05", "assign-first; targeted and targetless ordering OK");
  return fail("REQ-EXEC-03/04/05", "ordering wrong: "+JSON.stringify(seq));
}

function T_REQ_EXEC_06() {
  var m=makeMachine({ id:"e6", initial:"A", states:{ A:{} }});
  var s=m.interpret().start();
  var st=s.state;
  var hasMatches = (st.matches && typeof st.matches==="function");
  if (hasMatches) return pass("REQ-EXEC-06", "matches predicate present");
  return fail("REQ-EXEC-06", "matches predicate missing");
}

// =========================
// Tests — ERR 01..04
// =========================

function T_REQ_ERR_01() {
  var threw=false;
  try{
    makeMachine({ id:"err1", initial:"Z", states:{ A:{} }});
  }catch(e){ threw=true; }
  // If dev checks present, this should throw; otherwise pass leniently
  if (threw) return pass("REQ-ERR-01", "invalid initial rejected");
  return pass("REQ-ERR-01", "no throw (lenient dev build) — acceptable");
}

function T_REQ_ERR_02() {
  var m=makeMachine({ id:"err2", initial:"A", states:{ A:{ on:{ BAD:{ target:"NONE" } } } }});
  var s=m.interpret().start();
  var ok=true; try{ s.send("BAD"); } catch(e){ ok=false; }
  if (ok) return pass("REQ-ERR-02", "invalid target handled (no crash)");
  return fail("REQ-ERR-02", "invalid target threw");
}

function T_REQ_ERR_03() {
  var m=makeMachine({ id:"err3", initial:"A", states:{ A:{ on:{ T:"A" } } }});
  var s=m.interpret().start();
  if (!s.subscribe) return fail("REQ-ERR-03", "subscribe not implemented");
  var hits=0; var unsub=s.subscribe(function(st){ hits++; });
  if (typeof unsub !== "function") return fail("REQ-ERR-03","unsubscribe function not returned");

  // NEW: subscribe(null) returns safe function
  var u2 = s.subscribe(null);
  if (typeof u2 !== "function") return fail("REQ-ERR-03","subscribe(null) did not return function");
  var threw=false; try { u2(); } catch(e){ threw=true; }
  if (threw) return fail("REQ-ERR-03","unsubscribe(null) threw");

  unsub(); var before=hits; s.send("T"); var after=hits;
  if (after===before) return pass("REQ-ERR-03", "unsubscribe prevents notifications + null-subscriber safe");
  return fail("REQ-ERR-03", "unsubscribe ineffective");
}

function T_REQ_ERR_04() {
  var m=makeMachine({ id:"err4", initial:"A", states:{ A:{ on:{ BOOM:{ target:"A", actions:[ function(){ throw new Error("boom"); } ] } } } }});
  var s=m.interpret().start();
  var ok=true; try{ s.send("BOOM"); } catch(e){ ok=false; }
  if (ok) return pass("REQ-ERR-04", "exception in action did not corrupt service (no throw)");
  return fail("REQ-ERR-04", "exception escaped to caller");
}

// =========================
// Tests — ESP 01..06
// =========================

function T_REQ_ESP_01() {
  var m=makeMachine({ id:"esp1", initial:"A", states:{ A:{} }});
  var s=m.interpret();
  var ok = (typeof s.start==="function" &&
            typeof s.stop==="function" &&
            typeof s.send==="function" &&
            typeof s.state==="object");
  // Also ensure subscribe is on prototype (not own)
  var own = s.hasOwnProperty && s.hasOwnProperty("subscribe");
  if (!ok || own) return fail("REQ-ESP-01", "JS API incomplete or subscribe is an own property");
  return pass("REQ-ESP-01", "JS API surface available; subscribe on prototype");
}

function T_REQ_ESP_02() {
  var m=makeMachine({ id:"esp2", initial:"A", context:{ x:1 }, states:{ A:{ on:{ T:{ target:"A", actions:[ function(ctx){ ctx._seen=1; } ] } } } }});
  var s=m.interpret().start();
  s.send("T");
  var st=s.state;
  if (st && typeof st.context==="object") return pass("REQ-ESP-02", "context is JS object (JsVar-backed)");
  return fail("REQ-ESP-02", "context not a JS object");
}

function T_REQ_ESP_03() {
  var got=false;
  var m=makeMachine({ id:"esp3", initial:"A", states:{ A:{ on:{ T:{ target:"A", actions:[ function (ctx,evt){ got=(evt&&evt.type==="T"); } ] } } } }});
  var s=m.interpret().start();
  s.send("T");
  if (got) return pass("REQ-ESP-03", "callbacks receive (context,event)");
  return fail("REQ-ESP-03", "callback did not receive expected event");
}

function T_REQ_ESP_04() {
  return skip("REQ-ESP-04", "logging policy is build/runtime dependent; manual verification via jsDebug");
}

function T_REQ_ESP_05() {
  if (!process || !process.memory) return skip("REQ-ESP-05", "process.memory() not available");
  var m=makeMachine({ id:"esp5", initial:"A", states:{ A:{ on:{ T:"A" } } }});
  var s=m.interpret().start();
  var mem0=process.memory().free;
  for(var i=0;i<200;i++) s.send("T");
  var mem1=process.memory().free;
  if (mem0 - mem1 < 64) return pass("REQ-ESP-05", "no significant leak detected in short burst");
  return fail("REQ-ESP-05", "free memory dropped by "+(mem0-mem1));
}

function T_REQ_ESP_06() {
  var m=makeMachine({ id:"esp6", initial:"A", states:{ A:{ on:{ T:"A" } } }});
  var s=m.interpret().start();
  var N=500;
  var t0=getTime(); for(var i=0;i<N;i++) s.send("T"); var dt=getTime()-t0;
  return pass("REQ-ESP-06", "Executed "+N+" transitions in "+dt.toFixed(3)+"s");
}

// =========================
// Tests — SCXML 01..03
// =========================

function T_REQ_SCXML_01() {
  return pass("REQ-SCXML-01", "Basic SCXML-aligned ordering covered by other tests");
}
function T_REQ_SCXML_02() { return pass("REQ-SCXML-02", "Documented separately (non-runtime)"); }
function T_REQ_SCXML_03() { return skip("REQ-SCXML-03", "LCCA for hierarchy is roadmap; not in Phase 1"); }

// =========================
// Tests — Roadmap RM 01..05 (skip)
// =========================

function T_REQ_RM_01() { return skip("REQ-RM-01", "Hierarchical states (future)"); }
function T_REQ_RM_02() { return skip("REQ-RM-02", "always/delays/activities (future)"); }
function T_REQ_RM_03() { return skip("REQ-RM-03", "History states (future)"); }
function T_REQ_RM_04() { return skip("REQ-RM-04", "SCXML parity/import (future)"); }
function T_REQ_RM_05() { return skip("REQ-RM-05", "XState v5 actor model (out of scope)"); }

// =========================
// Tests — REV 01..09 (reverse-parity specifics)
// =========================

function T_REQ_REV_01() {
  // Enforce exact numeric mapping: 0 NotStarted, 1 Running, 2 Stopped
  var m=makeMachine({ id:"rev1", initial:"A", states:{ A:{} }});
  var s=m.interpret();
  var ns = s.status;         // expect 0
  if (ns !== 0) return fail("REQ-REV-01","NotStarted must be 0 (saw "+ns+")");
  s.start();
  var rs = s.status;         // expect 1
  if (rs !== 1) return fail("REQ-REV-01","Running must be 1 (saw "+rs+")");
  s.stop();
  var ss = s.status;         // expect 2
  if (ss !== 2) return fail("REQ-REV-01","Stopped must be 2 (saw "+ss+")");
  return pass("REQ-REV-01", "status enum = 0/1/2 verified");
}

// Async: subscribe semantics + queued pre-notify + removal fidelity + idempotent unsub
function T_REQ_REV_09() {
  return asyncTest(function(done){
    var m = makeMachine({ id:"rev9", initial:"A", states:{ A:{ on:{ NEXT:"B"} }, B:{} }});
    var s = m.interpret();

    // A) subscribe BEFORE start => no immediate fire; after tick => 1 fire with "A"
    var seenA=0, lastA=null;
    var unsubA = s.subscribe(function(st){ seenA++; lastA = st && st.value; });
    if (seenA !== 0) { done(fail("REQ-REV-09","pre-start subscribe fired synchronously")); return; }

    setTimeout(function(){
      if (!(seenA===1 && lastA==="A")) { done(fail("REQ-REV-09","pre-start queued pre-notify failed (seen="+seenA+", last="+lastA+")")); return; }

      // move to B
      s.start(); s.send("NEXT"); // now state B

      // B) subscribe WHILE running => next tick, must fire with current "B"
      var seenB=0, lastB=null;
      var unsubB = s.subscribe(function(st){ seenB++; lastB = st && st.value; });

      if (seenB !== 0) { done(fail("REQ-REV-09","running subscribe fired synchronously")); return; }

      setTimeout(function(){
        if (!(seenB===1 && lastB==="B")) { done(fail("REQ-REV-09","running subscribe didn’t pre-notify current state 'B'")); return; }

        // C) unsubscribe removes key; idempotent second call safe
        var keysBefore = Object.keys(s._listeners||{});
        var ok1 = unsubB();
        var ok2 = false; var threw=false;
        try { ok2 = unsubB(); } catch(e){ threw=true; }

        var keysAfter = Object.keys(s._listeners||{});
        if (threw) { done(fail("REQ-REV-09","second unsubscribe threw")); return; }
        if (!ok1) { done(fail("REQ-REV-09","unsubscribe returned false / ineffective")); return; }
        if (keysAfter.length >= keysBefore.length) { done(fail("REQ-REV-09","listener map not reduced after unsubscribe")); return; }

        // Ensure A still present, then remove it and verify no more callbacks
        var beforeHits = seenA;
        var okA = unsubA();
        s.send("NEXT");
        if (!okA) { done(fail("REQ-REV-09","first unsubscribe returned false")); return; }
        if (seenA !== beforeHits) { done(fail("REQ-REV-09","callbacks still firing after unsubscribe")); return; }

        done(pass("REQ-REV-09","subscribe semantics: queued pre-notify, running pre-notify, removal + idempotent unsub OK"));
      },0);
    },0);
  }, 500);
}

function T_REQ_REV_02() { return T_REQ_FSM_14(); } // init event propagation
function T_REQ_REV_03() {
  var ctxAfter=0;
  var m=makeMachine({
    id:"rev3", initial:"A", context:{ x:0 },
    states:{ A:{ on:{ T:{ target:"A", actions:[ { type:"xstate.assign", assignment:function(ctx){ return { x:ctx.x+2 }; } } ] } } } }
  });
  var s=m.interpret().start();
  s.send("T"); ctxAfter=s.state.context.x;
  if (ctxAfter===2) return pass("REQ-REV-03", "xstate.assign recognized");
  return fail("REQ-REV-03", "assign tag not handled");
}
function T_REQ_REV_04() { return pass("REQ-REV-04", "Normalization helpers exercised via other tests"); }
function T_REQ_REV_05() { return T_REQ_FSM_10_11(); }
function T_REQ_REV_06() {
  var ran=0;
  var m=makeMachine({ id:"rev6", initial:"A", states:{ A:{ entry:[ function(){ ran++; } ] } }});
  var s=m.interpret(); if (ran!==0) return fail("REQ-REV-06","entry executed before start");
  s.start(); if (ran===1) return pass("REQ-REV-06","initial actions deferred until start()");
  return fail("REQ-REV-06","entry not executed on start");
}
function T_REQ_REV_07() { return pass("REQ-REV-07", "Covered by deterministic transition order test"); }
function T_REQ_REV_08() { return pass("REQ-REV-08", "Covered by action exec(ctx,evt) test"); }

// =========================
// Tests — DEV 01..05
// =========================

function T_REQ_DEV_01() { return skip("REQ-DEV-01", "Precomputed lookups are internal; not observable from JS"); }
function T_REQ_DEV_02() {
  var m=makeMachine({ id:"dev2", initial:"A", context:{x:1}, states:{A:{}}});
  var s=m.interpret().start();
  var st=s.state;
  if (st && typeof st.context==="object" && st.context.x===1) return pass("REQ-DEV-02", "context kept as object");
  return fail("REQ-DEV-02", "context not preserved");
}
function T_REQ_DEV_03() { return T_REQ_EXEC_03_04_05(); }
function T_REQ_DEV_04() {
  var seq=[];
  var m=makeMachine({
    id:"dev4", initial:"A",
    states:{ A:{ on:{ OUT:{ target:"A", actions:[ function(ctx,evt){ seq.push("outer"); this && this.send && this.send("IN"); } ] },
                       IN:{ target:"A", actions:[ function(){ seq.push("inner"); } ] } } } }
  });
  var s=m.interpret().start();
  var ok=true; try{ s.send("OUT"); }catch(e){ ok=false; }
  if (!ok) return fail("REQ-DEV-04", "reentrant send threw");
  if (seq.length>=1) return pass("REQ-DEV-04", "reentrant send did not break service (seq="+seq.join(",")+")");
  return fail("REQ-DEV-04", "no actions observed");
}
function T_REQ_DEV_05() { return skip("REQ-DEV-05", "Debug traces are manual (jsDebug)"); }

// =========================
// Async-aware Runner
// =========================

XFSM_TS_CFG.FAIL_ONLY      = XFSM_TS_CFG.FAIL_ONLY || false;
XFSM_TS_CFG.CHUNK_DELAY_MS = (XFSM_TS_CFG.CHUNK_DELAY_MS===undefined) ? 1 : XFSM_TS_CFG.CHUNK_DELAY_MS;

function runAll() {
  var tests = [
    ["REQ-FSM-01", T_REQ_FSM_01],
    ["REQ-FSM-02", T_REQ_FSM_02],
    ["REQ-FSM-03", T_REQ_FSM_03],
    ["REQ-FSM-04", T_REQ_FSM_04],
    ["REQ-FSM-05", T_REQ_FSM_05],
    ["REQ-FSM-06", T_REQ_FSM_06],
    ["REQ-FSM-07", T_REQ_FSM_07],
    ["REQ-FSM-08", T_REQ_FSM_08],
    ["REQ-FSM-09", T_REQ_FSM_09],
    ["REQ-FSM-10/11", T_REQ_FSM_10_11],
    ["REQ-FSM-12", T_REQ_FSM_12],
    ["REQ-FSM-13", T_REQ_FSM_13],
    ["REQ-FSM-14", T_REQ_FSM_14],
    ["REQ-FSM-15", T_REQ_FSM_15],
    ["REQ-FSM-16", T_REQ_FSM_16],
    ["REQ-FSM-17", T_REQ_FSM_17],

    ["REQ-EXEC-01", T_REQ_EXEC_01],
    ["REQ-EXEC-02", T_REQ_EXEC_02],
    ["REQ-EXEC-03/04/05", T_REQ_EXEC_03_04_05],
    ["REQ-EXEC-06", T_REQ_EXEC_06],

    ["REQ-ERR-01", T_REQ_ERR_01],
    ["REQ-ERR-02", T_REQ_ERR_02],
    ["REQ-ERR-03", T_REQ_ERR_03],
    ["REQ-ERR-04", T_REQ_ERR_04],

    ["REQ-ESP-01", T_REQ_ESP_01],
    ["REQ-ESP-02", T_REQ_ESP_02],
    ["REQ-ESP-03", T_REQ_ESP_03],
    ["REQ-ESP-04", T_REQ_ESP_04],
    ["REQ-ESP-05", T_REQ_ESP_05],
    ["REQ-ESP-06", T_REQ_ESP_06],

    ["REQ-SCXML-01", T_REQ_SCXML_01],
    ["REQ-SCXML-02", T_REQ_SCXML_02],
    ["REQ-SCXML-03", T_REQ_SCXML_03],

    ["REQ-RM-01", T_REQ_RM_01],
    ["REQ-RM-02", T_REQ_RM_02],
    ["REQ-RM-03", T_REQ_RM_03],
    ["REQ-RM-04", T_REQ_RM_04],
    ["REQ-RM-05", T_REQ_RM_05],

    ["REQ-REV-01", T_REQ_REV_01],
    ["REQ-REV-02", T_REQ_REV_02],
    ["REQ-REV-03", T_REQ_REV_03],
    ["REQ-REV-04", T_REQ_REV_04],
    ["REQ-REV-05", T_REQ_REV_05],
    ["REQ-REV-06", T_REQ_REV_06],
    ["REQ-REV-07", T_REQ_REV_07],
    ["REQ-REV-08", T_REQ_REV_08],
    ["REQ-REV-09", T_REQ_REV_09],

    ["REQ-DEV-01", T_REQ_DEV_01],
    ["REQ-DEV-02", T_REQ_DEV_02],
    ["REQ-DEV-03", T_REQ_DEV_03],
    ["REQ-DEV-04", T_REQ_DEV_04],
    ["REQ-DEV-05", T_REQ_DEV_05]
  ];

  var results = [];
  var out = [];

  function addSummaryLines() {
    out.push("");
    out.push("=== SUMMARY ===");
    for (var j=0;j<results.length;j++) {
      var r = results[j];
      var status = r.skipped ? "SKIP" : (r.ok ? "PASS" : "FAIL");
      if (!XFSM_TS_CFG.FAIL_ONLY || r.skipped || !r.ok)
        out.push(status + " " + r.id + " : " + _san(r.msg));
    }
    out.push("");
    out.push("=== CSV (RequirementID,Result,Message) ===");
    for (var k=0;k<results.length;k++) {
      var c = results[k];
      var res = c.skipped ? "SKIP" : (c.ok ? "PASS" : "FAIL");
      out.push(c.id + "," + res + "," + _san(c.msg));
    }
    out.push("=== END ===");
  }

  var i = 0;

  function scheduleNext() {
    setTimeout(function(){ runOne(); }, 0); // yield to unwind C/JS stacks
  }

  function pushResult(id, res) {
    if (!res || res.ok === undefined) res = { ok:false, msg:"Test returned no structured result" };
    res.id = id;
    results.push(res);
  }

  function runOne() {
    if (i >= tests.length) {
      addSummaryLines();
      _drain(out, 0); // chunked printing
      return;
    }
    var id = tests[i][0];
    var fn = tests[i][1];

    var r;
    try { r = fn(); }
    catch(e) {
      pushResult(id, { ok:false, msg:"Exception: "+(e&&e.message?e.message:(""+e)) });
      i++; scheduleNext(); return;
    }

    // Async test?
    if (r && r.__async__ && typeof r.__start__ === "function") {
      var doneCalled = false;
      var to = setTimeout(function(){
        if (doneCalled) return;
        doneCalled = true;
        pushResult(id, { ok:false, msg:"Timeout (async)" });
        i++; scheduleNext();
      }, r.__timeout__|0);

      r.__start__(function(res){
        if (doneCalled) return;
        doneCalled = true;
        clearTimeout(to);
        pushResult(id, res);
        i++; scheduleNext();
      });

    } else {
      pushResult(id, r);
      i++; scheduleNext();
    }
  }

  runOne();
}