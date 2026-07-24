import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const generated = readFileSync(
  resolve('include/crowdy/generated/operations.hpp'),
  'utf8',
);

function document(name) {
  const match = generated.match(
    new RegExp(
      `k${name}IsolatedDocument = R"gql\\(([\\s\\S]*?)\\)gql";`,
    ),
  );
  assert.ok(match, `missing generated document for ${name}`);
  return match[1];
}

test('generated operation documents exclude unrelated roots', () => {
  const claim = document('MarketplaceClaimGridChunk');
  assert.match(claim, /claimGridChunk/);
  assert.doesNotMatch(claim, /appCodeAdmissionQueue/);
  assert.doesNotMatch(claim, /appPlayerCodeListingVersions/);

  const versions = document('MarketplaceAppListingVersions');
  assert.match(versions, /appPlayerCodeListingVersions/);
  assert.doesNotMatch(versions, /claimGridChunk/);
});
