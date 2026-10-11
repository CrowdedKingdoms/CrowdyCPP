#!/usr/bin/env node
// The CrowdyJS parity pin must be THIS TIER'S published release: its commit is the target of
// CrowdyJS's `<tier>/v<version>` tag for the pinned version, where the tier is this branch's.
//
// Reachability from this tier's branch (check-parity-target-tier.mjs, run by the control
// plane's preflight) says the commit was promoted here; it does not say it was released.
// 0.59.0 pinned CrowdyJS `ca9fbb5` as 18.6.0 while `dev/v18.6.0` is `7662d0b0`: reachable,
// never published (it first shipped in `dev/v18.7.0`), and nothing refused it.
//
// Only this tier's tag counts. CrowdyJS tags each tier's release at the commit that tier
// released, so one version is three commits (18.4.0 is `dev/v18.4.0` 4e7e595b, `test/v18.4.0`
// 2bfbb081, `prod/v18.4.0` 7f805609), and each branch here pins its own tier's. Accepting any
// tier's tag let a `dev` pin of a `prod/v…` commit pass, or a `test` pin of a release CrowdyJS
// has not made on `test`; that is another tier's artifact, and CI runs no reachability check to
// catch it. A promotion therefore re-pins to the destination's tag, after CrowdyJS has
// released the version on that tier.
//
// The tier is the ref's: the pull request's base, else the pushed branch. `dev`, `test` and
// `prod` (or `<tier>/…`) name theirs; any other branch is a feature branch and counts as dev.
//
//   node tools/parity/pin-tag.mjs --ref "$PIN_REF"   # CI: the base ref, else the branch
//   node tools/parity/pin-tag.mjs --tier test        # judge as a tier explicitly
//   node tools/parity/pin-tag.mjs                    # GITHUB_BASE_REF, GITHUB_REF_NAME, or the checkout
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

/** The tier a ref is judged as: `dev`, `test` or `prod` (or `<tier>/…`), else dev. */
export function tierOfRef(ref) {
  const bare = String(ref).replace(/^refs\/(heads|tags)\//, '');
  const head = bare.split('/')[0];
  return TIERS.includes(head) ? head : 'dev';
}

/**
 * Where the tier comes from: `--tier`, else `--ref`, else the pull request's base, else the
 * pushed branch, else the checked-out branch. In CI, no ref at all means the workflow did not
 * give this gate its input, and a gate that cannot see its input must not pass.
 */
export function resolveTier({ args = [], env = {}, branch = '' }) {
  const flag = (name) => {
    const i = args.indexOf(name);
    return i >= 0 ? (args[i + 1] ?? '') : undefined;
  };
  const tier = flag('--tier');
  if (tier !== undefined) {
    if (!TIERS.includes(tier)) throw new Error(`--tier must be one of ${TIERS.join(', ')}, not '${tier}'`);
    return { tier, source: '--tier' };
  }
  const candidates = [
    [flag('--ref'), '--ref'],
    [env.GITHUB_BASE_REF, 'GITHUB_BASE_REF (the pull request base)'],
    [env.GITHUB_REF_NAME, 'GITHUB_REF_NAME'],
    [branch === 'HEAD' ? '' : branch, 'the checked-out branch'],
  ];
  for (const [ref, source] of candidates) {
    if (ref) return { tier: tierOfRef(ref), source: `${source} '${ref}'` };
  }
  throw new Error(
    env.GITHUB_ACTIONS
      ? 'no ref to judge the pin by (--ref, GITHUB_BASE_REF and GITHUB_REF_NAME are all empty). In CI that is a misconfiguration, not a feature branch.'
      : 'no ref and no checked-out branch to take a tier from; pass --tier',
  );
}

/** The verdict on a pin, given the tags. Pure, so the cases are testable without a network. */
export function pinTagVerdict({ version, commit, tags, tier }) {
  if (!TIERS.includes(tier)) throw new Error(`tier must be one of ${TIERS.join(', ')}, not '${tier}'`);
  const short = (sha) => sha.slice(0, 10);
  const own = `${tier}/v${version}`;
  if (tags.get(own) === commit) {
    return { ok: true, message: `CrowdyJS ${version} pin ${short(commit)} is ${own}, this tier's release` };
  }
  const others = TIERS.filter((t) => t !== tier)
    .map((t) => `${t}/v${version}`)
    .filter((name) => tags.has(name));
  const pinIs = others.filter((name) => tags.get(name) === commit);
  const head = `the CrowdyJS parity pin ${version}@${short(commit)} is not ${tier}'s release: `;
  const another = pinIs.length > 0 ? ` (the pin is ${pinIs.join(', ')}, another tier's release)` : '';
  if (tags.has(own)) {
    return {
      ok: false,
      message: `${head}${own} is ${short(tags.get(own))}${another}. Pin that commit (npm run parity:repin)`,
    };
  }
  const known = others.length > 0 ? ` (${others.map((name) => `${name} is ${short(tags.get(name))}`).join(', ')})` : '';
  return {
    ok: false,
    message: `${head}no ${own} tag exists yet${known}. Release CrowdyJS ${version} on ${tier} first, then pin the commit ${own} points at`,
  };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const root = join(fileURLToPath(new URL('.', import.meta.url)), '..', '..');
  let resolved;
  try {
    let branch = '';
    try {
      branch = execFileSync('git', ['-C', root, 'rev-parse', '--abbrev-ref', 'HEAD'], {
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
      }).trim();
    } catch {
      // Not a checkout; the explicit inputs still decide.
    }
    resolved = resolveTier({ args: process.argv.slice(2), env: process.env, branch });
  } catch (err) {
    console.log(`::error::${err.message}`);
    process.exit(1);
  }
  const { version, commit } = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'))
    .crowdyjsParityTarget;
  const remote = process.env.CROWDYJS_REMOTE ?? 'https://github.com/CrowdedKingdoms/CrowdyJS.git';
  const text = execFileSync(
    'git',
    ['ls-remote', '--tags', remote, ...TIERS.map((tier) => `refs/tags/${tier}/v${version}*`)],
    { encoding: 'utf8' },
  );
  const verdict = pinTagVerdict({ version, commit, tags: parseTagRefs(text), tier: resolved.tier });
  const why = `judged as ${resolved.tier}, from ${resolved.source}`;
  console.log(verdict.ok ? `ok: ${verdict.message} (${why})` : `::error::${verdict.message} (${why})`);
  process.exit(verdict.ok ? 0 : 1);
}
