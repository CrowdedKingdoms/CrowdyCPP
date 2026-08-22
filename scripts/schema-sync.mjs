#!/usr/bin/env node
/**
 * Owns the committed `schema.gql` snapshot and its provenance record.
 *
 *   node scripts/schema-sync.mjs --check --offline   # per-commit CI gate, no network
 *   node scripts/schema-sync.mjs --check             # is the snapshot behind the live SDL?
 *   node scripts/schema-sync.mjs                     # sync from the live SDL, record provenance
 *
 * WHY --offline EXISTS, and why CI uses it. Until 2026-08-22 the per-commit CI gate ran
 * `--check` against the LIVE published SDL, which made a branch's verdict a property of the
 * world at the moment the job ran rather than a property of its commit. The same commit
 * passed and then failed with nothing about it changed: publishing `cks-docs prod/v0.1.1`
 * moved the served SDL, and CrowdyCPP's CI went red two hours later on a commit that had
 * touched CODEOWNERS. A verdict nobody can act on by changing their own branch is not a
 * gate -- it is a notification wearing a gate's costume, and it recurs on every publish.
 *
 * So the comparison is pinned. `schema.gql` IS the pinned artifact: the snapshot every
 * external build compiles against. CI now asserts two things about it that are true or
 * false of the commit alone, and asks the network nothing:
 *
 *   1. CANONICAL FORM -- re-normalising the committed file reproduces it byte for byte.
 *      Catches a hand-edit that is still valid SDL but is not what this tool would write.
 *   2. PROVENANCE -- its sha256 matches `publishedSchemaSnapshot.snapshotSha256` in
 *      package.json. Catches a hand-edit that happens to be canonical, such as deleting a
 *      field, which check 1 alone would pass.
 *
 * Neither can be broken by anybody else publishing anything, which is the whole point.
 * Whether the snapshot is BEHIND the live SDL is a real question and still asked -- by
 * `.github/workflows/schema-drift.yml`, on a schedule, where the answer is a report that
 * names who published and what to run rather than a red X on an innocent commit.
 *
 * THE HAZARD THIS SHAPE INHERITS is the committed generated file the build never
 * regenerates -- the same one being closed in `cks-docs` right now, where the generated
 * reference pages sat an hour behind the hand-written ones and shipped `Throws CONFLICT`
 * to customers after it had stopped being true. A committed artifact only stays honest if
 * something regenerates it and something else notices when nothing has. Here that is the
 * pair: `codegen.mjs --check` proves the C++ is regenerated FROM this file on every
 * commit, and the scheduled drift job proves somebody is told when the file itself falls
 * behind its upstream. Removing either one leaves a snapshot that can rot in silence.
 *
 * There is ONE schema because there is one API at one origin. This script used
 * to keep a second snapshot, schema.management.gql, so codegen could label each
 * operation Management/Game/Both. That plane distinction is gone as of 0.20.0,
 * and keeping the snapshot would have been worse than useless: the published
 * management SDL is now DERIVED from the unified schema by filtering it to a
 * root-field allowlist, so it is a strict subset. Validating against a subset
 * answers "is this operation in the management docs tab", not "will the server
 * accept it" — and every operation outside the allowlist would have been
 * labelled Game by a check that had stopped describing a real endpoint.
 *
 * Requires the maintainer-only Node dependencies (`npm ci`). Normal CMake
 * builds never invoke this script. Override the source with --game <path|url>.
 */
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mergeTypeDefs } from '@graphql-tools/merge';
import { print } from 'graphql';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SNAPSHOT = resolve(root, 'schema.gql');
const PACKAGE = resolve(root, 'package.json');

const DEFAULTS = {
  game: 'https://docs.crowdedkingdoms.com/schema/game-api.graphql',
};

function parseArgs(argv) {
  const args = { ...DEFAULTS, check: false, offline: false };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--game') args.game = argv[++i];
    else if (argv[i] === '--check') args.check = true;
    else if (argv[i] === '--offline') args.offline = true;
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  if (args.offline && !args.check) {
    throw new Error('--offline only makes sense with --check');
  }
  return args;
}

async function load(src) {
  if (/^https?:\/\//.test(src)) {
    const res = await fetch(src);
    if (!res.ok) throw new Error(`fetch ${src} failed: HTTP ${res.status}`);
    return await res.text();
  }
  return readFileSync(src, 'utf8');
}

// Normalised rather than verbatim, so codegen's input is stable against the
// SDL's field ordering and description churn.
const normalise = (sdl) => print(mergeTypeDefs([sdl])) + '\n';
const sha256 = (text) => createHash('sha256').update(text).digest('hex');

// The git blob id of the bytes as SERVED, which is what makes attribution possible: the
// same id appears in cks-docs' tree for `static/schema/game-api.graphql`, so the drift job
// can name the exact commit that published what is live instead of reporting that two
// files differ. Hashing the served bytes rather than our normalised copy is the point --
// normalisation is lossy with respect to formatting, and a blob id has to match theirs.
const gitBlob = (text) => {
  const body = Buffer.from(text, 'utf8');
  return createHash('sha1')
    .update(Buffer.concat([Buffer.from(`blob ${body.length}\0`), body]))
    .digest('hex');
};

const readProvenance = () =>
  JSON.parse(readFileSync(PACKAGE, 'utf8')).publishedSchemaSnapshot ?? null;

const args = parseArgs(process.argv);

// ---------------------------------------------------------------------------
// Per-commit gate. Asks the network nothing; every failure is fixable on the branch.
// ---------------------------------------------------------------------------
if (args.check && args.offline) {
  const failures = [];
  if (!existsSync(SNAPSHOT)) {
    failures.push('schema.gql is missing');
  } else {
    const committed = readFileSync(SNAPSHOT, 'utf8');
    // A snapshot that does not PARSE has to be a named failure, not an exception. The
    // first sabotage run of this gate deleted a field, produced invalid SDL, and got a
    // GraphQLError stack trace out of `print(mergeTypeDefs(...))` -- which says where the
    // parser gave up and not one word about what to do. A gate that crashes has told the
    // reader less than one that refuses.
    let canonical = null;
    try {
      canonical = normalise(committed);
    } catch (err) {
      failures.push(
        `schema.gql is not parseable as SDL: ${err.message.split('\n')[0]}\n` +
          '  It has been hand-edited into something invalid. Restore it with\n' +
          '  `git checkout -- schema.gql`, or re-run `node scripts/schema-sync.mjs`.',
      );
    }
    if (canonical !== null && canonical !== committed) {
      failures.push(
        'schema.gql is not in canonical form -- it has been hand-edited, or was written\n' +
          '  by an older normaliser. Re-run `node scripts/schema-sync.mjs` to rewrite it.',
      );
    }
    const provenance = readProvenance();
    if (!provenance?.snapshotSha256) {
      failures.push(
        'package.json has no publishedSchemaSnapshot.snapshotSha256. Run\n' +
          '  `node scripts/schema-sync.mjs` to record where this snapshot came from.',
      );
    } else if (provenance.snapshotSha256 !== sha256(committed)) {
      failures.push(
        `schema.gql does not match its provenance record.\n` +
          `  recorded ${provenance.snapshotSha256.slice(0, 16)} (synced ${provenance.syncedAt})\n` +
          `  actual   ${sha256(committed).slice(0, 16)}\n` +
          '  The snapshot was edited without syncing. Re-run `node scripts/schema-sync.mjs`.',
      );
    }
  }
  if (failures.length > 0) {
    for (const f of failures) console.error(`schema.gql: ${f}`);
    process.exit(1);
  }
  const p = readProvenance();
  console.log(
    `schema.gql is canonical and matches its provenance record\n` +
      `  synced ${p.syncedAt} from ${p.url}\n` +
      `  served blob at that time: ${p.sourceBlob}`,
  );
  process.exit(0);
}

// ---------------------------------------------------------------------------
// Live comparison, and the sync itself. Both talk to the published SDL on purpose.
// ---------------------------------------------------------------------------
const served = await load(args.game);
const normalised = normalise(served);

if (args.check) {
  if (!existsSync(SNAPSHOT) || readFileSync(SNAPSHOT, 'utf8') !== normalised) {
    console.error(`schema.gql drifted from ${args.game}`);
    console.error('Run the same command without --check, then regenerate codegen.');
    process.exitCode = 1;
  } else {
    console.log('schema.gql matches the supplied SDL');
  }
} else {
  writeFileSync(SNAPSHOT, normalised);

  // The provenance record is written in the same act as the snapshot, so the two cannot
  // disagree unless somebody edits one by hand -- which is precisely what --offline
  // refuses. A record updated by a separate step would be a second copy of a fact.
  // `url` is the endpoint drift is measured AGAINST and is always the published one;
  // `syncedFrom` is where these particular bytes were read this time. They differ when a
  // maintainer syncs from a file or from a pinned raw ref -- and conflating them would
  // quietly repoint the daily drift job at a URL that can never move, which is a check
  // that cannot fail wearing the costume of one that passed.
  const pkg = JSON.parse(readFileSync(PACKAGE, 'utf8'));
  pkg.publishedSchemaSnapshot = {
    url: DEFAULTS.game,
    ...(args.game === DEFAULTS.game ? {} : { syncedFrom: args.game }),
    sourceBlob: gitBlob(served),
    snapshotSha256: sha256(normalised),
    syncedAt: new Date().toISOString(),
  };
  writeFileSync(PACKAGE, JSON.stringify(pkg, null, 2) + '\n');

  console.log(
    `schema.gql written from:\n  ${args.game}\n` +
      `  served blob ${pkg.publishedSchemaSnapshot.sourceBlob}\n` +
      `  snapshot    ${pkg.publishedSchemaSnapshot.snapshotSha256.slice(0, 16)}\n` +
      'Now regenerate codegen: node scripts/codegen.mjs',
  );
}
