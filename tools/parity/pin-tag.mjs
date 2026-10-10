#!/usr/bin/env node
// The CrowdyJS parity pin must be a PUBLISHED release: its commit is the target of a CrowdyJS
// `<tier>/v<version>` tag for the pinned version.
//
// Reachability from this tier's branch (check-parity-target-tier.mjs, run by the control
// plane's preflight) says the commit was promoted here; it does not say it was released.
// 0.59.0 pinned CrowdyJS `ca9fbb5` as 18.6.0 while `dev/v18.6.0` is `7662d0b0`: reachable,
// never published (it first shipped in `dev/v18.7.0`), and nothing refused it.
//
// Any tier's tag will do. One commit travels the ladder (promotions merge forward), so the
// `dev/v<version>` tag's target is what every tier pins; requiring this tier's own tag would
// refuse the commit `test` and `prod` correctly pin before their own tags exist.
//
//   node tools/parity/pin-tag.mjs           # reads package.json, asks GitHub for the tags
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const TIERS = ['dev', 'test', 'prod'];

/**
 * `git ls-remote --tags` output -> tag name -> the commit it points at (an annotated tag's
 * peeled `^{}` line wins over the tag object's own sha).
 */
export function parseTagRefs(text) {
  const out = new Map();
  for (const line of text.split('\n')) {
    const m = /^([0-9a-f]{40})\s+refs\/tags\/(.+?)(\^\{\})?$/.exec(line.trim());
    if (!m) continue;
    const [, sha, name, peeled] = m;
    if (peeled || !out.has(name)) out.set(name, sha);
  }
  return out;
}

/** The verdict on a pin, given the tags. Pure, so the cases are testable without a network. */
export function pinTagVerdict({ version, commit, tags }) {
  const named = TIERS.map((tier) => `${tier}/v${version}`);
  const matching = named.filter((name) => tags.get(name) === commit);
  if (matching.length > 0) {
    return { ok: true, message: `CrowdyJS ${version} pin ${commit.slice(0, 10)} is ${matching.join(', ')}` };
  }
  const elsewhere = named
    .filter((name) => tags.has(name))
    .map((name) => `${name} is ${tags.get(name).slice(0, 10)}`);
  return {
    ok: false,
    message:
      `the CrowdyJS parity pin ${version}@${commit.slice(0, 10)} is not a published release: ` +
      (elsewhere.length > 0
        ? `${elsewhere.join(', ')}. Pin that commit (npm run parity:repin)`
        : `no ${named.join(' / ')} tag exists yet. Pin a CrowdyJS release that has been tagged`),
  };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const root = join(fileURLToPath(new URL('.', import.meta.url)), '..', '..');
  const { version, commit } = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'))
    .crowdyjsParityTarget;
  const remote = process.env.CROWDYJS_REMOTE ?? 'https://github.com/CrowdedKingdoms/CrowdyJS.git';
  const text = execFileSync(
    'git',
    ['ls-remote', '--tags', remote, ...TIERS.map((tier) => `refs/tags/${tier}/v${version}*`)],
    { encoding: 'utf8' },
  );
  const verdict = pinTagVerdict({ version, commit, tags: parseTagRefs(text) });
  console.log(verdict.ok ? `ok: ${verdict.message}` : `::error::${verdict.message}`);
  process.exit(verdict.ok ? 0 : 1);
}
