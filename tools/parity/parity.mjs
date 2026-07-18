#!/usr/bin/env node
/**
 * Parity audit: proves CrowdyCPP covers the full public GraphQL surface and
 * every CrowdyJS abstraction layer, or names the gap.
 *
 *  1. Schema surface: every non-deprecated root field in schema.gql must be
 *     used by an operation document, an inline document in the C++ headers,
 *     or carry an explicit waiver.
 *  2. Method surface: every public method on CrowdyJS's domain/kit/stores/
 *     world classes must map to a CrowdyCPP method (alias table for
 *     idiomatic renames) or carry a waiver.
 *
 * Usage:
 *   node tools/parity/parity.mjs [--crowdyjs <path-to-CrowdyJS-checkout>]
 *                                [--write docs/parity-matrix.md]
 *
 * The method diff needs a CrowdyJS checkout (public repo); without one, only
 * the schema surface section is produced.
 */
import { readFileSync, readdirSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

const args = process.argv.slice(2);
function argOf(flag, fallback) {
  const i = args.indexOf(flag);
  return i >= 0 ? args[i + 1] : fallback;
}
const crowdyjsPath = argOf('--crowdyjs', join(root, '..', 'CrowdyJS'));
const writePath = argOf('--write', null);

// ---------------------------------------------------------------------------
// Waivers: root fields intentionally not wrapped, with the reason.
// ---------------------------------------------------------------------------
const FIELD_WAIVERS = {
  // Replication in CrowdyCPP is native UDP directly with the replication
  // server, always. The GraphQL UDP proxy exists for clients that cannot
  // open UDP sockets (browsers -> CrowdyJS).
  connectUdpProxy: 'browser proxy path; CrowdyCPP replicates natively over UDP',
  disconnectUdpProxy: 'browser proxy path; CrowdyCPP replicates natively over UDP',
  udpProxyConnectionStatus: 'browser proxy path; CrowdyCPP replicates natively over UDP',
  sendActorUpdate: 'proxy send; native ACTOR_UPDATE_REQUEST instead',
  sendVoxelUpdate: 'proxy send; native VOXEL_UPDATE_REQUEST instead',
  sendAudioPacket: 'proxy send; native CLIENT_AUDIO_PACKET instead',
  sendTextPacket: 'proxy send; native CLIENT_TEXT_PACKET instead',
  sendClientEvent: 'proxy send; native CLIENT_EVENT_NOTIFICATION instead',
  sendSingleActorMessage: 'proxy send; native SINGLE_ACTOR_MESSAGE instead',
  sendChannelMessage: 'proxy send; native CHANNEL_MESSAGE_REQUEST instead',
  udpNotifications: 'browser realtime stream; native UDP notifications instead',
};

// CrowdyJS members that have no C++ counterpart by design.
const METHOD_WAIVERS = {
  'UdpAPI.*': 'whole domain waived: browser proxy path; native replication client instead',
  'BrowserSessionPkceStore.*': 'browser sessionStorage helper; native flows keep the verifier in memory',
  'AuthAPI.setToken': 'covered by CrowdyClient::setToken / auth().setToken',
  'AuthAPI.getToken': 'covered by CrowdyClient::getToken / auth().getToken',
  'WorldClient.subscribe': 'native: Connection::setHandlers / WorldSession',
  'ActorClient.subscribe': 'native: Connection::setHandlers / WorldSession',
  'PortalAPI.handleAuthorizeRequest': 'Overworld browser page; native side uses createAuthorizationCode',
  'WorldSessionCore.on': 'C++ WorldSession is tick-driven; handlers are installed on the Connection',
  'WorldSessionCore.onDispose': 'C++ WorldSession is tick-driven; RAII disposal',
  'WorldSessionCore.setSendTracker': 'internal wiring; ErrorStore::recordSend is called by WorldSession::tick',
  'WorldSessionCore.trackSend': 'internal wiring; ErrorStore::recordSend is called by WorldSession::tick',
  'HostTracker.beat': 'internal; WorldSession::tick heartbeats on the configured interval',
  'LocalActorStore.patchState': 'byte codec: mutate the state struct and call setState',
  'SocialKit.chat.onMessage': 'receive chat via WorldSession channelInbox / Connection channel handler',
};

// Idiomatic renames: `TsClass.tsMethod` -> C++ method name (same class).
const METHOD_ALIASES = {
  'ActorsAPI.delete': 'remove',
  'AvatarsAPI.delete': 'remove',
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
};

// ---------------------------------------------------------------------------
// 1. Schema root fields
// ---------------------------------------------------------------------------
function schemaRootFields(schemaText) {
  const fields = { Query: [], Mutation: [], Subscription: [] };
  for (const rootName of Object.keys(fields)) {
    const re = new RegExp(`\\ntype ${rootName} \\{([\\s\\S]*?)\\n\\}`, 'g');
    let m;
    while ((m = re.exec(schemaText)) !== null) {
      // Strip docstrings, then take field names at nesting depth 1.
      const body = m[1].replace(/"""[\s\S]*?"""/g, ' ').replace(/"[^"\n]*"/g, ' ');
      let depth = 0;
      for (const rawLine of body.split('\n')) {
        const line = rawLine.trim();
        if (depth === 0) {
          const fm = line.match(/^([a-zA-Z_]\w*)\s*[(:]/);
          if (fm) {
            const deprecated = /@deprecated/.test(line);
            fields[rootName].push({ name: fm[1], deprecated });
          }
        }
        for (const ch of rawLine) {
          if (ch === '(') depth++;
          else if (ch === ')') depth--;
        }
      }
    }
    // A field may span lines until its closing paren carries @deprecated;
    // do a second pass for deprecations anywhere near the field name.
    for (const f of fields[rootName]) {
      const depRe = new RegExp(`${f.name}[\\s\\S]{0,2000}?(?:@deprecated)[\\s\\S]{0,100}?`);
      // Keep the simple per-line detection; refine only if needed.
      void depRe;
    }
  }
  return fields;
}

// Root fields referenced by the committed operation documents.
function operationRootFields() {
  const used = new Set();
  const opsDir = join(root, 'operations');
  for (const domain of readdirSync(opsDir)) {
    const dir = join(opsDir, domain);
    if (!statSync(dir).isDirectory()) continue;
    for (const file of readdirSync(dir)) {
      if (!file.endsWith('.graphql')) continue;
      collectRootFields(readFileSync(join(dir, file), 'utf8'), used);
    }
  }
  return used;
}

// Root fields referenced by inline documents in the C++ headers.
function inlineRootFields() {
  const used = new Set();
  const walk = (dir) => {
    for (const entry of readdirSync(dir)) {
      const p = join(dir, entry);
      if (statSync(p).isDirectory()) walk(p);
      else if (entry.endsWith('.hpp')) {
        const text = readFileSync(p, 'utf8');
        const re = /"(?:query|mutation)\s+\w+[^"]*"/g;
        // Inline docs are split across string literals; join adjacent ones.
        const joined = text.replace(/"\s*\n\s*"/g, '');
        for (const m of joined.matchAll(/"((?:query|mutation)\s+\w+[\s\S]*?)"/g)) {
          collectRootFields(m[1], used);
        }
        void re;
      }
    }
  };
  walk(join(root, 'include'));
  return used;
}

/** Add every root-level selection name of every operation in `doc`. */
function collectRootFields(doc, out) {
  // Walk the whole document tracking brace/paren depth. Depth 1 inside an
  // operation's selection set is the root level; collect the first
  // identifier of each selection there (skipping fragments and directives).
  let braceDepth = 0;
  let parenDepth = 0;
  let i = 0;
  let expectField = false;
  while (i < doc.length) {
    const ch = doc[i];
    if (ch === '{') {
      braceDepth++;
      if (braceDepth === 1) expectField = true;
      i++;
      continue;
    }
    if (ch === '}') {
      braceDepth--;
      i++;
      continue;
    }
    if (ch === '(') { parenDepth++; i++; continue; }
    if (ch === ')') { parenDepth--; i++; continue; }
    if (ch === '#') {  // comment to end of line
      while (i < doc.length && doc[i] !== '\n') i++;
      continue;
    }
    if (braceDepth === 1 && parenDepth === 0 && /[a-zA-Z_]/.test(ch)) {
      let j = i;
      while (j < doc.length && /[\w]/.test(doc[j])) j++;
      const word = doc.slice(i, j);
      // Aliases: `alias: field` — take the field, not the alias.
      let k = j;
      while (k < doc.length && /\s/.test(doc[k])) k++;
      if (doc[k] === ':') {
        i = k + 1;  // skip to the real field name
        continue;
      }
      if (!word.startsWith('__') && word !== 'fragment' && word !== 'on') out.add(word);
      i = j;
      continue;
    }
    i++;
  }
}

// ---------------------------------------------------------------------------
// 2. Method surfaces
// ---------------------------------------------------------------------------

/** Public methods per exported TS class (including nested object literals). */
function tsClassMethods(dir, files) {
  const classes = {};
  for (const file of files) {
    const p = join(dir, file);
    if (!existsSync(p)) continue;
    const text = readFileSync(p, 'utf8');
    const classRe = /export class (\w+)[^{]*\{/g;
    let cm;
    while ((cm = classRe.exec(text)) !== null) {
      const name = cm[1];
      const body = balancedBlock(text, classRe.lastIndex - 1);
      const methods = new Set();
      // Plain + async methods at depth 1.
      for (const mm of body.matchAll(/(?:^|\n)  (?:async |readonly )?([a-zA-Z_]\w*)\s*[(=<]/g)) {
        const id = mm[1];
        if (['constructor', 'private', 'protected', 'static', 'get', 'set', 'if', 'for',
             'return', 'switch', 'while', 'catch', 'readonly', 'async'].includes(id)) continue;
        methods.add(id);
      }
      // Nested object-literal members: `name: async (...)` under a readonly
      // group (e.g. economy shop/trades/market) -> group.member.
      for (const gm of body.matchAll(/readonly (\w+) = \{([\s\S]*?)\n  \};/g)) {
        methods.delete(gm[1]);
        for (const mm of gm[2].matchAll(/(?:^|\n)    ([a-zA-Z_]\w*)\s*:\s*(?:async )?\(/g)) {
          methods.add(`${gm[1]}.${mm[1]}`);
        }
      }
      if (methods.size > 0) classes[name] = [...methods].sort();
    }
  }
  return classes;
}

/** Public methods per C++ class in the headers (public sections only). */
function cppClassMethods() {
  const classes = {};
  const walk = (dir) => {
    for (const entry of readdirSync(dir)) {
      const p = join(dir, entry);
      if (statSync(p).isDirectory()) walk(p);
      else if (entry.endsWith('.hpp')) parseHeader(p);
    }
  };
  const parseHeader = (p) => {
    const text = readFileSync(p, 'utf8');
    const classRe = /class (\w+)[^;{]*\{/g;
    let cm;
    while ((cm = classRe.exec(text)) !== null) {
      const name = cm[1];
      const body = balancedBlock(text, classRe.lastIndex - 1);
      const methods = new Set();
      let visible = /\n public:/.test(body) ? false : true;
      for (const rawLine of body.split('\n')) {
        const line = rawLine.trim();
        if (line === 'public:') { visible = true; continue; }
        if (line === 'private:' || line === 'protected:') { visible = false; continue; }
        if (!visible) continue;
        const mm = rawLine.match(/^  [\w:<>&,*\s]+?[&*\s](\w+)\(/);
        if (mm && !['if', 'for', 'while', 'switch', 'return', 'operator'].includes(mm[1]) &&
            mm[1] !== name) {
          methods.add(mm[1]);
        }
      }
      if (methods.size > 0) {
        classes[name] = [...new Set([...(classes[name] ?? []), ...methods])].sort();
      }
    }
  };
  walk(join(root, 'include', 'crowdy'));
  return classes;
}

function balancedBlock(text, openBraceIndex) {
  let depth = 0;
  for (let i = openBraceIndex; i < text.length; ++i) {
    if (text[i] === '{') depth++;
    else if (text[i] === '}') {
      depth--;
      if (depth === 0) return text.slice(openBraceIndex + 1, i);
    }
  }
  return text.slice(openBraceIndex + 1);
}

const norm = (s) => s.toLowerCase().replace(/[^a-z0-9]/g, '');

// TS class -> C++ class mapping (same-named unless listed).
const CLASS_MAP = {
  AuthAPI: 'AuthAPI', UsersAPI: 'UsersAPI', PortalAPI: 'PortalAPI',
  ServerStatusAPI: 'ServerStatusAPI', ChunksAPI: 'ChunksAPI', VoxelsAPI: 'VoxelsAPI',
  ActorsAPI: 'ActorsAPI', AvatarsAPI: 'AvatarsAPI', StateAPI: 'StateAPI', HostAPI: 'HostAPI',
  TeleportAPI: 'TeleportAPI', TeamsAPI: 'TeamsAPI', ChannelsAPI: 'ChannelsAPI',
  GameModelAPI: 'GameModelAPI', GameAppsAPI: 'GameAppsAPI', PlatformAPI: 'PlatformAPI',
  OrganizationsAPI: 'OrganizationsAPI', AppsAPI: 'AppsAPI', AppAccessAPI: 'AppAccessAPI',
  BillingAPI: 'BillingAPI', PaymentsAPI: 'PaymentsAPI', QuotasAPI: 'QuotasAPI',
  EnvironmentsAPI: 'EnvironmentsAPI', UsageAPI: 'UsageAPI',
  SharedEnvironmentAPI: 'SharedEnvironmentAPI', ControlPlaneAPI: 'OperatorAPI',
  AdminAPI: 'AdminAPI', UdpAPI: null /* waived */, WorldClient: 'WorldClient',
  ActorClient: 'ActorClient', GameKitClient: 'GameKitClient',
  InventoryKit: 'InventoryKit', ObjectsKit: 'ObjectsKit', NpcsKit: 'NpcsKit',
  PlotsKit: 'PlotsKit', EconomyKit: 'EconomyKit', ProgressionKit: 'ProgressionKit',
  LootKit: 'LootKit', QuestsKit: 'QuestsKit', CombatKit: 'CombatKit',
  MatchesKit: 'MatchesKit', DecksKit: 'DecksKit', WorldsimKit: 'WorldsimKit',
  SocialKit: 'SocialKit', LeaderboardsKit: 'LeaderboardsKit', FeaturesKit: 'FeaturesKit',
  LocalActorStore: 'LocalActorStore', RemoteActorStore: 'RemoteActorStore',
  ChannelInbox: 'Inbox', ActorInbox: 'Inbox', EventRouter: 'EventRouter',
  HostTracker: 'WorldSession', SaveStateStore: 'SaveStateStore',
  AvatarStateStore: 'AvatarStateStore', ContainerMirror: 'ContainerMirror',
  WorldSessionCore: 'WorldSession', ChunkStore: 'ChunkStore', ErrorStore: 'ErrorStore',
  RemoteActorLane: 'RemoteActorLane',
};

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
const schema = readFileSync(join(root, 'schema.gql'), 'utf8');
const rootFields = schemaRootFields(schema);
const usedFields = operationRootFields();
for (const f of inlineRootFields()) usedFields.add(f);

let report = '# CrowdyCPP parity matrix\n\n';
report += 'Generated by `tools/parity/parity.mjs`. Regenerate after any surface change.\n\n';

// Section 1: schema coverage.
report += '## GraphQL root-field coverage\n\n';
let missingFields = 0;
for (const rootName of ['Query', 'Mutation', 'Subscription']) {
  const rows = [];
  for (const f of rootFields[rootName]) {
    let status;
    if (usedFields.has(f.name)) status = 'covered';
    else if (FIELD_WAIVERS[f.name]) status = `waived: ${FIELD_WAIVERS[f.name]}`;
    else if (f.deprecated) status = 'waived: deprecated';
    else {
      status = '**MISSING**';
      missingFields++;
    }
    rows.push(`| \`${f.name}\` | ${status} |`);
  }
  report += `### ${rootName} (${rootFields[rootName].length})\n\n| Field | Status |\n|---|---|\n${rows.join('\n')}\n\n`;
}

// Section 2: method coverage.
if (existsSync(crowdyjsPath)) {
  const tsDomains = tsClassMethods(join(crowdyjsPath, 'src', 'domains'),
      readdirSync(join(crowdyjsPath, 'src', 'domains')));
  const tsKit = tsClassMethods(join(crowdyjsPath, 'src', 'kit'),
      readdirSync(join(crowdyjsPath, 'src', 'kit')).filter((f) => f.endsWith('.ts')));
  const tsStores = tsClassMethods(join(crowdyjsPath, 'src', 'stores'),
      readdirSync(join(crowdyjsPath, 'src', 'stores')));
  const tsWorld = tsClassMethods(join(crowdyjsPath, 'src'), ['world.ts']);
  const tsAll = { ...tsDomains, ...tsKit, ...tsStores, ...tsWorld };
  const cppAll = cppClassMethods();

  report += '## CrowdyJS class/method coverage\n\n';
  let missingMethods = 0;
  for (const [tsClass, methods] of Object.entries(tsAll).sort()) {
    const target = CLASS_MAP[tsClass];
    if (target === null || METHOD_WAIVERS[`${tsClass}.*`]) {
      report += `### ${tsClass} — waived (${METHOD_WAIVERS[`${tsClass}.*`] ?? 'no C++ counterpart by design'})\n\n`;
      continue;
    }
    const cppMethods = new Set((cppAll[target ?? tsClass] ?? []).map(norm));
    const rows = [];
    for (const m of methods) {
      const key = `${tsClass}.${m}`;
      let status;
      if (METHOD_WAIVERS[key]) status = `waived: ${METHOD_WAIVERS[key]}`;
      else if (cppMethods.has(norm(METHOD_ALIASES[key] ?? m))) status = 'covered';
      else {
        status = '**MISSING**';
        missingMethods++;
      }
      rows.push(`| \`${m}\` | ${status} |`);
    }
    const cppName = target ?? tsClass;
    report += `### ${tsClass} -> ${cppName}\n\n| CrowdyJS method | Status |\n|---|---|\n${rows.join('\n')}\n\n`;
  }
  report += `\n**Missing methods: ${missingMethods}**\n`;
} else {
  report += `## CrowdyJS class/method coverage\n\nSkipped: no CrowdyJS checkout at ${crowdyjsPath}.\n`;
}

report += `\n**Missing root fields: ${missingFields}**\n`;

if (writePath) {
  writeFileSync(join(root, writePath), report);
  console.log(`wrote ${writePath}`);
}
const summary = report.split('\n').filter((l) => l.includes('MISSING') || l.startsWith('**'));
console.log(summary.join('\n') || 'no gaps');
