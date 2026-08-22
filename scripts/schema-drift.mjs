#!/usr/bin/env node
/**
 * Reports when the committed `schema.gql` snapshot has fallen behind the PUBLISHED SDL.
 *
 *   node scripts/schema-drift.mjs                 # print the report, exit 0/1
 *   node scripts/schema-drift.mjs --issue         # also open or update the tracking issue
 *
 * WHY THIS IS A SEPARATE, SCHEDULED JOB. Asking "is our snapshot current?" is a real and
 * necessary question, but the answer is a fact about the WORLD, not about a commit -- so it
 * cannot live in per-commit CI without making a branch's verdict move underneath it. It did
 * live there until 2026-08-22, and on 2026-08-21 a CODEOWNERS-only commit went red two hours
 * after it was pushed because `cks-docs` published a new SDL in between. Nothing the author
 * could do to their branch would have made it green, and nothing they did made it red.
 *
 * The split is: `schema-sync.mjs --check --offline` gates the commit, this reports on the
 * world. See the long comment at the top of schema-sync.mjs for the pinning argument.
 *
 * WHAT MAKES A DRIFT REPORT WORTH READING. A job that fires to say two files differ becomes
 * the red nobody reads, and then the one real drift is invisible among the noise. So this
 * one is required to answer three questions every time it fires:
 *
 *   WHO PUBLISHED IT   -- resolved, not guessed. The bytes served are hashed as a git blob
 *                         and looked up in `cks-docs`' own history for
 *                         `static/schema/game-api.graphql`, which names the exact commit,
 *                         its author and its subject line. Both repos are public, so this
 *                         needs no credential beyond the default token.
 *   WHAT CHANGED       -- type and field names, added and removed, not a line count.
 *   WHAT TO RUN        -- the two commands, in order, that close it.
 *
 * It also distinguishes a drift that MATTERS from one that does not. The snapshot is a
 * NORMALISED copy, so an upstream republish that only reorders fields or edits a
 * description changes the served bytes and changes nothing we compile against. That case
 * is reported as informational and does not fail: treating it as urgent is how a signal
 * gets trained out of a team.
 */
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mergeTypeDefs } from '@graphql-tools/merge';
import { parse, print } from 'graphql';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const pkg = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8'));
const snapshot = readFileSync(resolve(root, 'schema.gql'), 'utf8');
const provenance = pkg.publishedSchemaSnapshot;

const DOCS_REPO = 'CrowdedKingdoms/cks-docs';
const DOCS_PATH = 'static/schema/game-api.graphql';
const SELF = process.env.GITHUB_REPOSITORY || 'CrowdedKingdoms/CrowdyCPP';
const ISSUE_MARKER = '<!-- schema-drift -->';

const gitBlob = (text) => {
  const body = Buffer.from(text, 'utf8');
  return createHash('sha1')
    .update(Buffer.concat([Buffer.from(`blob ${body.length}\0`), body]))
    .digest('hex');
};

async function gh(path, method = 'GET', body = null) {
  const token = process.env.GITHUB_TOKEN || process.env.GH_TOKEN;
  const res = await fetch(`https://api.github.com${path}`, {
    method,
    headers: {
      Accept: 'application/vnd.github+json',
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      'Content-Type': 'application/json',
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok && res.status !== 404) {
    throw new Error(`GET ${path} -> HTTP ${res.status} ${(await res.text()).slice(0, 200)}`);
  }
  return res.status === 404 ? null : res.json();
}

const findIssue = async () => {
  const found = await gh(
    `/search/issues?q=${encodeURIComponent(`repo:${SELF} is:issue is:open "${ISSUE_MARKER}"`)}`,
  );
  return found?.items?.[0] ?? null;
};

// Resolve served bytes to the commit that produced them. An unmatched blob is reported as
// unmatched rather than guessed at: "the newest commit touching the path" is usually right
// and is not the same claim, and a report that quietly substitutes one for the other is
// how an attribution becomes folklore.
async function attribute(blob) {
  const commits = await gh(`/repos/${DOCS_REPO}/commits?sha=prod&path=${DOCS_PATH}&per_page=20`);
  for (const c of commits ?? []) {
    const meta = await gh(`/repos/${DOCS_REPO}/contents/${DOCS_PATH}?ref=${c.sha}`);
    if (meta?.sha === blob) {
      return {
        matched: true,
        sha: c.sha,
        subject: c.commit.message.split('\n')[0],
        author: c.commit.author.name,
        date: c.commit.author.date,
      };
    }
  }
  return { matched: false, newest: commits?.[0] ?? null };
}

// Type and field names, so the report names what moved. Descriptions and directives are
// deliberately out of scope: they churn constantly and they are not what a C++ consumer
// compiles against.
function surface(sdl) {
  const map = new Map();
  for (const def of parse(sdl).definitions) {
    if (!def.name) continue;
    map.set(
      def.name.value,
      new Set((def.fields ?? def.values ?? []).map((f) => f.name.value)),
    );
  }
  return map;
}

function compareSurface(before, after) {
  const addedTypes = [...after.keys()].filter((t) => !before.has(t));
  const removedTypes = [...before.keys()].filter((t) => !after.has(t));
  const changed = [];
  for (const [type, fields] of after) {
    if (!before.has(type)) continue;
    const was = before.get(type);
    const add = [...fields].filter((f) => !was.has(f));
    const rem = [...was].filter((f) => !fields.has(f));
    if (add.length || rem.length) changed.push({ type, add, rem });
  }
  return { addedTypes, removedTypes, changed };
}

const bullet = (items, cap = 12) => {
  const shown = items.slice(0, cap).map((s) => `- ${s}`);
  if (items.length > cap) shown.push(`- …and ${items.length - cap} more`);
  return shown.join('\n');
};

// A local path is accepted as a source so this job can be REHEARSED against a file that
// looks like tomorrow's publish. The first attempt to rehearse it had to edit package.json
// to point the tool at a fixture, which tests a configuration nobody runs.
async function loadSource(src) {
  if (!/^https?:\/\//.test(src)) {
    return { text: readFileSync(src, 'utf8'), lastModified: `local file ${src}` };
  }
  const res = await fetch(src);
  if (!res.ok) throw new Error(`fetch ${src} failed: HTTP ${res.status}`);
  return { text: await res.text(), lastModified: res.headers.get('last-modified') ?? 'unknown' };
}

async function main() {
  const i = process.argv.indexOf('--source');
  const url = i === -1
    ? provenance?.url ?? 'https://docs.crowdedkingdoms.com/schema/game-api.graphql'
    : process.argv[i + 1];
  const { text: served, lastModified } = await loadSource(url);
  const servedBlob = gitBlob(served);

  if (servedBlob === provenance?.sourceBlob) {
    console.log(`schema.gql is current.\n  served blob ${servedBlob} == recorded sourceBlob`);
    // Closing is part of the job. An issue that stays open after the thing it describes is
    // fixed trains everybody to ignore the next one, which costs exactly as much as never
    // having filed it -- and unlike a red run, nothing else ever clears it.
    if (process.argv.includes('--issue')) {
      const open = await findIssue();
      if (open) {
        await gh(`/repos/${SELF}/issues/${open.number}/comments`, 'POST', {
          body: `Closed automatically: the snapshot now matches the published SDL (served blob \`${servedBlob.slice(0, 12)}\`).`,
        });
        await gh(`/repos/${SELF}/issues/${open.number}`, 'PATCH', { state: 'closed' });
        console.log(`closed issue #${open.number}`);
      }
    }
    return 0;
  }

  const wouldWrite = print(mergeTypeDefs([served])) + '\n';
  const material = wouldWrite !== snapshot;
  const who = await attribute(servedBlob);

  const lines = [];
  lines.push(material
    ? '**The published SDL has moved and the snapshot must be synced.**'
    : '**The published SDL was republished, but nothing we compile against changed.** '
      + 'Formatting or descriptions only; syncing is optional.');
  lines.push('');
  lines.push('| | |');
  lines.push('|---|---|');
  lines.push(`| Served | \`${servedBlob.slice(0, 12)}\`, last modified ${lastModified} |`);
  lines.push(`| Our snapshot synced from | \`${(provenance?.sourceBlob ?? 'none').slice(0, 12)}\` on ${provenance?.syncedAt ?? 'never'} |`);
  if (who.matched) {
    lines.push(`| Published by | [\`${who.sha.slice(0, 8)}\`](https://github.com/${DOCS_REPO}/commit/${who.sha}) — ${who.subject} |`);
    lines.push(`| Author, date | ${who.author}, ${who.date} |`);
  } else {
    lines.push(`| Published by | **unresolved** — the served bytes match no \`${DOCS_PATH}\` blob in the last 20 \`prod\` commits. Either it was published from somewhere other than \`cks-docs\` \`prod\`, or the deploy is older than that window. |`);
    if (who.newest) lines.push(`| Newest commit on that path | \`${who.newest.sha.slice(0, 8)}\` ${who.newest.commit.message.split('\n')[0]} (this is context, **not** the attribution) |`);
  }
  lines.push('');

  if (material) {
    const diff = compareSurface(surface(snapshot), surface(wouldWrite));
    lines.push('### What changed');
    if (diff.addedTypes.length) lines.push(`**Types added**\n${bullet(diff.addedTypes)}`);
    if (diff.removedTypes.length) lines.push(`**Types removed**\n${bullet(diff.removedTypes)}`);
    if (diff.changed.length) {
      lines.push('**Fields changed**');
      lines.push(bullet(diff.changed.map(({ type, add, rem }) =>
        `\`${type}\`: ${[...add.map((f) => `+${f}`), ...rem.map((f) => `-${f}`)].join(', ')}`)));
    }
    if (!diff.addedTypes.length && !diff.removedTypes.length && !diff.changed.length) {
      lines.push('No type or field names moved — the change is in argument types, defaults or nullability.');
    }
    lines.push('');
    lines.push('### What to run');
    lines.push('```bash');
    lines.push('npm ci');
    lines.push('node scripts/schema-sync.mjs   # rewrites schema.gql and its provenance record');
    lines.push('node scripts/codegen.mjs       # regenerates include/crowdy/generated/');
    lines.push('```');
    lines.push('Commit both, on `dev`. `schema-sync.mjs --check --offline` in CI will refuse the');
    lines.push('first without the second, because the provenance record and the snapshot are');
    lines.push('written in the same act.');
  }

  const report = lines.join('\n');
  console.log(report);

  if (process.argv.includes('--issue')) {
    const title = material
      ? 'schema.gql is behind the published SDL'
      : 'published SDL republished (no material change)';
    const existing = await findIssue();
    const body = `${ISSUE_MARKER}\n${report}\n\n<sub>Opened by \`scripts/schema-drift.mjs\` on a schedule. It updates this issue rather than opening a new one each day; it closes it when the snapshot catches up.</sub>`;
    if (existing) {
      await gh(`/repos/${SELF}/issues/${existing.number}`, 'PATCH', { title, body });
      console.log(`\nupdated issue #${existing.number}`);
    } else {
      const made = await gh(`/repos/${SELF}/issues`, 'POST', { title, body, labels: ['schema-drift'] });
      console.log(`\nopened issue #${made.number}`);
    }
  }

  // Informational republishes do not fail. Only a snapshot that must move does.
  return material ? 1 : 0;
}

// COULD NOT RUN is a third outcome and it gets its own code. The first rehearsal of this
// script threw on an unreachable source and exited 1 -- the same code as "the snapshot is
// behind" -- so a scheduled job that had failed to answer the question would have been
// indistinguishable from one that answered it badly, and the issue it files says something
// specific and wrong. 0 current, 1 drifted, 2 the job broke.
try {
  process.exitCode = await main();
} catch (err) {
  console.error(`schema-drift could not run: ${err.message}`);
  console.error('This is NOT a drift report. Nothing is known about the snapshot either way.');
  process.exitCode = 2;
}
