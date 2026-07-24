#!/usr/bin/env node
/**
 * Bidirectional CrowdyJS/CrowdyCPP parity gate.
 *
 * The baseline gate accepts only named, reviewed differences:
 *   - portable gap: real parity work that remains for a later plan phase;
 *   - native equivalent: C++ implements the contract through its native path;
 *   - browser exclusion: the surface is inherently browser/UI specific.
 *
 * Any new, changed, or stale classification fails. `--strict` additionally
 * fails while a portable gap remains, for the final parity/release gate.
 *
 * Usage:
 *   node tools/parity/parity.mjs [--crowdyjs <checkout>]
 *     [--write docs/parity-matrix.md | --check docs/parity-matrix.md]
 *     [--strict]
 */
import {
  existsSync,
  readFileSync,
  readdirSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { execFileSync } from 'node:child_process';
import { basename, dirname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  compareSchemaSurfaces,
  parseSchemaSurface,
  rootFieldEntries,
} from './schema-surface.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjsPath = resolve(
  options.crowdyjs ?? join(root, '..', 'CrowdyJS'),
);

const CATEGORY = Object.freeze({
  PORTABLE: 'portable-gap',
  NATIVE: 'native-equivalent',
  BROWSER: 'browser-exclusion',
});

// The canonical API schemas currently contain ensure-container additions made
// after the pinned CrowdyJS v12 snapshot. These are real portable differences,
// not omissions to hide. The entries must disappear once CrowdyJS catches up.
const SCHEMA_BASELINE = {
  'input:EnsureContainerInput': classification(
    CATEGORY.PORTABLE,
    'canonical API SDL is ahead of CrowdyJS v12; no C++ domain wrapper yet',
    'definition-only',
    'CrowdyCPP',
  ),
  'type:GmContainer.bindingKey': classification(
    CATEGORY.PORTABLE,
    'canonical API SDL is ahead of CrowdyJS v12',
    'member-only',
    'CrowdyCPP',
  ),
  'type:GmEnsureContainerResult': classification(
    CATEGORY.PORTABLE,
    'canonical API SDL is ahead of CrowdyJS v12; no C++ domain wrapper yet',
    'definition-only',
    'CrowdyCPP',
  ),
  'type:Mutation.gameModelEnsureContainer': classification(
    CATEGORY.PORTABLE,
    'canonical API SDL is ahead of CrowdyJS v12; no C++ operation yet',
    'member-only',
    'CrowdyCPP',
  ),
  'type:Query.gameModelContainers': classification(
    CATEGORY.PORTABLE,
    'canonical API adds the bindingKey filter that CrowdyJS v12 does not expose',
    'member-signature',
    'both',
  ),
};

const ROOT_CLASSIFICATIONS = {
  ...classifyNames(
    'Query',
    [
      'appPlayerCodeListingVersions',
      'cpCrowdyStudioAgentPlatformPolicy',
      'crowdyStudioAgentBudget',
      'crowdyStudioAgentEffectivePolicy',
      'crowdyStudioAgentHistory',
      'crowdyStudioAgentPolicy',
      'crowdyStudioAgentSession',
      'crowdyStudioAgentSessions',
      'crowdyStudioAgentToolDescriptors',
      'crowdyStudioAgentUsage',
      'crowdyStudioCommonFiles',
      'crowdyStudioLibraryFiles',
      'crowdyStudioProject',
      'crowdyStudioProjects',
    ],
    CATEGORY.PORTABLE,
    'portable Crowdy Studio/agent query; later strict-parity phase',
  ),
  ...classifyNames(
    'Mutation',
    [
      'claimGridChunk',
      'releaseClaimedGrid',
    ],
    CATEGORY.PORTABLE,
    'portable gameplay domain operation; later portable-gaps phase',
  ),
  ...classifyNames(
    'Mutation',
    [
      'crowdyStudioProjectCreate',
      'crowdyStudioProjectSaveMetadata',
      'crowdyStudioProjectSave',
      'crowdyStudioProjectSaveFiles',
      'crowdyStudioProjectSetArchived',
      'crowdyStudioLibrarySave',
      'crowdyStudioLibrarySetArchived',
      'crowdyStudioProjectImportFile',
      'crowdyStudioCommonPublish',
      'crowdyStudioProjectCreateFromModules',
    ],
    CATEGORY.PORTABLE,
    'portable Crowdy Studio mutation; later Crowdy Studio phase',
  ),
  ...classifyNames(
    'Mutation',
    [
      'crowdyStudioAgentCreateSession',
      'crowdyStudioAgentAttachClient',
      'crowdyStudioAgentSetMode',
      'crowdyStudioAgentAcknowledgeEvents',
      'crowdyStudioAgentHeartbeat',
      'crowdyStudioAgentSendMessage',
      'crowdyStudioAgentApproveTool',
      'crowdyStudioAgentRejectTool',
      'crowdyStudioAgentToolResult',
      'crowdyStudioAgentGrantLease',
      'crowdyStudioAgentRevokeLease',
      'crowdyStudioAgentPause',
      'crowdyStudioAgentResume',
      'crowdyStudioAgentCancelRun',
      'crowdyStudioAgentCloseSession',
      'setCrowdyStudioAgentPolicy',
      'cpSetCrowdyStudioAgentPlatformPolicy',
      'cpSetCrowdyStudioAgentAppKill',
    ],
    CATEGORY.PORTABLE,
    'portable Agentic Studio mutation; later agent-controller phase',
  ),
  'Mutation.gameModelEnsureContainer': classification(
    CATEGORY.PORTABLE,
    'canonical API operation is newer than CrowdyJS v12 and has no C++ wrapper',
  ),
  'Subscription.gameModelContainerChanged': classification(
    CATEGORY.PORTABLE,
    'portable GraphQL-WebSocket stream; later graphql-ws phase',
  ),
  'Subscription.crowdyStudioAgentEvents': classification(
    CATEGORY.PORTABLE,
    'portable ordered agent event stream; later graphql-ws/agent phase',
  ),
  ...classifyNames(
    'Mutation',
    [
      'connectUdpProxy',
      'disconnectUdpProxy',
      'sendActorUpdate',
      'sendVoxelUpdate',
      'sendAudioPacket',
      'sendTextPacket',
      'sendClientEvent',
      'sendSingleActorMessage',
      'sendChannelMessage',
    ],
    CATEGORY.NATIVE,
    'browser GraphQL UDP proxy maps to CrowdyCPP native signed UDP',
  ),
  'Query.udpProxyConnectionStatus': classification(
    CATEGORY.NATIVE,
    'native replication Connection exposes transport state directly',
  ),
  'Subscription.udpNotifications': classification(
    CATEGORY.NATIVE,
    'native replication Connection receives and dispatches UDP notifications',
  ),
};

const METHOD_CLASSIFICATIONS = {
  'WorldClient.subscribe': classification(
    CATEGORY.NATIVE,
    'Connection handlers and WorldSession receive native UDP events',
  ),
  'PortalAPI.handleAuthorizeRequest': classification(
    CATEGORY.BROWSER,
    'Overworld browser-page redirect handler; native uses authorization-code APIs',
  ),
  'WorldSessionCore.on': classification(
    CATEGORY.NATIVE,
    'tick-driven WorldSession installs typed Connection handlers',
  ),
  'WorldSessionCore.onDispose': classification(
    CATEGORY.NATIVE,
    'RAII destruction replaces browser disposal callbacks',
  ),
  'WorldSessionCore.setSendTracker': classification(
    CATEGORY.NATIVE,
    'internal native WorldSession/ErrorStore wiring',
  ),
  'WorldSessionCore.trackSend': classification(
    CATEGORY.NATIVE,
    'internal native WorldSession/ErrorStore wiring',
  ),
  'HostTracker.beat': classification(
    CATEGORY.NATIVE,
    'WorldSession::tick performs the configured heartbeat',
  ),
  'LocalActorStore.patchState': classification(
    CATEGORY.NATIVE,
    'mutate the native state struct and call setState',
  ),
  'ActorInbox.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native inbox retains bytes and leaves typed decoding to the caller',
  ),
  'ChannelInbox.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native inbox retains bytes and leaves typed decoding to the caller',
  ),
  'EventRouter.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native event router retains bytes and leaves typed decoding to the caller',
  ),
  'SocialKit.chat.onMessage': classification(
    CATEGORY.NATIVE,
    'WorldSession channel inbox / Connection channel handler',
  ),
  'PlayerComputeAPI.artifactBytes': classification(
    CATEGORY.BROWSER,
    'ArrayBuffer worker convenience; native artifact bytes are decoded directly',
  ),
  'MarketplaceAPI.clientArtifactBytes': classification(
    CATEGORY.BROWSER,
    'ArrayBuffer worker convenience; native artifact bytes are decoded directly',
  ),
  'GameModelAPI.containerChanged': classification(
    CATEGORY.PORTABLE,
    'portable GraphQL-WebSocket stream; later graphql-ws phase',
  ),
  'MarketplaceAPI.claimGridChunk': classification(
    CATEGORY.PORTABLE,
    'portable gameplay wrapper; later portable-gaps phase',
  ),
  'MarketplaceAPI.releaseClaimedGrid': classification(
    CATEGORY.PORTABLE,
    'portable gameplay wrapper; later portable-gaps phase',
  ),
  'AvatarStateStore.privateState': classification(
    CATEGORY.PORTABLE,
    'owner-only avatar state cache is not implemented in CrowdyCPP',
  ),
  'ChunkStore.pendingWriteBacks': classification(
    CATEGORY.PORTABLE,
    'write-back queue observability is not implemented in CrowdyCPP',
  ),
  'ChunkStore.revision': classification(
    CATEGORY.PORTABLE,
    'cache revision counter is not implemented in CrowdyCPP',
  ),
  'ErrorStore.total': classification(
    CATEGORY.PORTABLE,
    'lifetime error counter is not implemented in CrowdyCPP',
  ),
  'LocalActorStore.lastError': classification(
    CATEGORY.PORTABLE,
    'local actor send-error snapshot is not implemented in CrowdyCPP',
  ),
  'LocalActorStore.lastSent': classification(
    CATEGORY.PORTABLE,
    'typed outbound update snapshot is not implemented in CrowdyCPP',
  ),
  'LocalActorStore.state': classification(
    CATEGORY.PORTABLE,
    'read access to the current local actor state is not implemented in CrowdyCPP',
  ),
  'LocalActorStore.status': classification(
    CATEGORY.PORTABLE,
    'local actor replication lifecycle status is not implemented in CrowdyCPP',
  ),
  'RemoteActorLane.revision': classification(
    CATEGORY.PORTABLE,
    'lane revision counter is not implemented in CrowdyCPP',
  ),
  'RemoteActorStore.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native actor stores retain bytes and leave typed decoding to the caller',
  ),
  'RemoteActorStore.revision': classification(
    CATEGORY.PORTABLE,
    'actor-registry revision counter is not implemented in CrowdyCPP',
  ),
  'SaveStateStore.dirty': classification(
    CATEGORY.PORTABLE,
    'dirty/autosave state is not implemented in CrowdyCPP',
  ),
  'SaveStateStore.lastSavedAt': classification(
    CATEGORY.PORTABLE,
    'last-save timestamp is not implemented in CrowdyCPP',
  ),
  'SaveStateStore.set': classification(
    CATEGORY.PORTABLE,
    'local dirty setter/autosave semantics are not implemented in CrowdyCPP',
  ),
};

const CLASS_CLASSIFICATIONS = {
  UdpAPI: classification(
    CATEGORY.NATIVE,
    'browser GraphQL UDP proxy maps to the native replication client',
  ),
  BrowserSessionPkceStore: classification(
    CATEGORY.BROWSER,
    'browser sessionStorage helper; native applications own verifier persistence',
  ),
  CrowdyAgentBrowserToolDispatcher: classification(
    CATEGORY.BROWSER,
    'browser executor implementation is excluded; a native dispatcher is a separate portable gap',
  ),
  CrowdyStudioAPI: classification(
    CATEGORY.PORTABLE,
    'portable cloud project domain; later Crowdy Studio phase',
  ),
  CrowdyStudioController: classification(
    CATEGORY.PORTABLE,
    'portable headless authoring controller; later Crowdy Studio phase',
  ),
  CrowdyStudioAgentController: classification(
    CATEGORY.PORTABLE,
    'portable durable agent controller; later agent-controller phase',
  ),
  CrowdyAgentGraphQLTransport: classification(
    CATEGORY.PORTABLE,
    'portable HTTP/GraphQL-WebSocket agent transport; later agent phases',
  ),
  CrowdyAgentToolRegistry: classification(
    CATEGORY.PORTABLE,
    'portable immutable descriptor registry; later agent-controller phase',
  ),
  AgentControlLeaseManager: classification(
    CATEGORY.PORTABLE,
    'portable native Play lease gate and synchronous preemption; later player-host phase',
  ),
};

const METHOD_ALIASES = {
  'ActorsAPI.delete': 'remove',
  'AvatarsAPI.delete': 'remove',
  'PlayerComputeAPI.delete': 'remove',
  'TeamsAPI.remove': 'remove',
  'ChannelsAPI.remove': 'remove',
  'StateAPI.delete': 'remove',
  'QuotasAPI.delete': 'remove',
  'GameModelAPI.automations': 'automationsList',
  'GameModelAPI.getFunction': 'function',
  'OperatorAPI.usage': 'usageSummary',
  'AppsAPI.app': 'get',
  'AppsAPI.appBySlug': 'getBySlug',
  'AppsAPI.myApps': 'mine',
  'AppAccessAPI.usersByApp': 'userAccessByApp',
  'AppAccessAPI.usersByAppConnection': 'userAccessConnection',
  'BillingAPI.buddyTiers': 'buddyBillingTiers',
  'BillingAPI.graphqlTiers': 'graphqlBillingTiers',
  'BillingAPI.postgresTiers': 'postgresBillingTiers',
  'EnvironmentsAPI.list': 'forOrg',
  'OrganizationsAPI.bySlug': 'getBySlug',
  'OrganizationsAPI.setMemberRoles': 'updateMemberRoles',
  'PaymentsAPI.all': 'checkouts',
  'PaymentsAPI.allConnection': 'checkoutsConnection',
  'PaymentsAPI.capturePaypal': 'capturePaypalCheckout',
  'PaymentsAPI.create': 'createCheckout',
  'PaymentsAPI.events': 'paymentEvents',
  'PaymentsAPI.eventsConnection': 'paymentEventsConnection',
  'PaymentsAPI.mine': 'myCheckouts',
  'PaymentsAPI.mineConnection': 'myCheckoutsConnection',
  'SharedEnvironmentAPI.autoBilling': 'orgAutoBilling',
  'SharedEnvironmentAPI.freeAppQuota': 'orgFreeAppQuota',
  'SharedEnvironmentAPI.paymentMethods': 'orgPaymentMethods',
  'ControlPlaneAPI.setDeletionProtection': 'setEnvironmentDeletionProtection',
  'EconomyKit.trades.get': 'trade',
  'EconomyKit.trades.listMine': 'myTrades',
  'EconomyKit.trades.offer': 'tradeOffer',
  'EconomyKit.trades.accept': 'tradeAccept',
  'EconomyKit.trades.cancel': 'tradeCancel',
  'UsageAPI.orgSummary': 'orgSummary',
  'AvatarStateStore.publicState': 'identityState',
  'ChunkStore.get': 'find',
  'ErrorStore.last': 'recent',
  'HostTracker.isHost': 'amIHost',
  'RemoteActorLane.count': 'size',
  'RemoteActorLane.get': 'find',
  'RemoteActorStore.count': 'size',
  'RemoteActorStore.get': 'find',
};

const CLASS_MAP = {
  AuthAPI: 'AuthAPI',
  UsersAPI: 'UsersAPI',
  PortalAPI: 'PortalAPI',
  ServerStatusAPI: 'ServerStatusAPI',
  ChunksAPI: 'ChunksAPI',
  VoxelsAPI: 'VoxelsAPI',
  ActorsAPI: 'ActorsAPI',
  AvatarsAPI: 'AvatarsAPI',
  StateAPI: 'StateAPI',
  HostAPI: 'HostAPI',
  TeleportAPI: 'TeleportAPI',
  TeamsAPI: 'TeamsAPI',
  ChannelsAPI: 'ChannelsAPI',
  GameModelAPI: 'GameModelAPI',
  GameAppsAPI: 'GameAppsAPI',
  PlayerComputeAPI: 'PlayerComputeAPI',
  PlatformAPI: 'PlatformAPI',
  OrganizationsAPI: 'OrganizationsAPI',
  AppsAPI: 'AppsAPI',
  AppAccessAPI: 'AppAccessAPI',
  BillingAPI: 'BillingAPI',
  PaymentsAPI: 'PaymentsAPI',
  QuotasAPI: 'QuotasAPI',
  EnvironmentsAPI: 'EnvironmentsAPI',
  UsageAPI: 'UsageAPI',
  SharedEnvironmentAPI: 'SharedEnvironmentAPI',
  ControlPlaneAPI: 'OperatorAPI',
  AdminAPI: 'AdminAPI',
  WorldClient: 'WorldClient',
  ActorClient: 'ActorClient',
  GameKitClient: 'GameKitClient',
  InventoryKit: 'InventoryKit',
  ObjectsKit: 'ObjectsKit',
  NpcsKit: 'NpcsKit',
  PlotsKit: 'PlotsKit',
  EconomyKit: 'EconomyKit',
  ProgressionKit: 'ProgressionKit',
  LootKit: 'LootKit',
  QuestsKit: 'QuestsKit',
  CombatKit: 'CombatKit',
  MatchesKit: 'MatchesKit',
  DecksKit: 'DecksKit',
  WorldsimKit: 'WorldsimKit',
  SocialKit: 'SocialKit',
  LeaderboardsKit: 'LeaderboardsKit',
  FeaturesKit: 'FeaturesKit',
  LocalActorStore: 'LocalActorStore',
  RemoteActorStore: 'RemoteActorStore',
  ChannelInbox: 'Inbox',
  ActorInbox: 'Inbox',
  EventRouter: 'EventRouter',
  HostTracker: 'WorldSession',
  SaveStateStore: 'SaveStateStore',
  AvatarStateStore: 'AvatarStateStore',
  ContainerMirror: 'ContainerMirror',
  WorldSessionCore: 'WorldSession',
  ChunkStore: 'ChunkStore',
  ErrorStore: 'ErrorStore',
  RemoteActorLane: 'RemoteActorLane',
  CrowdyStudioAPI: 'CrowdyStudioAPI',
  CrowdyStudioController: 'CrowdyStudioController',
  CrowdyStudioAgentController: 'CrowdyStudioAgentController',
  CrowdyAgentGraphQLTransport: 'CrowdyAgentGraphQLTransport',
  CrowdyAgentToolRegistry: 'CrowdyAgentToolRegistry',
  AgentControlLeaseManager: 'AgentControlLeaseManager',
};

const STRICT_NATIVE_SURFACE_GAPS = [
  'Gameplay: claim/release grid chunks, safe gameplay-token rotation, /graphql endpoint normalization, and kit verdict-error mapping.',
  'Crowdy Studio: cloud project/library/common-file domain plus headless revision, checkpoint, patch, and runtime controller.',
  'Realtime: pluggable graphql-transport-ws client with RAII subscriptions, reconnect, replay, and game-thread dispatch.',
  'Agentic Studio: generated operations, descriptor validation, durable controller, approvals, budgets, heartbeat, and epoch fencing.',
  'Native player host: tool dispatcher, Studio/player-host adapters, lease manager, immediate human preemption, and late-result fencing.',
];

if (!existsSync(join(crowdyjsPath, 'schema.gql'))) {
  fail(
    `CrowdyJS checkout is required at ${crowdyjsPath}; pass --crowdyjs <path>.`,
  );
}

const cppSchema = parseSchemaSurface(
  readFileSync(join(root, 'schema.gql'), 'utf8'),
);
const jsSchema = parseSchemaSurface(
  readFileSync(join(crowdyjsPath, 'schema.gql'), 'utf8'),
);
const schemaDifferences = compareSchemaSurfaces(
  cppSchema,
  jsSchema,
  'CrowdyCPP',
  'CrowdyJS',
);
const usedFields = operationRootFields();
for (const field of inlineRootFields()) usedFields.add(field);

const state = {
  unclassified: [],
  stale: [],
  portable: [],
  native: [],
  browser: [],
  deprecated: [],
  usedSchemaClassifications: new Set(),
  usedRootClassifications: new Set(),
  usedMethodClassifications: new Set(),
  usedClassClassifications: new Set(),
};

const targetVersion = JSON.parse(
  readFileSync(join(crowdyjsPath, 'package.json'), 'utf8'),
).version;
const targetCommit = gitRevision(crowdyjsPath);

let report = '# CrowdyCPP parity matrix\n\n';
report +=
  'Generated by `tools/parity/parity.mjs`; do not edit by hand. ' +
  'Portable gaps are real missing work, not waivers. Native equivalents and ' +
  'browser exclusions are the only intentional waivers.\n\n';
report += '## Target\n\n';
report += `- CrowdyJS: \`${targetVersion}\` at \`${targetCommit}\`\n`;
report += '- CrowdyCPP schema: current canonical Management + Game SDL merge\n';
report += '- Gate mode: reviewed baseline (use `--strict` to reject every portable gap)\n\n';

report += '## Bidirectional schema comparison\n\n';
report +=
  '| Difference | Direction | Classification |\n' +
  '|---|---|---|\n';
if (schemaDifferences.length === 0) {
  report += '| — | — | schemas match semantically |\n';
}
for (const difference of schemaDifferences) {
  const expected = SCHEMA_BASELINE[difference.id];
  let status;
  if (
    expected &&
    expected.expectedKind === difference.kind &&
    expected.expectedSide === difference.side
  ) {
    state.usedSchemaClassifications.add(difference.id);
    recordClassification(state, expected, `schema:${difference.id}`);
    status = renderClassification(expected);
  } else {
    status = '**UNCLASSIFIED SCHEMA DIFFERENCE**';
    state.unclassified.push(`schema:${difference.id} (${difference.message})`);
  }
  report +=
    `| \`${escapeCell(difference.id)}\` | ${escapeCell(difference.side)} | ` +
    `${escapeCell(status)} |\n`;
}
report += '\n';

report += '## GraphQL root-field implementation\n\n';
for (const rootName of ['Query', 'Mutation', 'Subscription']) {
  const cppFields = new Map(
    rootFieldEntries(cppSchema, rootName).map((field) => [field.name, field]),
  );
  const jsFields = new Map(
    rootFieldEntries(jsSchema, rootName).map((field) => [field.name, field]),
  );
  const names = [...new Set([...cppFields.keys(), ...jsFields.keys()])].sort();
  report +=
    `### ${rootName} (${names.length} unique fields)\n\n` +
    '| Field | Schema | Implementation |\n' +
    '|---|---|---|\n';
  for (const name of names) {
    const cppField = cppFields.get(name);
    const jsField = jsFields.get(name);
    const key = `${rootName}.${name}`;
    const schemaStatus =
      cppField && jsField
        ? cppField.signature === jsField.signature
          ? 'both'
          : 'both; signature differs'
        : cppField
          ? 'CrowdyCPP only'
          : 'CrowdyJS only';
    let status;
    if (usedFields.has(name)) {
      status = 'covered';
    } else if ((cppField ?? jsField)?.deprecated) {
      status = 'deprecated waiver';
      state.deprecated.push(`root:${key}`);
    } else {
      const expected = ROOT_CLASSIFICATIONS[key];
      if (expected) {
        state.usedRootClassifications.add(key);
        recordClassification(state, expected, `root:${key}`);
        status = renderClassification(expected);
      } else {
        status = '**UNCLASSIFIED GAP**';
        state.unclassified.push(`root:${key}`);
      }
    }
    report +=
      `| \`${escapeCell(name)}\` | ${escapeCell(schemaStatus)} | ` +
      `${escapeCell(status)} |\n`;
  }
  report += '\n';
}

const tsAll = collectTsClasses(crowdyjsPath);
const cppAll = cppClassMethods();
report += '## CrowdyJS class/method implementation\n\n';
for (const [tsClass, methods] of Object.entries(tsAll).sort(([left], [right]) =>
  left.localeCompare(right),
)) {
  const classClassification = CLASS_CLASSIFICATIONS[tsClass];
  const cppName = CLASS_MAP[tsClass] ?? tsClass;
  if (classClassification) {
    state.usedClassClassifications.add(tsClass);
  }
  const cppMethods = new Set((cppAll[cppName] ?? []).map(normalizeName));
  report += `### ${tsClass} -> ${cppName}\n\n`;
  report += '| CrowdyJS method | Status |\n|---|---|\n';
  for (const method of methods) {
    const key = `${tsClass}.${method}`;
    let status;
    if (cppMethods.has(normalizeName(METHOD_ALIASES[key] ?? method))) {
      status = 'covered';
    } else {
      const expected = METHOD_CLASSIFICATIONS[key] ?? classClassification;
      if (expected) {
        if (METHOD_CLASSIFICATIONS[key]) state.usedMethodClassifications.add(key);
        recordClassification(state, expected, `method:${key}`);
        status = renderClassification(expected);
      } else {
        status = '**UNCLASSIFIED GAP**';
        state.unclassified.push(`method:${key}`);
      }
    }
    report += `| \`${escapeCell(method)}\` | ${escapeCell(status)} |\n`;
  }
  report += '\n';
}

report += '## Strict-native surfaces not represented by one method\n\n';
for (const gap of STRICT_NATIVE_SURFACE_GAPS) {
  state.portable.push(`surface:${gap}`);
  report += `- **portable gap:** ${gap}\n`;
}
report += '\n';

checkStaleClassifications(state);

report += '## Summary\n\n';
report += `- Semantic schema differences: ${schemaDifferences.length}\n`;
report += `- Portable gap entries: ${state.portable.length}\n`;
report += `- Native-equivalent waivers: ${state.native.length}\n`;
report += `- Browser-only waivers: ${state.browser.length}\n`;
report += `- Deprecated waivers: ${state.deprecated.length}\n`;
report += `- Unclassified differences: ${state.unclassified.length}\n`;
report += `- Stale classifications: ${state.stale.length}\n\n`;
const portableByKind = countPrefixes(state.portable);
report +=
  `Portable breakdown: ${portableByKind.schema} schema, ` +
  `${portableByKind.root} roots, ${portableByKind.method} methods, ` +
  `${portableByKind.surface} cross-cutting surfaces.\n\n`;

report += '### Exact portable gaps\n\n';
for (const id of [...new Set(state.portable)].sort()) {
  report += `- \`${escapeInlineCode(id)}\`\n`;
}
report += '\n';

if (state.unclassified.length > 0) {
  report += '### Unclassified failures\n\n';
  for (const id of state.unclassified.sort()) report += `- \`${escapeInlineCode(id)}\`\n`;
  report += '\n';
}
if (state.stale.length > 0) {
  report += '### Stale baseline failures\n\n';
  for (const id of state.stale.sort()) report += `- \`${escapeInlineCode(id)}\`\n`;
  report += '\n';
}
report = `${report.trimEnd()}\n`;

if (options.write) {
  const path = repoPath(options.write);
  writeFileSync(path, report);
  console.log(`wrote ${relativeName(path)}`);
}
if (options.check) {
  const path = repoPath(options.check);
  if (!existsSync(path) || readFileSync(path, 'utf8') !== report) {
    state.unclassified.push(`generated-report:${relativeName(path)}`);
    console.error(
      `${relativeName(path)} is stale; regenerate with ` +
        `node tools/parity/parity.mjs --crowdyjs <path> --write ${relativeName(path)}`,
    );
  } else {
    console.log(`${relativeName(path)} is current`);
  }
}

console.log(
  [
    `schema differences=${schemaDifferences.length}`,
    `portable gaps=${state.portable.length}`,
    `native waivers=${state.native.length}`,
    `browser waivers=${state.browser.length}`,
    `unclassified=${state.unclassified.length}`,
    `stale=${state.stale.length}`,
  ].join(' '),
);

if (
  state.unclassified.length > 0 ||
  state.stale.length > 0 ||
  (options.strict && state.portable.length > 0)
) {
  process.exitCode = 1;
}

function parseArgs(args) {
  const parsed = {
    crowdyjs: null,
    write: null,
    check: null,
    strict: false,
  };
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === '--strict') {
      parsed.strict = true;
    } else if (['--crowdyjs', '--write', '--check'].includes(argument)) {
      const value = args[++index];
      if (!value) fail(`missing value for ${argument}`);
      parsed[argument.slice(2)] = value;
    } else {
      fail(`unknown argument: ${argument}`);
    }
  }
  if (parsed.write && parsed.check) fail('--write and --check are mutually exclusive');
  return parsed;
}

function classification(category, reason, expectedKind, expectedSide) {
  return { category, reason, expectedKind, expectedSide };
}

function classifyNames(rootName, names, category, reason) {
  return Object.fromEntries(
    names.map((name) => [`${rootName}.${name}`, classification(category, reason)]),
  );
}

function renderClassification(value) {
  if (value.category === CATEGORY.PORTABLE) return `portable gap — ${value.reason}`;
  if (value.category === CATEGORY.NATIVE) return `native equivalent — ${value.reason}`;
  return `browser exclusion — ${value.reason}`;
}

function recordClassification(state, value, id) {
  if (value.category === CATEGORY.PORTABLE) state.portable.push(id);
  else if (value.category === CATEGORY.NATIVE) state.native.push(id);
  else if (value.category === CATEGORY.BROWSER) state.browser.push(id);
  else state.unclassified.push(`${id} (unknown classification ${value.category})`);
}

function checkStaleClassifications(state) {
  for (const key of Object.keys(SCHEMA_BASELINE)) {
    if (!state.usedSchemaClassifications.has(key)) {
      state.stale.push(`schema:${key}`);
    }
  }
  for (const key of Object.keys(ROOT_CLASSIFICATIONS)) {
    if (!state.usedRootClassifications.has(key)) {
      state.stale.push(`root:${key}`);
    }
  }
  for (const key of Object.keys(METHOD_CLASSIFICATIONS)) {
    if (!state.usedMethodClassifications.has(key)) {
      state.stale.push(`method:${key}`);
    }
  }
  for (const key of Object.keys(CLASS_CLASSIFICATIONS)) {
    if (!state.usedClassClassifications.has(key)) {
      state.stale.push(`class:${key}`);
    }
  }
}

function operationRootFields() {
  const used = new Set();
  const operations = join(root, 'operations');
  for (const domain of readdirSync(operations).sort()) {
    const directory = join(operations, domain);
    if (!statSync(directory).isDirectory()) continue;
    for (const file of readdirSync(directory).sort()) {
      if (!file.endsWith('.graphql')) continue;
      collectRootFields(readFileSync(join(directory, file), 'utf8'), used);
    }
  }
  return used;
}

function inlineRootFields() {
  const used = new Set();
  walkFiles(join(root, 'include'), (path) => {
    if (!path.endsWith('.hpp')) return;
    const text = readFileSync(path, 'utf8');
    for (const match of text.matchAll(/R"gql\(([\s\S]*?)\)gql"/gu)) {
      collectRootFields(match[1], used);
    }
    const joined = text.replace(/"\s*\n\s*"/gu, '');
    for (const match of joined.matchAll(
      /"((?:query|mutation|subscription)\s+\w+[\s\S]*?)"/gu,
    )) {
      collectRootFields(match[1], used);
    }
  });
  return used;
}

/** Add each operation's root-level selections to `out`. */
function collectRootFields(document, out) {
  const tokens = documentTokens(document);
  let index = 0;
  while (index < tokens.length) {
    const kind = tokens[index];
    if (!['query', 'mutation', 'subscription'].includes(kind)) {
      index++;
      continue;
    }
    index++;
    if (isName(tokens[index])) index++;
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
    index = skipDirectives(tokens, index);
    if (tokens[index] !== '{') continue;
    const close = matchingToken(tokens, index, '{', '}');
    collectSelections(tokens.slice(index + 1, close), out);
    index = close + 1;
  }
}

function collectSelections(tokens, out) {
  let index = 0;
  while (index < tokens.length) {
    if (tokens[index] === '...') {
      index++;
      if (tokens[index] === 'on') index += 2;
      else index++;
      continue;
    }
    if (!isName(tokens[index])) {
      index++;
      continue;
    }
    let field = tokens[index++];
    if (tokens[index] === ':') {
      index++;
      field = tokens[index++];
    }
    if (field && !field.startsWith('__')) out.add(field);
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
    index = skipDirectives(tokens, index);
    if (tokens[index] === '{') {
      index = matchingToken(tokens, index, '{', '}') + 1;
    }
  }
}

function documentTokens(document) {
  const stripped = document
    .replace(/#[^\n]*/gu, ' ')
    .replace(/"""[\s\S]*?"""/gu, ' ')
    .replace(/"(?:\\.|[^"\\])*"/gu, '""');
  return stripped.match(/\.{3}|[_A-Za-z][_0-9A-Za-z]*|[$!():=@[\]{|}]/gu) ?? [];
}

function matchingToken(tokens, openIndex, open, close) {
  let depth = 0;
  for (let index = openIndex; index < tokens.length; index++) {
    if (tokens[index] === open) depth++;
    else if (tokens[index] === close) {
      depth--;
      if (depth === 0) return index;
    }
  }
  throw new Error(`unclosed GraphQL document ${open}`);
}

function skipGroup(tokens, index, open, close) {
  return matchingToken(tokens, index, open, close) + 1;
}

function skipDirectives(tokens, start) {
  let index = start;
  while (tokens[index] === '@') {
    index += 2;
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
  }
  return index;
}

function isName(value) {
  return typeof value === 'string' && /^[_A-Za-z][_0-9A-Za-z]*$/u.test(value);
}

function collectTsClasses(crowdyjs) {
  const all = {};
  const directories = [
    join(crowdyjs, 'src', 'domains'),
    join(crowdyjs, 'src', 'kit'),
    join(crowdyjs, 'src', 'stores'),
  ];
  for (const directory of directories) {
    mergeClasses(
      all,
      tsClassMethods(
        directory,
        readdirSync(directory).filter((file) => file.endsWith('.ts')),
      ),
    );
  }
  mergeClasses(all, tsClassMethods(join(crowdyjs, 'src'), ['world.ts']));
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'crowdy-studio'), ['controller.ts']),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'crowdy-agent'), [
      'browser-dispatcher.ts',
      'controller.ts',
      'graphql-transport.ts',
      'registry.ts',
    ]),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'player-host'), ['lease-manager.ts']),
  );
  return all;
}

function tsClassMethods(directory, files) {
  const classes = {};
  for (const file of files) {
    const path = join(directory, file);
    if (!existsSync(path)) continue;
    const text = readFileSync(path, 'utf8');
    const classPattern = /export\s+class\s+(\w+)[^{]*\{/gu;
    let classMatch;
    while ((classMatch = classPattern.exec(text)) !== null) {
      const name = classMatch[1];
      const body = balancedBlock(text, classPattern.lastIndex - 1);
      const methods = new Set();
      const methodPattern =
        /(?:^|\n)  (?!private\b|protected\b)(?:(?:public|static|async|readonly|get|set)\s+)*([A-Za-z_]\w*)\s*(?:<[^;\n{]+>)?\s*[(=<]/gu;
      for (const methodMatch of body.matchAll(methodPattern)) {
        const identifier = methodMatch[1];
        if (
          ![
            'constructor',
            'if',
            'for',
            'return',
            'switch',
            'while',
            'catch',
          ].includes(identifier)
        ) {
          methods.add(identifier);
        }
      }
      for (const groupMatch of body.matchAll(
        /readonly\s+(\w+)\s*=\s*\{([\s\S]*?)\n  \};/gu,
      )) {
        methods.delete(groupMatch[1]);
        for (const methodMatch of groupMatch[2].matchAll(
          /(?:^|\n)    ([A-Za-z_]\w*)\s*:\s*(?:async\s*)?\(/gu,
        )) {
          methods.add(`${groupMatch[1]}.${methodMatch[1]}`);
        }
      }
      if (methods.size > 0) classes[name] = [...methods].sort();
    }
  }
  return classes;
}

function cppClassMethods() {
  const classes = {};
  walkFiles(join(root, 'include', 'crowdy'), (path) => {
    if (!path.endsWith('.hpp')) return;
    const text = readFileSync(path, 'utf8');
    const classPattern = /(?:^|\n)class\s+(\w+)[^;{]*\{/gu;
    let classMatch;
    while ((classMatch = classPattern.exec(text)) !== null) {
      const name = classMatch[1];
      const body = balancedBlock(text, classPattern.lastIndex - 1);
      const methods = new Set();
      let visible = !/\n\s*public:/u.test(body);
      for (const rawLine of body.split('\n')) {
        const line = rawLine.trim();
        if (line === 'public:') {
          visible = true;
          continue;
        }
        if (line === 'private:' || line === 'protected:') {
          visible = false;
          continue;
        }
        if (!visible) continue;
        const methodMatch = rawLine.match(
          /^\s{2}[\w:<>&,*\s]+?[&*\s](\w+)\s*\(/u,
        );
        if (
          methodMatch &&
          !['if', 'for', 'while', 'switch', 'return', 'operator'].includes(
            methodMatch[1],
          ) &&
          methodMatch[1] !== name
        ) {
          methods.add(methodMatch[1]);
        }
      }
      if (methods.size > 0) {
        classes[name] = [
          ...new Set([...(classes[name] ?? []), ...methods]),
        ].sort();
      }
    }
  });
  return classes;
}

function balancedBlock(text, openBraceIndex) {
  let depth = 0;
  let quote = null;
  let escaped = false;
  let lineComment = false;
  let blockComment = false;
  for (let index = openBraceIndex; index < text.length; index++) {
    const character = text[index];
    const next = text[index + 1];
    if (lineComment) {
      if (character === '\n') lineComment = false;
      continue;
    }
    if (blockComment) {
      if (character === '*' && next === '/') {
        blockComment = false;
        index++;
      }
      continue;
    }
    if (quote) {
      if (!escaped && character === quote) quote = null;
      escaped = !escaped && character === '\\';
      if (character !== '\\') escaped = false;
      continue;
    }
    if (character === '/' && next === '/') {
      lineComment = true;
      index++;
      continue;
    }
    if (character === '/' && next === '*') {
      blockComment = true;
      index++;
      continue;
    }
    if (character === '"' || character === "'" || character === '`') {
      quote = character;
      escaped = false;
      continue;
    }
    if (character === '{') depth++;
    else if (character === '}') {
      depth--;
      if (depth === 0) return text.slice(openBraceIndex + 1, index);
    }
  }
  return text.slice(openBraceIndex + 1);
}

function mergeClasses(target, source) {
  for (const [name, methods] of Object.entries(source)) {
    target[name] = [...new Set([...(target[name] ?? []), ...methods])].sort();
  }
}

function walkFiles(directory, callback) {
  for (const entry of readdirSync(directory).sort()) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) walkFiles(path, callback);
    else callback(path);
  }
}

function normalizeName(value) {
  return value.toLowerCase().replace(/[^a-z0-9]/gu, '');
}

function gitRevision(directory) {
  try {
    return execFileSync('git', ['rev-parse', 'HEAD'], {
      cwd: directory,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch {
    return 'unknown';
  }
}

function repoPath(path) {
  return isAbsolute(path) ? path : join(root, path);
}

function relativeName(path) {
  return path.startsWith(`${root}/`) ? path.slice(root.length + 1) : path;
}

function escapeCell(value) {
  return String(value).replace(/\|/gu, '\\|').replace(/\n/gu, ' ');
}

function escapeInlineCode(value) {
  return String(value).replace(/`/gu, '\\`');
}

function countPrefixes(values) {
  const counts = { schema: 0, root: 0, method: 0, surface: 0 };
  for (const value of values) {
    const prefix = String(value).split(':', 1)[0];
    if (prefix in counts) counts[prefix]++;
  }
  return counts;
}

function fail(message) {
  console.error(message);
  process.exit(2);
}
