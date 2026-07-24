#!/usr/bin/env node
/**
 * Refresh the committed schema snapshot (schema.gql) from the published
 * production SDLs:
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
import { readFileSync, writeFileSync } from 'node:fs';
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
const destination = resolve(root, 'schema.gql');
if (args.check) {
  const committed = readFileSync(destination, 'utf8');
  if (committed !== merged) {
    console.error(
      `schema.gql drifted from:\n  ${args.management}\n  ${args.game}\n` +
        'Run the same command without --check, then regenerate codegen.',
    );
    process.exitCode = 1;
  } else {
    console.log('schema.gql matches the supplied Management and Game SDLs');
  }
} else {
  writeFileSync(destination, merged);
  console.log(
    `schema.gql written (${merged.length} bytes) from:\n` +
      `  ${args.management}\n  ${args.game}`,
  );
}
