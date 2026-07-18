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
 * Usage: node scripts/schema-sync.mjs [--management <src>] [--game <src>]
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const DEFAULTS = {
  management: 'https://docs.crowdedkingdoms.com/schema/management-api.graphql',
  game: 'https://docs.crowdedkingdoms.com/schema/game-api.graphql',
};

function parseArgs(argv) {
  const args = { ...DEFAULTS };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--management') args.management = argv[++i];
    else if (argv[i] === '--game') args.game = argv[++i];
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

/**
 * Merge two SDLs the way the CrowdyJS snapshot does: concatenate, dropping
 * duplicate type definitions (the shared scalar/type definitions appear in
 * both APIs' SDLs) and merging the root Query/Mutation/Subscription types.
 */
function mergeSdl(a, b) {
  const blocks = (sdl) => {
    const out = [];
    const lines = sdl.split('\n');
    let cur = [];
    let depth = 0;
    for (const line of lines) {
      cur.push(line);
      for (const ch of line) {
        if (ch === '{') depth++;
        else if (ch === '}') depth--;
      }
      if (depth === 0 && cur.join('\n').trim().length > 0 && /}\s*$/.test(line)) {
        out.push(cur.join('\n'));
        cur = [];
      } else if (depth === 0 && /^(scalar|directive)\s/.test(line.trim())) {
        out.push(cur.join('\n'));
        cur = [];
      }
    }
    if (cur.join('\n').trim()) out.push(cur.join('\n'));
    return out;
  };

  const nameOf = (block) => {
    const m = block.match(/(?:type|input|enum|interface|union|scalar)\s+(\w+)/);
    return m ? m[1] : null;
  };

  const rootNames = new Set(['Query', 'Mutation', 'Subscription']);
  const seen = new Map();
  const roots = new Map();
  const ordered = [];

  for (const block of [...blocks(a), ...blocks(b)]) {
    const name = nameOf(block);
    if (name && rootNames.has(name)) {
      const bodyMatch = block.match(/\{([\s\S]*)\}\s*$/);
      const body = bodyMatch ? bodyMatch[1] : '';
      roots.set(name, (roots.get(name) ?? '') + body);
      continue;
    }
    const key = name ?? block.trim();
    if (seen.has(key)) continue;
    seen.set(key, true);
    ordered.push(block);
  }

  const merged = [];
  for (const [name, body] of roots) merged.push(`type ${name} {\n${body}\n}`);
  return [...merged, ...ordered].join('\n\n') + '\n';
}

const args = parseArgs(process.argv);
const [management, game] = await Promise.all([load(args.management), load(args.game)]);
const merged = mergeSdl(management, game);
writeFileSync(resolve(root, 'schema.gql'), merged);
console.log(`schema.gql written (${merged.length} bytes) from:\n  ${args.management}\n  ${args.game}`);
