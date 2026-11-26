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
`docs/compatibility.md` names the pin too and is edited by hand. Five things
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

## Fleet sessions (unreleased; dev only until Michael says otherwise)

`replication::FleetSession` (`crowdy/replication/fleet_session.hpp`) is one realtime session of an
organization's game server that carries its players
(`cks-project-root/project-root-docs/DESIGN-2026-11-fleet-session.md`, step F3). An organization's
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

The wire is the realtime servers' v0.50.0 as built (the build contract's section 8;
`cks-project-root/project-root-docs/DESIGN-2026-11-fleet-session.md` for the design). What a
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
- An error frame has no HMAC of its own. With `fleet_` set, `Connection::poll()` acts on
  `TOKEN_EXPIRED` only when `Event::verified` (the frame came in a verified signed bundle);
  every error frame reaches `genericError` with `GenericError::verified` saying which. A
  player's `Connection` acts on either, as it always has (changing that is the operator's
  call). Anything new a fleet session does on the server's word needs the same check.
- 51 and 52 carry no time, so a signed one can be replayed. A 52 for a row the table does not
  hold reports nothing (it is still counted in `messagesReceivedByType`); a 51 with no carry
  waiting is ignored; a reason byte other than 1, 2 and 5 is `FleetReleaseReason::Unknown`
  (never a cast). A status 3 while more than one carry has left settles nothing by itself
  (`Player::proofRefused`, `sentSinceRefusal`): one more carry leaves at once and its answer,
  or the deadline, ends the carry `ProofRefused`.
- A row's token is a `wire::ScrubbedToken64`, zeroed by its destructor (a `volatile` byte
  loop, `wire::scrub`): keep a player's token out of any member that outlives the row.
- The socket is `connect()`ed to the server, so the kernel drops other sources. `UdpSocket`
  takes its `gate_` shared in `send()` and `wake()` and exclusively in `open()` and `close()`,
  because a fleet session's game thread sends while the network thread reopens the socket;
  `recv` stays on the network thread and takes no lock.

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

The Application API calls are `client.admin().fleetSessions()` (`crowdy/domains/fleet_sessions.hpp`:
`open`, `refresh`, `close`) and `exec().serverEndpoint` / `connectAsServer`. They are the first
wrappers here that only an org token can call: the bearer is whatever the client holds, so a
game server gives its client the org token with `setToken`. `mintManagedPlayerToken` stays
unwrapped. **Refresh and close work only with the org token that opened the session**; say so
wherever these calls are described. The four documents are `R"gql(...)gql"` constants in the
headers (the audience gate and the matrix's root-field scan read them there) because
`schema.gql` is the published SDL and does not carry the fields: cks-game-api's own `schema.gql`
orders its types differently, so syncing from it rewrites 14,000 lines and brings fields
unrelated to this. When the SDL is published with v2.89.0, sync as usual, move the four
documents to `operations/` (`fleet/`, and `exec/Exec.graphql`), and use the generated constants.

`replication::FleetSession` makes no API call. `refreshDue()` (half the token's lifetime gone, or
`moveOrdered()`) is the only policy it holds, and the caller acts on it: `fleetSessions().refresh(
session)` names the server the session is on unless a move was ordered, and
`domains::applyFleetRefresh` hands the answer to `FleetSession::applyRefresh`. A move order
with no `FleetConfig::reassign` reopens the session at the server it has and sets `moveOrdered()`.

Still to do: the version bump and release, and the CrowdyJS and CrowdyPy mirrors.
`NATIVE_FIRST_SURFACES` in `tools/parity/parity.mjs` lists the surfaces in the matrix and goes
stale, on purpose, when CrowdyJS carries one.

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
