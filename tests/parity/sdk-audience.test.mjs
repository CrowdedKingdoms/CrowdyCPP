/**
 * The SDK is for normal clients and designed for production: players, developers and
 * org-admins. It wraps no root field that only a super-admin or a platform operator can call
 * (operator decision, 2026-09-28); platform tooling calls those fields directly. CrowdyJS
 * carries the same rule (test/unit/sdk-audience.test.mjs).
 *
 * Two nets, because the schema has no structured marker for either role: the fields the game
 * API guards with @RequiresSuperAdmin / @RequiresOperator / OperatorGuard, and any root field
 * whose description says it is operator- or super-admin-only. "Wrapped" is every root field an
 * operation document under operations/ or an inline R"gql(...)gql" document in include/ selects.
 * The quotas.set refusal of a platform-global rule is covered by client_portable_test.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildSchema, parse } from 'graphql';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

/** cks-game-api dev (da1179f9): root fields only a super-admin or an operator can call. */
const PLATFORM_ONLY = [
  // super-admin
  'setSuperAdmin',
  'setOperator',
  'setEarlyAccessOverride',
  'updateUserType',
  'forceLogoutUser',
  'usersPaginated',
  'usersConnection',
  'checkouts',
  'checkoutsConnection',
  'paymentEvents',
  'paymentEventsConnection',
  'setOrgStatus',
  'setAppVisibility',
  // operator
  'cpBillingCreditOverbill',
  'cpSetCrowdyStudioAgentAppKill',
  'cpSetCrowdyStudioAgentPlatformPolicy',
  'creditOrgWallet',
  'forgetEmailDeliverability',
  'reinstateOrganization',
  'retireOrganization',
  'runSharedUsageBillingTick',
  'sendTestEmail',
  'setBillingRate',
  'setHostedGameListing',
  'setOrgBillingExempt',
  'takeDownHostedGame',
  'agentRateCards',
  'allHostedGames',
  'billingExemptOrgs',
  'billingRateCard',
  'cpBillingInvariantRuns',
  'cpBillingReconciliations',
  'cpBillingWriteOffs',
  'cpCrowdyStudioAgentCatalog',
  'cpCrowdyStudioAgentPlatformPolicy',
  'emailDeliverability',
  'emailDeliveryConfig',
  'retiredOrganizations',
];

const PLATFORM_DESCRIPTION =
  /\b(operator|super[- ]?admins?)( only\b|:)|\brestricted to super[- ]?admins?\b|\brequires a super[- ]?admin\b/i;

function walk(dir, ext, out = []) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) walk(path, ext, out);
    else if (path.endsWith(ext)) out.push(path);
  }
  return out;
}

function documents() {
  const docs = walk(join(repo, 'operations'), '.graphql').map((f) => ({
    where: relative(repo, f),
    text: readFileSync(f, 'utf8'),
  }));
  for (const f of walk(join(repo, 'include'), '.hpp')) {
    if (f.includes(`${join('include', 'crowdy', 'generated')}`)) continue;
    const text = readFileSync(f, 'utf8');
    for (const m of text.matchAll(/R"gql\(([\s\S]*?)\)gql"/g)) {
      docs.push({ where: relative(repo, f), text: m[1] });
    }
  }
  return docs;
}

/** Root field name -> the first document that selects it. */
function wrappedRootFields() {
  const all = documents();
  const fragments = new Map();
  const parsed = all.map(({ where, text }) => ({ where, doc: parse(text, { noLocation: true }) }));
  for (const { doc } of parsed) {
    for (const def of doc.definitions) {
      if (def.kind === 'FragmentDefinition') fragments.set(def.name.value, def);
    }
  }
  const out = new Map();
  const visit = (selectionSet, where, seen) => {
    for (const sel of selectionSet.selections) {
      if (sel.kind === 'Field') {
        if (!sel.name.value.startsWith('__') && !out.has(sel.name.value)) out.set(sel.name.value, where);
      } else if (sel.kind === 'InlineFragment') {
        visit(sel.selectionSet, where, seen);
      } else if (sel.kind === 'FragmentSpread' && !seen.has(sel.name.value)) {
        const frag = fragments.get(sel.name.value);
        if (frag) visit(frag.selectionSet, where, new Set([...seen, sel.name.value]));
      }
    }
  };
  for (const { where, doc } of parsed) {
    for (const def of doc.definitions) {
      if (def.kind === 'OperationDefinition') visit(def.selectionSet, where, new Set());
    }
  }
  return out;
}

function rootFieldDescriptions() {
  const schema = buildSchema(readFileSync(join(repo, 'schema.gql'), 'utf8'));
  const out = new Map();
  for (const type of [schema.getQueryType(), schema.getMutationType(), schema.getSubscriptionType()]) {
    if (!type) continue;
    for (const field of Object.values(type.getFields())) {
      out.set(field.name, (field.description ?? '').replace(/\s+/g, ' '));
    }
  }
  return out;
}

test('no SDK document selects a root field only a super-admin or an operator can call', () => {
  const wrapped = wrappedRootFields();
  assert.ok(wrapped.size > 100, `only ${wrapped.size} wrapped root fields found; the scan is broken`);
  const described = [...rootFieldDescriptions()]
    .filter(([, d]) => PLATFORM_DESCRIPTION.test(d))
    .map(([name]) => name);
  const forbidden = new Set([...PLATFORM_ONLY, ...described]);
  const offenders = [...wrapped]
    .filter(([name]) => forbidden.has(name))
    .map(([name, where]) => `${name} (${where})`);
  assert.deepEqual(
    offenders,
    [],
    'the SDK wraps platform-only root fields; remove them (see AGENTS.md "the SDK is for normal clients")',
  );
});

test('the platform-only list names real root fields', () => {
  const roots = rootFieldDescriptions();
  const missing = PLATFORM_ONLY.filter((name) => !roots.has(name));
  assert.deepEqual(missing, [], 'PLATFORM_ONLY names a root field schema.gql no longer has; re-classify');
});

test('the description net recognises platform-only fields and leaves org-admin ones alone', () => {
  const roots = rootFieldDescriptions();
  for (const name of ['setOrgStatus', 'retireOrganization', 'emailDeliverability', 'allHostedGames', 'checkouts']) {
    assert.match(roots.get(name) ?? '', PLATFORM_DESCRIPTION, `${name} should read as platform-only`);
  }
  for (const name of ['orgMembers', 'inviteOrgMember', 'createApp', 'myCheckouts']) {
    assert.doesNotMatch(roots.get(name) ?? '', PLATFORM_DESCRIPTION, `${name} should read as org-admin`);
  }
});
