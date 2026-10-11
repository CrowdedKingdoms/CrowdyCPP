import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseTagRefs, pinTagVerdict, resolveTier, tierOfRef } from '../../tools/parity/pin-tag.mjs';

const RELEASED = '7662d0b0' + 'a'.repeat(32);
const UNRELEASED = 'ca9fbb50' + 'b'.repeat(32);
const TAG_OBJECT = 'f00dfeed' + 'c'.repeat(32);
const TEST_RELEASE = '2bfbb081' + 'd'.repeat(32);
const PROD_RELEASE = '7f805609' + 'e'.repeat(32);

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

test("a pin on this tier's tagged release passes", () => {
  const tags = new Map([['dev/v18.6.0', RELEASED]]);
  const verdict = pinTagVerdict({ version: '18.6.0', commit: RELEASED, tags, tier: 'dev' });
  assert.equal(verdict.ok, true);
  assert.match(verdict.message, /dev\/v18\.6\.0, this tier's release/);
});

test("refuses 0.59.0's pin: reachable, but not the commit 18.6.0 was released from", () => {
  const tags = new Map([['dev/v18.6.0', RELEASED]]);
  const verdict = pinTagVerdict({ version: '18.6.0', commit: UNRELEASED, tags, tier: 'dev' });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /not dev's release: dev\/v18\.6\.0 is 7662d0b0/);
});

test('refuses a version no tier has tagged yet', () => {
  const verdict = pinTagVerdict({ version: '18.9.0', commit: RELEASED, tags: new Map(), tier: 'dev' });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /no dev\/v18\.9\.0 tag exists yet\. Release CrowdyJS 18\.9\.0 on dev first/);
});

test('a tag of another version at the same commit does not count', () => {
  const tags = new Map([['dev/v18.6.1', RELEASED]]);
  assert.equal(pinTagVerdict({ version: '18.6.0', commit: RELEASED, tags, tier: 'dev' }).ok, false);
});

test('a dev tag on test is refused: CrowdyJS has not released the version on test', () => {
  const tags = new Map([['dev/v18.8.0', RELEASED]]);
  const verdict = pinTagVerdict({ version: '18.8.0', commit: RELEASED, tags, tier: 'test' });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /not test's release: no test\/v18\.8\.0 tag exists yet \(dev\/v18\.8\.0 is 7662d0b0/);
  assert.match(verdict.message, /Release CrowdyJS 18\.8\.0 on test first/);
});

test("a prod tag on dev is refused, and names dev's own release", () => {
  const tags = new Map([
    ['dev/v18.4.0', RELEASED],
    ['test/v18.4.0', TEST_RELEASE],
    ['prod/v18.4.0', PROD_RELEASE],
  ]);
  const verdict = pinTagVerdict({ version: '18.4.0', commit: PROD_RELEASE, tags, tier: 'dev' });
  assert.equal(verdict.ok, false);
  assert.match(verdict.message, /dev\/v18\.4\.0 is 7662d0b0aa \(the pin is prod\/v18\.4\.0, another tier's release\)/);
  assert.equal(pinTagVerdict({ version: '18.4.0', commit: PROD_RELEASE, tags, tier: 'prod' }).ok, true);
  assert.equal(pinTagVerdict({ version: '18.4.0', commit: TEST_RELEASE, tags, tier: 'test' }).ok, true);
});

test('a tier branch is judged as its tier, and any other branch as dev', () => {
  assert.equal(tierOfRef('dev'), 'dev');
  assert.equal(tierOfRef('test'), 'test');
  assert.equal(tierOfRef('prod'), 'prod');
  assert.equal(tierOfRef('refs/heads/prod'), 'prod');
  assert.equal(tierOfRef('test/hotfix-pin'), 'test');
  assert.equal(tierOfRef('fix/input-log-review'), 'dev');
  assert.equal(tierOfRef('testing-something'), 'dev');
  // A promotion branch is re-pinned to its destination's tag, so it is judged as that tier.
  assert.equal(tierOfRef('promote/dev-to-test-20260915'), 'test');
  assert.equal(tierOfRef('refs/heads/promote/test-to-prod-20261012'), 'prod');
  assert.equal(tierOfRef('promote/dev-to-test'), 'test');
  assert.equal(tierOfRef('promote/something-else'), 'dev');
});

test('the pull request base decides over the pushed branch, and --tier over both', () => {
  const env = { GITHUB_BASE_REF: 'test', GITHUB_REF_NAME: '153/merge' };
  assert.equal(resolveTier({ env }).tier, 'test');
  assert.equal(resolveTier({ env: { GITHUB_REF_NAME: 'prod' } }).tier, 'prod');
  assert.equal(resolveTier({ args: ['--ref', 'feat/x'], env }).tier, 'dev');
  assert.equal(resolveTier({ args: ['--tier', 'prod'], env }).tier, 'prod');
  assert.equal(resolveTier({ args: ['--ref', ''], env }).tier, 'test');
  assert.equal(resolveTier({ branch: 'fix/input-log-review' }).tier, 'dev');
  assert.throws(() => resolveTier({ args: ['--tier', 'staging'] }), /--tier must be one of/);
});

test('no ref at all is refused, in CI as a misconfiguration', () => {
  assert.throws(() => resolveTier({ env: { GITHUB_ACTIONS: 'true' }, branch: 'HEAD' }), /misconfiguration/);
  assert.throws(() => resolveTier({ branch: '' }), /pass --tier/);
});
