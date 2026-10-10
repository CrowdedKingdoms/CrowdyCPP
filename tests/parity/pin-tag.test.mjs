import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseTagRefs, pinTagVerdict } from '../../tools/parity/pin-tag.mjs';

const RELEASED = '7662d0b0' + 'a'.repeat(32);
const UNRELEASED = 'ca9fbb50' + 'b'.repeat(32);
const TAG_OBJECT = 'f00dfeed' + 'c'.repeat(32);

test('parses lightweight and annotated tags, the peeled commit winning', () => {
  const tags = parseTagRefs(
    [
      `${RELEASED}\trefs/tags/dev/v18.6.0`,
      `${TAG_OBJECT}\trefs/tags/test/v18.6.0`,
      `${RELEASED}\trefs/tags/test/v18.6.0^{}`,
    ].join('\n'),
  );
  assert.equal(tags.get('dev/v18.6.0'), RELEASED);
  assert.equal(tags.get('test/v18.6.0'), RELEASED);
});

test('a pin on a tagged release passes, whichever tier tagged it', () => {
  const tags = new Map([['dev/v18.6.0', RELEASED]]);
  const verdict = pinTagVerdict({ version: '18.6.0', commit: RELEASED, tags });
  assert.equal(verdict.ok, true);
  assert.match(verdict.message, /dev\/v18\.6\.0/);
});

test("refuses 0.59.0's pin: reachable, but not the commit 18.6.0 was released from", () => {
  const tags = new Map([['dev/v18.6.0', RELEASED]]);
  const verdict = pinTagVerdict({ version: '18.6.0', commit: UNRELEASED, tags });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /not a published release: dev\/v18\.6\.0 is 7662d0b0/);
});

test('refuses a version no tier has tagged yet', () => {
  const verdict = pinTagVerdict({ version: '18.9.0', commit: RELEASED, tags: new Map() });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /no dev\/v18\.9\.0 \/ test\/v18\.9\.0 \/ prod\/v18\.9\.0 tag exists/);
});

test('a tag of another version at the same commit does not count', () => {
  const tags = new Map([['dev/v18.6.1', RELEASED]]);
  assert.equal(pinTagVerdict({ version: '18.6.0', commit: RELEASED, tags }).ok, false);
});
