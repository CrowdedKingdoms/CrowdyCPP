#!/usr/bin/env node
/**
 * Refresh the committed per-endpoint schema snapshots and merged schema.gql
 * from the published production SDLs:
 *   https://docs.crowdedkingdoms.com/schema/management-api.graphql
 *   https://docs.crowdedkingdoms.com/schema/game-api.graphql
 *
 * Maintainers only; external builds use the committed snapshot. Override the
 * sources with --management <path|url> and --game <path|url>.
 *
 * Requires the maintainer-only Node dependencies (`npm ci`). Normal CMake
 * builds never invoke this script.
 *
 * Usage: node scripts/schema-sync.mjs [--management <src>] [--game <src>]
 *                                     [--check]
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mergeTypeDefs } from '@graphql-tools/merge';
import { print } from 'graphql';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const DEFAULTS = {
  management: 'https://docs.crowdedkingdoms.com/schema/management-api.graphql',
  game: 'https://docs.crowdedkingdoms.com/schema/game-api.graphql',
};

function parseArgs(argv) {
  const args = { ...DEFAULTS, check: false };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--management') args.management = argv[++i];
    else if (argv[i] === '--game') args.game = argv[++i];
    else if (argv[i] === '--check') args.check = true;
    else throw new Error(`unknown argument: ${argv[i]}`);
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

const args = parseArgs(process.argv);
const [management, game] = await Promise.all([load(args.management), load(args.game)]);
const merged = print(mergeTypeDefs([management, game])) + '\n';
const snapshots = [
  {
    destination: resolve(root, 'schema.management.gql'),
    label: 'schema.management.gql',
    source: args.management,
    text: management,
  },
  {
    destination: resolve(root, 'schema.game.gql'),
    label: 'schema.game.gql',
    source: args.game,
    text: game,
  },
  {
    destination: resolve(root, 'schema.gql'),
    label: 'schema.gql',
    source: `${args.management} + ${args.game}`,
    text: merged,
  },
];
if (args.check) {
  const drifted = snapshots.filter(
    ({ destination, text }) =>
      !existsSync(destination) ||
      readFileSync(destination, 'utf8') !== text,
  );
  if (drifted.length > 0) {
    for (const { label, source } of drifted) {
      console.error(`${label} drifted from ${source}`);
    }
    console.error(
      'Run the same command without --check, then regenerate codegen.',
    );
    process.exitCode = 1;
  } else {
    console.log(
      'Per-endpoint snapshots and schema.gql match the supplied ' +
        'Management and Game SDLs',
    );
  }
} else {
  for (const { destination, text } of snapshots) {
    writeFileSync(destination, text);
  }
  console.log(
    `schema.management.gql, schema.game.gql, and schema.gql written from:\n` +
      `  ${args.management}\n  ${args.game}`,
  );
}
