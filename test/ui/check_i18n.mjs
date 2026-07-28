// Host test for the web UI localization (i18n) framework.
//
// WHY THIS EXISTS
// ---------------
// The UI ships an English-keyed translation dictionary (const I18N = { fr:{…} })
// plus an i18n() helper and an applyI18n() DOM pass. Two classes of regression
// are easy to introduce and invisible until a French user loads the page:
//   1. a translation whose {placeholder} names don't match the English key's
//      (e.g. "Applied {n} dBm" mistranslated with "{nb}") — the value would
//      render a literal "{n}" or drop the number,
//   2. an empty / non-string translation, or an accidentally gutted table.
// It also pins the load-bearing wiring (i18n fallback, interpolation, the
// language <select>, and the /api/device round-trip) so the framework can't be
// half-removed without a red test.
//
// Zero deps: Node's vm + regex extraction of the inline literal/function.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');

let failures = 0;
const fail = (m) => { console.error('FAIL: ' + m); failures++; };
const ok = (m) => console.log('  ok: ' + m);

// ── 1. Extract and evaluate the I18N literal + i18n() helper ──────────────
const dictM = /const I18N = (\{[\s\S]*?\n\};)/.exec(html);
if (!dictM) { console.error('FAIL: could not locate `const I18N = { … };`'); process.exit(1); }
const fnM = /function i18n\(s, params\)\{[\s\S]*?\n\}/.exec(html);
if (!fnM) { console.error('FAIL: could not locate `function i18n(s, params){ … }`'); process.exit(1); }

const ctx = { result: null };
vm.createContext(ctx);
try {
  vm.runInContext(
    'const I18N = (' + dictM[1].replace(/;\s*$/, '') + ');\n' +
    'let LANG = "en";\n' +
    fnM[0] + '\n' +
    'result = { I18N, ' +
    '  enFallback: i18n("Settings"),' +               // no `en` table → identity
    '  missing: i18n("___no_such_key___"),' +          // unknown → identity
    '  frBefore: (LANG = "fr", i18n("Settings")),' +   // fr lookup
    '  interp: i18n("Applied {n} dBm", {n: 11}),' +     // placeholder fill
    '};',
    ctx
  );
} catch (e) {
  console.error('FAIL: I18N/i18n did not evaluate: ' + e.message);
  process.exit(1);
}

const { I18N, enFallback, missing, frBefore, interp } = ctx.result;

// ── 2. i18n() semantics ───────────────────────────────────────────────────
if (enFallback === 'Settings') ok('English (no table) falls back to the source string');
else fail('English fallback expected "Settings", got "' + enFallback + '"');

if (missing === '___no_such_key___') ok('unknown key falls back to the source string');
else fail('unknown-key fallback broken, got "' + missing + '"');

if (frBefore === 'Réglages') ok('fr lookup: "Settings" -> "Réglages"');
else fail('fr lookup expected "Réglages", got "' + frBefore + '"');

if (interp === '11 dBm appliqué') ok('placeholder interpolation fills {n}');
else fail('interpolation expected "11 dBm appliqué", got "' + interp + '"');

// ── 3. Dictionary integrity ────────────────────────────────────────────────
const fr = I18N.fr;
if (!fr || typeof fr !== 'object') { fail('I18N.fr missing or not an object'); }
else {
  const keys = Object.keys(fr);
  if (keys.length >= 50) ok(`fr table has ${keys.length} entries`);
  else fail(`fr table unexpectedly small (${keys.length} entries) — was it gutted?`);

  const params = (s) => (s.match(/\{(\w+)\}/g) || []).sort().join(',');
  let badVal = 0, badParam = 0;
  for (const k of keys) {
    const v = fr[k];
    if (typeof v !== 'string' || v.trim() === '') { badVal++; fail(`empty/non-string translation for "${k}"`); continue; }
    // Every {placeholder} in the translation must exist in the English key.
    const kp = params(k), vp = params(v);
    if (vp && vp !== kp) { badParam++; fail(`placeholder mismatch for "${k}": key[${kp}] vs value[${vp}]`); }
  }
  if (!badVal) ok('every fr value is a non-empty string');
  if (!badParam) ok('every fr value only uses placeholders present in its key');

  // A few load-bearing keys must be present (catches an accidental rename).
  for (const k of ['Climate', 'Status', 'Settings', 'Sign in', 'Save failed', 'Disconnected']) {
    if (!(k in fr)) fail(`load-bearing key missing from fr table: "${k}"`);
  }
  ok('load-bearing keys present');
}

// ── 4. Wiring is present (framework not half-removed) ──────────────────────
for (const [needle, what] of [
  ['function applyI18n', 'applyI18n() DOM pass'],
  ['function setLang', 'setLang() resolver'],
  ['id="uiLang"', 'language <select>'],
  ['function saveLang', 'saveLang() persister'],
  ['{lang:v}', 'device POST sends {lang}'],
  ["setLang('')", 'startup seeds navigator language'],
]) {
  if (html.includes(needle)) ok(`present: ${what}`);
  else fail(`missing wiring: ${what} (expected to find \`${needle}\`)`);
}

if (failures) { console.error(`\ncheck_i18n: ${failures} failure(s)`); process.exit(1); }
console.log('\ncheck_i18n: all checks passed');
