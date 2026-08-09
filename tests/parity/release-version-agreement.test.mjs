import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..', '..');
const read = (relative) => readFileSync(join(root, relative), 'utf8');

/**
 * The version is written in four places, and they have to agree.
 *
 * This has broken a release three times: `fix(release): request the CrowdyCPP
 * 0.15 package line`, again at 0.18, and again at 0.20. Every time it was the
 * same file — the consumer fixture's `find_package(CrowdyCPP <minor>)` — and
 * every time it was found by CI, roughly two minutes into a job that first
 * builds and installs the whole SDK.
 *
 * `docs/release-checklist.md` already lists the step. Three misses says the
 * problem is not that nobody was told; it is that nothing refused. A stale
 * request is invisible locally because the installed package config uses
 * SameMinorVersion, so it only fails once a package of the NEW minor exists to
 * reject it.
 */
function projectVersion() {
  const match = /project\(CrowdyCPP VERSION (\d+\.\d+\.\d+)/u.exec(
    read('CMakeLists.txt'),
  );
  assert.ok(match, 'could not read project(CrowdyCPP VERSION ...) from CMakeLists.txt');
  return match[1];
}

test('package.json version matches the CMake project version', () => {
  assert.equal(JSON.parse(read('package.json')).version, projectVersion());
});

test('the consumer fixture requests the current minor line', () => {
  const source = read('tests/consumer/CMakeLists.txt');
  const match = /find_package\(CrowdyCPP (\d+\.\d+)(?:\.\d+)? CONFIG/u.exec(source);
  assert.ok(
    match,
    'tests/consumer/CMakeLists.txt must request an explicit CrowdyCPP minor: ' +
      'requesting none would accept any version and prove nothing',
  );
  const [major, minor] = projectVersion().split('.');
  assert.equal(
    match[1],
    `${major}.${minor}`,
    'the consumer fixture requests a different minor than the project builds, ' +
      'so the install test cannot link and CI fails after building the whole SDK',
  );
});

test('the README target line names the current version', () => {
  // Not cosmetic: this line is the version claim a reader takes as current, and
  // it sits next to the parity target it is asserting a gate result for.
  const version = projectVersion();
  assert.match(
    read('README.md'),
    new RegExp(`\\*\\*v${version.replace(/\./gu, '\\.')}\\b`, 'u'),
    `README.md has no **v${version}** target line`,
  );
});
