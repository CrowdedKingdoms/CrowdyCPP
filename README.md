# CrowdyCPP

The official portable C++ SDK for **Crowded Kingdoms**. CrowdyCPP gives native
games typed clients for auth, GraphQL HTTP and WebSocket APIs, and — unlike
the browser-first [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) SDK —
a **native UDP replication client** that speaks the
[Replication API wire protocol](https://docs.crowdedkingdoms.com/replication-api/intro)
directly to the replication servers, with zero-copy binary framing and no
GraphQL proxy in the hot path.

CrowdyCPP is designed from first principles to be:

- **Portable.** Standard C++20, CMake, Linux/Windows/macOS. No engine types,
  no framework assumptions. Every platform dependency (HTTP, crypto, clock,
  logging, allocation) sits behind a small interface you can replace.
- **High performance.** Steady-state zero heap allocation on the replication
  path, zero-copy datagram parsing, lock-free queues between the network and
  game threads, batched socket I/O, and no exceptions on hot paths.
- **Embeddable.** Usable directly by a native game, and equally designed to be
  wrapped by engine-specific SDKs — see
  [Wrapping CrowdyCPP in engines](#wrapping-crowdycpp-in-engines) for the
  Unreal Engine plan.

CrowdyCPP mirrors the [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS)
API surface (same domains, same two-token model, same error codes) so the
[platform docs](https://docs.crowdedkingdoms.com) and examples translate
directly. Where CrowdyJS routes realtime traffic through the Game API's
GraphQL UDP proxy (a browser constraint), CrowdyCPP opens a raw UDP socket and
implements the
[wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats)
and [HMAC scheme](https://docs.crowdedkingdoms.com/replication-api/hmac)
natively.

**v0.14.0 strict parity:** the generated matrix now reports zero portable
gaps, unclassified differences, and stale classifications against CrowdyJS
12.0.0. Production Agentic Studio uses typed GraphQL-WS durable events,
reconnect gap-fill, and lifetime-safe controller construction; the native
player-host dispatcher has an exact `IAgentBrowserToolDispatcher` bridge.
Game-model container changes have a typed subscription, and current platform
extensions add keyed container ensure/filter plus app listing-version
administration. Session stores expose real revision, dirty/save, queue, error,
local-actor, and owner-private avatar observability. See the
[compatibility matrix](docs/compatibility.md) and
[0.14 migration notes](MIGRATION.md).

**v0.10.0:** operator compute-ceilings coverage (`operator_().computePlatformCeilings()`
/ `setComputePlatformCeilings(input)` — the Track F platform-ceilings surface;
patch semantics: omit = unchanged, explicit null = clear, value = set; requires
`is_operator`). Requires the 2026-07-20 `cks-management-api` dev line.

**P1 player runtime:** `playerCompute()` wraps player-authored SERVER/CLIENT
Rust/WASM bound to player-owned grids; `gameApps()` now exposes first-class
ownership reads/assignments/transfers; and `admin().apps()` wraps app code
admission mode plus code/author/org allow-list administration.
`playerModel()` wraps owner/grid-confined flexible containers and player
automations. These methods
require the 2026-07-20 game-api/management-api player-runtime schemas.

**Player-code bundles:** `playerCompute().setRequires(...)` binds an immutable
SERVER version to its required CLIENT companion. `marketplace().gridClientMods`
returns marketplace/self-authored attachment provenance plus aggregate
per-author capability hashes; `marketplace().trustGridAuthor(...)` records the
visitor's capability-bound grant. Native clients fetch attachment artifacts
through `clientArtifact` and own their local sandbox/lifecycle implementation.

**Portable gameplay lifecycle:** `marketplace().claimGridChunk(...)` and
`releaseClaimedGrid(...)` mirror CrowdyJS's app-token SELF_CLAIM flow with
decimal-string BigInts. `refreshGameplayToken()` safely quiesces an active
native UDP connection, rotates the portal token, and reconnects the same
connection with its handlers preserved; its staged result distinguishes
refresh failure (old token retained) from reconnect failure (fresh token
retained). Routed HTTP and WebSocket bases normalize to one `/graphql`, while
explicit complete custom endpoints are preserved.

**Crowdy Studio (CrowdyJS v12 portable parity):** `crowdyStudio()` is the typed,
app-scoped project/library/common-file API, including optimistic atomic saves,
archive/restore, provenance-preserving imports, authored-module recovery, and
permission-gated common publication. `crowdy::studio::CrowdyStudioController`
is the headless project/autosave/conflict/checkpoint/runtime state machine.
Engines inject synchronization, approval, crypto, clock, and CLIENT artifact
runtime seams; CrowdyCPP does not ship DOM, Monaco, CSS, a Rust worker, or an
engine renderer.

**Native Agentic Studio:** `client.crowdyStudioAgent()` exposes the exact
`crowdy.studio-agent/1` Game/Management operations, while
`crowdy::agent::CrowdyStudioAgentController` provides durable attach/replay,
epoch fencing, approvals, leases, budgets, heartbeat renewal, and a typed host
tool seam. The immutable 28-tool registry is verified against CrowdyJS v12
canonical SHA-256 fixtures. There is no provider client, DOM driver, raw
GraphQL/UDP executor, or generic tool authority in this surface. See
[Native Agentic Studio](docs/native-agent-api.md).

**v0.9.0:** flow correlation (`gameModel().flow(appId, flowId)` — stitch one
flow correlation id into a single cross-engine timeline of model events,
automation runs, and compute module runs, each time-ascending; a diagnostics
surface gated by `manage_apps`), and the nullable `flowId` field on the
default `GmEvent` / `GmAutomationRun` / `WasmModuleRun` selections. Requires
the 2026-07-19 `cks-game-api` dev line (`gameModelFlow` + the flow-id
columns); older servers reject the operations with a GraphQL validation
error (everything else keeps working).

**v0.8.0:** container query predicates (`gameModel().containersWhere(...)` —
`where`/`limit`/`offset` on container lists), automation compute actions
(`actionKind: "compute_invoke"` with `computeModuleName`/`computeExport` on
`upsertAutomation`; automations invoke a compute-module export directly),
invoke-trigger contracts (`contractJson` on compute trigger reads — typed
params/result declarations validated server-side pre-sandbox), and the
`crowdy::kit::run_optimistic_action` helper (the packaged optimistic apply →
referee invoke → confirm/rollback loop with actionId receipts).
`gameModelContainerChanged` is available through the generic
`client.subscriptions()` GraphQL-WebSocket primitive; higher layers decide how
to pull and reduce events. Requires the 2026-07
`cks-game-api` dev line; older servers reject the new arguments/fields (omit
them and everything else keeps working).

**v0.7.0:** inventory blueprint/runtime parity includes generated atomic
crafting and barter transactions, explicit owner-mirror representation, and
the hardened admin-created/server-granted stack posture.

## Layout

```text
include/crowdy/          public headers
  core/                  bytes, endian, result, clock, logging, allocator interfaces
  wire/                  zero-copy wire codec + HMAC framing (header-only)
  graphql/               GraphQL HTTP + WebSocket transports, JSON, errors
  replication/           native UDP replication client
  session/               world session layer (actors, chunks, inboxes, host)
  kit/                   Game Kit (blueprints + runtime helpers)
  player_host/           typed Play capabilities, observations, leases, adapters
  agent/                 native local-tool dispatcher (no model/server fallback)
src/                     implementation
include/crowdy/generated/  committed codegen output (operations + enums)
operations/              GraphQL operation documents (codegen input)
schema.management.gql    exact published Management API SDL snapshot
schema.game.gql          exact published Game API SDL snapshot
schema.gql               merged snapshot used for cross-SDK schema comparison
scripts/                 schema sync + codegen (Node, maintainers only)
tests/                   unit tests (ctest) + env-gated e2e tests
benchmarks/              micro + end-to-end benchmarks
```

## Build

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev   # Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # unit tests: no network required
```

A clean external clone builds offline: the schema snapshot (`schema.gql`) and
the generated code (`include/crowdy/generated/`) are committed. Only maintainers run the
schema sync / codegen scripts (see [Schema refresh](#schema-refresh-and-codegen)).

Dependencies (all replaceable through interfaces):

| Dependency | Used by | Replaceable via |
|---|---|---|
| libcurl | default HTTP transport | `crowdy::graphql::IHttpTransport` |
| libcurl 7.86+ WebSocket APIs | optional default GraphQL subscriptions | `crowdy::graphql::IWebSocketTransport` |
| OpenSSL (libcrypto) | HMAC-SHA256 | `crowdy::core::ICrypto` |
| yyjson (vendored) | JSON parse/serialize | internal only, not on the UDP path |

The wire and replication layers depend only on BSD/Winsock sockets and the
`ICrypto` interface — no libcurl, no JSON.

CMake feature-detects libcurl's WebSocket symbols. If they are absent (or
`CROWDY_WITH_CURL_WEBSOCKETS=OFF`), the SDK builds with a clear no-default
fallback and `makeCurlWebSocketTransport()` returns null; injected engine
transports continue to work on Linux, macOS, and Windows. The factory also
checks that the linked libcurl actually advertises both `ws` and `wss`, since
some distributions expose the APIs while compiling those protocols out.

`CROWDY_NO_EXCEPTIONS=ON` enables the non-throwing compatibility path for
blocking GraphQL requests (failures return an invalid `Json`); use `*Async`
callbacks when typed failure details are required. Injected transports in this
mode must not throw across the SDK boundary.

## Quick start

```cpp
#include <crowdy/crowdy.hpp>

int main() {
  // 1) Identity client (Management API) — passwordless sign-in.
  crowdy::CrowdyClient identity(crowdy::ClientConfig{
      .managementUrl = "https://management.example.com",
  });
  auto login = identity.auth().devLogin("player@example.com");  // dev/test only

  // 2) Mint an app-scoped token and build the per-game client.
  auto minted = identity.portal().mintAppToken(appId);
  crowdy::CrowdyClient game(crowdy::ClientConfig{
      .httpUrl = minted.gameApiUrl,
      .managementUrl = "https://management.example.com",
  });
  game.setToken(minted.token);

  // 3) Connect the native replication client (assigns a server, installs the
  //    UDP session, waits for session-ready).
  crowdy::replication::Config repl{.appId = appId};
  auto conn = game.replication().connect(repl);

  // 4) Subscribe and join the world.
  conn->onActorUpdate([](const crowdy::replication::ActorUpdate& u) {
    // u.uuid, u.chunk, u.payload (span over the datagram — copy if you keep it)
  });
  conn->sendActorUpdate({.chunk = {0, 0, 0},
                         .uuid = myActorUuid,
                         .payload = poseBytes,
                         .distance = 8,
                         .decay = crowdy::wire::DecayRate::Exponential});

  // 5) Pump notifications from your game loop (or use the owned-thread mode).
  while (running) {
    conn->poll();
  }
}
```

The full lifecycle (tokens, server assignment, reconnect commands, token
refresh) is documented in
[Authenticate and assign](https://docs.crowdedkingdoms.com/replication-api/authenticate-and-assign)
and handled by `crowdy::replication` automatically.

### Generic GraphQL subscriptions

`GraphQLSubscriptionClient` implements `graphql-transport-ws` for portable
push APIs. It normalizes an HTTP(S) API URL to one WS(S) `/graphql` endpoint,
authenticates with the shared token in `connection_init`, bounds UTF-8 JSON
messages, and replays active operations after capped jittered reconnects.

```cpp
crowdy::graphql::GraphQLSubscriptionCallbacks callbacks;
callbacks.onNext = [](crowdy::graphql::GraphQLSubscriptionOutcome next) {
  if (next.ok()) {
    // Read next.data on the game thread.
  }
};
callbacks.onError = [](crowdy::graphql::GraphQLSubscriptionError error) {
  // Branch on error.kind / error.code; terminal auth, app-scope, and stale
  // client-epoch failures are not reconnected.
};
callbacks.onReconnect = [](crowdy::graphql::GraphQLReconnectInfo replay) {
  // Optionally start a durable gap-fill query before replayed events arrive.
};

auto subscription = game.subscriptions().subscribe(
    "subscription Changes($appId: BigInt!) {"
    " gameModelContainerChanged(appId: $appId) { containerId changedKeys }"
    "}",
    crowdy::graphql::JVal::object({{"appId", appId}}), "Changes",
    std::move(callbacks));

while (running) game.poll();  // all callbacks are delivered here
// subscription.cancel() is explicit; destruction also cancels.
```

Use the typed wrappers where available:
`game.gameModel().containerChanged(...)` maps container metadata pushes, and
`client.createCrowdyStudioAgentController(...)` owns the durable Agentic Studio
event adapter and replay/gap-fill lifecycle. The generic client remains for
application-specific subscriptions. See
[GraphQL WebSocket examples](docs/graphql-websocket.md).

## Sub-clients at a glance

CrowdyCPP mirrors CrowdyJS's domain layout. Game-client surface (end-user,
app-scoped token):

| Sub-client | What it does |
|---|---|
| `client.auth()` | Passwordless sign-in (magic link, social/OIDC, dev bypass), log out, linked identities. |
| `client.users()` | `me`, `updateGamertag`, profile reads. |
| `client.session()` | Token store, restore, set/get token. |
| `client.portal()` | `mintAppToken`, `refresh`, PKCE portal entry for cross-origin handoffs. |
| `client.serverStatus()` | `gameClientBootstrap(appId)` — version info, spatial limits. |
| `client.chunks()`, `client.voxels()`, `client.actors()`, `client.avatars()`, `client.state()` | World data reads + writes. |
| `client.host()` | Host election reads + actor liveness heartbeat. |
| `client.teleport()` | Teleport requests. |
| `client.channels()`, `client.teams()` | Messaging channels and app-scoped teams. |
| `client.gameModel()` | Abstract game model: containers, properties, functions, sessions, automations. |
| `client.compute()` | **Compute Modules** — server-side Rust/WASM logic: author + deploy source (`upsertModule`, `deploySource`), compile polling (`moduleVersions`), triggers + policy, synchronous `invoke`, monitoring (`moduleRuns`, `moduleStats`, `moduleLogs`, `appDiagnostics`). Server-only execution; see the [Compute Modules docs](https://docs.crowdedkingdoms.com/game-api/compute-modules). |
| `client.playerCompute()` | Player-authored SERVER/CLIENT Rust/WASM bound to player-owned grids: deploy, activate/deactivate, list modules/versions, and remove self-authored modules. |
| `client.marketplace()` | Player-code store/install/consent plus player-authorized one-chunk claim/release (`claimGridChunk`, `releaseClaimedGrid`) on the app-token Game API. |
| `client.crowdyStudio()` | Caller-owned Crowdy Studio projects and reusable files: list/get/create, revision-fenced atomic saves, metadata/file updates, archives, personal library, curated common files, copy-by-value imports, and authored-module recovery. |
| `client.gameApps()` | App grids, first-class ownership (`ownership` / `assignOwnership` / `transferOwnership`), and grid runtime-permission administration. |
| `client.subscriptions()` | Generic `graphql-transport-ws` operations with RAII cancellation, reconnect/replay notification, and game-thread delivery from `poll()`. |
| `client.crowdyStudioAgent()` | Exact Agentic Studio sessions/history/descriptors/budgets/control operations on Game API plus policy/usage/operator controls on Management API. Pair with `crowdy::agent::CrowdyStudioAgentController`; see [native agent integration](docs/native-agent-api.md). |
| `client.replication()` | **Native UDP** replication: connect/assign, spatial sends, notifications, channel publish, single-actor messages, heartbeats. |
| `crowdy::session::WorldSession` | SDK-managed game state: your actor with a fixed-Hz send loop, remote-actor registry with staleness + interpolation history, chunk/voxel cache, inboxes, host tracking — see [the session layer](#the-session-layer-data-structures-that-do-the-bookkeeping). |
| `crowdy::kit::makeKit(client, appId)` | Game Kit: ready-made mappings of game concepts onto the game model across 15 genre layers, plus the engine-aware helpers (`mobs()` refereed attacks, `pets()`, `engines()` capability detection, the `crowdy/kit/wire.hpp` engine pose codec + event parsers), blueprint builders, and `deploy()` for the admin "load the rules" step — see [Game Kit](#game-kit-genre-building-blocks-over-the-game-model). |

Studio-admin surface (privileged; drive with an org/admin token from a trusted
context): `client.admin().organizations() / apps()` (including player-code
admission policy) `/ appAccess() / billing() /
payments() / quotas() / environments() / usage() / sharedEnvironment()`.
Operator surface (platform operations, requires operator rights):
`client.operator_()`. The SDK never relaxes server-side authorization — these
are typed wrappers; the caller still needs the right token and permission.

## Headless Crowdy Studio

`client.crowdyStudio()` targets the Game API with the app-scoped player token.
All ids and revisions remain decimal strings, project source is filtered by
the server's `(app, owner, project)` tuple, and nullable metadata patches use
`CrowdyStudioPatchField<T>` so omit, explicit null, and value stay distinct.
The API exposes no raw operation executor.

For an engine-owned editor, construct the controller from injected interfaces:

```cpp
crowdy::studio::CrowdyStudioPlayerComputeRuntime runtime(
    game.playerCompute(), &engineArtifactRuntime);
crowdy::studio::CrowdyStudioController studio(
    {.appId = appId, .gridId = gridId},
    game.crowdyStudio(), runtime, engineCrypto, engineClock,
    &durableSynchronization, &agentApprovalGate);

studio.initialize();
studio.updateFile(crowdy::studio::CrowdyStudioTarget::Server,
                  "src/lib.rs", source);
studio.tick();  // engine-loop autosave/retry/monitor pump
```

Runtime actions are revision-bound:

- draft/live plans must name the exact complete project target set;
- live plans additionally bind pairing preference and the canonical
  full-project content hash, then pass through the injected agent approval
  gate before compilation;
- full-stack publication compiles CLIENT then SERVER, binds `setRequires`,
  enables SERVER, then starts the exact CLIENT artifact through the engine
  runtime;
- checkpoint restore likewise requires the external agent layer's opaque,
  exact approval grant;
- `state.runtimeSync` explicitly distinguishes never-run, running-saved,
  running-stale, and stopped state.

The synchronization and runtime interfaces are intentionally server-free in
unit tests. They do not grant grid permissions or source visibility: Game API
ownership, target write/run permissions, and admission checks still execute on
every playerCompute call. See [MIGRATION.md](MIGRATION.md) for the additive
surface notes.

## Native player-host and local tool integration

Native games can expose agent-addressable Play controls without browser APIs
through the installed headers under `crowdy/player_host/` and
`crowdy/agent/native_tool_dispatcher.hpp`.

- `PlayerHostAdapterV1` is the only gameplay execution boundary. Implement it
  over the same movement, inventory, interaction, crafting, mount, combat,
  chat, and travel intent services that human controls use. Do not hand an
  adapter a generic `CrowdyClient`, transport, input-injection, or raw network
  escape hatch.
- `AgentControlLeaseManager` binds Play authority to the exact client epoch,
  game context, controlled entity, capability revision, lease scopes, fresh
  observation, heartbeat, and command rate. Call `tick()` from the game thread.
  Human input, Escape, Stop, death, disconnect, and permission/admission/context
  changes call the corresponding synchronous preemption method.
- `NativeToolDispatcherV1` is an execute-once callback router for the 14
  mandatory `game.*` tools and the 11 native Studio/runtime tools. It validates
  canonical v12 descriptor digests, typed input/output bounds, mode, deadline,
  epoch, context, lease, and approval metadata; late or ambiguous effects are
  never blindly retried. Server, model, and provider tools have no local
  fallback.
- `CrowdyStudioHostAdapter` connects local Studio/runtime tools to the same
  headless Studio controller used by human actions. Engine adapters own thread
  scheduling; callbacks may complete inline or later, and cancellation tokens
  provide a cooperative stop signal while the dispatcher fences late results.

World coordinates, distances, health values, fuel, revisions, and other
contract values that may exceed a native or JSON number remain decimal
strings. The typed schemas reject non-canonical forms before an adapter runs.

`NativeBrowserToolDispatcherAdapter` is the narrow Agent Controller bridge. It
validates canonical JSON, converts into closed native variants, forwards
`NativeToolResultV1` output/error/timing/context, maps cancellation reasons,
and pumps deadlines from the controller loop. Clear execute-once records only
after the attached session is closed. See the
[native player-host example](docs/native-player-host.md).

## The native replication client

`client.replication()` implements the public
[Replication API](https://docs.crowdedkingdoms.com/replication-api/intro):

- **Connect** = mint/hold an app token → `serverWithLeastClients` on the Game
  API (which installs your UDP session server-side) → wait for session-ready →
  signed UDP traffic to the returned host and client port.
- **Sends**: actor updates, voxel updates, audio, text, client events, generic
  spatial, single-actor messages, channel publishes, and idle heartbeats — all
  HMAC-SHA256 signed per the
  [HMAC guide](https://docs.crowdedkingdoms.com/replication-api/hmac).
- **Receives**: bundle unpacking, per-notification HMAC verification
  (constant-time), typed dispatch, and error frames correlated by sequence
  number.
- **Lifecycle**: automatic app-token refresh before expiry, verified
  `COMMAND_RECONNECT` handling (reassign within the grace period), and a
  silent-drop watchdog (traffic going out with nothing coming back triggers
  reassignment — see
  [Troubleshooting](https://docs.crowdedkingdoms.com/replication-api/troubleshooting)).

Two integration modes:

- **Owned net thread** (default): the SDK runs a receive/send thread and hands
  you notifications through a lock-free SPSC ring; you drain it with `poll()`
  from your game thread.
- **Manual pump**: no SDK threads at all. You call `pump()` from your own
  network thread (or tick) and `poll()` from the game thread. This is the mode
  engine wrappers use.

Performance characteristics: after connect, the steady-state path performs no
heap allocation (pooled datagram buffers), no copies on parse (payloads are
spans into the receive buffer until you copy them), and no exceptions.
Notification callbacks run on the thread that calls `poll()` — never on the
network thread.

## The session layer: data structures that do the bookkeeping

The replication client moves datagrams; `crowdy::session::WorldSession` turns
them into game state. Every store below is a structure that multiplayer games
otherwise hand-write (and debug) themselves — using them means your first
playable build is a render loop over ready-made state instead of weeks of
netcode bookkeeping. One connection feeds all of them; you call
`session.tick()` once per frame and read plain snapshots (single-threaded by
design, so reads never lock).

| Structure | What it replaces | How it makes you faster |
|---|---|---|
| `LocalActorStore` (`session.self()`) | your presence loop | Joins the world, re-sends state at a fixed Hz with send-on-change dedup, periodic keyframes, and cheap idle heartbeats so presence never lapses; tracks `lastAck()` from self-echoes. You just `setState(bytes)` from the game loop — or `moveTo(chunk)` on boundary crossings for an immediate send. |
| `RemoteActorStore` (`session.actors()`) | everyone-else tracking | Self-filtered registry keyed by actor uuid with staleness reaping, `onJoin`/`onLeave`/`onUpdate` callbacks, and a per-actor sample history (state + server timestamp pairs) ready for interpolation/extrapolation. Render directly from `list()`. |
| `RemoteActorLane` (`actors().lane("mobs", ...)`) | per-kind actor lists | Filtered sub-registries (players vs mobs vs vehicles) so each kind is classified once at ingest — no per-frame re-scanning or re-decoding of the full registry. |
| `ChunkStore` (`session.chunks()`) | terrain sync | Chunk/voxel cache: bulk `ensureAround()` hydration from the durable store, realtime merge of incoming voxel edits, optimistic `setVoxel()` (applies locally, replicates, queues persistence), worldgen write-back via `seed()`, `pruneBeyond()`/`flush()` for streaming worlds, and `voxelTypeAt()`/`voxelStateAt()` reads for meshing/collision. |
| `EventRouter` (`session.events()`) | RPC dispatch switch | Routes typed client/server events (`[u16 eventType][state]`) to per-type handlers and retains `lastEvent(type)` — your gameplay events become `events().on(kDoorOpened, ...)` instead of a hand-rolled switch over payload bytes. |
| `Inbox` (`channelInbox()` / `directInbox()`) | chat/message queues | Bounded queues for channel and direct messages with `drain()`, non-consuming `messages()`, `onMessage` callbacks, and channel discovery — plus `send()` helpers back through the connection. |
| `ErrorStore` (`session.errors()`) | "why was that send rejected?" | Correlates server error frames (sequence-numbered, uint8 wrap) with the *kind* of send that used that sequence, so a permission denial points at "your voxel edit", not a bare error code. |
| Host tracking (`amIHost()` / `onHostChanged`) | election polling | Heartbeats host eligibility on a cadence and caches the elected host with a change callback; gate host-only simulation without writing the polling loop. |
| `SaveStateStore` / `AvatarStateStore` | persistence plumbing | Byte-level caches over the durable save/avatar surfaces with explicit `load()`/`save()`; base64 stays at the wire boundary, your code sees bytes. |
| `ContainerMirror` | game-model polling | The notify-to-pull client: `watch()` containers, re-pull on demand or when a bound channel pings, read versioned snapshots via `get()`/`onChange` — server-authoritative state without hammering the API. |
| `PodCodec<T>` / `UnrealPose` | wire layout code | Your replicated state as a packed struct: the struct layout *is* the little-endian wire layout (static-asserted), with the 88-byte Unreal-compatible pose included. No serializer to write, nothing to keep in sync. |
| `IUuidStore` (memory/file) | identity persistence | Persist your actor uuid across restarts so remote registries treat you as the same actor. |

Under the hood these sit on the same primitives the hot path uses — the
lock-free SPSC ring between network and game thread, pooled fixed-size
buffers, and zero-copy parsed views — so the convenience layer does not trade
away the performance story.

## Game Kit: genre building blocks over the game model

The platform's [game model](https://docs.crowdedkingdoms.com/game-api/game-models)
gives you server-authoritative rules without running a server: typed
containers, properties, and transactional functions gated by **invoke
policies** (`owner_of_self`, `condition` expressions, `is_host`,
`is_current_turn`, ...), plus
[automations](https://docs.crowdedkingdoms.com/game-api/autonomous-processes)
that run functions server-side on schedules or events. The **Game Kit** maps
traditional game concepts onto that machinery so you don't design the schema
yourself. Two phases, matching the platform's model:

1. **Studio loads the rules** — blueprint builders emit declarative bundles
   (container types, property schemas, policy-gated functions, automations);
   `deploy()` seeds them in one idempotent pass. Admin context
   (`manage_apps`) only — never the shipped client.
2. **The game client plays** — runtime kits wrap the conventions with typed
   helpers. Authority is enforced server-side on every call: `KitInvokeResult`
   carries the verdict (`success == false` with `errorMessage` on a policy
   denial — never an exception), so an untrusted client can try anything and
   change nothing it shouldn't.

```cpp
// Studio (admin token): one-time "load the rules".
auto adminKit = crowdy::kit::makeKit(admin, appId);
adminKit.deploy({crowdy::kit::inventoryBlueprint(),
                 crowdy::kit::lockBlueprint({.objectTypeName = "Door",
                                             .authority = {crowdy::kit::LockAuthority::key()}})});

// Game client (player token): typed runtime helpers.
auto kit = crowdy::kit::makeKit(game, appId);
auto bag = kit.inventory().ensure(myUserId);
auto result = kit.objects().open(doorId, keyId);
if (!result.success) showLockedMessage(result.errorMessage);
```

### Genre and capability map

Every layer is a blueprint builder plus a runtime kit. Compose the layers
your genre needs — they share the model, so they interoperate (a quest can
pay into a wallet, a plot purchase can grant enforced build permissions):

| Genre / concept | Builder → runtime | Capabilities |
|---|---|---|
| Items & bags (RPG, survival, sandbox) | `inventoryBlueprint` → `kit.inventory()` | Per-player bags and item stacks (`item_id`/`quantity`/`slot`); owner-gated grant/consume/move and atomic two-stack transfer; the consume guard refuses overdraw server-side. |
| Doors, chests, gates | `lockBlueprint` → `kit.objects()` | Lockable world objects with pluggable authority: key item, owner, group/team permission, grid permission, enforced chunk permission, or custom policy trees; several lock types per app via `objectsFor()`. |
| NPCs & world ticks | `npcBlueprint` → `kit.npcs()` | Server-driven behaviors (interval/cron/event triggers) with selector targeting — wander, restock, guards reacting only to intruders via permission predicates; runs with no client online. |
| Land ownership (MMO, sandbox) | `plotBlueprint` → `kit.plots()` | Buy/rent/evict plots where payment and **replication-enforced grid permissions** commit atomically — buying land grants real build rights, not just a database row. |
| Economy (any genre) | `economyBlueprint` → `kit.economy()` | Multi-currency wallets, atomic shop purchases, escrow player trades, a player market with escrowed listings, restock automation; every mutation guard is server-side. |
| Progression (RPG, arcade) | `progressionBlueprint` → `kit.progression()` | XP/levels on a configurable curve, skill trees with prerequisite chains, achievements, host-gated rating. |
| Loot (RPG, roguelike) | `lootBlueprint` → `kit.loot()` | Weighted tables compiled into seed-driven server expressions (clients can't reroll), atomic single-claim drops, event-triggered drops. |
| Quests (RPG, live-ops) | `questsBlueprint` → `kit.quests()` | Event-driven progress via automations, atomic reward turn-in (items + currency in one transaction), cron daily resets. |
| Combat (action, MMO) | `combatBlueprint` → `kit.combat()` | Server-authoritative damage/death/respawn, status-effect ticks over automation selectors, turn-based and host-synced modes. |
| Matches & lobbies (arena, board, card) | `matchesBlueprint` → `kit.matches()` | Session lobbies, rounds, turn order via the platform's session-turn authority, scores, per-match notification channel (notify-to-pull re-pulls on ping). |
| Hidden information (card games) | `decksBlueprint` → `kit.decks()` | Hidden hands via owner-visibility properties, server-dealt shuffles by position — opponents' cards never reach your client. |
| Living world (farming, survival) | `worldsimBlueprint` → `kit.worldsim()` | Day/night clock with spatial notifications, resource nodes with regen + atomic gather, crops, wave counters — all automation-driven. |
| Social (MMO, co-op) | `guildBlueprint` → `kit.social()` | Parties and guilds over teams + channels, guild chat, territory grants, guild hall (a group-permission lock) + guild bank (a shared inventory) composites. |
| Leaderboards (arcade, competitive) | `leaderboardsBlueprint` → `kit.leaderboards()` | Trusted keep-best submits (server/host/automation authority — anti-cheat by construction), ranking reads, cron season resets. |
| Monetization | `featureGate` → `kit.features()` | Feature keys granted per access tier; AND a gate into any builder's policy (`andPolicies(..., featureGate("vip"))`) to tier-gate a capability. |

Trusted mutations (XP grants, loot rolls, currency mints, score submits) take
a `TrustedAuthority` — server, host, automation, owner, or a custom policy —
so reward-granting functions are never plain player calls. The C++ builders
emit **the same model definitions as CrowdyJS's** (verified structurally in
CI-adjacent tooling), so a world deployed from either SDK is playable from
both, and studios can seed from TypeScript tooling while the game ships C++.

## Wrapping CrowdyCPP in engines

CrowdyCPP is the intended foundation for engine-specific SDKs, including the
official [Crowdy Unreal SDK](https://github.com/CrowdedKingdoms/CrowdySDK-Unreal)
(docs: [Unreal SDK guide](https://docs.crowdedkingdoms.com/unreal-sdk/intro)).
The design rules that make it wrappable:

1. **No engine types, ever.** The public API uses `std::span`, `std::string_view`,
   and POD structs. Nothing in CrowdyCPP allocates with `new` on hot paths or
   leaks platform handles, so an engine can marshal at the boundary it chooses.
2. **Pluggable platform services.** Engines inject their own implementations:
   - `IHttpTransport` — Unreal wraps `FHttpModule` so all GraphQL traffic uses
     the engine's HTTP stack, proxies, and certificate handling. (Alternatively
     link the default libcurl transport; Unreal ships libcurl + OpenSSL in its
     ThirdParty tree.)
   - `IWebSocketTransport` / `IWebSocketConnection` — create a dormant socket,
     install its event callback in `start()`, and provide thread-safe,
     non-blocking `send()` / `close()`. The engine may complete on any thread;
     CrowdyCPP fences stale connections and posts user callbacks to `poll()`.
   - `ICrypto` — Unreal binds its bundled OpenSSL for HMAC-SHA256.
   - `ILogger` / `IAllocator` / `IClock` — adapters onto `UE_LOG`, `FMemory`,
     and engine time so SDK activity shows up in engine tooling.
3. **Threading stays with the engine.** Use manual-pump mode: the plugin runs
   `pump()` on an `FRunnable` network thread (or the task graph) and `poll()`
   on the game thread from a ticker. Callbacks therefore fire on the game
   thread, where `UObject`s are safe to touch. Nothing in CrowdyCPP spawns
   threads in this mode.
4. **Binary state stays binary.** Actor-state payloads are opaque bytes on the
   wire. An Unreal wrapper maps its entity replication snapshots directly into
   the payload span — no base64, no JSON, no intermediate copies. The
   open-source [cks-loadtest](https://github.com/CrowdedKingdoms/cks-loadtest)
   tool includes an Unreal-compatible 88-byte actor-state layout that
   interoperates with the current Unreal SDK's pose format.
5. **Session layer maps to entity systems.** `WorldSession`'s remote-actor
   registry (staleness, sample history) is exactly the input an engine wrapper
   needs to drive owner/proxy entity components and interpolation; the chunk
   cache backs voxel/terrain streaming; inboxes back chat and direct messages.

The expected Unreal integration shape: CrowdyCPP builds as a static library in
a `ThirdParty` module of the plugin; the plugin's subsystems (connection,
entities, voice, teams, persistence) become thin adapters over
`crowdy::CrowdyClient`, `crowdy::replication`, and `crowdy::session::WorldSession`,
replacing the plugin's bespoke networking while keeping its Blueprint-facing
API stable. Other engines (custom C++ engines, Godot via GDExtension, Unity
via a C shim) follow the same recipe.

## Two tokens, two clients

CrowdyCPP follows the platform's
[portal / app-scoped token model](https://docs.crowdedkingdoms.com/management-api/portals-and-app-tokens):

1. Passwordless sign-in yields an **identity session token** — valid only for
   the Management API (account, studio admin, minting).
2. Gameplay requires a short-lived **app-scoped token** per app
   (`portal().mintAppToken(appId)`), which is also the 64-octet HMAC key for
   native UDP. With an active native connection, rotate it through
   `refreshGameplayToken()` so the old socket is quiesced before the bearer
   changes and the same handlers reconnect under the fresh token. Use
   `portal().refresh()` directly only when no replication lifecycle needs to
   be preserved.
3. Build one identity client and one client per game. All world/UDP calls run
   on the game client.

## Server compatibility

CrowdyCPP targets the current platform APIs and degrades gracefully on older
deployments:

- **Game-model invoke denials:** current servers report invoke-policy denials
  as `FORBIDDEN` GraphQL errors; newer builds resolve them as
  `success: false` invoke results (with a failure event). The kit's
  `kitInvoke` maps both onto `KitInvokeResult{success:false, errorMessage}`,
  so kit code behaves identically on either generation. A `BAD_REQUEST` whose
  message begins with the stable `Invoke params violate` contract prefix is
  also a typed unsuccessful verdict; unrelated BAD_REQUESTs still throw.
- **`userAppState` round-trip:** older game-api builds stored the base64
  `state` input verbatim and re-encoded on read (reads returned
  base64(base64(bytes))); newer builds round-trip symmetrically. Decode
  defensively if you must read rows written through an old server.
- **Compute Modules (`client.compute()`):** requires a `cks-game-api` build
  that serves the `compute*` root fields (v0.13.13+ dev line). Older servers
  reject compute operations with a GraphQL validation error; every other
  sub-client is unaffected.
- **Realtime + live-ops surfaces (v0.6.0):** `abilities()` (server-validated
  casts), `movement()` (warden violation books, observe/flag), `territory()`
  (control points + factions), `racing()` (server-timed laps, ghosts, the
  possession ball), `liveops()` (event windows, seasons, battle-pass
  composition), `moderation()` + `telemetry()` (model-first), the loot
  engine path (`enginePull` pity rolls), `compute().templates()` /
  `deployTemplate()` (the platform engine registry), and the type-94..98
  wire parsers. Capability-detected as always.
- **Session-genre engine surfaces (v0.5.0):** `kit.instances()` /
  `director()` / `matchmaking()` / `minigames()`, the engine paths on
  `matches()` (`engineReady`/`engineSubmitMove`/`findByProposal`),
  `decks()` (hidden hands via the deck engine), `leaderboards()`
  (server-ranked pages), `economy().orderBook()` (escrowed bid/ask), the
  quests tutorial sequencing, and the type-91/92/93 wire parsers talk to
  the Wave 2 engine templates. Capability-detected; model-only deployments
  keep today's behavior.
- **Engine kit surfaces (v0.4.0):** `kit.mobs()` / `kit.pets()` /
  `combat().attackRouted()` / `worldsim().forecast()` and the
  `crowdy/kit/wire.hpp` pose/lane registry talk to compute-module game
  engines built on the Wave 0/1 `cks-game-api` dev line (`crowdy-game-kit`
  crates). Capability detection (`kit.engines()`) makes them degrade
  gracefully — model-only deployments keep today's behavior.

## Errors

GraphQL-layer failures throw structured exceptions mirroring CrowdyJS:
`CrowdyHttpError`, `CrowdyGraphQLError` (preserves `extensions.code`,
`remediation`), `CrowdyNetworkError`, `CrowdyTimeoutError`,
`CrowdyProtocolError`. Branch on `error.code()` rather than parsing messages.
Subscriptions are non-throwing: `onNext` receives
`GraphQLSubscriptionOutcome`, while `onError` receives a typed terminal
`GraphQLSubscriptionError`. Destroying its move-only handle suppresses queued
callbacks and sends protocol `complete` when connected.

The replication layer never throws on the hot path: sends return
`crowdy::Result` codes, server-reported failures arrive as
`GenericError` notifications correlated by sequence number, and connection
state changes surface through a status callback. Note the protocol's
documented semantics: UDP is best-effort, sequence numbers are correlation
(not idempotency), and **auth failures are often silent drops** — see
[Operations](https://docs.crowdedkingdoms.com/replication-api/operations).

## Schema refresh and codegen

The GraphQL surface is generated from committed artifacts so external builds
never need network access or sibling repos:

```bash
# Maintainer-only Node dependencies (not part of a CMake consumer build).
npm ci

# Maintainers: refresh exact endpoint snapshots + the merged comparison schema
node scripts/schema-sync.mjs            # writes schema.*.gql + schema.gql
node scripts/codegen.mjs                # regenerates include/crowdy/generated/
# commit all schema snapshots and include/crowdy/generated/ together
```

`scripts/schema-sync.mjs` downloads and commits the exact published SDLs
(`https://docs.crowdedkingdoms.com/schema/management-api.graphql` and
`.../game-api.graphql`) before merging them; `--management <path|url>` /
`--game <path|url>` override the sources. Operation documents live in
`operations/<domain>/*.graphql` and follow the same shapes as CrowdyJS.
The merge uses the same GraphQL merge/printer pipeline as CrowdyJS; `--check`
compares all three committed snapshots with explicitly supplied sources without
writing. Codegen isolates each named operation with only its transitive
fragments and validates it against the exact Management and Game SDLs; an
operation invalid on both planes fails generation. It embeds both endpoint
schema digests plus the merged schema and operation-input digests in generated
headers, and `node scripts/codegen.mjs --check` verifies them without modifying
files.

### Parity maintenance gates

CrowdyCPP tracks one reviewed CrowdyJS commit in
`.github/workflows/ci.yml`. After either SDK changes its public surface:

```bash
# Optional for nonstandard layouts. Otherwise tools resolve ../CrowdyJS,
# ./CrowdyJS (CI), then the sibling of this worktree's primary git checkout.
export CROWDYJS_PATH=/path/to/CrowdyJS

# Compare both schemas in both directions, audit roots/methods, and refresh docs.
node tools/parity/parity.mjs --write docs/parity-matrix.md
npm run check:operations
npm run check:parity

# CrowdyJS must be built first; verifies all 28 descriptor digests and the
# closed 16-reason preemption vocabulary against C++ schema/codegen fixtures.
npm run check:agent-fixtures

# Parser/gate behavior.
npm test
```

An explicitly configured `CROWDYJS_PATH` is authoritative and fails clearly
when invalid. Automatic resolution likewise fails if no deterministic
sibling/CI/worktree checkout exists; it never skips or weakens parity.

The reviewed baseline accepts only named classifications. A **portable gap** is
shown as missing work and is not presented as parity; native equivalents and
inherently browser-only surfaces are the only waivers. New differences and
stale classifications fail. `--strict` additionally fails on every remaining
portable gap and is intended for the final strict-parity release gate.

Blueprint builders are a compiled structural gate:

```bash
cmake -S . -B build-parity -DCROWDY_BUILD_PARITY_TOOLS=ON
cmake --build build-parity --target crowdy_blueprint_dump
node tools/parity/dump-blueprints.mjs /path/to/built/CrowdyJS > /tmp/js.json
./build-parity/crowdy_blueprint_dump > /tmp/cpp.json
node tools/parity/blueprints-diff.mjs /tmp/js.json /tmp/cpp.json
```

When intentionally changing the target, update the pinned CrowdyJS SHA, sync
the descriptor/preemption fixtures with `agent-fixtures.mjs --write`,
regenerate `docs/parity-matrix.md`, and commit the schema plus both generated
headers in the same change. None of these maintainer gates run during a normal
external CMake build.

## Tests

- `ctest` — offline unit tests (wire codec golden vectors, HMAC vectors,
  GraphQL-WebSocket handshake/reconnect/frame/cancellation behavior, bundle
  parsing, malformed-input fuzz, codec round-trips).
- A build configured with `CROWDY_NO_EXCEPTIONS=ON` runs the full
  non-throwing ctest matrix. Only assertions whose subject is a
  blocking exception translation are replaced by equivalent fail-closed or
  async `GraphQLOutcome` checks; their test targets are not dropped.
- `npm test` — offline Node tests for schema/parity parser behavior.
- `tests/e2e/` — end-to-end suites (two-client fan-out, gamer journey, token
  refresh/reconnect, opt-in marketplace chunk claim/release) that run against
  a deployment you configure via
  environment variables (`CROWDY_E2E_MANAGEMENT_URL`, `CROWDY_E2E_HTTP_URL`,
  `CROWDY_E2E_EMAIL`, `CROWDY_E2E_APP_ID`, …). Skipped when unset.
- `benchmarks/` — codec ns/op, HMAC throughput, and end-to-end echo latency
  against an env-configured deployment.

## Docs

- [Replication API (native UDP)](https://docs.crowdedkingdoms.com/replication-api/intro)
- [Wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats) · [HMAC](https://docs.crowdedkingdoms.com/replication-api/hmac)
- [Management API](https://docs.crowdedkingdoms.com/management-api/intro) · [Game API](https://docs.crowdedkingdoms.com/game-api/intro)
- [Native Agentic Studio](docs/native-agent-api.md)
- [Native player host](docs/native-player-host.md) · [GraphQL WebSockets](docs/graphql-websocket.md)
- [CrowdyJS / CrowdyCPP / Game API compatibility](docs/compatibility.md)
- [Game Models](https://docs.crowdedkingdoms.com/game-api/game-models) · [Grids & permissions](https://docs.crowdedkingdoms.com/game-api/grids-and-permissions)
- [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) — the TypeScript SDK this API surface mirrors
- Agent index: [llms.txt](https://docs.crowdedkingdoms.com/llms.txt)

## License

MIT
