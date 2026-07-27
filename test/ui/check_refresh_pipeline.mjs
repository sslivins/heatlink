// End-to-end smoke test for the web UI's refresh() poll+render pipeline.
//
// WHY THIS EXISTS
// ---------------
// refresh() used to wrap the ENTIRE /api/status + /api/settings poll AND all
// rendering in a single try/catch whose catch flipped the WiFi badge red and set
// the subtitle to "Disconnected". Any client-side JS bug in the render path (e.g.
// a deleted `const MODE_COLOR = {...}` -> ReferenceError) therefore masqueraded
// as a connectivity loss: the device was perfectly reachable, /api/status
// returned 200, yet the whole UI screamed "Disconnected / WiFi red / blank
// sensors". This shipped for real twice (v0.2.96 and again during the TX-power
// change).
//
// check_render_runtime.mjs proves the render functions don't throw, but it uses a
// permissive Proxy DOM that ABSORBS every write, so it cannot observe STATE and
// cannot tell a render failure apart from a transport failure. This test closes
// that gap. It drives the REAL refresh()/saveTxPow()/syncTxSetting() with:
//   * a strict, state-observable DOM (real nodes keyed by the actual id="..."
//     attributes in index.html; unknown ids return null, exactly like a browser),
//   * a strict URL+method fetch mock that records calls and can fail on demand,
//   * a fresh vm context per stateful case (so module-level flags like txSynced
//     never leak between cases).
//
// The load-bearing assertions:
//   1. a RENDER exception must NOT set the WiFi badge red / "Disconnected"
//      (it must console.error instead and stay "connected"),
//   2. a genuine TRANSPORT failure (fetch reject on /api/status) MUST show
//      "Disconnected",
//   3. a healthy poll paints the expected observable state.
//
// No external deps: Node's vm + a hand-rolled DOM. Runs in plain CI Node.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');

const scriptM = /<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/i.exec(html);
if (!scriptM) { console.error('FAIL: no inline <script> found'); process.exit(1); }
const script = scriptM[1];

// Every id the browser would actually have. getElementById returns null for
// anything outside this set — so a render that references a non-existent id
// throws (real bug) instead of being silently swallowed.
const knownIds = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));

// ── Strict, observable DOM node ───────────────────────────────────────────
function makeNode(id, doc, poison) {
  const classes = new Set();
  const attrs = Object.create(null);
  const style = new Proxy({ __props: Object.create(null) }, {
    get(t, p) {
      if (p === 'setProperty') return (k, v) => { t.__props[k] = String(v); };
      if (p === 'getPropertyValue') return (k) => (k in t.__props ? t.__props[k] : '');
      if (p === 'removeProperty') return (k) => { delete t.__props[k]; };
      if (p === '__props') return t.__props;
      return p in t.__props ? t.__props[p] : '';
    },
    set(t, p, v) { t.__props[p] = String(v); return true; },
  });
  const node = {
    id, tagName: 'DIV', nodeType: 1,
    _text: '', _html: '', value: '', checked: false, disabled: false, min: '', max: '',
    style, children: [], options: [], dataset: Object.create(null),
    setAttribute(k, v) { attrs[k] = String(v); if (k === 'class') node.className = String(v); },
    getAttribute(k) { return k in attrs ? attrs[k] : null; },
    removeAttribute(k) { delete attrs[k]; },
    hasAttribute(k) { return k in attrs; },
    addEventListener() {}, removeEventListener() {},
    focus() { doc.activeElement = node; }, blur() {},
    scrollIntoView() {}, setPointerCapture() {}, releasePointerCapture() {},
    appendChild(c) { node.children.push(c); return c; },
    removeChild(c) { const i = node.children.indexOf(c); if (i >= 0) node.children.splice(i, 1); return c; },
    insertBefore(c) { node.children.unshift(c); return c; },
    replaceChildren() { node.children.length = 0; },
    remove() {},
    cloneNode() { return makeNode(id + '#clone', doc); },
    querySelector() { return makeNode('qs', doc); },
    querySelectorAll() { return []; },
    getBoundingClientRect() { return { top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; },
    contains() { return false; },
    getContext() { return null; },
  };
  Object.defineProperty(node, 'textContent', {
    configurable: true,
    get() { return node._text; },
    set(v) { if (poison === 'textContent') throw new Error('poisoned textContent on #' + id); node._text = String(v); },
  });
  Object.defineProperty(node, 'innerHTML', {
    configurable: true,
    get() { return node._html; },
    set(v) { if (poison === 'innerHTML') throw new Error('poisoned innerHTML on #' + id); node._html = String(v); },
  });
  Object.defineProperty(node, 'className', {
    configurable: true,
    get() { return [...classes].join(' '); },
    set(v) { classes.clear(); String(v).split(/\s+/).filter(Boolean).forEach((c) => classes.add(c)); },
  });
  node.classList = {
    add(...c) { c.forEach((x) => classes.add(x)); },
    remove(...c) { c.forEach((x) => classes.delete(x)); },
    toggle(c, f) { const want = f === undefined ? !classes.has(c) : !!f; if (want) classes.add(c); else classes.delete(c); return want; },
    contains(c) { return classes.has(c); },
  };
  return node;
}

function makeDoc(poison) {
  const cache = Object.create(null);
  const doc = {
    hidden: false, cookie: '', title: '', referrer: '',
    getElementById(id) {
      if (!knownIds.has(id)) return null;
      if (!cache[id]) cache[id] = makeNode(id, doc, poison && poison.id === id ? poison.prop : null);
      return cache[id];
    },
    createElement() { return makeNode('el', doc); },
    createElementNS() { return makeNode('el', doc); },
    querySelector() { return makeNode('qs', doc); },
    querySelectorAll() { return []; },
    addEventListener() {}, removeEventListener() {},
  };
  doc.documentElement = makeNode('html', doc);
  doc.body = makeNode('body', doc);
  doc.head = makeNode('head', doc);
  doc.activeElement = makeNode('active-default', doc);
  return doc;
}

// ── Fetch helpers ─────────────────────────────────────────────────────────
function jsonResp(obj, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => obj, text: async () => JSON.stringify(obj) };
}
// Bootstrap: make loadAuth() return false (web auth on, not authenticated) so the
// startup IIFE short-circuits BEFORE pollOnce() — each case then drives refresh()
// itself against a controlled fetch handler with a clean slate.
async function bootstrapFetch(url) {
  if (url.startsWith('/api/auth')) return jsonResp({ web_auth_enabled: true, authenticated: false, hostname: 'boot' });
  return jsonResp({});
}

// ── Context factory: a fresh script eval per stateful case ────────────────
function loadScript({ poison } = {}) {
  const doc = makeDoc(poison);
  const calls = [];
  const errors = [];
  let handler = bootstrapFetch;
  const fetch = async (url, opts = {}) => {
    calls.push({ url, method: (opts.method || 'GET').toUpperCase(), body: opts.body });
    return handler(url, opts);
  };
  const memStore = () => ({ getItem: () => null, setItem() {}, removeItem() {}, clear() {} });
  const sandbox = {
    document: doc,
    navigator: { userAgent: 'node', onLine: true, serviceWorker: undefined },
    location: { href: 'http://device/', pathname: '/', reload() {}, replace() {} },
    history: { pushState() {}, replaceState() {} },
    localStorage: memStore(), sessionStorage: memStore(),
    getComputedStyle: () => ({ getPropertyValue: () => '' }),
    matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }),
    fetch, AbortController,
    setTimeout: () => 0, clearTimeout: () => {}, setInterval: () => 0, clearInterval: () => {},
    requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
    addEventListener() {}, removeEventListener() {},
    alert() {}, confirm() { return true; }, prompt() { return null; },
    console: { log() {}, warn() {}, info() {}, debug() {}, error(...a) { errors.push(a.map(String).join(' ')); } },
    URL, URLSearchParams,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.self = sandbox;

  const harness = script + `
;globalThis.__probe = {
  refresh, saveTxPow, syncTxSetting,
  setClimate: (o) => Object.assign(climate, o),
  getTxSynced: () => txSynced,
};`;
  const ctx = vm.createContext(sandbox);
  vm.runInContext(harness, ctx, { filename: 'index.html#inline' });

  return {
    doc, calls, errors, probe: sandbox.__probe,
    setFetch: (h) => { handler = h; },
    reset: () => { calls.length = 0; errors.length = 0; },
    el: (id) => doc.getElementById(id),
  };
}

const flush = async (n = 4) => { for (let i = 0; i < n; i++) await new Promise((r) => setImmediate(r)); };

// ── Fixtures ──────────────────────────────────────────────────────────────
const goodStatus = {
  unit_connected: true, mqtt_connected: true, mqtt_configured: true,
  hostname: 'heatlink-test', ip: '1.2.3.4', name: 'Master', version: '1.0.8',
  rssi: -62, ssid: "y'all 2.4ghz",
  tx_power_dbm: 20, tx_power_min_dbm: 2, tx_power_max_dbm: 20,
  uptime_s: 3600, free_heap: 120000,
  power: { present: false },
  diag: { vin_min_mv: 4200, vin_sag_count: 0, reset_reason: 'poweron', last_was_brownout: false, brownout_count: 0, wifi_drop_count: 0 },
};
const goodSettings = {
  temp_unit: 'C', power: 'ON', mode: 'COOL', temperature: 22, roomTemperature: 20.5,
  operating: true, fan: 'AUTO', vane: 'AUTO', wideVane: '|', connected: true,
  compressorFrequency: 30, outsideTemp: 18, inputPowerW: 800, energyKwh: 12.3,
  runtimeHours: 100, targetHumidity: 50, subMode: 'NORMAL', stage: '2', errorCode: null,
  caps: { wideVane: { detected: 'unsupported', override: 'auto', detecting: false, show: false } },
};
const goodStatusSettingsFetch = async (url) => {
  if (url.startsWith('/api/status')) return jsonResp(goodStatus);
  if (url.startsWith('/api/settings')) return jsonResp(goodSettings);
  throw new Error('unexpected fetch ' + url);
};

// ── Test harness ──────────────────────────────────────────────────────────
let failures = 0;
let current = '';
function ok(cond, msg) {
  if (!cond) { failures++; console.error(`  FAIL [${current}]: ${msg}`); }
}
function eq(actual, expected, msg) {
  ok(actual === expected, `${msg} (got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)})`);
}
async function testCase(name, fn) {
  current = name;
  try { await fn(); }
  catch (e) {
    failures++;
    console.error(`  FAIL [${name}]: threw ${e.name}: ${e.message}`);
    console.error('        ' + (e.stack || '').split('\n')[1]?.trim());
  }
}

// 1) Healthy poll paints the expected observable state.
await testCase('healthy refresh paints state', async () => {
  const t = loadScript(); await flush(); t.reset();
  t.setFetch(goodStatusSettingsFetch);
  await t.probe.refresh();
  eq(t.el('bWifi').className, 'badge on', 'WiFi badge is connected');
  eq(t.el('bUnit').className, 'badge on', 'unit badge connected');
  eq(t.el('bMqtt').className, 'badge on', 'mqtt badge connected');
  eq(t.el('subtitle').textContent, 'heatlink-test · 1.2.3.4', 'subtitle shows host/ip');
  eq(t.el('wsTx').textContent, '20 dBm', 'TX power stat rendered');
  eq(String(t.el('txPow').value), '20', 'TX slider seeded to current');
  eq(t.el('txPowVal').textContent, '20 dBm', 'TX slider label seeded');
  eq(t.probe.getTxSynced(), true, 'txSynced latched after first status');
  eq(t.el('dgRoom').textContent, '20.5 °C', 'room temp rendered from settings');
  eq(t.errors.length, 0, 'no console.error on a healthy poll');
  ok(t.calls.some((c) => c.url.startsWith('/api/status') && c.method === 'GET'), 'fetched /api/status');
  ok(t.calls.some((c) => c.url.startsWith('/api/settings') && c.method === 'GET'), 'fetched /api/settings');
});

// 2) THE load-bearing case: a render exception must NOT masquerade as offline.
await testCase('render error does NOT disconnect', async () => {
  const t = loadScript({ poison: { id: 'uptime', prop: 'textContent' } }); await flush(); t.reset();
  t.setFetch(goodStatusSettingsFetch);
  await t.probe.refresh();
  eq(t.el('bWifi').className, 'badge on', 'WiFi stays connected despite render throw');
  ok(t.el('subtitle').textContent !== 'Disconnected', 'subtitle is NOT "Disconnected"');
  eq(t.el('subtitle').textContent, 'heatlink-test · 1.2.3.4', 'subtitle still shows host/ip');
  ok(t.errors.some((e) => e.includes('status render failed')), 'render failure was logged via console.error');
});

// 3) A genuine transport failure MUST show Disconnected (and skip settings).
await testCase('transport failure DOES disconnect', async () => {
  const t = loadScript(); await flush(); t.reset();
  t.setFetch(async (url) => {
    if (url.startsWith('/api/status')) throw new Error('network down');
    return jsonResp({});
  });
  await t.probe.refresh();
  eq(t.el('bWifi').className, 'badge off', 'WiFi badge red on real transport failure');
  eq(t.el('subtitle').textContent, 'Disconnected', 'subtitle shows Disconnected');
  ok(!t.calls.some((c) => c.url.startsWith('/api/settings')), 'settings not fetched after transport failure');
});

// 4) A 401 re-shows the login gate and does not fall through to render/settings.
await testCase('401 re-shows login gate', async () => {
  const t = loadScript(); await flush();
  // Precondition: startup (web auth on, unauthenticated) already left the gate visible.
  eq(t.el('loginGate').classList.contains('hidden'), false, 'login gate visible at start');
  t.el('loginGate').classList.add('hidden'); // hide it, then prove refresh() re-shows it
  t.reset();
  t.setFetch(async (url) => (url.startsWith('/api/status') ? jsonResp({}, 401) : jsonResp({})));
  await t.probe.refresh();
  eq(t.el('loginGate').classList.contains('hidden'), false, 'login gate re-shown on 401');
  ok(!t.calls.some((c) => c.url.startsWith('/api/settings')), 'settings not fetched on 401');
});

// 5) saveTxPow posts the right body, applies the echoed value, shows success.
await testCase('saveTxPow success posts {tx_power} and applies echo', async () => {
  const t = loadScript(); await flush(); t.reset();
  t.el('txPow').value = '11';
  let body = null;
  t.setFetch(async (url, opts) => {
    if (url === '/api/device' && (opts.method || '').toUpperCase() === 'POST') {
      body = JSON.parse(opts.body);
      return jsonResp({ tx_power_dbm: 11 });
    }
    throw new Error('unexpected fetch ' + url);
  });
  await t.probe.saveTxPow();
  ok(body && body.tx_power === 11, 'POST body carried {tx_power:11}');
  eq(String(t.el('txPow').value), '11', 'slider set to echoed value');
  eq(t.el('txPowVal').textContent, '11 dBm', 'slider label set to echoed value');
  ok(t.el('txMsg').textContent.startsWith('Applied 11'), 'success message shown');
  ok(t.el('txMsg').className.includes('ok'), 'success message styled ok');
});

// 6) saveTxPow surfaces a non-2xx as a save failure.
await testCase('saveTxPow non-2xx shows failure', async () => {
  const t = loadScript(); await flush(); t.reset();
  t.setFetch(async () => jsonResp({}, 500));
  await t.probe.saveTxPow();
  eq(t.el('txMsg').textContent, 'Save failed', 'failure message shown');
  ok(t.el('txMsg').className.includes('err'), 'failure message styled err');
});

// 7) saveTxPow surfaces a network reject as a save failure.
await testCase('saveTxPow network reject shows failure', async () => {
  const t = loadScript(); await flush(); t.reset();
  t.setFetch(async () => { throw new Error('offline'); });
  await t.probe.saveTxPow();
  eq(t.el('txMsg').textContent, 'Save failed', 'failure message shown on reject');
});

// 8) syncTxSetting: bounds applied, first sync seeds once, later polls don't fight.
await testCase('syncTxSetting bounds + first-sync-once', async () => {
  const t = loadScript(); await flush();
  t.probe.syncTxSetting({ tx_power_dbm: 14, tx_power_min_dbm: 2, tx_power_max_dbm: 20 });
  eq(String(t.el('txPow').min), '2', 'slider min applied');
  eq(String(t.el('txPow').max), '20', 'slider max applied');
  eq(String(t.el('txPow').value), '14', 'slider seeded from first status');
  eq(t.el('txPowVal').textContent, '14 dBm', 'label seeded from first status');
  eq(t.probe.getTxSynced(), true, 'txSynced latched');
  // A later poll must NOT overwrite the (possibly user-adjusted) slider.
  t.probe.syncTxSetting({ tx_power_dbm: 8 });
  eq(String(t.el('txPow').value), '14', 'later poll does not re-seed the slider');
});

// 9) syncTxSetting must not fight the user while they drag the slider.
await testCase('syncTxSetting yields to active slider', async () => {
  const t = loadScript(); await flush();
  const sl = t.el('txPow');
  sl.focus(); // becomes document.activeElement
  t.probe.syncTxSetting({ tx_power_dbm: 9, tx_power_min_dbm: 2, tx_power_max_dbm: 20 });
  eq(String(sl.min), '2', 'bounds still applied while active');
  eq(String(sl.max), '20', 'bounds still applied while active');
  ok(String(sl.value) !== '9', 'value NOT overwritten while slider is focused');
  eq(t.probe.getTxSynced(), false, 'not marked synced while user is dragging');
});

// 10) syncTxSetting is a no-op (no throw, no latch) when tx fields are absent.
await testCase('syncTxSetting no-op on absent fields', async () => {
  const t = loadScript(); await flush();
  t.probe.syncTxSetting({});
  eq(t.probe.getTxSynced(), false, 'stays unsynced when no tx_power provided');
});

if (failures > 0) {
  console.error(`refresh-pipeline check FAILED: ${failures} assertion(s)`);
  process.exit(1);
}
console.log('refresh-pipeline check passed: transport vs render errors are classified correctly + TX settings behave');
