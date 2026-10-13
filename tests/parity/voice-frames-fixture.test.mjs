/**
 * The voice payload convention is held to the same cases in both SDKs: voice_frames_test replays
 * `tools/parity/fixtures/voice-frames.json` against crowdy/media/voice_frames.hpp, and CrowdyJS
 * replays its `test/unit/fixtures/voice-frames.json` against src/media/voice-frames.ts. The copy
 * here must be the pinned commit's, byte for byte, or the two could disagree and both stay green.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('the voice fixture is the pinned CrowdyJS commit\'s', () => {
  const ours = readFileSync(join(repo, 'tools', 'parity', 'fixtures', 'voice-frames.json'), 'utf8');
  const theirs = readFileSync(join(crowdyjs, 'test', 'unit', 'fixtures', 'voice-frames.json'), 'utf8');
  assert.equal(ours, theirs);
});
