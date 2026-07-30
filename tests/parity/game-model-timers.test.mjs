/**
 * Guards the game-model timer surface the C++ client exposes: the three timer
 * operations must be generated, bound to the Game endpoint, and select every
 * GmTimer field. The generic isolation test already validates each operation
 * against the SDLs; this pins the specific contract callers depend on so a
 * regenerated header cannot quietly drop it.
 */
import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..', '..');
const generated = readFileSync(
  join(root, 'include/crowdy/generated/operations.hpp'),
  'utf8',
);
const header = readFileSync(
  join(root, 'include/crowdy/domains/game_model.hpp'),
  'utf8',
);

function isolatedDocument(name) {
  const matches = [
    ...generated.matchAll(
      new RegExp(`k${name}IsolatedDocument = R"gql\\(([\\s\\S]*?)\\)gql";`, 'gu'),
    ),
  ];
  assert.equal(matches.length, 1, `expected one document constant for ${name}`);
  return matches[0][1];
}

test('the timer operations are generated and routed like their siblings', () => {
  // The API is unified, so both endpoint SDLs accept the game-model surface and
  // codegen resolves these to `Both` — the same binding every other
  // gameModel operation gets.
  const siblingBinding =
    /kGameModelInvokeEndpoint = GraphQLEndpoint::(\w+);/u.exec(generated)?.[1];
  assert.ok(siblingBinding, 'expected a gameModelInvoke endpoint binding');

  for (const name of [
    'GameModelScheduleInvoke',
    'GameModelCancelTimer',
    'GameModelTimers',
  ]) {
    assert.ok(isolatedDocument(name).length > 0, `${name} document is empty`);
    assert.match(
      generated,
      new RegExp(`k${name}Endpoint = GraphQLEndpoint::${siblingBinding};`, 'u'),
      `${name} should route like the other gameModel operations`,
    );
  }
});

test('GmTimerFields selects every field a caller needs', () => {
  // Omitting a field here yields silent nulls in the parsed result rather than
  // an error, so the selection set is asserted explicitly.
  const document = isolatedDocument('GameModelTimers');
  for (const field of [
    'timerId',
    'appId',
    'sessionId',
    'selfContainerId',
    'functionName',
    'paramsJson',
    'fireAt',
    'dedupeKey',
    'cascadeDepth',
    'flowId',
    'armedBy',
    'createdAt',
  ]) {
    assert.match(document, new RegExp(`\\b${field}\\b`, 'u'), `select ${field}`);
  }
});

test('cancelTimer accepts either selector, and neither is required', () => {
  const document = isolatedDocument('GameModelCancelTimer');
  assert.match(document, /\$timerId: String\b(?!!)/u);
  assert.match(document, /\$dedupeKey: String\b(?!!)/u);
  assert.match(document, /\$appId: BigInt!/u);
});

test('the trigger fragment carries writeSource and the firing diagnostics', () => {
  const document = isolatedDocument('GameModelAutomationTriggers');
  for (const field of [
    'writeSource',
    'lastMatchedAt',
    'matchCount24h',
    'warnings',
  ]) {
    assert.match(document, new RegExp(`\\b${field}\\b`, 'u'), `select ${field}`);
  }
});

test('the function fragment carries declarative timer effects', () => {
  const document = isolatedDocument('GameModelFunction');
  for (const field of ['delayMsExpression', 'dedupeKeyExpression']) {
    assert.match(document, new RegExp(`\\b${field}\\b`, 'u'), `select ${field}`);
  }
});

test('GameModelAPI exposes the timer methods with async twins', () => {
  for (const method of ['scheduleInvoke', 'cancelTimer', 'timers']) {
    assert.match(
      header,
      new RegExp(`\\b${method}\\(`, 'u'),
      `GameModelAPI should expose ${method}`,
    );
    assert.match(
      header,
      new RegExp(`\\b${method}Async\\(`, 'u'),
      `GameModelAPI should expose ${method}Async`,
    );
  }
});
