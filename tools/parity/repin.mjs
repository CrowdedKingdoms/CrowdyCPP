#!/usr/bin/env node
/**
 * Move the CrowdyJS parity pin to a new commit and bring every derived
 * artifact along with it.
 *
 * Re-pinning by hand means editing package.json, running five fixture
 * generators with the right flags, restamping the two Studio-state fixtures,
 * and regenerating the matrix with the same gate mode CI checks. Missing any
 * one of those surfaces later as a confusing parity failure rather than as
 * "you forgot a step", so this does the whole set.
 *
 * Usage:
 *   node tools/parity/repin.mjs --crowdyjs <checkout> [--commit <sha>] [--check]
 *
 * The checkout must be built (npm ci && npm run build) — the generators import
 * its dist/. --commit defaults to the checkout's HEAD; pass it explicitly when
 * pinning something other than what is checked out. --check reports what would
 * change without writing.
 *
 * Afterwards run `npm run check:release`, and reconfigure any existing CMake
 * build dir so the embedded fixture headers pick up the new content.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjs = resolve(options.crowdyjs);

const version = JSON.parse(
  readFileSync(join(crowdyjs, 'package.json'), 'utf8'),
).version;
const commit =
  options.commit ??
  execFileSync('git', ['-C', crowdyjs, 'rev-parse', 'HEAD'], {
    encoding: 'utf8',
  }).trim();

// A dirty checkout would bake uncommitted CrowdyJS behavior into fixtures that
// claim to describe `commit`, which is exactly the drift the pin exists to stop.
const dirty = execFileSync('git', ['-C', crowdyjs, 'status', '--porcelain'], {
  encoding: 'utf8',
}).trim();
if (dirty) {
  throw new Error(
    `CrowdyJS checkout has uncommitted changes; fixtures generated from it ` +
      `would not describe ${commit.slice(0, 10)}:\n${dirty}`,
  );
}

const packagePath = join(root, 'package.json');
const packageRaw = readFileSync(packagePath, 'utf8');
const pkg = JSON.parse(packageRaw);
const previous = pkg.crowdyjsParityTarget;
console.log(`pin ${previous.version}@${previous.commit.slice(0, 10)} -> ${version}@${commit.slice(0, 10)}`);

if (options.check) {
  if (previous.version === version && previous.commit === commit) {
    console.log('pin is already current');
    process.exit(0);
  }
  console.log('pin would move; rerun without --check to apply');
  process.exit(1);
}

pkg.crowdyjsParityTarget = { version, commit };
writeFileSync(packagePath, `${JSON.stringify(pkg, null, 2)}\n`);

// Regenerate everything the pin feeds. Order matters only in that the matrix
// is written last, so it reports against the refreshed fixtures.
const generators = [
  'agent-fixtures.mjs',
  'control-gate-fixtures.mjs',
  'studio-host-fixtures.mjs',
  'layout-fixtures.mjs',
  'studio-state-fixtures.mjs',
];
for (const generator of generators) {
  run(['tools/parity/' + generator, '--crowdyjs', crowdyjs, '--write']);
}

// --strict must match how CI checks it: the matrix records its own gate mode,
// so a matrix written without --strict never satisfies a --strict check.
run([
  'tools/parity/parity.mjs',
  '--crowdyjs',
  crowdyjs,
  '--strict',
  '--write',
  'docs/parity-matrix.md',
]);

console.log(
  '\nre-pinned. Remaining steps this cannot do for you:\n' +
    '  1. Tests and docs that assert the pin literally:\n' +
    "     rg -l '" +
    previous.commit.slice(0, 10) +
    "' tests docs\n" +
    '  2. npm run check:release\n' +
    '  3. Reconfigure existing CMake build dirs (embedded fixture headers).\n' +
    '  4. The blueprint structural gate (see README.md).',
);

function run(args) {
  process.stdout.write(`  ${args[0].replace('tools/parity/', '')} ... `);
  execFileSync(process.execPath, args, { cwd: root, stdio: 'pipe' });
  console.log('ok');
}

function parseArgs(raw) {
  const parsed = { crowdyjs: null, commit: null, check: false };
  for (let index = 0; index < raw.length; index++) {
    const argument = raw[index];
    if (argument === '--check') {
      parsed.check = true;
    } else if (argument === '--crowdyjs') {
      parsed.crowdyjs = raw[++index];
    } else if (argument === '--commit') {
      parsed.commit = raw[++index];
    } else {
      throw new Error(`unknown argument: ${argument}`);
    }
  }
  if (!parsed.crowdyjs) {
    throw new Error(
      'missing --crowdyjs <checkout>: a built CrowdyJS checkout to pin against',
    );
  }
  return parsed;
}
