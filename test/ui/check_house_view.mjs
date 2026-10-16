// Runtime test for the House view (issue #131): drives the real renderHouse()
// from the inline script with representative /api/house payloads and checks the
// card visibility, outdoor-unit sectioning/order and per-head links.
//
// Zero deps: loads the inline <script> into a vm with a permissive fake DOM
// (same approach as check_render_runtime.mjs) but records innerHTML / hidden
// state per element id so the rendered output can be asserted.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');
const scriptM = /<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/i.exec(html);
if (!scriptM) { console.error('FAIL: no inline <script> found'); process.exit(1); }

const UNI = new Proxy(function () {}, {
  apply() { return UNI; },
  construct() { return UNI; },
  set() { return true; },
  has() { return true; },
  get(_t, prop) {
    switch (prop) {
      case 'classList': return { add() {}, remove() {}, toggle() {}, contains() { return false; } };
      case 'children':
      case 'options': return [];
      case 'length': return 0;
      case 'querySelectorAll': return () => [];
      case 'querySelector':
      case 'closest': return () => UNI;
      case 'then': return undefined;
      case Symbol.iterator: return function* () {};
      case Symbol.toPrimitive:
      case 'toString':
      case 'valueOf': return () => '';
      default: return UNI;
    }
  },
});

// Recorded elements for the ids the House view touches.
const els = {};
function recEl(id) {
  if (!els[id]) {
    const cls = new Set(['hidden']);
    els[id] = {
      innerHTML: '',
      classList: {
        add: (c) => cls.add(c), remove: (c) => cls.delete(c),
        toggle: (c, on) => (on === undefined ? (cls.has(c) ? cls.delete(c) : cls.add(c)) : on ? cls.add(c) : cls.delete(c)),
        contains: (c) => cls.has(c),
      },
      addEventListener() {},
      dataset: {},
      style: {},
    };
  }
  return els[id];
}
const tracked = new Set(['houseCard', 'hMembers']);

const documentStub = new Proxy(UNI, {
  get(_t, prop) {
    if (prop === 'getElementById') return (id) => (tracked.has(id) ? recEl(id) : UNI);
    if (prop === 'createElement' || prop === 'createElementNS' || prop === 'querySelector') return () => UNI;
    if (prop === 'querySelectorAll') return () => [];
    if (prop === 'addEventListener' || prop === 'removeEventListener') return () => {};
    if (prop === 'hidden') return false;
    if (prop === 'cookie') return '';
    return UNI;
  },
});
const fetchPayload = { enabled: false, authenticated: true, role: 'admin', temp_unit: 'C' };
const sandbox = {
  document: documentStub, window: UNI,
  navigator: { userAgent: 'node', onLine: true },
  location: { href: 'http://device/', pathname: '/', reload() {}, replace() {} },
  history: { pushState() {}, replaceState() {} },
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  getComputedStyle: () => ({ getPropertyValue: () => '' }),
  matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }),
  fetch: async () => ({ ok: true, status: 200, json: async () => fetchPayload, text: async () => '{}' }),
  AbortController,
  setTimeout: () => 0, clearTimeout: () => {}, setInterval: () => 0, clearInterval: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  console: { log() {}, warn() {}, error() {}, info() {}, debug() {} },
  URL, URLSearchParams,
};
sandbox.globalThis = sandbox;
sandbox.self = sandbox;
const ctx = vm.createContext(sandbox);
vm.runInContext(scriptM[1] + '\n;globalThis.__renderHouse = renderHouse;', ctx);
const renderHouse = sandbox.__renderHouse;

let failures = 0;
const check = (cond, msg) => { if (!cond) { failures++; console.error('  FAIL: ' + msg); } };
const card = () => recEl('houseCard');
const out = () => recEl('hMembers').innerHTML;
const order = (...xs) => {
  const h = out();
  const idx = xs.map((x) => h.indexOf(x));
  return idx.every((v) => v >= 0) && idx.every((v, i) => i === 0 || v > idx[i - 1]);
};

const self = { uid: 'e6c8', name: 'Master Bedroom', ghash: 'aaaa1111', glabel: 'Bedrooms',
  unit_connected: true, power: 'ON', mode: 'HEAT', roomTemp: 21, targetTemp: 22, ip: '192.168.1.108' };
const peer = (o) => Object.assign({ reachable: true, summary_supported: true, unit_connected: true,
  power: 'ON', mode: 'COOL', roomTemp: 24, targetTemp: 22 }, o);

// 1. Single group only → hidden.
renderHouse({ self, heads: [peer({ uid: 'b1', name: 'Twins Room', ghash: 'aaaa1111', ip: '10.0.0.2' })] });
check(card().classList.contains('hidden'), 'single-group install should hide the House card');

// 2. No other heads at all → hidden.
renderHouse({ self, heads: [] });
check(card().classList.contains('hidden'), 'single-head install should hide the House card');

// 3. Two outdoor units + a standalone + offline/old-firmware heads.
renderHouse({ self, heads: [
  peer({ uid: 'b1', name: 'Twins Room', ghash: 'aaaa1111', glabel: 'Bedrooms', ip: '10.0.0.2' }),
  peer({ uid: 'c1', name: 'Kitchen', ghash: 'bbbb2222', glabel: 'Main Floor', ip: '10.0.0.3' }),
  peer({ uid: 'c2', name: 'Den', ghash: 'bbbb2222', glabel: 'Main Floor', ip: '10.0.0.4', reachable: false }),
  peer({ uid: 'd1', name: 'Garage', ip: '10.0.0.5', summary_supported: false }),
] });
check(!card().classList.contains('hidden'), 'multi-group install should show the House card');
check(order('Bedrooms', 'Master Bedroom', 'Twins Room', 'Main Floor', 'Den', 'Kitchen', 'Standalone', 'Garage'),
  'sections should be: own outdoor unit (self first), other units, standalone last');
check(out().includes('data-gurl="http://10.0.0.3/"'), 'peer cards should link to the head');
check(!out().includes('data-gurl="http://192.168.1.108/"'), 'self card should not link to itself');
check(out().includes('Offline'), 'unreachable head should be tagged Offline');
check(out().includes('Update firmware'), 'head without summary should be tagged Update firmware');
check(!out().includes('data-gact'), 'House cards must not carry group control buttons');

// 4. Standalone self with another head → self in the Standalone section.
renderHouse({ self: { uid: 'e6c8', name: 'Solo', unit_connected: false },
  heads: [peer({ uid: 'c1', name: 'Kitchen', ghash: 'bbbb2222', glabel: 'Main Floor', ip: '10.0.0.3' })] });
check(!card().classList.contains('hidden'), 'standalone head with others should show the House card');
check(order('Standalone', 'Solo', 'Main Floor', 'Kitchen'), 'standalone self section first, then other units');

// 5. Names are escaped.
renderHouse({ self, heads: [peer({ uid: 'x', name: '<img src=x>', ghash: 'cccc', ip: '10.0.0.9' })] });
check(!out().includes('<img'), 'head names must be HTML-escaped');

if (failures) { console.error(`house-view check FAILED: ${failures} problem(s)`); process.exit(1); }
console.log('house-view check passed: visibility, sectioning, links and escaping');
