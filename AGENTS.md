# CrowdyCPP agent guidance

**RULE: THE SDK IS FOR NORMAL CLIENTS, AND IT IS DESIGNED FOR PRODUCTION (operator decision,
2026-09-28).** It serves players, developers and org-admins. It may carry org-admin features
(there will be many org-admins), but NEVER a wrapper for something only a super-admin or a
platform operator can call, and no testing helper. Platform tooling and test helpers live in
our own repos as scripts that call GraphQL directly. The per-release default origin
(`kDefaultTier`) is unaffected. So, before wrapping a root field:

- Read its resolver in cks-game-api at the same tier (`git show origin/dev:src/...`).
  `@RequiresSuperAdmin()`, `@RequiresOperator()`, `OperatorGuard` (on the method or on its
  resolver class), or a body that refuses everyone but a super-admin or an operator means it
  does not belong here. Also read the services the body calls for such a refusal.
- A field with an org-admin path and a platform path is wrapped for the org-admin path only,
  and the SDK refuses the other before any request: `setQuota` / `deleteQuota` with neither
  an org nor an app are platform-global, so `admin().quotas().set` needs an `appId` or an
  `orgId` (`deleteQuota` takes only a quota id, so its scope is the server's to judge).
- `tests/parity/sdk-audience.test.mjs` (`npm test`, CI `parity-baseline`) fails when any
  document under `operations/` or inline `R"gql(...)gql"` document in `include/` selects a
  root field on its platform-only list, or one whose schema description says operator- or
  super-admin-only. When cks-game-api adds such a field, add it to that list; do not wrap
  it. CrowdyJS carries the same test (`test/unit/sdk-audience.test.mjs`); this list holds
  every field on its list, plus the platform-only fields CrowdyJS never wrapped.
- 0.51.0 removed the last ones (MIGRATION.md lists them). A published release tag is never
  moved: a change after `dev/vX.Y.Z` shipped is a new version.

CrowdyCPP is a standalone public C++20 SDK. A normal configure, build, install,
or unit-test run must not require network access, Node, CrowdyJS, or private
platform repositories. Schema and generated artifacts are committed. Optional
libraries stay behind options that default OFF: `CROWDY_WITH_OPUS` (0.60.0) builds
the libopus wrapper only when asked, and fails the configure if libopus is missing.

GitHub default is **`prod`**. Work lands on `dev`. Parity pin is CrowdyJS —
read `crowdyjsParityTarget` in `package.json` (source of truth CI checks out);
do not hardcode the version number in this sentence. One GraphQL origin since 0.20.0 (`managementUrl` removed). Paid
player-code commerce and grid sales are off the public schema; do not
re-add those Marketplace operations until ck-api flips
`PAID_PLAYER_COMMERCE_ENABLED`. Gameplay
is PostgreSQL + Citus, not galaxy. `cks-management-api` is not a running
service (GitHub repo still exists, **archived**; not a schema source).

**TIER ALIGNMENT (hard rule).** This branch’s CrowdyJS parity pin must point at
a commit that exists on the **same** CrowdyJS tier branch (`dev`↔`dev`,
`test`↔`test`, `prod`↔`prod`). Do not re-pin CrowdyCPP `dev` to a SHA that only
lives on CrowdyJS `prod`/`latest`, and do not promote this repo’s pin to a
higher-tier CrowdyJS artifact on a `dev` PR — promote each ladder separately.

## Public-surface maintenance

When the unified GraphQL surface changes:

1. Run `npm ci` for maintainer-only schema tooling.
2. Refresh `schema.gql` with `scripts/schema-sync.mjs` from the published
   unified SDL (`/schema/game-api.graphql`). The management docs SDL is
   derived from that schema; do not sync a second endpoint.
3. Run `node scripts/codegen.mjs`; commit `schema.gql`, `package.json` (the sync
   rewrites its `publishedSchemaSnapshot` provenance record in the same act),
   `include/crowdy/generated/enums.hpp`, and
   `include/crowdy/generated/operations.hpp` together. CI's schema gate is
   offline and checks the snapshot against that record, so committing the
   snapshot without the record — or either without the regenerated headers —
   is refused on the branch rather than discovered later.
4. Run `node scripts/codegen.mjs --check`.
5. Compare the reviewed CrowdyJS checkout with
   `node tools/parity/parity.mjs --crowdyjs <path> --write
   docs/parity-matrix.md`, then run the same command with `--check`.
6. If portable Studio layout changed, build CrowdyJS and run
   `node tools/parity/layout-fixtures.mjs --crowdyjs <path>`. Use `--write`
   only for an intentional coordinated fixture update.
7. If Studio diagnostics parsing changed, run `studio-state-fixtures.mjs` the
   same way. (The agent, control-gate and Studio-host fixtures went with the
   Crowdy Agent orchestrator in 0.34.0; the agent runs in the browser now and
   this SDK only reads its policy, consent and usage.) Bound-project saves
   are portable as of 0.38.0 (`saveProject` commits through
   `crowdyStudioGitHubPutFile` / `DeleteFile`); the hosted GitHub settings
   card on `CrowdyStudioController` stays a browser exclusion.

### CrowdyPy wraps this tree

CrowdyPy (the Python SDK) vendors this repository at a pinned release and binds the
replication `Connection`, `WorldSession` and the wrapper seams added for it in 0.54.0:
`Config::onEventsReady`, `IChunkSource` with the injectable `ChunkStore` constructor,
`IHostElection` and `WorldSessionServices`. A change to those contracts is a change to
CrowdyPy too: say so in `MIGRATION.md`, and CrowdyPy re-vendors at its next release
(`scripts/vendor_crowdycpp.py`). Its Windows build includes these headers after
`<windows.h>`, whose `far` and `near` macros are empty, so no public header may use
either as an identifier (`tests/windows_macros_test.cpp` holds that).

## Releasing

**Cut the tag.** This SDK has no registry — consumers clone a ref and
`cmake --install` it — so the tag IS the artifact, and a release nobody can pin
is not a release. From 0.20.0 to 0.24.0 this repo had no release path at all,
four versions shipped untagged, and an SDK author re-reported a defect that had
been fixed five days earlier because the newest tag was `v0.19.0`.

Work through [`docs/release-checklist.md`](docs/release-checklist.md), then tag
the tier branch after the merge lands on it:

```bash
git checkout dev && git pull
V="v$(node -p "require('./package.json').version")"
git tag -a "dev/$V" -m "CrowdyCPP dev $V" && git push origin "dev/$V"
```

Annotated, and tier-prefixed. The push runs
[`.github/workflows/release.yml`](.github/workflows/release.yml), whose guard
refuses a tag outside the branch it names and a tag that is not the version the
tree builds, before installing the package and linking `tests/consumer` against
it. Both gates have self-tests and CI runs them on every branch push.

The version lives in **six** places and `npm test` refuses when they disagree.
Two of them — `package-lock.json` and `docs/compatibility.md` — were added to
that gate at 0.25.0, after the lock file was found six releases stale.

## Moving the CrowdyJS pin

The CrowdyJS commit in `package.json` is deliberately pinned and consumed by
CI, which checks out that exact commit. Update the target, the generated
matrix, and every fixture in one reviewed change:

```bash
npm run parity:repin -- --crowdyjs /path/to/CrowdyJS   # built, and clean
rg -l '<old-commit-prefix>' tests docs                 # literal assertions
npm run check:release
```

For a **promotion** (`dev` → `test` → `prod`) that whole sequence, plus the
origin regen, the `docs/compatibility.md` pin and the schema resync from
CrowdyJS at `origin/<to>`, is
`infra-control-plane/scripts/ops/promote.mjs --repo CrowdyCPP --from <tier> --to <tier>` — it builds CrowdyJS at exactly
`origin/<to>` in a temporary worktree, takes the source side of the pin-carrying
files on conflict, re-pins, runs `check:release` with `CROWDYJS_PATH` at that
worktree, and opens the PR. It refuses when CrowdyJS at `origin/<to>` is not the
version this repo's pin names: promote CrowdyJS first.

`parity:repin` rewrites the pin, copies the fixtures CrowdyJS owns (the ck-exec
gateway cases, `tools/parity/fixtures/exec-gateway-cases.json`, and since 0.60.0
the voice payload cases, `tools/parity/fixtures/voice-frames.json`, which
`exec-gateway-fixture.test.mjs` and `voice-frames-fixture.test.mjs` hold to the
pinned commit's), and reruns the fixture generators plus the matrix; it prints
the steps it cannot do for you. A C++ test replays each copy (`exec_test`,
`voice_frames_test`), so a behaviour change lands in CrowdyJS's fixture first.
Some wire vectors are literal in both repos' tests instead of a fixture: the opcode 35
(channel audio) bytes in `wire_test` are the ones CrowdyJS's
`test/unit/channel-audio.test.mjs` asserts, so a layout change edits both.
`docs/compatibility.md` names the pin too and is edited by hand. Four things
reliably bite when this is done by hand:

- **The two repos have a merge order.** A CrowdyCPP change that mirrors new
  CrowdyJS behavior cannot go green until that CrowdyJS commit is fetchable
  from GitHub (CI checks out the pinned SHA). CrowdyJS's default branch is
  **`prod`**; land the commit on `dev` and promote so the SHA exists remotely,
  then re-pin here. A pin pointing at an unpushed commit fails at checkout.
- **`parity.mjs` embeds its own gate mode**, so a matrix written without
  `--strict` never satisfies the `--strict` check CI runs. Write and check with
  the same flags. `parity:repin` always writes the strict form.
- **Reconfigure existing CMake build dirs after touching a fixture.** The agent
  and Studio-layout fixtures are embedded into headers by `configure_file`, and
  the JSONs are registered in `CMAKE_CONFIGURE_DEPENDS` so a rebuild picks them
  up — but a build tree configured before that was added will keep serving a
  stale header and fail the replay tests against a file you just fixed.
- **Some fixtures assert the pin literally** in C++ tests and
  `docs/parity-matrix.md`. The `rg` above is how you find them.

Parity classifications are strict:

- `portable gap` means missing implementation that remains visible and must be
  removed as work lands;
- `native equivalent` is allowed only when CrowdyCPP provides the same contract
  through its native architecture;
- `browser exclusion` is allowed only for browser/UI/worker-specific behavior.

Do not label planned native WebSockets, Crowdy Studio, agent control, leases, or
player-host work as browser-only. New differences and stale classifications
must fail the baseline gate. `parity.mjs --strict` must pass before declaring
strict portable parity complete.

The class scan reads only `src/domains`, `src/kit`, `src/stores`, `world.ts`
and the Studio modules. A CrowdyJS module outside it enters the matrix through
`CROSS_CUTTING_EXPORT_MODULES`, where every export is classified and a new one
fails: the CLIENT-half runner, broker and glue (`src/grid-mods/exec-client-halves.ts`,
`src/player-runtime/player-code-broker.ts`, `glue-runtime.ts`,
`client-host-calls.ts`) sit there as browser exclusions since 0.49.0, because
this SDK runs no WASM. A behaviour
change that adds no method moves no row: pin it in `CROSS_CUTTING_BEHAVIORS`
with markers from the CrowdyJS source, as 0.49.0 did for 17.14.0's three World
Stores changes, so the matrix says what it means here and goes stale when that
source changes.

## The legacy engines (removed in 0.50.0)

The game model and its automations, Studio compute, player compute (both
targets) and the player model are gone, from the Game API too (ck-api
v2.27.0): ck-exec (`client.exec()`) replaced them. The Game Kit keeps
`social()`, `kit/wire.hpp` and `kit/actions.hpp`; Crowdy Studio's SERVER target
is a mod and its CLIENT target that mod's CLIENT half (`CrowdyStudioModRuntime`).
Do not re-add a wrapper for a `gameModel*`, `compute*`, `playerModel*`,
`playerCompute*`, player-code marketplace or WASM-policy field: the schema no
longer has them, and codegen refuses an operation that names one. The Game API
keeps tier features (`admin().appAccess()`), grid claims, the studio moderation
fields and the compute budget (`gen::computeUnits`).

## Writing tests against `graphql::Json`

Index arrays with `at(i)`. `json[0]` is now a compile error: `operator[]` takes
a `string_view`, and the literal `0` used to bind to it as a null pointer and
segfault in `strlen` at runtime.
