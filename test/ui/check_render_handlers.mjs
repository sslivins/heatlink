// Regression test for member-row rendering with hostile display names.
//
// The Zones panel builds each member row's markup as an HTML string in
// memberRow(). A head's display name is user-controlled ("Eric's Room",
// "Kids' Room", …) and can contain a single quote, double quote, backslash or
// angle brackets. An earlier cut built the remove/leave buttons as an inline
//     onclick="groupRemoveMember('<uid>','<name>')"
// with a name-escaper that did NOT escape the single quote. For a name like
// "Eric's Room" the apostrophe closed the JS string early, so the button's
// handler was a SyntaxError and clicking it did NOTHING (no confirm, no remove)
// — with zero console noise the user could see.
//
// check_ui_syntax.mjs cannot catch this: the broken JS only exists inside a
// template string that becomes an attribute at runtime, not in the inline
// <script> it parses. This test renders memberRow() with adversarial names and
// asserts the result is safe: (1) any inline on*= handler value still parses as
// JS, and (2) the buttons carry the delegated data-* hooks (not interpolated
// inline handlers), so a quote in a name can never break the click path again.
//
// No external deps: memberRow depends only on gEsc(), so both functions are
// sliced out of the inline <script> by brace-matching and evaluated in a vm.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');

// Grab the (single) inline script body.
const scriptM = /<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/i.exec(html);
if (!scriptM) { console.error('FAIL: no inline <script> found'); process.exit(1); }
const script = scriptM[1];

// Extract a top-level `function NAME(...) { ... }` by brace matching.
function extractFn(src, name) {
  const start = src.indexOf('function ' + name);
  if (start < 0) throw new Error(`function ${name} not found`);
  const braceOpen = src.indexOf('{', start);
  if (braceOpen < 0) throw new Error(`no body for ${name}`);
  let depth = 0;
  for (let i = braceOpen; i < src.length; i++) {
    const c = src[i];
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return src.slice(start, i + 1); }
  }
  throw new Error(`unbalanced braces in ${name}`);
}

let memberRow;
try {
  const ctx = vm.createContext({ TUNIT: 'C' });
  // memberRow now also calls modeCap/fmtTemp/unitSym (+ isF/cToF that fmtTemp
  // uses). Slice them all out of the inline script so the test drives the real
  // render path, not a stub.
  const helpers = ['gEsc', 'isF', 'cToF', 'unitSym', 'fmtTemp', 'modeCap', 'memberRow'];
  vm.runInContext(helpers.map(h => extractFn(script, h)).join('\n'), ctx);
  // Expose for calling.
  memberRow = vm.runInContext('memberRow', ctx);
} catch (e) {
  console.error('FAIL: could not load render helpers: ' + e.message);
  process.exit(1);
}

// Names crafted to break naive string interpolation.
const HOSTILE = [
  "Eric's Room",
  "Kids' Room",
  'A"B quote',
  "'); alert(1);//",
  'back\\slash',
  '<script>x</script>',
  'both\'"kinds',
];

let failures = 0;
function fail(msg) { console.error('  FAIL: ' + msg); failures++; }

for (const name of HOSTILE) {
  // isPeer=true renders the ✕ remove button; isPeer=false renders Leave.
  for (const isPeer of [true, false]) {
    let out;
    try {
      out = memberRow({
        name, mode: 'HEAT',
        tag: isPeer ? '' : 'you', isLock: false, uid: 'abcd', isPeer,
        room: 21.5, target: 22, url: isPeer ? 'http://192.168.1.9/' : null,
      });
    } catch (e) {
      fail(`memberRow threw for name=${JSON.stringify(name)} isPeer=${isPeer}: ${e.message}`);
      continue;
    }

    // (1) Every inline event-handler attribute value must be valid JS. This is
    //     the exact property the apostrophe bug violated.
    const handlerRe = /\son[a-z]+\s*=\s*("([^"]*)"|'([^']*)')/gi;
    let h;
    while ((h = handlerRe.exec(out)) !== null) {
      const code = h[2] !== undefined ? h[2] : h[3];
      try { new vm.Script(code); }
      catch (e) {
        fail(`inline handler is broken JS for name=${JSON.stringify(name)}: <${code}> (${e.message})`);
      }
    }

    // (2) The buttons must use the delegated data-* hooks, not interpolated
    //     inline handlers — so a quote in a name can never reach a JS context.
    if (/\sonclick\s*=/i.test(out)) {
      fail(`member button uses inline onclick (data can break it) for name=${JSON.stringify(name)}`);
    }
    const wantAct = isPeer ? 'data-gact="remove"' : 'data-gact="leave"';
    if (!out.includes(wantAct)) {
      fail(`missing ${wantAct} for name=${JSON.stringify(name)} isPeer=${isPeer}`);
    }
  }
}

if (failures > 0) {
  console.error(`member-row render check FAILED: ${failures} problem(s)`);
  process.exit(1);
}
console.log(`member-row render check passed: ${HOSTILE.length} hostile name(s) render safely`);

// Tandem line + per-head turn badges on the Zones tab.
{
  const ctx = vm.createContext({});
  vm.runInContext('function i18n(s,p){ return p ? s.replace(/\\{(\\w+)\\}/g,(m,k)=>p[k]) : s; }\n' +
    ['tandemNow', 'zoneBadge'].map(h => extractFn(script, h)).join('\n'), ctx);
  const tandemNow = vm.runInContext('tandemNow', ctx);
  const zoneBadge = vm.runInContext('zoneBadge', ctx);
  const head = (uid, mode, x = {}) => ({ uid, known: true, mode, ...x });
  const badge = (sl, h, unit = {}) => { const b = zoneBadge({ slice: sl, unit }, h); return b ? b.t + (b.s ? ' ' + b.s : '') : null; };
  const base = { enabled: true, active: true, max_slice_s: 1800, min_slice_s: 600 };
  const heatRun = { ...base, phase: 'run', mode: 'heat', elapsed_s: 720, members: [
    { uid: 'a', wants: 'heat', directive: 'run' }, { uid: 'b', wants: 'cool', need: 'yes', directive: 'hold' },
    { uid: 'c', wants: 'none', directive: 'free' }] };
  const expect = (got, want, what) => { if (JSON.stringify(got) !== JSON.stringify(want)) fail(`${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); };
  const waitAs = x => ({ ...heatRun, members: [heatRun.members[0], { ...heatRun.members[1], ...x }] });
  expect(tandemNow({ enabled: false }), null, 'Tandem off hides the line');
  // 30 min max − 12 min elapsed → the switch comes within 18 min.
  expect(tandemNow(heatRun), { s: 'heating', next: 'cooling within 18 min' }, 'countdown while a room needs its turn');
  expect(tandemNow({ ...heatRun, elapsed_s: 1799 }), { s: 'heating', next: 'cooling within 1 min' }, 'countdown rounds up');
  expect(tandemNow({ ...heatRun, elapsed_s: 1900 }), { s: 'heating', next: 'cooling next' }, 'overdue: no fake 1 min');
  expect(tandemNow(heatRun, { defrost: true }), { s: 'heating' }, 'no countdown in defrost');
  expect(tandemNow({ ...heatRun, members: [heatRun.members[0]] }), { s: 'heating' }, 'no countdown when nobody waits');
  expect(tandemNow(waitAs({ need: 'no' })), { s: 'heating' }, 'no countdown when the waiting room has no demand');
  expect(tandemNow(waitAs({ need: 'checking' })), { s: 'heating' }, 'unknown need is not demand');
  expect(tandemNow(waitAs({ need: 'checking', checking: true })), { s: 'heating', next: 'checking rooms that want cooling…' }, 'air check running');
  expect(tandemNow({ ...waitAs({ need: 'no' }), members: [{ ...heatRun.members[0], satisfied: true }, { ...heatRun.members[1], need: 'no' }] }),
    { s: 'heating', next: 'rooms at setpoint' }, 'turn rooms parked');
  const idle = { ...base, phase: 'idle', mode: 'cool', members: [{ uid: 'a', wants: 'heat', need: 'no', directive: 'hold' }] };
  expect(tandemNow(idle), { s: 'idle', next: 'waiting for demand', tip: 'No room needs heat or cool right now' }, 'idle');
  expect(tandemNow({ ...idle, members: [{ ...idle.members[0], need: 'checking', checking: true }] }).next, 'checking rooms…', 'idle air check');
  const change = { ...base, phase: 'changeover', mode: 'cool', elapsed_s: 30, members: heatRun.members.map(m => ({ ...m, directive: m.wants === 'none' ? 'free' : 'hold' })) };
  expect(tandemNow(change).s, 'switching to cooling…', 'changeover line');

  // Zone badges. settle_s 180, decide_s 1800, 720 s into a heat turn.
  const run = { ...heatRun, settle_s: 180, decide_s: 1800, members: [
    { uid: 'a', wants: 'heat', need: 'yes', directive: 'run' },
    { uid: 'b', wants: 'cool', need: 'yes', directive: 'hold' },
    { uid: 'c', wants: 'cool', need: 'no', directive: 'hold' },
    { uid: 'd', wants: 'cool', need: 'checking', checking: true, directive: 'hold' },
    { uid: 'g', wants: 'cool', need: 'checking', directive: 'hold' },
    { uid: 'e', wants: 'heat', need: 'yes', directive: 'run', satisfied: true },
    { uid: 'f', wants: 'heat', need: 'no', directive: 'run' }] };
  expect(badge(run, head('a', 'HEAT', { operating: true })), 'Heating', 'running room');
  expect(badge(run, head('a', 'HEAT')), null, 'on its turn but cycled off: no badge');
  expect(badge({ ...run, elapsed_s: 60 }, head('a', 'HEAT')), 'Starting…', 'turn just began');
  // 1800 − 720 + 180 = 1260 s → ~21 min.
  expect(badge(run, head('b', 'COOL')), 'Up next ~21 min', 'held room with demand is up next');
  expect(badge(run, head('c', 'COOL')), 'No demand', 'held room without demand is not up next');
  expect(badge(run, head('d', 'COOL')), 'Checking room', 'held room being air-checked');
  expect(badge(run, head('g', 'COOL')), 'Waiting', 'held room not checked yet');
  expect(badge({ ...run, elapsed_s: 2100 }, head('b', 'COOL')), 'Up next', 'overdue: no fake ETA');
  expect(badge(run, head('b', 'COOL'), { defrost: true }), 'Up next', 'defrost: no ETA');
  expect(badge({ ...run, phase: 'idle' }, head('c', 'COOL')), 'No demand', 'idle: no demand');
  expect(badge(run, head('e', 'HEAT')), 'Parked', 'satisfied room parks');
  expect(badge({ ...run, elapsed_s: 60 }, head('f', 'HEAT')), 'No demand', 'running mode but at temperature: never Starting');
  expect(badge(run, head('a', 'HEAT', { subMode: 'DEFROST' }), { defrost: true }), 'Defrost', 'defrost');
  expect(badge(run, head('a', 'HEAT', { subMode: 'PREHEAT' })), 'Starting…', 'preheat');
  expect(badge(run, head('a', 'OFF')), null, 'off room: no badge');
  expect(badge(run, { uid: 'z', known: false, offline: true }), 'Offline', 'offline peer');
  const chg = { ...change, settle_s: 180, members: [
    { uid: 'b', wants: 'cool', need: 'yes', directive: 'hold' },
    { uid: 'c', wants: 'cool', need: 'no', directive: 'hold' }] };
  expect(badge(chg, head('b', 'COOL')), 'Starting…', 'changeover: incoming room with demand starts');
  expect(badge(chg, head('c', 'COOL')), 'No demand', 'changeover: incoming room without demand');
  expect(badge({ members: [{ uid: 'a', satisfied: true }] }, head('a', 'HEAT')), 'Parked', 'Tandem off: parked');
  expect(badge({}, head('a', 'COOL', { standby: true })), 'Blocked', 'Tandem off: blocked by the other mode');
  if (failures > 0) { console.error(`tandem status check FAILED: ${failures} problem(s)`); process.exit(1); }
  console.log('tandem status check passed: countdown + zone badges');
}
