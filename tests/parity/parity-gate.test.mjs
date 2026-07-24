import assert from 'node:assert/strict';
import {
  mkdtempSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('reviewed parity baseline and generated matrix pass', () => {
  const result = runParity('--check', 'docs/parity-matrix.md');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /unclassified=0 stale=0/u);
});

test('strict mode accepts the zero-gap implementation', () => {
  const result = runParity('--strict');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /portable gaps=0/u);
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

test('CrowdyJS resolver fails clearly when no checkout exists', () => {
  const directory = mkdtempSync(join(tmpdir(), 'crowdycpp-no-js-'));
  const configured = process.env.CROWDYJS_PATH;
  try {
    delete process.env.CROWDYJS_PATH;
    assert.throws(
      () => resolveCrowdyJsPath(directory),
      /Set CROWDYJS_PATH.*Checked:/u,
    );
  } finally {
    if (configured === undefined) delete process.env.CROWDYJS_PATH;
    else process.env.CROWDYJS_PATH = configured;
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
