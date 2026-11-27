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
CI's `opus-build-test` job (0.67.4) is the only one that turns it on: a change to
`crowdy/media/opus.hpp`, `src/media/opus.cpp` or `cmake/CrowdyFindOpus.cmake` is judged there.

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
CI holds the pin to the commit of CrowdyJS's tag for **this** tier
(`tools/parity/pin-tag.mjs`: the pull request's base, a feature branch counting
as `dev`), so the pin differs per tier and a promotion re-pins to the
destination's tag.

## Public-surface maintenance

When the unified GraphQL surface changes:

1. Run `npm ci` for maintainer-only schema tooling.
2. Refresh `schema.gql` with `scripts/schema-sync.mjs` from the published
   unified SDL (`/schema/application-api.graphql`; `/schema/game-api.graphql` until January 1, 2027). The management docs SDL is
   derived from that schema; do not sync a second endpoint.
3. Run `node scripts/codegen.mjs`; commit `schema.gql`, `package.json` (the sync
   rewrites its `publishedSchemaSnapshot` provenance record in the same act),
   `include/crowdy/generated/enums.hpp`, and
   `include/crowdy/generated/operations.hpp` together. CI's schema gate is
   offline and checks the snapshot against that record, so committing the
   snapshot without the record — or either without the regenerated headers —
   is refused on the branch rather than discovered later.
4. Run `node scripts/codegen.mjs --check`. An enum value spelled like a C++ keyword or like a
   macro `<math.h>` defines on MSVC and macOS (`DOMAIN`, `OVERFLOW` and the other SVID codes;
   `PLATFORM_MACROS` in the generator) gets a trailing underscore (`FreeTierLevel::DOMAIN_`); its
   wire spelling is unchanged. Linux compiles either, so only the Windows and macOS jobs catch a
   new one (0.89.0).
5. Compare the reviewed CrowdyJS checkout with
   `node tools/parity/parity.mjs --crowdyjs <path> --write
   docs/parity-matrix.md`, then run the same command with `--check`.
6. If portable Studio layout changed, build CrowdyJS and run
   `node tools/parity/layout-fixtures.mjs --crowdyjs <path>`. Use `--write`
   only for an intentional coordinated fixture update.
7. If Studio diagnostics parsing or CrowdyJS's starter projects changed, run
   `studio-state-fixtures.mjs` the same way; `--record` rewrites every case's
   expected answer from CrowdyJS (after adding a case, or when CrowdyJS changed
   on purpose). (The agent, control-gate and Studio-host fixtures went with the
   Crowdy Agent orchestrator in 0.34.0; the agent runs in the browser now and
   this SDK only reads its policy, consent and usage.) Bound-project saves
   are portable as of 0.38.0 (`saveProject` commits through
   `crowdyStudioGitHubPutFile` / `DeleteFile`); the hosted GitHub settings
   card on `CrowdyStudioController` stays a browser exclusion.

### CrowdyPy wraps this tree

CrowdyPy (the Python SDK) vendors this repository at a pinned release and binds the
replication `Connection`, `WorldSession` and the wrapper seams added for it in 0.54.0:
`Config::onEventsReady`, `IChunkSource` with the injectable `ChunkStore` constructor (since
0.82.0 called on the store's I/O thread, or on the thread that calls `runIo()`; CrowdyPy turns
`Options::ioThread` off and its staged source answers inline),
`IHostElection` and `WorldSessionServices`. Since 0.84.0 a source should fill
`StoredVoxelState::updatedAtMs` (the entry's `updatedAt`): without it the store merges by arrival
order. A change to those contracts is a change to
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
`docs/compatibility.md` names the pin too and is edited by hand. When the new
CrowdyJS carries surface the matrix does not classify yet, `parity:repin` stops at
its last step with the pin, the literals and the fixtures already written and a
failing matrix on disk: classify what `docs/parity-matrix.md` lists under
"Unclassified failures" and "Stale baseline failures", write it again with
`node tools/parity/parity.mjs --crowdyjs <path> --strict --write docs/parity-matrix.md`,
then do the steps the tool would have printed (the `rg`, `check:release`, the build
dirs). If CrowdyJS's `schema.gql` moved, run `npm run schema:drift` first: 0.94.0
synced the published SDL in the same change, which left only the fields it did
not carry yet for a dated group, and 0.94.1 is what that group cost the same day
("Fleet sessions"). Five things reliably bite when this is done by hand:

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
- **A schema synced ahead of the pin is a dated group.** When `schema.gql` comes
  from a Game API branch before the CrowdyJS that mirrors it is released on the
  tier, the differences go in a dated `SCHEMA_BASELINE` group in
  `tools/parity/parity.mjs` (0.40.0 and 0.63.0 did this). The re-pin turns every
  entry stale and the gate refuses until you delete the group; update the pin
  paragraph in `docs/compatibility.md` and the README target line with it.

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

The class scan reads only `src/domains`, `src/kit`, `src/kit/modules` (since 0.64.0),
`src/stores`, `world.ts` and the Studio modules. A CrowdyJS module outside it enters the matrix through
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
v2.27.0): ck-exec (`client.exec()`) replaced them. Since 0.64.0 the Game Kit is the client
half of `ckx_sdk::kit`: typed modules for ck-exec's starter hubs over one `KitRuntime`
(`crowdy/kit/runtime.hpp`, `mirror.hpp`, `modules.hpp`, `topology.hpp`, `realtime.hpp`, and
since 0.65.0 `genres.hpp` for the genre starters and since 0.66.0 `economy.hpp` for the
`player` and `market` hubs, whose `PlayerKit` makes its mirrors once the runtime knows the
player's id and relays them into stores that exist from the start; `Duel.throw` is `throwHand`, aliased in
`METHOD_ALIASES`),
beside `social()`, `kit/wire.hpp` and `kit/actions.hpp`. Its protocol is held to ck-exec's
golden record (`tools/parity/fixtures/kit-protocol.json`, copied by `parity:repin` from the
pinned CrowdyJS): `kit_runtime_test` replays each starter's script through a gateway that answers
only the next recorded step. A starter change rewrites the record in ck-exec, CrowdyJS copies it,
then this repo and CrowdyPy do, on the same tier. The runtime's waits run on one timer thread
(like `ExecConnection`'s deadlines) and post to the dispatcher; module state lives in a
`shared_ptr` core that callbacks hold weakly, so a module destroyed with a reply in flight is
never touched. Crowdy Studio's SERVER target
is a mod and its CLIENT target that mod's CLIENT half (`CrowdyStudioModRuntime`).
Do not re-add a wrapper for a `gameModel*`, `compute*`, `playerModel*`,
`playerCompute*`, player-code marketplace or WASM-policy field: the schema no
longer has them, and codegen refuses an operation that names one. The Game API
keeps tier features (`admin().appAccess()`), grid claims, the studio moderation
fields and the compute budget (`gen::computeUnits`).

## Teen safety (0.75.0, 0.76.0)

`client.safety()`, `client.moderation()` (the same `ModerationAPI` as `admin().moderation()`), the
age, country and document calls on `AuthAPI`, `platform().territoryPolicies()`,
`admin().apps().recording()` / `setRequiredDocuments()`, `portal().minecraftChallenge()` /
`signInWithMinecraft()` and the `graphql::*Of` refusal readers mirror CrowdyJS 18.24.0 and
18.25.0 name for name (`safetyModeratorRequiredOf`, `hasSafetyModerator` on app reads,
`TerritoryPolicy::regionCode`, and `parentalConsentUnavailableOf` returning the refusal with its
`reason` since 0.76.0). Do not read `minimumAge` or `ageBracket` from `AGE_REQUIREMENT_NOT_MET`:
program decision 35 says no refusal names an age, and ck-api v2.61.0 sends neither. CrowdyJS
keeps a shared fragment in a file of its own; this codegen resolves fragments only within one
file, so `operations/safety/` and `operations/moderation/` keep each domain's fragments and
operations together.

`crowdy::startGameSession` (`crowdy/game_session.hpp`) starts a session from any app token
(a mint, a portal exchange, an organization's sign-in, `signInWithMinecraft`); the Minecraft
mod's native libraries build on it. A `GameSession`'s connection refreshes its token and moves
through its client, so its assignment and destruction release the connection first; keep that
if you change the struct. `e2e_teen_safety` exercises it live with `CROWDY_E2E_MANAGED_FILE`.

**Never sign up or record an age under 13 against a shared tier.** Since ck-api v2.61.0 an age
refusal at `register` or `recordPlayerAge` locks the caller's address and user agent out of both
for 24 hours, which on the shared builder blocks every harness; a managed player's refusal locks
only that player (its organization and external id). `e2e_teen_safety` therefore registers its
one CK account only with `CROWDY_E2E_TEEN_SIGNUPS=1`, and reads the existing-player cases from
`CROWDY_E2E_PLAYER_EMAIL` / `_PASSWORD`.

## ChunkStore merging (0.84.0)

`ChunkStore::confirm()` is the one path a confirmed value takes (a notification, a catch-up edit,
an echo settling a voxel): it compares the server's time with the voxel's (`ChunkData::times_`,
16 bytes per voxel newer than `storedAtMs`) and drops an older change. `arrivals_` and `mergeSeq_`
order merges for a source without times. `applyStored()` keeps a merged value newer than the
copy's entry (or row) for its voxel. A voxel with unanswered edits is a `PendingCell`: its edits
over the confirmed `base`; `finishEdit()` shows the next optimistic edit or the base. `show()`
reports a change only when what the voxel shows differs, so `ingest()` returns false for most
echoes. An `IAsyncChunkSource` answer reaches the store through `ReplySink` (detached in the
destructor, so a late answer is dropped) and is parsed in `serviceIo()`. Session tests that fake
an echo must carry the edit's state. `CONTROL_BOUND_ACK` (46) records the nonce it verified in
`boundNonce_`; legacy 22 and 28 are dropped only while that is the current `controlNonce_`, so a
reassignment that races an acknowledgement cannot carry it over.

## The voxels write and the read limits (0.85.0)

ck-api v2.70.0 supersedes the edits recorded at or before a whole-chunk write and reports it as
`Chunk.voxelsUpdatedAt`; chunk-state and LOD writes move only `updatedAt` (OI-2026-11-13-003).
`applyStored()` dates a copy's voxels by `StoredChunk::voxelsUpdatedAtMs` (an `std::optional`: 0 is
the server's null, no floor; unset is a source that does not say, dated by `updatedAtMs` as before)
into `storedAtMs`, and keeps the row's time in `rowAtMs`, which the hydration and catch-up
staleness checks compare. `ingestChunkChanged()` still compares `storedAtMs`: `world.set_chunk`
sends its voxels write. The parsers (`storedChunkOf`, `chunkPage`, `editsPage`) take a
`ReadBudget` over `ChunkReadLimits` and check a base64 length before decoding it. The curl
transport's `BodySink` counts the decompressed body against `HttpRequest::maxResponseBytes`;
`interpret()` refuses a larger body from any transport (`Errc::ResponseTooLarge`, kind Protocol).
`limitProtocols()` keeps requests and redirects on http and https (https stays on https).
`graphql_client_test`'s `LoopbackHttp` serves the over-the-limit bodies; it is POSIX-only.

## Spatial distance by message type; group caps (0.88.0)

`SpatialSend::distance` defaults to `replication::kTypeDefaultDistance` (0xFF), which
`Connection::sendLongSpatial` resolves before encoding: 1 chunk for `ClientAudioPacket` and
`ClientVideoPacket`, 8 for every other type, as CrowdyJS's `binary-wire.ts` and CrowdyPy's
`replication.py` default them (operator, 2026-11-19, cks-biz BIZ-031 Q1). Nothing puts 0xFF on the
wire; `replication_test`'s `runSpatialDistanceDefaults` reads each type's byte off the socket. A
new message type that should not reach 8 chunks by default goes into that resolution too.
`graphql::groupLimitReachedOf` reads ck-api v2.77.0's `GROUP_LIMIT_REACHED` from
`GraphQLErrorDetail::limit` and `limitValue` (extensions `limit` and `max`; not `max`, which
`<windows.h>` defines as a macro), and since 0.89.0 `graphql::appLimitReachedOf` reads
ck-api v2.80.0's `APP_LIMIT_REACHED` from the same two fields.

## The native Studio dock calls (0.86.0, 0.87.0, 0.93.0)

The Unreal SDK docks Agentic Crowdy Studio in its games, so this SDK carries what that dock calls on
the app-token client. `crowdyStudioAgent().liveModding` wraps `crowdyStudioLiveModding` (the parity
matrix lists it covered; CrowdyJS leaves it to its dock's host). `models()` is `GET
/v1/model/models` over `GraphQLClient::getOutcome` / `getAsync`, the one REST seam: the origin of
`endpoint()`, the bearer `AuthState` holds at send time, the client's transports, timeout,
response limit, dispatcher and `AsyncScope`. Add another REST route there, never a second HTTP path.

`crowdy/studio/starter_projects.hpp` is CrowdyJS's `createCrowdyStudioStarterProject` byte for
byte, JavaScript's `trim()` and the two non-ASCII letters `toLowerCase()` turns into ASCII (U+0130,
U+212A) included. `tools/parity/fixtures/crowdy-studio-starter.v1.json` holds its cases, recorded
from CrowdyJS (`studio-state-fixtures.mjs --record`) and replayed by `studio_dock_test`; a CrowdyJS
starter change lands there first. The en dash in the CLIENT `Cargo.toml` is a `\xE2\x80\x93` escape,
as `runtime.hpp` writes its curly apostrophes: MSVC reads a source's bytes by its code page.

`CrowdyStudioModRunner` (`crowdy/studio/mod_runner.hpp`) is the controller's draft test and live
deploy over the `...Async` calls: one `Run` per call, every callback holding it weakly and checking
`cancel()`, build polls on one timer thread that posts to the client's dispatcher (KitRuntime's
pattern), and `finish()` called once. It needs a dispatcher; `createCrowdyStudioModRunner()` gives
it the client's. `crowdyStudioIsModName` and `crowdyStudioModCrate` in `runtime.hpp` are the crate
rules it shares with `CrowdyStudioModRuntime`. `crowdy/studio_origin.hpp` derives Studio from the
endpoint (`ck.<root>` or `ck-<label>.<root>` to `studio.<root>`); `default_origin.hpp` stays
generated and untouched.

`parseRustcDiagnostics` uses no `std::regex`, and nothing here may again: MSVC's throws
`error_stack` and libstdc++'s overflows a 1 MiB stack on an 8 KiB line. Its four scanners each give
exactly the match of the pattern written above them, lazy `.+?` groups giving back a leading space
included, and they stay linear: `LineScan` finds the next `]` once and the trailing spaces once.
`crowdy_studio_state_test` reads hostile 16 KiB lines against a time bound.

0.87.0 (ck-api v2.73.0, CrowdyJS 18.33.0) adds the platform's half. Every build the runner, the
controller and `CrowdyStudioModRuntime` queue sends `ExecStudioSource` (the project and the
`revision.id` its files are), never the rider mod built from the mod starter. A new project's
CLIENT target is the platform's starter (`exec().modClientStarter`, through
`ICrowdyStudioRuntime::clientStarter` or the async helper), and `crowdyStudioClientStarterFiles`
is the fallback for a host that answers 503: keep it byte for byte the platform's
`compute-toolchain/client-starters/client-half` (cks-game-api), whose copy CrowdyJS also carries.
`crowdyStudio().links()` is `crowdyStudioLinks`; `studioOriginFor` stays the offline derivation.
`tools/parity/repin.mjs` restamps the fixtures even when the pin does not move: a promotion merges
in fixtures stamped for the source tier.

0.93.0 (CrowdyJS 18.39.1, the Unreal SDK's follow-up asks) makes a run survive a dropped read.
`crowdy/graphql/dropped_read.hpp` (`graphql::detail`, not API) is the one rule, shared with
CrowdyJS's `src/dropped-read.ts` and CrowdyPy's `_dropped_read.py`; change the three together. A
dropped read is Network, Timeout, Protocol other than `Errc::ResponseTooLarge`, an Http 5xx or 429
without GraphQL errors, or a GraphQL error with `retryable && blame == "PLATFORM"` (`retryable`
alone defaults true; that pair is `PLATFORM_BUSY`). `isDroppedReadException()` is the same verdict
for what `GraphQLClient::request` throws, called inside a catch block; it rethrows to classify, so
it needs no RTTI. The runner's `poll()` counts a dropped status read as a missed poll (toward
`pollLimit`, waiting at least `retryAfterMs`, the limit's message naming the last miss) and
`later()` reads the project, live-modding, listing and starter reads once more; mutations are never
repeated. `CrowdyStudioModRuntime::buildStatus` reports a dropped read as
`CrowdyStudioRuntimeVersion::readFailure`, outside the deploy that follows a success, and the
controller counts it as a missed poll; `ensureClientOnlyMod` reads its two reads once more and
`ExecAPI::waitForModBuild` reads again until its deadline (exceptions build only).
`readFailure = std::nullopt` keeps a runtime's three-member brace init free of
`-Wmissing-field-initializers`.

Each `CrowdyStudioRunTarget` settles: `state` moves as a step completes (Building once a build is
queued, Built or BuildFailed, Deployed, SwitchedOn, Attached, Consented), and `recordModState`
copies `modId`, `enabled`, `running` and `blocked` from the deploy, switch-on and listing answers;
a `blocked` of null clears it, a missing key leaves it (`Json::isNull` is true for a missing member,
so it checks `ok()`). The CLIENT target carries the mod it rides. A switch-on answer with `blocked`
still attaches and consents (the API attaches to a blocked mod and filters at serve time), then
`succeed()` ends `Blocked`. `guard()` and `start()` catch everything a step or `onProgress`
throws and finish `Failed`; `finish()` drops what `onDone` throws.
`createCrowdyStudioModRunner(appToken)` builds a `GraphQLClient` with its own `AuthState` over
the client's sync and async transports and dispatcher, at `gql_->endpoint()`; its
`WRONG_DATACENTER` handler holds the client weakly (never `this`, which a `CrowdyClient` move
leaves dangling), checks `isSameEstate` and moves only itself. The runner keeps that `AuthState`
for `setAppToken`.

## Fleet sessions (0.94.0)

0.94.0 (CrowdyJS 18.40.0) is the release that carries them. The platform's parts are on dev, test
and prod since 21:08 UTC on 2026-11-26 (Application API v2.89.1, realtime servers v0.50.3, ck-exec
0.25.0), and fleet sessions are off for every organization until Crowded Kingdoms switches them on
for one, so `open` answers `FORBIDDEN` until then.

`replication::FleetSession` (`crowdy/replication/fleet_session.hpp`) is one realtime session of an
organization's game server that carries its players (the fleet session design, step F3). An
organization's
own server is an audience this SDK serves, beside players, developers and org-admins: it holds the
org token, and nothing here is for a platform operator.

It owns a `Connection` and adds to it through three private seams, all inert on any other
connection: `Connection::FleetHooks` (`fleet_`: the three server messages queued as
`Event::Kind::Fleet`, a `tick` at the end of every housekeeping pass, an `advertised` call after
each capabilities advertisement, extra capability bits), the `Sender` argument of the private send
workers (whose `gameTokenId` the tail carries; the HMAC is always the connection's token), and
`src/replication/dispatch.hpp` (the typed dispatch both `poll()`s use). Keep `Connection`'s public
API and its behaviour without `fleet_` as they are; a new `Connection` send needs its `...As`
worker and a `FleetSession` form.

The carried table changes on two threads under `playersMutex_`: `carry()`, `release()` and the
sends from any thread, the resends from the network thread's `tick`, and results, releases and
deadlines from `poll()`, which is also the only thread a callback fires on (after the lock is
dropped). A carry that is refused or times out removes its row.

The wire is the realtime servers' v0.50.3 as built (the docs site's wire formats page has the
bytes). v0.50.3 is the first release with fleet sessions to leave dev: name v0.50.3 in anything
public. (The two wire changes below date from v0.50.1, which never left dev either.) What a
change here must keep:

- 51, 52 and 53 are read in both forms, whatever the session advertised: followed by their own
  HMAC (alone, or in a plain bundle; `wire::verifyFleetFrame`), or bare inside a verified signed
  bundle. `Connection::handleDatagram` verifies and strips the HMAC and queues the frame without
  it, which is why a 1,242-byte datagram fits the event ring's 1,232-byte slots.
- The notification inside a 53 is the unsigned form. `parseFleetDeliveredSpatial` refuses
  anything else, so nothing an ordinary session would have had to verify gets in that way.
- Under its own id the session sends 29, 49, 50 and 26 and nothing else. Its capabilities are
  always on and leave first (`ready()`); a new send that is the session's own needs the servers
  to take it first.
- Carries are paced (`takeCarryTurnLocked`: 50 a second after a burst) and each send has a new
  time and proof.
- A new session epoch on the server the carries were made on leaves them standing; another
  server, another fleet token or id, and a session opened again carry every player again with
  `carried` false until each 51 (`recarryAllLocked`).
- An error frame has no HMAC of its own; v0.50.1 sends a fleet session's inside its signed
  bundles, so `GenericError::verified` is true for them. With `fleet_` set no error frame
  changes anything, verified or not: `TOKEN_EXPIRED` answers one send and is about that
  player's token (the fleet token's own expiry is a signed 45, reason 2), and of the eight
  reasons for `UNAUTHORIZED` most are not about the carry. `FleetSession::onGenericError`
  hands every frame to `genericError` and, for a verified one, names the player through
  `sendRefused` when the sequence number allows it: `sentBySequence_` (filled by the
  `FleetHooks::sent` hook in Connection's three send workers) keeps, per number, the last
  player and when a message for anyone else last left under it, and a player is named only
  when nothing for anyone else left under the number in `kFleetRefusalWindowMs`. A player's
  `Connection` reassigns on a verified `TOKEN_EXPIRED` only (0.95.0, below). Anything new a
  fleet session or a connection does on the server's word needs a verified frame.
- A verified COMMAND_SESSION_RELEASED (45, or 28 before the bound forms are acknowledged)
  with any reason but 5 ends the session's carries (`FleetHooks::sessionReleased`, on the
  network thread; the handler fires from `poll()` through a two-byte `Fleet` event). Rows
  stay, `carried` false and an attempt begun, and `carriesHeld_` keeps every FLEET_CARRY in
  until `place()`, a new token or a new epoch that carries again: a server that ended the
  session would answer none, and each would time out and take its row along. v0.50.1 sends
  reason 0 to a session closed through the API. Reason 5 is Connection's idle path.
- A refreshed token's carry (`PlayerToken::replaces`, the contract's 9.3) moves the row to
  the new id (`supersedeLocked`, by node handle: a row holds two `ScrubbedToken64` and does
  not move) and keeps `previousId` and `previousKey` to go back to. `aliases_` maps the old
  id to the row while it waits, `retired_` remembers ids for a minute so their late 51 and
  52 are counted (`Stats::replacedIdFrames`) and ignored. `Player::sendId` is the id the
  player's sends name: it switches in `sendCarryLocked` when the new 49 leaves, after
  `sendsInFlight_` reaches 0 (`SendScope` counts a send from its check to its transmission,
  under `playersMutex_`), so no message names the old id after the carry has left. A send
  worker for a player goes through `SendScope`; do not send for a player around it.
- A status 1 is asked once more (`Player::noLiveToken`): the first sets the next send a
  resend interval away, and only a status 1 after that carry left settles.
- 51 and 52 carry no time, so a signed one can be replayed. A 52 for a row the table does not
  hold reports nothing (it is still counted in `messagesReceivedByType`); a 51 with no carry
  waiting is ignored; a reason byte other than 1, 2 and 5 is `FleetReleaseReason::Unknown`
  (never a cast). A 51 does not say which carry it answers, so a status 3 settles the carry
  only when every carry that left has had one (`Player::refusals == sends`); until then the
  first sends one more carry at once, the resends go on, and the deadline ends the carry
  `ProofRefused`. Do not settle on "the second status 3": two late carries, or a follow-up
  sent in the millisecond of the carry before it (the same bytes, a replay), each give one
  while the server carries the player.
- A row's token is a `wire::ScrubbedToken64`, zeroed by its destructor (a `volatile` byte
  loop, `wire::scrub`): keep a player's token out of any member that outlives the row.
- The socket is `connect()`ed to the server, so the kernel drops other sources. `UdpSocket`
  takes its `gate_` shared in `send()` and `wake()` and exclusively in `open()` and `close()`,
  because a fleet session's game thread sends while the network thread reopens the socket;
  `recv` stays on the network thread and takes no lock.
- A full send buffer is `WouldBlock` on every platform (`socket_errors.hpp`: EAGAIN,
  EWOULDBLOCK, ENOBUFS; macOS loopback gives ENOBUFS on a burst). A test that sends in a burst
  sends again on `WouldBlock` only: `SocketError` is a fault, `UdpSocket::lastSendFault()` and
  the WARN line name it, and retrying it in a test would hide it.

**`fleet_session_test` must not depend on how long a datagram takes to cross the loopback.**
Linux hands a loopback datagram to its receiver before `sendto()` returns; macOS does not, and
a step that pumped once or read "what has arrived by now" failed there in one job of four.
The fake server therefore knows its session (`FakeServer::serves`, which `openConnected`
calls) and waits on counts, in real time, with the test's clock standing still: a read first
takes as many datagrams as `Stats::datagramsSent` says left and hands them over in the order
of their sequence numbers, and `FakeServer::send` returns when `Stats::datagramsReceived` has
moved. A new case binds its server before the session opens, reads with `recvOne`, `takeAll`
(`sentSoFar`, `drain`) or `expectNothing`, and never sleeps or pumps "long enough". Reproduce
a loopback that takes time on Linux in a network namespace of its own (the head of the test
has the commands; never put netem on the host's `lo`), and **reproduce a failure on purpose
before calling it fixed**: four green macOS jobs can be chance. Two things about that tool:

- netem reorders. With jitter always; with a plain delay on any machine with more than one
  CPU (about 300 of 140,000 datagrams of one thread at 2 ms on eight cores). The count-and-
  sequence harness holds under both. `runSendsInFlightSwitchWithTheCarry` cannot: it judges by
  the order of arrival from two sending threads, which is the point of it (sequence order
  would hide a send that left after the carry). Run it under `taskset -c 0`, or leave it out
  with `FLEET_TEST_LOOPBACK_REORDERS=1`; it names the reordering it saw when it fails.
- `replication_test` and `fleet_sessions_api_test` use the same fake-server pattern and held
  under a delay of 2 ms and of 5 ms (2026-11-27: every run, but for one of 95 runs of
  `replication_test` at 5 ms, at `members == 2` in `runBundlingNetThread`, a real-clock case
  about a 2 ms window; it did not come back in 70 more runs, 30 of them under CPU load, so
  nothing was changed for it). `replication_test` fails under jitter, at checks that read two
  datagrams sent in one pass in the order they arrive. No loopback reorders, so it was left
  as it is; give it this harness if it ever fails on macOS. `session_test`'s first case
  (`run`) does the same and fails under jitter in the same way (5 of 8 on 2026-11-27); its
  two keepalive cases keep one datagram in flight at a time and held, as the whole file
  does under a plain 5 ms delay (8 of 8).

`e2e_fleet_session` is the live suite (`CROWDY_FLEET_*`, label `e2e_optional`; its header says
what it leaves on a tier). It passed on dev on 2026-11-26 against the realtime servers' build
before v0.50.1 and Application API v2.89.1 (`docs/e2e-coverage.md`). Three of its steps
differ on v0.50.1 (a closed session is told, a refusal is verified, a refreshed token's carry
has no refused send): each prints what it saw, and `CROWDY_FLEET_REALTIME_MIN=0.50.1` makes
that behaviour required. It ran so on dev on 2026-11-26 against v0.50.1 and against v0.50.3
(exit 0: a closed session told after 10 ms, refusals verified, no send refused across a refreshed
token's carry, no notification of another app and none unsigned). What the runs
showed, which a change here should keep in mind:

- That earlier build sent a refusal (`UNAUTHORIZED`) outside a signed bundle, so
  `GenericError::verified` was false for it.
- It sent a session closed through the API nothing more: no error frame, no 46 for its
  capabilities, no 52. v0.50.1 sends a signed 45 with reason 0, once, which can be lost; a
  session never authorized on its server is still sent nothing. So the silence timeout is on
  by default for a fleet session (Michael, 2026-11-26): `FleetConfig::transport` starts from `fleetTransportDefaults()`, 45,000 ms, and
  a player's `Connection` keeps 0. The rule is `FleetSession::checkSilence`, on the network
  thread at the end of every housekeeping pass, and `Connection`'s own watchdog is skipped
  when `fleet_` is set. It counts only what verified (`Connection::lastVerifiedMs_`: a signed
  bundle, a bound control frame, a frame with its own HMAC, or the assignment itself). It
  fires once per silence: `moveOrdered()` and `refreshDue()` turn true, the socket and the
  state stay, and `place()` (setServer, applyRefresh) starts the clock again and carries
  every player again, the same server included. With `FleetConfig::reassign` it asks for a
  reassignment instead. Keep these two: the capabilities interval is at most a third of the
  timeout, and the answer to the advertisement that leaves as the timeout passes has
  `kAcknowledgementGraceMs` to arrive, so two lost acknowledgements in a row are not silence.
  That wait is the smaller of `kAcknowledgementGraceMs` and the capabilities interval, counted
  from the last advertisement (0.95.0): at a timeout of 3,000 the interval is 1,000 ms, as long
  as the grace, so the check always found a fresh advertisement to wait for and never fired.
  `runEverySilenceTimeoutFires` holds 3,000, 3,002, 3,300, 4,500 and 45,000 to "after the
  timeout, and no later than one interval after it".
  `configFor` in `fleet_session_test` switches the timeout off, because those cases read every
  datagram and move a manual clock past a silent server; `watchedConfigFor` has the default.
- A fleet session takes nothing that is not its own (`Connection::handleDatagram`, under
  `fleet_`): a long-spatial notification naming another app is dropped and counted in
  `Stats::wrongApp`, plain and inside a 53, and a plain one that nothing signed in
  `Stats::unsignedNotifications` (`countNotOwn` logs each at the first and at every doubling).
  The dev servers were seen sending a fleet session another app's state inside signed bundles
  that verify; that is a server defect, and the guard stays whatever the servers do. A
  player's `Connection` makes both checks since 0.95.0 ("A player's connection", below).
  `e2e_fleet_session`'s last step requires both counts and the HMAC failures to be 0 and
  prints what the log named when they are not.
- `Connected` does not wait for the 46: after `sessionReadyWaitMs` (1.5 s) the session is
  `Connected` whether or not the server acknowledged its capabilities.

The golden bytes in `wire_test` and `fleet_session_test` (FLEET_CARRY, FLEET_RELEASE, a carried
player's actor update, and a signed 51, 52 and two 53s) were computed independently in Python
(the script is quoted in `wire_test`) and are what the realtime servers' tests are checked
against: a layout change edits both sides.

`ExecConnection::openAsServer` / `connectAsServer` mark a connection as a server's
(`Impl::server_`), which is what lets `callAs` send client frame 0x05; the role is how the
connection was made, never read from the connect token. `openAsServer` never reconnects (it
forces `reconnect` off, as `open` does); `connectAsServer` does, with a dialer.
`tools/parity/fixtures/exec-client-frames.json` is ck-exec 0.25.0's own
`crates/ckx-proto/tests/client-frames.json` (branch `dev`), byte for byte, and the same file
CrowdyJS tests against: copy it over again when ck-exec's changes, never edit it here. Cases
only this SDK has (a player id past 2^53, an empty key, a multi-byte key) are a table in
`exec_test`.

The Application API calls are `client.fleetSessions()`, the same instance as
`client.admin().fleetSessions()` as CrowdyJS has both (`crowdy/domains/fleet_sessions.hpp`:
`open`, `refresh`, `close`), and `exec().serverEndpoint` / `connectAsServer`. They are the first
wrappers here that only an org token can call: the bearer is whatever the client holds, so a
game server gives its client the org token with `setToken`. `mintManagedPlayerToken` stays
unwrapped. **Refresh and close work only with the org token that opened the session**; say so
wherever these calls are described. Since 0.94.1 the four documents are generated
(`operations/fleetSessions/FleetSessions.graphql`, CrowdyJS's directory name, and
`operations/exec/Exec.graphql`), byte for byte what 0.94.0 wrote as `R"gql(...)gql"` constants in
its headers; `FleetSessionsAPI::k...Document` and `ExecAPI::kExecConnectAsServerDocument` are
aliases of the generated constants, kept because 0.94.0 published them.

**0.94.1 is what a dated `SCHEMA_BASELINE` group costs when the published SDL moves.** 0.94.0
synced `schema.gql` to the published SDL of 2026-11-25 (v2.88.0) and listed the 14 fleet fields
CrowdyJS 18.40.0's copy had beyond it (the Application API's dev schema, v2.89.1) as a dated
group. The SDL was published with v2.89.1 at 21:44 UTC that day, seven minutes after that sync
and 43 minutes before the tag; nothing on `dev` noticed, because CI's schema gate is offline,
and `promote.mjs` to test was refused at
`parity:repin` with `stale=14`. The fix was the sync, the group's removal and the documents'
move, and because generated headers ship, a patch version. So: before a release that carries
such a group, and again before promoting it, run `npm run schema:drift`. If the SDL has caught
up, sync first and release without the group. The published SDL now orders its types as the
Application API's own schema does, so that sync rewrote the snapshot's 14,000 lines once; the
matrix compares by meaning.

`replication::FleetSession` makes no API call. `refreshDue()` (half the token's lifetime gone, or
`moveOrdered()`) is the only policy it holds, and the caller acts on it: `fleetSessions().refresh(
session)` names the server the session is on unless a move was ordered, and
`domains::applyFleetRefresh` hands the answer to `FleetSession::applyRefresh`. A move order
with no `FleetConfig::reassign` reopens the session at the server it has and sets `moveOrdered()`.

What the 0.94.0 release pass did, and what is left (0.94.1 changed none of it):

- The pin moved to CrowdyJS 18.40.0 (`dev/v18.40.0`, 13ac165), so `NATIVE_FIRST_SURFACES` in
  `tools/parity/parity.mjs` is empty again (it lists a surface this SDK carries before CrowdyJS
  does, and goes stale, on purpose, when CrowdyJS carries it). CrowdyJS keeps fleet sessions
  outside the class scan, so `src/fleet-wire.ts`, `src/fleet/session.ts`, `src/fleet/socket.ts`
  and `src/server-only.ts` are in `CROSS_CUTTING_EXPORT_MODULES` with every export mapped, and
  seven `fleet-*` entries in `CROSS_CUTTING_BEHAVIORS` hold the rest. A new export in one of
  those modules fails the gate at the next re-pin: map it.
- **The table cap is CrowdyJS's since 0.95.0** (`fleet-session-table-cap` in
  `CROSS_CUTTING_BEHAVIORS` now says so). `tableLimit()` is `maxPlayers` (1,024 when
  `FleetConfig::maxPlayers` is 0 or above it) and `kFleetTableMargin` (32) more rows. `carry()`
  for a new player at the limit sends nothing, makes no row and returns `Ok`: the result,
  `FleetCarryStatus::TableFull`, waits in `endedHere_` for the next `poll()`, where every other
  result is reported (`expireCarries`), and `Stats::carriesRefusedTableFull` counts it. A carry
  for a player in the table, and one whose `replaces` names a row, pass (`supersedeLocked`
  runs before the check). `fleetConfigOf` and `applyFleetRefresh` carry `maxPlayers`.
- `CrowdyClient::fleetSessions()` was added for CrowdyJS's `client.fleetSessions`.
- CrowdyPy has no fleet session. Its next release re-vendors at `dev/v0.94.1` and moves its own
  CrowdyJS pin to 18.40.0 in the same change (`MIGRATION.md`, 0.94.0 and 0.94.1).
- `e2e_fleet_session` has run on dev only. Nobody has run it on test or prod.

## A player's connection (0.95.0)

What `Connection::handleDatagram` does for a connection with no `fleet_`, and why
(`tests/player_connection_checks_test.cpp` holds every line of it; OI-2026-11-26-005):

- **Dropped and counted, never dispatched:** a long-spatial notification that nothing signed
  (`containsAuth` 0 outside a signed bundle whose HMAC held) in `Stats::unsignedNotifications`,
  unless `Config::verifyNotifications` is off; and one that names another app than
  `Config::appId` in `Stats::wrongApp`, however it was signed. The signature is read first, so
  an unsigned notification of another app counts once, as unsigned. `countNotOwn` is the one
  place both are counted and logged, for a fleet session too.
- **Still delivered, on purpose:** each of the 11 notification types with its own HMAC (alone
  or in a plain bundle), as a member of a signed bundle, and a frame too large for a bundle
  alone with its own HMAC; channel notifications (18, 36) and error frames (3) in any form,
  because their layouts have no HMAC; anything when `Config::appId` is 0 or the notification
  names app 0 (0 is no app, so it names no other); an unsigned one when
  `verifyNotifications` is off. Those are the forms the realtime servers send a player's
  session, per the public wire formats. A new form the servers start sending needs a case in
  `runWhatACorrectServerSendsArrives` before a check that could drop it.
- **`TOKEN_EXPIRED`:** `poll()` reassigns on a verified one. An unverified one sets
  `tokenCheckRequested_`, and housekeeping runs the proactive refresh's own code for it
  (`provider_->refreshToken(&current)`), at most once in `kTokenCheckIntervalMs` and only with
  `refreshLeadMs` above 0 and the state `Connected`. A refresh that fails changes nothing. The
  realtime servers send a player's session every error frame alone, so today every
  `TOKEN_EXPIRED` takes the unverified path. Do not add a reassignment, or any other state
  change, on an unverified frame: 3 bytes on the flow would move the player.
- **Still settled by an unverified refusal:** the awaited send it names (`pendingWait_`), and
  what `WorldSession` and `ChunkStore` do with a `GenericError` (an optimistic edit rolled
  back). That is how the servers refuse a player's send, so it cannot wait for a signature.

**A refused keepalive makes one report** (realtime servers v0.51.0, on dev since 2026-11-27;
test and prod follow). What a client can see, and all this repo may say of it: a keepalive
(26) is accepted only where the player's actor update would be; where the player has no
`access` every keepalive is answered `[3][seq][7]`, unsigned to a player's session and inside
a fleet session's signed bundle; the session stays connected; access granted later admits
the next keepalive within about 6 seconds. So the SDK must keep the keepalives going at the
same rate, change no connection state on the refusal, and tell the game once.

- `Connection` remembers each keepalive under its sequence number (`keepalivesSent_`, written
  only by a keepalive send). `sendNumber_` is 32 bits, the wire's sequence number its low
  byte, so `keepaliveAnswered` can tell that no later message took the number (fewer than
  257 sends since) without any other send writing anything. An answer more than
  `kKeepaliveAnswerWaitMs` (3 s) after the keepalive is not its answer.
- `poll()` sets `GenericError::keepalive`, counts `Stats::keepalivesRefused`, and on
  `UNAUTHORIZED` makes or sustains the report (`keepaliveRefused`, `notPresent_` keyed by
  carried player and actor; `Handlers::notPresent` at the first). `presencePass` at the end
  of `poll()` ends reports: `Accepted` when a keepalive there has gone 3 s without a
  refusal, `Stopped` after `kNotPresentIdleMs` (30 s) without a keepalive. A keepalive or an
  update that names another chunk ends it `Moved` from the sending thread, and `poll()`
  hands that over. The actor's own update coming back from that chunk ends it `Accepted`
  (a player's connection only: that echo is signed, where the refusal is not).
- The 3 s is longer than the 2 s between two of `LocalActorStore`'s keepalives on purpose:
  one lost refusal is answered for by the next keepalive's, and the report does not flap.
  A new assignment forgets which keepalives wait for an answer (`presenceRestart`), since
  the server the connection left answers nothing.
- A player's session is sent the refusal unsigned. The report, the two counters and the
  `keepalive` flag are all it may change. Do not add a reconnect, a token check, a change
  of rate or anything else to that path: 3 forged bytes would then drive it.
- `WorldSession` keeps a keepalive's refusal away from `LocalActorStore::recordError` and
  `ChunkStore::ingestError` (it used to pass every refusal to both, by sequence number
  alone) and records one `SendKind::Heartbeat` error per report in `errors()`. It never
  recorded keepalive sends, so each refusal used to arrive there as an error of unknown or
  stale kind. `LocalActorStore` already kept its rate: nothing in `tick()` reads an error.
  `runKeepalivesKeepTheirRate` (`session_test`) holds that: the same datagrams at the same
  times over 30 simulated seconds, refused or not. A refused update is still recorded each
  time, as before; where a player has no access that is one every keyframe (3 s).
- `FleetSession::onGenericError` makes the report for a carried player. The first one needs
  a verified refusal and `refusedPlayer(sequence)` naming the keepalive's player, as
  `sendRefused` does; once a report stands, any verified refusal of that player's keepalive
  there sustains it, because a busy session (over about 85 messages a second) reuses a
  number too soon to name anyone, and the report would otherwise end as `Accepted` every
  3 s. A keepalive's `UNAUTHORIZED` no longer reaches `sendRefused`. Nothing here ends a
  carry or sends one.
- Reports are capped at `kMaxNotPresentReports` (4,096); past it no new one is made.

**The SDK waits 30 s at most on a server's word** (`kMaxRetryAfterWaitMs`, `retryWaitMs()`
in `core/result.hpp`; the security review of a90ca15, medium). Every place a `retryAfterMs`
or a gateway's "retry in N ms" becomes a wait goes through it: `connectRetryWaitMs`
(`startGameSession`), `graphql::detail::retryAfterMsOf` and `isDroppedReadException` (the
Studio runner's timer, the Studio runtime's and `waitForModBuild`'s sleeps),
`ExecReply::retryAfterMs()` (the kit runtime's `hold`), and `crowdy::bounded()` where an
`ApiFailure` is made or kept (`apiFailureOf`, `Connection::failAssignment`, the dial's
explanation). A new wait on anything a server sent uses `retryWaitMs()`; a test of it
records the wait instead of sleeping (`GameSessionOptions::sleep`). `error_extensions.hpp`
reads a hint past int64 as the largest int64, where the conversion used to be out of range.

**A server's text is untrusted** (the same review, low). `serverText()` makes it one line
of 500 bytes at most; `bounded()` applies it to an `ApiFailure`'s code (64) and message.
Anything new that logs or stores what the Application API or a gateway said passes it
through one of them first. The gateway's HTTP refusal reason (`kRefusalReasonMaxChars` in
`exec.cpp`) has its own loop of the same kind.

**The receive path is measured.** `benchmarks/bench_receive.cpp` (and `benchmarks/README.md`
for the recorded numbers) times `pump()` and `poll()` apart over datagrams sent before the
clock starts. 0.94.0 cost a player 15 to 17 % there (found by CrowdyPy's benchmark of the
vendored core): a `clock_.monotonicMillis()` for every verified notification, and the fleet
frames handled inside the per-message lambda. Keep both off that path: one clock read a
datagram (`now`, used for `lastRecvMs_` and `lastVerifiedMs_`), and anything only a fleet
session receives in an out-of-line member (`admitFleetFrame`). Before a change to
`handleDatagram`, build the tag before it and the change as Release in scratch worktrees, link
the same `bench_receive.cpp` against each, and run them in turn pinned to one core; this
builder has no hardware counters and no valgrind, so removing one thing at a time and
measuring again is the method. Forcing `parseLongSpatial` inline and testing for a
notification first were both tried and measured no gain.

**A receive pass ends.** `receiveBatch` reads `kReceivePassDatagrams` at most, and the network
thread leaves it at the batch in hand once `running_` is false, so housekeeping, the bundle
window and `disconnect()` do not wait on what is arriving. The flood tests do not depend on
speed: `HookedCrypto` sends two datagrams for every HMAC the connection computes, so the
socket never drains, and the disconnect test holds the network thread inside a batch until
`disconnect()`'s debug log line ("disconnect: stopping the network thread") says `running_`
is false. Keep that log line where it is, between the store and the join: the test's
ordering hangs on it. "Never drains" holds only where the loopback hands a datagram over
before `sendto()` returns, so the test asks first (`loopbackIsImmediate`) and makes the
exact-count checks only then; on macOS, or under netem, it checks that both calls return
and that `disconnect()` waited for less than one batch. Every other case in the file sends
through `deliver`, which pumps until `Stats::datagramsReceived` has moved (15 of 15 under
a 5 ms delay and under 3 ms of jitter, in a namespace of its own).

**Why a connection could not be made.** `ApiFailure` (`core/result.hpp`) is the one type:
`replication::AssignmentFailure` and `domains::ExecConnectFailure` are aliases of it.
`ISessionProvider::assignServerOrSayWhy` is what `Connection::doAssign` calls;
`ClientSessionProvider` fills it from `serverStatus().serverWithLeastClientsOutcome` through
`graphql::apiFailureOf`. `ExecAPI::dialer` keeps the last refusal in a `DialRefusal` the
connection reads (`explainDialFailures`), and the dial's failure message carries the code
and the wait so `ExecReply::platformBusy()` and `retryAfterMs()` can read them. A new call
that can be refused `PLATFORM_BUSY` on the way to a connection should hand its caller an
`ApiFailure` the same way.

**What 0.95.0's first pass left for the release pass:** the version in its six places and the
README's version line; the CrowdyJS pin (18.41.0) with `npm run parity:repin` and the new
surface it brings mapped; the schema sync (Application API v2.90.0) and the wrappers that
need it; finishing `MIGRATION.md`'s "Unreleased" section (its title, the platform versions,
CrowdyPy's re-vendor line); `docs/compatibility.md`'s line for the release; cks-docs, which
has nothing yet on the refused keepalive or the 30 s ceiling. When the pin moves, look at
what CrowdyJS does with a refused keepalive and classify the not-present report either way.
While the sibling CrowdyJS checkout is ahead of the pin, the parity tools refuse it: point
`CROWDYJS_PATH` at a clone at the pinned commit, built (`npm ci && npm run build`).

## Writing tests against `graphql::Json`

Index arrays with `at(i)`. `json[0]` is now a compile error: `operator[]` takes
a `string_view`, and the literal `0` used to bind to it as a null pointer and
segfault in `strlen` at runtime. Iterate with `forEach` / `forEachMember`, never a
loop over `at(i)`: an index into an array of objects or arrays walks from its first
element, so the loop is quadratic (0.79.0 found one chunk's 65,536 voxel states
taking about two billion steps that way). `toMsgpack()` is iterative on purpose:
keep it free of recursion, because a response's depth is the server's to choose.
`fromMsgpack()` decodes peers' and mods' bytes: never write a container key as text
(0.83.0 refuses array, map, binary and extension keys; a key nested in keys was
escaped again at every level, gigabytes from 130 bytes), and keep `dump()` writing
NaN and infinity as `null` rather than failing the whole value.
