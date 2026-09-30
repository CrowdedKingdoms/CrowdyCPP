import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..', '..');
const operationsHpp = readFileSync(
  join(root, 'include/crowdy/generated/operations.hpp'),
  'utf8',
);

/**
 * Every document that returns an App selects `wildernessWritesOpen` (cks-game-api
 * #434), as CrowdyJS 18.0.4 does. A missing selection is neither a compile error
 * nor a server error: the field just arrives absent and reads as false, so an
 * org-admin tool would show every wilderness as closed.
 */
const APP_DOCUMENTS = ['App', 'AppBySlug', 'AppsForOrg', 'MyApps', 'CreateApp', 'UpdateApp'];

function generatedDocument(name) {
  const match = operationsHpp.match(
    new RegExp(`k${name}Document = R"gql\\(([\\s\\S]*?)\\)gql"`, 'u'),
  );
  assert.ok(match, `k${name}Document not found in operations.hpp`);
  return match[1];
}

for (const name of APP_DOCUMENTS) {
  test(`${name} selects wildernessWritesOpen`, () => {
    const source = readFileSync(join(root, 'operations', 'apps', `${name}.graphql`), 'utf8');
    assert.match(source, /\bwildernessWritesOpen\b/u, `operations/apps/${name}.graphql`);
    assert.match(generatedDocument(name), /\bwildernessWritesOpen\b/u, `k${name}Document`);
  });
}
