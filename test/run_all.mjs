// Canonical aggregator for the Node-based host tests (web UI + API contract).
//
// Runs every test/ui/*.mjs and test/api/*.mjs check in its OWN child process so
// module-level state, an uncaught rejection, or a process.exit() in one test
// can never mask or contaminate another. Any non-zero child exit fails the whole
// run. This is the SAME entry point CI uses (see .github/workflows/host-tests.yml)
// so a local `node test/run_all.mjs` is a faithful preflight before OTA/release.
//
// It intentionally does NOT run the C++ host tests (those need g++); CI runs
// those in separate steps. This aggregates only the zero-dependency Node checks.

import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const testDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(testDir, '..');

const dirs = ['ui', 'api'];
const tests = [];
for (const d of dirs) {
  const abs = path.join(testDir, d);
  if (!fs.existsSync(abs)) continue;
  for (const f of fs.readdirSync(abs).sort()) {
    if (f.endsWith('.mjs')) tests.push(path.join('test', d, f));
  }
}

if (tests.length === 0) {
  console.error('run_all: no *.mjs tests found under test/ui or test/api');
  process.exit(1);
}

let failed = 0;
for (const rel of tests) {
  process.stdout.write(`\n── ${rel} ─────────────────────────────────────────\n`);
  const res = spawnSync(process.execPath, [rel], { cwd: root, stdio: 'inherit' });
  if (res.status !== 0) {
    failed++;
    console.error(`✗ ${rel} exited with code ${res.status ?? 'signal ' + res.signal}`);
  }
}

console.log('\n' + '─'.repeat(60));
if (failed > 0) {
  console.error(`run_all: ${failed}/${tests.length} test file(s) FAILED`);
  process.exit(1);
}
console.log(`run_all: all ${tests.length} test file(s) passed`);
