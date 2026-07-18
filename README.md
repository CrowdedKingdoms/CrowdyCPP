# CrowdyCPP

The official portable C++ SDK for **Crowded Kingdoms**. CrowdyCPP gives native
games typed clients for auth, the world/replication GraphQL APIs, and — unlike
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

## Layout

```text
include/crowdy/          public headers
  core/                  bytes, endian, result, clock, logging, allocator interfaces
  wire/                  zero-copy wire codec + HMAC framing (header-only)
  graphql/               GraphQL-over-HTTP core (IHttpTransport, JSON, errors)
  replication/           native UDP replication client
  session/               world session layer (actors, chunks, inboxes, host)
  kit/                   Game Kit (blueprints + runtime helpers)
src/                     implementation
include/crowdy/generated/  committed codegen output (operations + enums)
operations/              GraphQL operation documents (codegen input)
schema.gql               committed schema snapshot (merged Management + Game API SDL)
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
| OpenSSL (libcrypto) | HMAC-SHA256 | `crowdy::core::ICrypto` |
| yyjson (vendored) | JSON parse/serialize | internal only, not on the UDP path |

The wire and replication layers depend only on BSD/Winsock sockets and the
`ICrypto` interface — no libcurl, no JSON.

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
| `client.gameApps()` | App grids + grid runtime-permission administration. |
| `client.replication()` | **Native UDP** replication: connect/assign, spatial sends, notifications, channel publish, single-actor messages, heartbeats. |
| `crowdy::session::WorldSession` | SDK-managed game state: your actor with a fixed-Hz send loop, remote-actor registry with staleness + interpolation history, chunk/voxel cache, inboxes, host tracking. |
| `crowdy::kit::makeKit(client, appId)` | Game Kit: ready-made mappings of game concepts onto the game model (inventory, lockable objects, NPCs, plots, economy, progression, loot, quests, combat, matches, decks, worldsim, social, leaderboards, feature gates), plus blueprint builders and `deploy()` for the admin "load the rules" step. |

Studio-admin surface (privileged; drive with an org/admin token from a trusted
context): `client.admin().organizations() / apps() / appAccess() / billing() /
payments() / quotas() / environments() / usage() / sharedEnvironment()`.
Operator surface (platform operations, requires operator rights):
`client.operator_()`. The SDK never relaxes server-side authorization — these
are typed wrappers; the caller still needs the right token and permission.

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
   native UDP. Refresh it with `portal().refresh()` before expiry; the
   replication client does this automatically while connected.
3. Build one identity client and one client per game. All world/UDP calls run
   on the game client.

## Server compatibility

CrowdyCPP targets the current platform APIs and degrades gracefully on older
deployments:

- **Game-model invoke denials:** current servers report invoke-policy denials
  as `FORBIDDEN` GraphQL errors; newer builds resolve them as
  `success: false` invoke results (with a failure event). The kit's
  `kitInvoke` maps both onto `KitInvokeResult{success:false, errorMessage}`,
  so kit code behaves identically on either generation.
- **`userAppState` round-trip:** older game-api builds stored the base64
  `state` input verbatim and re-encoded on read (reads returned
  base64(base64(bytes))); newer builds round-trip symmetrically. Decode
  defensively if you must read rows written through an old server.

## Errors

GraphQL-layer failures throw structured exceptions mirroring CrowdyJS:
`CrowdyHttpError`, `CrowdyGraphQLError` (preserves `extensions.code`,
`remediation`), `CrowdyNetworkError`, `CrowdyTimeoutError`,
`CrowdyProtocolError`. Branch on `error.code()` rather than parsing messages.

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
# Maintainers: refresh the schema snapshot from the published production SDLs
node scripts/schema-sync.mjs            # writes schema.gql
node scripts/codegen.mjs                # regenerates include/crowdy/generated/
# commit schema.gql and include/crowdy/generated/ together
```

`scripts/schema-sync.mjs` downloads the published SDLs
(`https://docs.crowdedkingdoms.com/schema/management-api.graphql` and
`.../game-api.graphql`) and merges them; `--management <path|url>` /
`--game <path|url>` override the sources. Operation documents live in
`operations/<domain>/*.graphql` and follow the same shapes as CrowdyJS.

## Tests

- `ctest` — unit tests (wire codec golden vectors, HMAC vectors, bundle
  parsing, malformed-input fuzz, codec round-trips). Offline.
- `tests/e2e/` — end-to-end suites (two-client fan-out, gamer journey, token
  refresh/reconnect) that run against a deployment you configure via
  environment variables (`CROWDY_E2E_MANAGEMENT_URL`, `CROWDY_E2E_HTTP_URL`,
  `CROWDY_E2E_EMAIL`, `CROWDY_E2E_APP_ID`, …). Skipped when unset.
- `benchmarks/` — codec ns/op, HMAC throughput, and end-to-end echo latency
  against an env-configured deployment.

## Docs

- [Replication API (native UDP)](https://docs.crowdedkingdoms.com/replication-api/intro)
- [Wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats) · [HMAC](https://docs.crowdedkingdoms.com/replication-api/hmac)
- [Management API](https://docs.crowdedkingdoms.com/management-api/intro) · [Game API](https://docs.crowdedkingdoms.com/game-api/intro)
- [Game Models](https://docs.crowdedkingdoms.com/game-api/game-models) · [Grids & permissions](https://docs.crowdedkingdoms.com/game-api/grids-and-permissions)
- [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) — the TypeScript SDK this API surface mirrors
- Agent index: [llms.txt](https://docs.crowdedkingdoms.com/llms.txt)

## License

MIT
