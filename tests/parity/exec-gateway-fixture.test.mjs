/**
 * Where a ck-exec connect token may go is decided by the same cases in both SDKs: exec_test runs
 * `tools/parity/fixtures/exec-gateway-cases.json` against `execGatewayRefusal`, and CrowdyJS runs
 * its `test/unit/fixtures/exec-gateway-cases.json` against its own. The copy here must be the
 * pinned commit's, byte for byte, or the two could disagree and both stay green.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('the exec gateway cases are the pinned CrowdyJS commit\'s', () => {
  const ours = readFileSync(join(repo, 'tools', 'parity', 'fixtures', 'exec-gateway-cases.json'), 'utf8');
  const theirs = readFileSync(join(crowdyjs, 'test', 'unit', 'fixtures', 'exec-gateway-cases.json'), 'utf8');
  assert.equal(ours, theirs);
});
