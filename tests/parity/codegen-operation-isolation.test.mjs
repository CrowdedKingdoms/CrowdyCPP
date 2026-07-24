import test from 'node:test';
import assert from 'node:assert/strict';
import {
  readFileSync,
  readdirSync,
  statSync,
} from 'node:fs';
import { basename, join, resolve } from 'node:path';
import {
  buildSchema,
  Kind,
  parse,
  validate,
} from 'graphql';

const root = resolve(import.meta.dirname, '..', '..');
const generated = readFileSync(
  join(root, 'include/crowdy/generated/operations.hpp'),
  'utf8',
);
const schemas = {
  management: buildSchema(
    readFileSync(join(root, 'schema.management.gql'), 'utf8'),
  ),
  game: buildSchema(readFileSync(join(root, 'schema.game.gql'), 'utf8')),
};

function document(name) {
  const matches = [...generated.matchAll(
    new RegExp(
      `k${name}IsolatedDocument = R"gql\\(([\\s\\S]*?)\\)gql";`,
      'gu',
    ),
  )];
  assert.equal(
    matches.length,
    1,
    `expected one generated document constant for ${name}`,
  );
  return matches[0][1];
}

test('every generated operation is isolated and endpoint-valid', () => {
  const contracts = operationContracts();
  assert.ok(contracts.length > 0);
  assert.equal(
    new Set(contracts.map(({ name }) => name)).size,
    contracts.length,
    'operation names must be globally unique so generated constants cannot collide',
  );

  for (const contract of contracts) {
    const text = document(contract.name);
    const parsed = parse(text, { noLocation: true });
    const operations = parsed.definitions.filter(
      (definition) => definition.kind === Kind.OPERATION_DEFINITION,
    );
    assert.equal(
      operations.length,
      1,
      `${contract.name} generated a non-isolated document`,
    );
    assert.equal(operations[0].name?.value, contract.name);

    const validEndpoints = Object.entries(schemas)
      .filter(([, schema]) => validate(schema, parsed).length === 0)
      .map(([plane]) => plane);
    assert.deepEqual(
      validEndpoints,
      contract.validEndpoints,
      `${contract.domain}/${contract.file}:${contract.name} endpoint drift`,
    );
    assert.ok(
      validEndpoints.length > 0,
      `${contract.name} is invalid on both endpoint SDLs`,
    );

    const endpoint =
      validEndpoints.length === 2
        ? 'Both'
        : validEndpoints[0] === 'management'
          ? 'Management'
          : 'Game';
    const endpointMatches = [...generated.matchAll(
      new RegExp(
        `k${contract.name}Endpoint = GraphQLEndpoint::${endpoint};`,
        'gu',
      ),
    )];
    assert.equal(
      endpointMatches.length,
      1,
      `${contract.name} generated endpoint metadata is missing or duplicated`,
    );
    const dispatchMatches = [...generated.matchAll(
      new RegExp(
        `operationName == "${contract.name}"\\) return ` +
          `k${contract.name}IsolatedDocument;`,
        'gu',
      ),
    )];
    assert.equal(
      dispatchMatches.length,
      1,
      `${contract.name} documentFor dispatch is missing or duplicated`,
    );
  }
});

test('C++ callers cannot send multi-operation file documents', () => {
  const multiOperationConstants = new Set(
    operationFiles()
      .filter(({ operations }) => operations.length > 1)
      .map(
        ({ domain, file }) =>
          `gen::${domain}::k${basename(file, '.graphql')}Document`,
      ),
  );
  const violations = [];
  for (const path of [
    ...filesBelow(join(root, 'include')),
    ...filesBelow(join(root, 'src')),
    ...filesBelow(join(root, 'tests')),
  ]) {
    if (!/\.(?:cpp|hpp)$/u.test(path) || path.includes('/generated/')) continue;
    const source = readFileSync(path, 'utf8');
    for (const constant of multiOperationConstants) {
      if (source.includes(constant)) {
        violations.push(`${path.slice(root.length + 1)} uses ${constant}`);
      }
    }
  }
  assert.deepEqual(violations, []);
});

test('dual-endpoint wrappers route operations only to valid planes', () => {
  const contracts = new Map(
    operationContracts().map((contract) => [contract.name, contract]),
  );
  assertRoutes(
    readFileSync(join(root, 'include/crowdy/domains/marketplace.hpp'), 'utf8'),
    /\b(game_|management_)\.run(?:Async)?\(\s*"([A-Za-z_]\w*)"/gu,
    (helper) => (helper === 'game_' ? 'game' : 'management'),
    contracts,
  );
  assertRoutes(
    readFileSync(
      join(root, 'include/crowdy/domains/crowdy_studio_agent.hpp'),
      'utf8',
    ),
    /\b(game(?:Input)?(?:Async)?|management(?:Input)?(?:Async)?)\(\s*"([A-Za-z_]\w*)"/gu,
    (helper) => (helper.startsWith('game') ? 'game' : 'management'),
    contracts,
  );
});

function assertRoutes(source, pattern, planeForHelper, contracts) {
  let count = 0;
  for (const match of source.matchAll(pattern)) {
    count++;
    const [, helper, operationName] = match;
    const contract = contracts.get(operationName);
    assert.ok(contract, `wrapper references unknown operation ${operationName}`);
    const plane = planeForHelper(helper);
    assert.ok(
      contract.validEndpoints.includes(plane),
      `${operationName} is routed to ${plane} but validates only on ` +
        contract.validEndpoints.join(', '),
    );
  }
  assert.ok(count > 0, 'endpoint routing gate did not inspect any wrappers');
}

function operationContracts() {
  const contracts = [];
  for (const { domain, file, document: source } of operationFiles()) {
    const fragments = new Map(
      source.definitions
        .filter((definition) => definition.kind === Kind.FRAGMENT_DEFINITION)
        .map((definition) => [definition.name.value, definition]),
    );
    for (const operation of source.definitions.filter(
      (definition) => definition.kind === Kind.OPERATION_DEFINITION,
    )) {
      const names = new Set();
      collectSpreads(operation, names);
      const selected = [];
      const pending = [...names];
      while (pending.length > 0) {
        const name = pending.shift();
        if (selected.some((fragment) => fragment.name.value === name)) continue;
        const fragment = fragments.get(name);
        assert.ok(fragment, `${domain}/${file} is missing fragment ${name}`);
        selected.push(fragment);
        const nested = new Set();
        collectSpreads(fragment, nested);
        pending.push(...nested);
      }
      const isolated = {
        kind: Kind.DOCUMENT,
        definitions: [operation, ...selected],
      };
      const validEndpoints = Object.entries(schemas)
        .filter(([, schema]) => validate(schema, isolated).length === 0)
        .map(([plane]) => plane);
      contracts.push({
        domain,
        file,
        name: operation.name.value,
        validEndpoints,
      });
    }
  }
  return contracts;
}

function operationFiles() {
  const files = [];
  const operations = join(root, 'operations');
  for (const domain of readdirSync(operations).sort()) {
    const directory = join(operations, domain);
    if (!statSync(directory).isDirectory()) continue;
    for (const file of readdirSync(directory).sort()) {
      if (!file.endsWith('.graphql')) continue;
      const document = parse(readFileSync(join(directory, file), 'utf8'), {
        noLocation: true,
      });
      files.push({
        domain,
        file,
        document,
        operations: document.definitions.filter(
          (definition) => definition.kind === Kind.OPERATION_DEFINITION,
        ),
      });
    }
  }
  return files;
}

function collectSpreads(node, names) {
  if (!node || typeof node !== 'object') return;
  if (node.kind === Kind.FRAGMENT_SPREAD) names.add(node.name.value);
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) {
      for (const entry of value) collectSpreads(entry, names);
    } else {
      collectSpreads(value, names);
    }
  }
}

function filesBelow(directory, out = []) {
  for (const entry of readdirSync(directory).sort()) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) filesBelow(path, out);
    else out.push(path);
  }
  return out;
}
