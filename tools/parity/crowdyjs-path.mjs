import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';

function isCrowdyJsCheckout(path) {
  return existsSync(join(path, 'schema.gql')) &&
    existsSync(join(path, 'package.json'));
}

/**
 * Resolve the pinned CrowdyJS checkout without weakening parity.
 *
 * An explicit CLI path or CROWDYJS_PATH is authoritative and must be valid.
 * Otherwise normal sibling, CI child-checkout, and git-worktree sibling
 * layouts are checked in deterministic order.
 */
export function resolveCrowdyJsPath(repoRoot, explicitPath = null) {
  const configured = explicitPath ?? process.env.CROWDYJS_PATH;
  if (configured) {
    const path = resolve(repoRoot, configured);
    if (isCrowdyJsCheckout(path)) return path;
    throw new Error(
      `Configured CrowdyJS checkout is invalid: ${path}. ` +
        'Expected schema.gql and package.json.',
    );
  }

  const candidates = [
    resolve(repoRoot, '..', 'CrowdyJS'),
    resolve(repoRoot, 'CrowdyJS'),
  ];
  try {
    const commonDirectory = execFileSync(
      'git',
      ['rev-parse', '--path-format=absolute', '--git-common-dir'],
      { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] },
    ).trim();
    candidates.push(join(dirname(dirname(commonDirectory)), 'CrowdyJS'));
  } catch {
    // Non-git source archives still use the normal sibling/child layouts.
  }

  const checked = [...new Set(candidates.map((path) => resolve(path)))];
  const found = checked.find(isCrowdyJsCheckout);
  if (found) return found;
  throw new Error(
    'CrowdyJS checkout not found. Set CROWDYJS_PATH or place CrowdyJS ' +
      `beside this checkout. Checked: ${checked.join(', ')}`,
  );
}
