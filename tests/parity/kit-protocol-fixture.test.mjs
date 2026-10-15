/**
 * The game kit protocol is held to one record in every SDK: kit_runtime_test replays
 * `tools/parity/fixtures/kit-protocol.json` (ck-exec's `crates/ckx-kit-protocol/kit-protocol.json`)
 * against crowdy/kit, and CrowdyJS replays its `test/unit/fixtures/kit-protocol.json` against
 * src/kit. The copy here must be the pinned commit's, byte for byte, or the two could disagree and
 * both stay green.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('the kit protocol record is the pinned CrowdyJS commit\'s', () => {
  const ours = readFileSync(join(repo, 'tools', 'parity', 'fixtures', 'kit-protocol.json'), 'utf8');
  const theirs = readFileSync(join(crowdyjs, 'test', 'unit', 'fixtures', 'kit-protocol.json'), 'utf8');
  assert.equal(ours, theirs);
});
