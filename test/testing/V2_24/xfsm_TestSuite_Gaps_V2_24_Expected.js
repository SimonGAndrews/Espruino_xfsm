// xfsm_TestSuite_Gaps_V2_24_Expected.js
// Espruino XFSM Gap Tests — EXPECTED PASS suite after fixes (V2_24)
// Mirrors the gap IDs and comments from xfsm_TestSuite_Gaps_V2_24.js

// =========================
// Test Harness (mirrors v3_2 style)
// =========================

function log(s) { print(s); }
function pass(id, msg) { return { id:id, ok:true,  msg:msg }; }
function fail(id, msg) { return { id:id, ok:false, msg:msg }; }
function skip(id, msg) { return { id:id, ok:true,  skipped:true, msg:msg || "Skipped" }; }

function asyncTest(startFn, timeoutMs) { return { __async__: true, __start__: startFn, __timeout__: (timeoutMs||400) }; }

var XFSM_TS_CFG = { FAIL_ONLY:false, CHUNK_DELAY_MS:0, MAX_MSG_LEN:160 };
function _san(s){ s=""+s; s=s.replace(/[\r\n]/g," ").replace(/,/g,";"); if(s.length>XFSM_TS_CFG.MAX_MSG_LEN)s=s.slice(0,XFSM_TS_CFG.MAX_MSG_LEN)+"…"; return s; }
function _drain(lines, i){ if(i>=lines.length) return; print(lines[i]); setTimeout(function(){ _drain(lines,i+1); }, XFSM_TS_CFG.CHUNK_DELAY_MS|0); }

function makeMachine(config, options) { return new Machine(config, options); }
function tracer(arr, label) { return function (ctx, evt) { arr.push(label); }; }

// =========================
// Expected-Pass Tests (one per gap)
// =========================

// G1: Targetless must NOT run exit/entry (expected behavior)
function T_G1_Targetless_Suppresses_ExitEntry() {
  var trace=[];
  var m = makeMachine({
    id:"g1", initial:"A",
    states:{
      A:{ exit:[tracer(trace,"exitA")], on:{ NOOP:{ actions:[tracer(trace,"nop")] } } },
      B:{ entry:[tracer(trace,"entryB")] }
    }
  });
  var s = m.interpret().start();
  s.send("NOOP");
  var got = trace.join(",");
  if (got==="nop") return pass("G1","targetless suppressed exit/entry");
  return fail("G1","targetless included extra actions: "+got);
}

// G2a/G2b: Assign must apply before non-assign even if ordered after
function T_G2a_AssignFirst_Targeted() {
  var seq=[];
  var m = makeMachine({ id:"g2a", initial:"A", context:{x:0}, states:{
    A:{ on:{ T:{ target:"A", actions:[ function(ctx){ seq.push("after:"+(ctx.x||0)); }, { type:"xstate.assign", assignment:function(ctx){ return { x:(ctx.x||0)+1 }; } } ] } } }
  }});
  var s = m.interpret().start();
  s.send("T");
  if (seq[0]==="after:1") return pass("G2a","assign-first honored for targeted");
  return fail("G2a","assign not applied before non-assign (saw "+seq.join(",")+")");
}
function T_G2b_AssignFirst_Targetless() {
  var seq=[];
  var m = makeMachine({ id:"g2b", initial:"A", context:{x:0}, states:{
    A:{ on:{ T:{ actions:[ function(ctx){ seq.push("after:"+(ctx.x||0)); }, { type:"xstate.assign", assignment:function(ctx){ return { x:(ctx.x||0)+1 }; } } ] } } }
  }});
  var s = m.interpret().start();
  s.send("T");
  if (seq[0]==="after:1") return pass("G2b","assign-first honored for targetless");
  return fail("G2b","assign not applied before non-assign (saw "+seq.join(",")+")");
}

// G3: No-match should return unchanged state (pure)
function T_G3_NoMatch_Returns_Unchanged_State_Object() {
  var m = makeMachine({ id:"g3", initial:"A", states:{ A:{} } });
  var st0 = m.initialState();
  var st1 = m.transition(st0, "NOPE");
  if (!st1) return fail("G3","transition() returned undefined on no-match (expected unchanged state object)");
  if (st1.value!==st0.value) return fail("G3","unexpected value change on no-match");
  if (st1.changed!==false) return fail("G3","changed not false on no-match");
  return pass("G3","unchanged state returned on no-match");
}

// G4: changed should be true when targetless actions occur
function T_G4_Changed_True_For_Targetless_Actions() {
  var m = makeMachine({ id:"g4", initial:"A", states:{ A:{ on:{ NOOP:{ actions:[ function(){} ] } } } }});
  var s = m.interpret().start();
  s.send("NOOP");
  if (s.state.changed===true) return pass("G4","changed=true for targetless actions");
  return fail("G4","changed=false after targetless actions (expected true)");
}

// G5: changed should be true when self-target with actions (A->A)
function T_G5_Changed_True_For_SelfTarget_With_Actions() {
  var m = makeMachine({ id:"g5", initial:"A", states:{ A:{ on:{ STAY:{ target:"A", actions:[ function(){} ] } } } }});
  var s = m.interpret().start();
  s.send("STAY");
  if (s.state.changed===true) return pass("G5","changed=true for self-target with actions");
  return fail("G5","changed=false for self-target with actions");
}

// G6: Nested states must be rejected
function T_G6_Nested_States_Rejected() {
  var threw=false;
  try { makeMachine({ id:"g6", initial:"A", states:{ A:{ states:{ AA:{} } } }}); } catch(e){ threw=true; }
  if (threw) return pass("G6","nested states rejected");
  return fail("G6","nested states accepted (should be rejected)");
}

// G7: Single (non-array) entry should execute on start()
function T_G7_InitialEntry_SingleItem_Executes() {
  var ran=0;
  var m = makeMachine({ id:"g7", initial:"A", states:{ A:{ entry: function(){ ran++; } } }});
  var s = m.interpret().start();
  if (ran===1) return pass("G7","single entry executed on start()");
  return fail("G7","single entry not executed (requires array normalization)");
}

// G8: send() should notify subscribers even when unchanged (no-match)
function T_G8_Subscribe_Notified_On_NoMatch() {
  // Important: account for queued pre-notify from subscribe().
  // Step 1: let pre-notify fire; Step 2: send NOOP; Step 3: expect +1 hit.
  return asyncTest(function(done){
    var m = makeMachine({ id:"g8", initial:"A", states:{ A:{} }});
    var s = m.interpret().start();
    var hits=0;
    s.subscribe(function(){ hits++; });
    setTimeout(function(){
      var pre = hits;             // after pre-notify
      s.send("NOOP");            // no-match
      setTimeout(function(){
        if (hits > pre) done(pass("G8","subscriber notified on no-match send"));
        else done(fail("G8","no subscriber notification on no-match send"));
      },0);
    },0);
  }, 500);
}

// G9: Service.send() should return this (chainable)
function T_G9_ServiceSend_Returns_This() {
  var m = makeMachine({ id:"g9", initial:"A", states:{ A:{ on:{ T:{ target:"A" } } } }});
  var s = m.interpret().start();
  var ret = s.send("T");
  if (ret === s) return pass("G9","send() returned this");
  return fail("G9","send() did not return this (got "+(typeof ret)+")");
}

// G10: Initial context should reflect assign preparation before start() (pure)
function T_G10_Initial_Context_Precompute_Before_Start() {
  var m = makeMachine({ id:"g10", initial:"A", context:{ c:0 }, states:{ A:{ entry:[ { type:"xstate.assign", assignment:function(ctx){ return { c:(ctx.c||0)+1 }; } } ] } }});
  var s = m.interpret();
  var st0 = s.state;
  if (st0 && st0.context && st0.context.c===1) return pass("G10","initial context precomputed before start()");
  return fail("G10","initial context not precomputed (c="+(st0&&st0.context?st0.context.c:"?")+")");
}

// =========================
// Runner
// =========================

function runAllGapsExpected() {
  var tests = [
    ["G1",  T_G1_Targetless_Suppresses_ExitEntry],
    ["G2a", T_G2a_AssignFirst_Targeted],
    ["G2b", T_G2b_AssignFirst_Targetless],
    ["G3",  T_G3_NoMatch_Returns_Unchanged_State_Object],
    ["G4",  T_G4_Changed_True_For_Targetless_Actions],
    ["G5",  T_G5_Changed_True_For_SelfTarget_With_Actions],
    ["G6",  T_G6_Nested_States_Rejected],
    ["G7",  T_G7_InitialEntry_SingleItem_Executes],
    ["G8",  T_G8_Subscribe_Notified_On_NoMatch],
    ["G9",  T_G9_ServiceSend_Returns_This],
    ["G10", T_G10_Initial_Context_Precompute_Before_Start]
  ];

  var results = [], out=[];
  function addSummary(){
    out.push(""); out.push("=== SUMMARY (Gaps Expected) ===");
    for (var i=0;i<results.length;i++) { var r=results[i]; var status=r.skipped?"SKIP":(r.ok?"PASS":"FAIL"); out.push(status+" "+r.id+" : "+_san(r.msg)); }
    out.push(""); out.push("=== CSV (GapID,Result,Message) ===");
    for (var j=0;j<results.length;j++) { var c=results[j]; var res=c.skipped?"SKIP":(c.ok?"PASS":"FAIL"); out.push(c.id+","+res+","+_san(c.msg)); }
    out.push("=== END ===");
  }

  var i=0; function scheduleNext(){ setTimeout(function(){ runOne(); },0); }
  function pushResult(id,r){ if(!r||r.ok===undefined) r={ ok:false, msg:"No structured result" }; r.id=id; results.push(r); }
  function runOne(){
    if (i>=tests.length) { addSummary(); _drain(out,0); return; }
    var id=tests[i][0], fn=tests[i][1];
    var r; try{ r=fn(); } catch(e){ pushResult(id,{ ok:false, msg:"Exception: "+(e&&e.message?e.message:(""+e)) }); i++; scheduleNext(); return; }
    if (r && r.__async__ && typeof r.__start__==="function") {
      var done=false; var to=setTimeout(function(){ if(done) return; done=true; pushResult(id,{ ok:false, msg:"Timeout (async)" }); i++; scheduleNext(); }, r.__timeout__|0);
      r.__start__(function(res){ if(done) return; done=true; clearTimeout(to); pushResult(id,res); i++; scheduleNext(); });
    } else { pushResult(id,r); i++; scheduleNext(); }
  }
  runOne();
}

// Execute
runAllGapsExpected();
