import assert from 'node:assert/strict';
import {
  existsSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = [
  process.env.CROWDYJS_PATH,
  join(repo, 'CrowdyJS'),
  join(repo, '..', 'CrowdyJS'),
].find((candidate) => candidate && existsSync(join(candidate, 'schema.gql')));

if (!crowdyjs) {
  throw new Error(
    'CrowdyJS checkout not found; set CROWDYJS_PATH for parity gate tests',
  );
}

test('reviewed parity baseline and generated matrix pass', () => {
  const result = runParity('--check', 'docs/parity-matrix.md');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /unclassified=0 stale=0/u);
});

test('strict mode rejects the visible portable gaps', () => {
  const result = runParity('--strict');
  assert.equal(result.status, 1, result.stderr || result.stdout);
  assert.match(result.stdout, /portable gaps=[1-9][0-9]*/u);
});

test('matrix drift is a gate failure', () => {
  const directory = mkdtempSync(join(tmpdir(), 'crowdycpp-parity-'));
  const stale = join(directory, 'parity-matrix.md');
  try {
    writeFileSync(stale, '# stale\n');
    const result = runParity('--check', stale);
    assert.equal(result.status, 1, result.stderr || result.stdout);
    assert.match(result.stderr, /is stale/u);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

function runParity(...extra) {
  return spawnSync(
    process.execPath,
    [
      'tools/parity/parity.mjs',
      '--crowdyjs',
      crowdyjs,
      ...extra,
    ],
    {
      cwd: repo,
      encoding: 'utf8',
    },
  );
}
