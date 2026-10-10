# CrowdyCPP migration notes

## 0.61.0 Builds report their SDK version

Mirrors CrowdyJS 18.8.0. Additive, with one floor: the build documents select
`ExecBuild.sdkVersion`, so `exec().build`, `buildStatus`, `waitForBuild`, `modBuild`,
`modClientBuild` and `modBuildStatus` need ck-api v2.40.2 or later (an older API refuses the
selection).

- **`sdkVersion`** in every build's JSON: the version of `ckx-sdk` the platform compiled the build
  against (`crowdy-client-sdk` for a CLIENT half), from the toolchain of the API instance that ran
  it. The platform points a crate at its own SDK copy whatever version the crate names, so this
  says what your code was checked against. Null until the build starts. The build log's first
  line names the same toolchain.

## 0.60.0 Voice helpers, opcode 140 and voxel edits in WorldSession, wide voxels

Mirrors CrowdyJS 18.7.0. Channel audio and the voxel echo need Buddy v0.37.0, and the token
mutations select `runtimeGate`, so this needs the ck-api release after v2.39.0 (an older one
refuses the selection). Signatures change (`ChunkStore::voxelTypeAt`, `ChunkStore::setVoxel`'s
refusal, `ChunkStore::ingest` returns `bool`), so rebuild; CrowdyPy re-vendors and can bind the new
`WorldSessionConfig` callbacks and `ChunkData::overlay`.

- **Voice helpers** (`crowdy/media/voice_frames.hpp`, header-only): an optional convention for
  what an audio payload carries. A 10-byte header, little-endian, goes in front of each codec
  frame: version 1, codec (`VoiceCodec`: 0 raw, 1 Opus 48 kHz mono, 2 G.711 µ-law 8 kHz), `u16`
  seq, `u32` timestamp in codec samples (milliseconds for raw), the frame's duration in ms, and
  `VoiceFlag::kSpurtStart` / `kSpurtEnd`. `encodeVoiceHeader` / `encodeVoicePacket` write it
  (an oversized frame gives an empty vector); `decodeVoicePacket` returns nullopt for a packet
  shorter than 10 bytes or of another version. `VoicePacketizer` numbers one sender's frames
  across the seq and timestamp wraps and sets the flags (`packetize(frame, last)`, `skip()` for
  silence). `VoiceJitterBuffer` takes `push(key, packet, nowMs)` per sender key and returns the
  due `VoicePlayout`s from `pull(key, nowMs)` / `poll(nowMs)`: in seq order, `targetDelayMs` (60)
  after a talk spurt's first packet, `gap` for each frame that never came, late packets dropped,
  at most `maxFrames` (64) a sender, starting over on a talk spurt or `resetAfterMs` (200) of
  silence. Out-of-range options are clamped (CrowdyJS throws `RangeError` for them). Both SDKs
  replay CrowdyJS's `test/unit/fixtures/voice-frames.json` (copied to
  `tools/parity/fixtures/voice-frames.json`). Positioning a voice stays the game's.
- **Opus, optional.** `-DCROWDY_WITH_OPUS=ON` (default OFF) builds `crowdy/media/opus.hpp` as the
  target `crowdy_opus` (linked by `CrowdyCPP::crowdy` in such a build): `OpusVoiceEncoder`
  (48 kHz mono 16-bit PCM; 10/20/40/60 ms frames; bitrate; packet loss for in-band FEC) and
  `OpusVoiceDecoder` (`decode`, `conceal` for a gap). libopus is found through its CMake
  package, pkg-config or a plain search, at build time and again by the installed config.
  Without the option nothing changes, and the header declares only `kOpusAvailable = false`.
- **`WorldSessionConfig::onVoxel`** is called for every inbound voxel update after `chunks()` has
  merged it, with the notification (sender, chunk) and the `wire::VoxelPayloadView` (position,
  type, state): the patch the Minecraft mod carried, upstreamed. A wrapper that applied that
  patch drops it.
- **`WorldSessionConfig::onGenericSpatial`** forwards GENERIC_SPATIAL_1 (140). `Connection` always
  had `Handlers::genericSpatial`, but `WorldSession` owns the handlers and never set it, so a
  session user saw none. CrowdyJS receives 140 on its binary relay only.
- **`ChunkStore` keeps wide voxel types and other addresses.** Positions and types are the app's
  signed 16-bit values, which the platform does not check. An edit the one-byte 16³ grid cannot
  hold (a type outside 0-255, a position outside 0-15) — a realtime update, a hydrated
  `voxelStates` entry, or a `setVoxel` — is kept whole in `ChunkData::overlay` (`OverlayVoxel`:
  `x`, `y`, `z` and its `VoxelState`; keyed by `voxelKey(x, y, z)`), and the grid holds 0 under
  an in-grid one. What changes:
  - `voxelTypeAt` returns `std::int16_t` (was `std::uint8_t`) and reads the overlay first;
    `voxelStateAt` returns the overlay's state too.
  - A realtime update outside 0-15 is no longer written into `ChunkData::voxels` at its aliased
    index — `(16, 0, 0)` overwrote `(0, 1, 0)`, and one far enough out wrote past the end of the
    array — and a type outside 0-255 is no longer stored truncated (300 read as 44).
  - A stored `voxelStates` entry outside 0-15 is kept in the overlay instead of ignored.
  - `setVoxel` keeps a position outside 0-15 in the overlay and sends it, where it used to return
    `InvalidArgument`; only a position outside 16 bits is refused now.

  The store is a 16×16×16 helper; a game with other addressing reads `onVoxel` and
  `StoredChunk::voxelStates`.
- **Channel audio** (Buddy v0.37.0). `Connection::sendChannelAudio(channelId, uuid, payload)` sends
  opcode 35 (opcode 17's layout and signing; `wire::encodeChannelAudio`, both through
  `wire::encodeChannelRequest`), at most 1,024 payload bytes; without the channel's `send_voice`
  and the app's `use_voice_chat` the server answers `UNAUTHORIZED` (7). No echo to the sender.
  Opcode 36, standalone or bundled, parses like 18 (`parseChannelNotification` takes either) into
  `Handlers::channelAudio`, and `WorldSessionConfig::onChannelAudio` forwards it (the channel inbox
  does not get it). `grids().createChannel` takes `membersCanSpeak`; `channels().create`'s input
  passes it through. See the README's voice section.
- **`wire::ErrorCode::AppPaused` (33)**: the replication server refuses a paused app's sends.
- **Voxel state at most 1,024 bytes** (`wire::voxel::kMaxStateSize`): `Connection::sendVoxelUpdate`
  and `ChunkStore::setVoxel` return `InvalidArgument` above it (positions and types were already
  `std::int16_t`); the server answers `INVALID_REQUEST` (15).
- **The echo of your own voxel edit.** Buddy v0.37.0 delivers every accepted edit back to its
  sender. `ChunkStore::setVoxel` records each send (uuid, sequence, voxel) for
  `kPendingEditTtlMs` (10 s, clock in `Options::now`) and `ingest()` skips its echo, so a local
  edit fires `onChunkChanged` once; the echo is applied only when another client's edit of the
  voxel arrived in between and no newer local edit is pending. `ingest()` returns whether it
  changed the cache. `onVoxel` sees the echoes too: compare uuid and sequence with your send's.
- **Pause and access refusals.** `domains::AppRuntimeGate` / `isAppPaused(gate)` and
  `AppTokenResponse::runtimeGate` (mint, exchange and refresh select it; a paused app still
  mints). `GraphQLErrorDetail` reads `ownedByCaller`, `suspendedUntil` and `reason`;
  `graphql::actorExistsOf`, `accessRefusalOf`, `appPausedOf` and the `k*Code` constants.
- **New API wraps** (each with its `Async` twin): `users().playerProfile(userId)` and
  `playerProfiles(userIds)` (at most `kPlayerProfilesMax`, 100; more is `INVALID_ARGUMENT`);
  `admin().appAccess().suspend(appId, userId, until, idempotencyKey)`, `unsuspend`,
  `resyncTierGridPermissions` (`manage_access_tiers`), `suspendedUntil` on every access record;
  `exec().restartType(appId, nodeType)` (`manage_compute`) and the new `status` fields
  (`budgetPauseReason`, `maxInstances`, `maxReservedMb`, `instanceLimit`, `instances`,
  `reservedMb`); `claimOwnerKeys` on `apps().get` / `update`; `voxelStatesTruncated` on chunk
  reads; `runtimeGate` and `wildernessWritesOpen` on `gameClientBootstrap`.

Parity: `tools/parity/parity.mjs` classifies CrowdyJS 18.7.0's `media/voice-frames.ts` as a native
equivalent (every export mapped to `voice_frames.hpp`) and pins the behaviour changes above
(`chunk-store-voxel-overlay`, `world-session-voxel-hook`, `world-session-generic-spatial`,
`generic-spatial-relay-only`, `world-session-channel-audio`, `chunk-store-self-echo`,
`voxel-edit-limits`, `udp-error-app-paused`, `grid-host-call-voxel-bounds`) and classifies
`Mutation.sendChannelAudio` as native (`Connection::sendChannelAudio`). `parity:repin` copies the voice
fixture with the exec gateway cases. They need the pin moved to the CrowdyJS 18.7.0 commit.

## 0.59.0 The input log

Pinned to CrowdyJS 18.6.0 (`ca9fbb5`); needs ck-api with the input log. Additive: nothing existing
changes shape, and CrowdyPy needs nothing beyond re-vendoring. The schema snapshot also carries
ck-api's dev/test staff-only gate; as in CrowdyJS, nothing wraps it (its operator fields are
platform-only).

- `client.inputLog().sessions(appId, first = 50, after = {}, filter = {})` lists an app's recorded
  sessions, newest first, as an `InputLogSessionConnection` (`edges[].node`, `pageInfo`,
  `totalCount`). A session is one game token's inputs; `filter` takes `userId`, `from`, `to` and
  `messageType`. Another user's sessions need `manage_apps` (`FORBIDDEN` otherwise).
- `client.inputLog().messages(appId, gameTokenId, first = 50, after = {}, filter = {})` reads one
  session's inputs, oldest first. `body` is the client message in base64, without its
  authentication tail; `sizeBytes` is what stored input logs are billed on; spatial inputs carry
  their chunk and actor, channel inputs their channel. **Keep paging while `pageInfo.hasNextPage`
  is true**: a page can be short, or empty, when it reached the server's time or scan limit.
  Another user's session answers `NOT_FOUND` without `manage_apps`.
- Both are game plane (the app-scoped client), and both answer `INPUT_LOG_UNAVAILABLE` on a
  deployment without input logging; `messages` also answers it, retryable with the same cursor,
  when the log cannot be read right now. Inputs are kept for the published retention.
- Every app read selects `replayLoggingEnabled`. Turning it on with `admin().apps().update` is
  refused with `INPUT_LOG_FUNDS_NEEDED` unless the org's wallet has a spendable balance or the org
  is exempt from billing.

`schema.gql` is the game API branch's SDL with the input log; resync from docs.dev's SDL once
cks-docs publishes it.

## 0.58.0 Channel messages limited by distance

Pinned to CrowdyJS 18.5.0 (`ae249cb`); needs Buddy v0.35.0 on the server side. Additive: nothing
existing changes shape, and CrowdyPy needs nothing beyond re-vendoring.

- `Connection::sendRangedChannelMessage(channelId, uuid, payload, origin, maxDistance)` publishes to
  a channel like `sendChannelMessage`, but only members near `origin` receive it: a member gets it
  when one of its live actors is in this connection's app within `maxDistance` chunks of `origin`,
  measured as the straight-line distance between chunk coordinates, boundary included. A member
  with no live actor does not receive it. `maxDistance` runs from 0 (the origin chunk only) to
  `wire::channel_ranged::kMaxDistance` (2147483647); above it the call returns `InvalidArgument`
  before anything is sent. It is not the 0-8 Chebyshev ring count spatial sends take.
- Members receive the ordinary `ChannelNotification` through `Handlers::channelMessage`, so a
  receiver needs no change. A refusal (no send right: `UNAUTHORIZED`) arrives as a `GenericError`
  for the returned sequence; an older Buddy drops opcode 32 silently.
- `wire::encodeRangedChannelMessage` / `RangedChannelMessageParams` / `wire::channel_ranged` are the
  codec (`MessageType::ChannelMessageRangedRequest = 32`).

`schema.gql` is the game API branch's SDL with `sendRangedChannelMessage` (it also carries `dev`'s
rate-card bands); resync from docs.dev's SDL once cks-docs publishes it.

## 0.57.0 The terms and age gate

Pinned to CrowdyJS 18.4.0 (`4e7e595`); needs ck-api `v2.35.0` (cks-game-api #437) for the two new
calls. Additive: nothing existing changes shape, and CrowdyPy needs nothing beyond re-vendoring.

- Since `v2.35.0` no gameplay token is issued — `portal().mintAppToken`, the portal authorization
  code, `portal().refresh` — until the player has agreed to the current required legal documents (Game
  Terms, API Terms, SDK Developer Terms, Free Tier and Billing Basis, Overworld Privacy Policy) and
  attested that they are at least 18, or the age of majority where they live if that is higher.
  The refusal's GraphQL error code is `LEGAL_ACCEPTANCE_REQUIRED`.
- A native game shows its own two checkboxes, linking each document, then calls
  `auth().recordPlayerConsents(true, true)` (or `recordPlayerConsentsAsync`) with the session
  token. `auth().playerLegalAcceptance()` says whether that is still needed. Call it only for a
  player who ticked both boxes: it records their agreement.
- A refresh refused this way cannot succeed on retry: the player never stored the terms, or a
  document has a new version. Ask again, record them, and mint again.
- `auth().registerUser(email, password, gamertag, acceptLegal, attestAgeOfMajority)` sends both
  fields. A request with a browser origin must send both `true`; a native one may use the
  three-argument overload and record them later.

`schema.gql` is docs.dev's SDL after cks-docs `dev/v0.2.90` (the gate's fields only).

## 0.56.0 Chunk loads apply recorded voxel edits

Still pinned to CrowdyJS 18.1.0 (`806b141`); needs ck-api `dev/v2.33.0` (cks-game-api #445) for
the edits to arrive. Additive for callers; breaking only for an `IChunkSource` that should report
states and does not. (OI-2026-10-02-006)

- Every voxel write but a chunk write-back (`updateChunk`) lands only in the chunk's edit log: a
  hub's or mod's `world.set_voxels`, `updateVoxel`, and realtime voxel updates. Since #445,
  `getChunk` and `getChunksByDistance` return each recorded edit as a `voxelStates` entry
  (`getChunksByDistance` only when `voxelStates` is selected) with its type, over a stored
  `voxels` that holds none of them. `ChunkStore::ensureAround` read `voxels` alone, so a block a
  hub placed was gone after a reload.
- `GetChunksByDistance` selects `voxelStates` (as `GetChunk` does), so `chunks().byDistance` and
  `ensureAround` get them in the same round trip. `ensureAround` puts each entry over the grid:
  its `voxelType` at the voxel's index, its state into `voxelStates` (an entry without a state
  clears the cached one), an entry outside 0-15 ignored. A chunk stored with `voxels: null`
  starts from zeros, as before.
- `IChunkSource::chunksAround` reports them in the new `StoredChunk::voxelStates`
  (`StoredVoxelState{x, y, z, voxelType, state}`). A source that leaves them empty keeps the old
  behaviour, cached states included, and loses every edit the grid lacks. A binding that
  supplies its own source should select `voxelStates` and fill them: CrowdyPy's, at its next
  re-vendor.
- A chunk the server has never stored is returned by neither read, even when its edit log has
  entries (a mod writing into an empty chunk).

## 0.55.0 Parity with CrowdyJS 18.1.0 (open grids; where a connect token may go)

Pinned to CrowdyJS 18.1.0 (`806b141`). `schema.gql` is cks-game-api `dev`'s after #436
(`dev/v2.31.0`; `scripts/schema-sync.mjs --game <that schema.gql>`, then
`node scripts/codegen.mjs`). Breaking only for a connection that dialed a gateway the new check
refuses, and for code that reads the `connect` callback's `Errc` (below).

- `gameApps().setOpenPermissions({appId, gridId, permissionKeys})` and
  `openPermissions(appId, gridId)` (with `…Async` twins; also `admin().grids()`) wrap
  `setGridOpenPermissions` / `gridOpenPermissions` (`manage_apps`). The first replaces the keys a
  grid grants every player with active access to the app, within its limits, and players who
  gain access later get them too; an empty `permissionKeys` closes it. Since #436 the most
  specific grid covering a chunk decides who may build there, so a zone nested in the world grid
  that everyone may build in must grant `update_voxel_data` itself. `BAD_REQUEST` refuses the
  app's world grid, the four player-code keys, an inactive key and a 33rd open grid in one app.
  (OI-2026-09-30-007)
- `exec().connect`, `connectAsync`, `connectAsDeveloper` and `connectAsDeveloperAsync` send the
  connect token only to a gateway that `execGatewayRefusal(gameApiUrl, gatewayUrl)` passes:
  `ws:` or `wss:`, `wss:` whenever the Game API is `https:`, no credentials in the URL, on the
  estate of the Game API (the client's `endpoint()` at each dial) or of `kDefaultHttpOrigin`,
  as `graphql::isSameEstate` bounds a move, and two IP literals only when equal. A loopback Game
  API may name a loopback gateway (ck-exec's local cluster). Any other gateway is never dialed:
  calls waiting fail `Unavailable` ("refusing the gateway …"), the `connect` callback gets
  `Errc::NotConnected`, and a reconnect asks the Game API again. `ExecConnection::open` still
  dials what it is given. The cases are `tools/parity/fixtures/exec-gateway-cases.json`, a copy
  of CrowdyJS's, and `tests/parity/exec-gateway-fixture.test.mjs` holds the copy to the pinned
  commit's. (OI-2026-09-30-010)
- A gateway that refuses the connect token is `Denied` again. Since ck-exec 0.10.0 a gateway
  answers the upgrade `HTTP 401` with the reason as its body, before any WebSocket exists, which
  this SDK reported as `Unavailable`. `graphql::WebSocketError` gained `httpStatus` and
  `httpBody`: an injected transport sets both; the curl transport sets the status only, since
  libcurl ends a refused upgrade at its headers. A `401` is `Denied` with the message `the
  gateway refused the connection (HTTP 401: <reason>)` (no `: <reason>` without a body), and
  the first connection's `connect` callback gets `Errc::Rejected` where it got
  `Errc::NotConnected`. A `429` (a player past 16 sessions to one app through a gateway, or a
  gateway past its total) is `Unavailable` with its reason. A gateway before 0.10.0 closed with
  4401, which stays `Denied`; the calls queued on a first connection that never opened now fail
  with the attempt's status (they were always `Unavailable`). (OI-2026-09-29-002)
- `ExecConnection::lastFailure()`: why the last attempt to connect failed, or the open
  connection was lost (`ExecReply`: status and reason); nullopt again once a connection opens.
- Also in this release, merged on `dev` after 0.54.0 (#134): `ChunkStore::pruneBeyond` no longer
  names a local `far`, which `<windows.h>` defines as nothing, so a Windows consumer that
  includes `<windows.h>` before `chunk_store.hpp` compiles again; `windows_macros_test` compiles
  the session and replication headers after `far` and `near` are defined.

## 0.54.0 Seams for wrapping the native core (no wire or parity change)

Additive. Pinned to CrowdyJS 18.0.4, as 0.53.0. These are the hooks a language binding needs
to run the replication core and the session stores without a `CrowdyClient`: CrowdyPy (the
Python SDK) binds this release.

- `Config::onEventsReady`: called when notifications are waiting for `poll()`, at most once
  between two `poll()` calls, from the thread that queued the first of them (the net thread, or
  the `pump()` caller), and again from `poll()` when a `maxEvents` bound left events queued. An
  event loop writes to a wake descriptor there and sleeps until there is something to dispatch,
  instead of polling on a timer. It must not block, throw, or call back into the connection.
- `IChunkSource`: where a `ChunkStore` hydrates from (`chunksAround`) and writes back to
  (`writeChunk`, whose `GraphQLOutcome` is classified exactly as a `ChunksAPI` write is). New
  constructor `ChunkStore(Connection&, IChunkSource*, appId, Options)`; the `ChunksAPI*`
  constructor is unchanged and now builds an adapter over the same interface. A literal
  `nullptr` still means "no durable store".
- `IHostElection` and `WorldSessionServices{chunks, host}`: a `WorldSession` built over
  injected services instead of a `CrowdyClient` (new constructor). The `CrowdyClient*`
  constructor is unchanged and builds adapters over both interfaces; the host heartbeat
  behaves as before (best-effort; a failed beat keeps the cached host).
- `WorldSessionConfig::onText`: proximity text, which the session has no store for, is
  forwarded to the game like `onAudio` / `onVideo`. It used to be dropped once a session owned
  the connection's handlers.

## 0.53.0 Parity with CrowdyJS 18.0.4 (chunk write-backs the server refuses)

Breaking for callers of `ChunkStore::flush()`, whose return type changed. Pinned to CrowdyJS
18.0.4. `schema.gql` is cks-game-api `dev`'s after #434 (`dev/v2.30.0`): it adds
`App.wildernessWritesOpen` and `UpdateAppInput.wildernessWritesOpen`.

- `ChunkStore` no longer retries a write-back forever. 0.52.0 caught every failure and left the
  chunk dirty, and because `tick()` always tried the first dirty chunk, one chunk the server
  refused (a claimed plot, a safe zone, a closed wilderness) or one that kept failing stopped
  every other chunk from being written back. Now:
  - a refusal the server will not change is sent once and dropped: `extensions.code`
    FORBIDDEN, SCOPE_MISSING, NOT_ALLOWED, BAD_REQUEST, BAD_USER_INPUT, INVALID_REQUEST,
    GRAPHQL_VALIDATION_FAILED or NOT_FOUND, `extensions.retryable: false`, or HTTP / an
    `extensions.httpStatus` of 400, 403, 404, 413 or 422;
  - any other failure (PLATFORM_BUSY, UNAUTHENTICATED, network, a timeout, a 5xx) is tried
    again after 0.7, 1.4, 2.8 and 5.6 s (`Options::writeBackAttempts` 5,
    `writeBackBackoffMs` 700) and then dropped;
  - a chunk waiting out its backoff does not hold up the others: `tick()` persists the first
    dirty chunk that is due.

  A dropped chunk keeps its local voxels and is no longer dirty; the store does not undo the
  edit. `onWriteBackFailed(cb)` reports each drop as a `ChunkWriteBackFailure{coord, reason
  (ChunkWriteBackDrop::Refused | Exhausted), attempts, error}`, where `error` is the last
  attempt's `graphql::GraphQLOutcome` (its `kind`, `httpStatus` and GraphQL errors). Undo or flag
  the edit there.
- `ChunkStore::flush()` returns a `ChunkFlushResult{persisted, dropped}` instead of a count:
  `persisted` is the old count and `dropped` the write-backs it gave up on. It waits out the
  backoff of a chunk whose failure can clear (`Options::sleep` replaces the wait, for tests).
  Replace `flush() >= n` with `flush().persisted >= n`.
- `pruneBeyond` gives a dirty chunk one attempt, as before, and now evicts it when that attempt
  is refused (reporting it) instead of keeping it dirty forever; one whose failure can clear
  still stays for the next tick.
- `GraphQLClient::requestOutcome(document, variables, operationName)` is the blocking twin of
  `requestAsync`: it returns the `GraphQLOutcome` instead of throwing, in both builds, so a
  no-exceptions build can tell a refusal from a network failure too.
  `ChunksAPI::updateOutcome(input)` is `update` through it.
- `GraphQLErrorDetail::httpStatus` carries `extensions.httpStatus` when the server sends it.
- `App`, `AppBySlug`, `AppsForOrg`, `MyApps`, `CreateApp` and `UpdateApp` select
  `wildernessWritesOpen`: whether players may write the app's wilderness (chunks only the app's
  world grid covers). An org-admin closes it with `admin().apps().update(appId,
  {wildernessWritesOpen: false})` (manage_apps); every replica refuses those writes within
  15 seconds.

## 0.52.0 Parity with CrowdyJS 18.0.3 (the P3 W5 client security review)

Not breaking. Pinned to CrowdyJS 18.0.3. `schema.gql` is cks-game-api `dev`'s after #431
(`scripts/schema-sync.mjs --game <that schema.gql>`, then `node scripts/codegen.mjs`): it adds
the two revoke mutations.

- `exec().revokeClientModConsent(appId, modId)` takes back the player's consent to one CLIENT
  half, whatever hash they consented to (`true` when they had); while they trust its author on
  its grid it is still served to them. `exec().revokeAuthorTrust(appId, gridId, authorId)` stops
  trusting an author on a grid and takes back the consent to each of their CLIENT halves there.
  Both have `…Async` twins and need ck-api `dev/v2.28.0` (OI-2026-09-28-001). A native engine
  that runs CLIENT halves offers the player both beside each running half, and stops the half
  itself: CrowdyJS's `ExecClientHalves.revoke` / `forgetAuthor` are the browser's runner.
- `ExecConnection` percent-encodes the connect token in the gateway URL
  (`/v1/connect?token=…`), as CrowdyJS's `encodeURIComponent` does. Tokens today are base64url
  JWS, which needed no escaping; a token with `+`, `/`, `=` or `&` would have been mangled.
- `modClientArtifactBytes` already refused a capability summary whose `hostFunctions` are not
  all strings; CrowdyJS refuses it now too.
- CrowdyJS 18.0.2 holds a CLIENT half to rules of its own on the page: `grid_permission_check`
  answers only for `write_server_code`, `run_server_code`, `write_client_code` and
  `run_client_code` and refuses any other key; a half's `emit_spatial` / `emit_channel` go out
  as `clientHalfActorUuid(gridId, name)` (the first 16 bytes of SHA-256 over
  `crowdy/client-half-actor/v1`, NUL, the grid id, NUL and the uuid the half named, as 32 hex
  characters), never the uuid the half named; `voxel_set` takes voxels 0-15 of type 0-255; a
  call naming its chunk a second way (`chunk`, `chunk_x`, …) is refused; and the glue caps the
  host-call requests (257 KiB), state blobs (1 MiB) and invoke replies (256 KiB) it copies out
  of a module. This SDK runs no CLIENT half; a native engine that does answers its sandbox's host
  calls itself, and should apply the same rules.

## 0.51.0 The SDK is for normal clients

Breaking. The SDK serves players, developers and org-admins and is designed for the production
environment (operator decision, 2026-09-28). It carries org-admin features but nothing only a
super-admin or a platform operator can call; platform tooling and test helpers call those fields
directly. Pinned to CrowdyJS 18.0.1, which made the same cut. `schema.gql` is unchanged: every
field below is still in the API, and the replacement for each removed wrapper is **call the API
directly from your own tooling** (`graphqlClient().request(...)` with your own document, or any
GraphQL client) with a super-admin or operator session.

| 0.50 | Game API root field (guard) | 0.51 |
|---|---|---|
| `client.operator_()` (`domains/operator.hpp`, `OperatorAPI`): `creditOrgWallet` / `creditOrgWalletAsync` | `creditOrgWallet` (`@RequiresOperator`) | Your own tooling |
| `users().paginated`, `listConnection` | `usersPaginated`, `usersConnection` (`@RequiresSuperAdmin`) | Your own tooling |
| `users().setSuperAdmin`, `setOperator`, `setEarlyAccessOverride`, `updateType`, `forceLogout` | `setSuperAdmin`, `setOperator`, `setEarlyAccessOverride`, `updateUserType`, `forceLogoutUser` (`@RequiresSuperAdmin`) | Your own tooling. `users().me`, `get`, `updateState`, `updateGamertag`, `deleteMyAccount` stay |
| `admin().organizations().setStatus` | `setOrgStatus` (`@RequiresSuperAdmin`) | Your own tooling |
| `admin().apps().setVisibility` | `setAppVisibility` (`@RequiresSuperAdmin`, the platform-wide override) | An org-admin changes their own app's visibility with `admin().apps().update(appId, {visibility})` (`manage_apps`) |
| `admin().payments().checkouts`, `checkoutsConnection`, `paymentEvents`, `paymentEventsConnection` | `checkouts`, `checkoutsConnection`, `paymentEvents`, `paymentEventsConnection` (`@RequiresSuperAdmin`) | Your own checkouts: `myCheckouts` / `myCheckoutsConnection` (stay). Platform-wide: your own tooling |
| `crowdyStudioAgent().platformPolicy`, `setPlatformPolicy`, `setOperatorAppKill` | `cpCrowdyStudioAgentPlatformPolicy`, `cpSetCrowdyStudioAgentPlatformPolicy`, `cpSetCrowdyStudioAgentAppKill` (`@RequiresOperator`) | Your own tooling. `policy`, `effectivePolicy`, `usage`, `setPolicy`, provider consent and model usage stay |
| `gen::crowdyStudioAgent::kCpCrowdyStudioAgentCatalogDocument` (no wrapper) | `cpCrowdyStudioAgentCatalog` (`@RequiresOperator`) | Your own tooling |

Every `…Async` twin went with its method. The generated documents for these fields
(`gen::users::kUsersPaginatedDocument`, `gen::controlPlane`, and so on) are gone too.

`admin().quotas().set` / `setAsync` now refuse a rule that names neither an `appId` nor an
`orgId` before any request. That is a platform-global quota, which only a super-admin can set.
Blocking, the refusal is a `graphql::CrowdyError` with code `INVALID_ARGUMENT` (an empty result
in a `CROWDY_NO_EXCEPTIONS` build). Async, it is an outcome with status `Errc::InvalidArgument`,
kind `Protocol` and the reason in `errorMessage`. Scope the rule to an app or an org
(`tierId` may narrow either). `remove(quotaId)` is unchanged.

`tests/parity/sdk-audience.test.mjs` keeps it this way: CI fails when an SDK document selects a
root field that only a super-admin or an operator can call.

## 0.50.0 The legacy engines are gone

Breaking. Pinned to CrowdyJS 18.0.0 and to the Game API after its legacy deletion (ck-api
`v2.27.0`, on dev): `schema.gql` no longer has the legacy engines' fields, so neither does this
SDK.

ck-exec (`client.exec()`) replaced the game model and its automations, Studio compute, player
compute (both targets) and the player model. Their surface is removed:

| 0.49 | 0.50 |
|---|---|
| `client.gameModel()` (`domains/game_model.hpp`): containers, functions, sessions, automations, timers, the container and player-count feeds | ck-exec hubs: state lives in a hub, calls are `ExecConnection::call`, pushes are `subscribe` |
| `client.compute()` (`domains/compute.hpp`): compute modules, templates, runs | `exec().build` / `deploy` / `logs` / `versions` |
| `client.playerModel()` (`domains/player_model.hpp`) | A mod's own state (`exec().mod*`) |
| `client.playerCompute()` (`domains/player_compute.hpp`), SERVER target: `invoke`, `runs`, `logs`, `setEnabled`, `setRequires` | A mod: `modBuild`, `modDeploy`, `modSetEnabled`, `modLogs`, and a call on `connect(appId, {execModType(name), gridId})` |
| `client.playerCompute()`, CLIENT target: `deploy`, `versions`, `artifact` / `artifactBytes`, `usage`, `setSwitch`, `switches`, `myModules`, `remove` | A mod's CLIENT half: `exec().modClientBuild`, `modClientDeploy`, `modClientDelete`, `modClientArtifactBytes` (checked against its digest), the kill ladder `modSetSwitch` |
| `domains::ClientArtifactBytes`, `decodeClientArtifactBytes` | `domains::ExecModClientArtifactBytes` |
| `marketplace()` player-code listings, versions, acquisitions, installs, grid client mods, trust and consent, `clientArtifact` / `clientArtifactBytes` | The mod marketplace (`exec().modPublish`, `modListings`, `modUnpublish`, `modInstall`) and CLIENT halves (`gridClientMods`, `consentClientMod`, `trustAuthor`). The grid claims and the studio moderation methods stay |
| `playerWallet().policies`, `setPolicy`, `deletePolicy` (player WASM policies) | None: a mod's compute is billed to its owner's wallet; `setSpendCap` bounds it |
| `operator_().computePlatformCeilings` / `setComputePlatformCeilings` | None: ck-exec's limits are the manifest's, within platform bounds. `creditOrgWallet` stays |
| `crowdyStudio().createProjectFromModules` | Create a project and start its SERVER target from `exec().modStarter` |
| Tier features on the game model | `admin().appAccess()`: `defineFeature`, `features`, `grantTierFeature`, `revokeTierFeature`, `tierFeatures` (and `…Async`), the same Game API fields |
| Game Kit: the blueprints, `deploy()`, the engines and the model-backed kits (`kit/core.hpp`, `inventory`, `objects`, `npcs`, `plots`, `economy`, `progression`, `loot`, `quests`, `combat`, `matches`, `decks`, `worldsim`, `leaderboards`, `features`, `notifications`, `mobs`, `pets`, and the realtime and session engine headers) | Hubs. `makeKit(client, appId, connection, options)` keeps `social()` (parties, guilds, chat; the guild blueprint went); `kit/wire.hpp` and `kit/actions.hpp` are unchanged |
| `session::ContainerMirror` (`session/model_mirror.hpp`) | A hub subscription |
| `studio/model_lint.hpp`, `CrowdyStudioDiagnosticSource::ModelLint` | None |
| `GraphQLErrorDetail::quarantinedKind`, `quarantinedName`, `quarantineReason` | None: only game-model objects were quarantined |
| `CrowdyStudioPlayerComputeRuntime` | `CrowdyStudioModRuntime(exec, clientRuntime, pump)` |
| `CrowdyStudioUsageSnapshot`, `ICrowdyStudioRuntime::usage`, `CrowdyStudioState::usage` | None: there is no compile quota; the Usage surface reads the wallet |

Crowdy Studio:

- The SERVER target is the grid's ck-exec mod. A deploy builds the target's crate files
  (`Cargo.toml`, `README.md`, `src/**/*.rs`) with `modBuild`, as crate `mod-<name>` when the module
  name does not start with a letter; `versions()` polls `modBuildStatus` and deploys the first
  successful build with `modDeploy`; enabling is `modSetEnabled`. The module name must be a valid
  mod name (`[a-z0-9_-]{1,48}`).
- The CLIENT target is that mod's CLIENT half, one `crowdy-client-sdk` crate (`crowdy-client-sdk =
  "0.1.0"`): a deploy builds it with `modClientBuild`, and running it attaches the build to the
  project's mod (`modClientDeploy`), consents to it as its author (`consentClientMod`), fetches it
  with `modClientArtifactBytes` and hands `ICrowdyStudioClientRuntime::start` a
  `CrowdyStudioClientArtifact{versionId, modName, module}`. A CLIENT-only project's CLIENT half
  rides the mod named for its CLIENT module: the runtime deploys the mod starter under that name
  when the grid has none of the player's, and switches it on; Stop switches it off. A CLIENT crate
  that still depends on `crowdy-compute-sdk` fails to compile with CrowdyJS 18's explanation
  (`kCrowdyStudioLegacyClientCrate`). Previews need ck-api `v2.25.1`.
- `CrowdyStudioDeployTargetInput` carries `modName` and `clientOnly`; the controller fills them.
- Invoke calls one of the mod's endpoints (`state` by default) over an exec connection and
  returns `CrowdyStudioInvokeResult{resultJson, durationUs}`. The runtime waits up to 30 s for
  the reply and calls the `pump` while it waits; the integration passes one that drains the
  client's dispatcher, and a caller that drives `poll()` on another thread can pass none.
- Logs are the project's mod's `ctx.log` lines (`CrowdyStudioLogLine`), a CLIENT-only project's
  too; the Runs surface (`CrowdyStudioPolledSurface::Runs`, `CrowdyStudioRun`), `setRequires` and
  a mod's client pairing are gone.
- `ICrowdyStudioRuntime` drops `setRequires`, `runs` and `usage`; `CrowdyStudioDeployTargetInput`
  carries the target's `files`.

Generated operations: `gen::compute`, `gen::gameModel`, `gen::playerModel`, `gen::playerCompute`,
`gen::runAdmission` and `gen::userCodeFaults` are gone, with the enums only they used
(`PlayerComputeTarget`, `PlayerFaultCode`, `UserCodeFault*`, `GmLint*`, ...). `gen::computeUnits`
keeps the compute budget documents (`AppComputeBudget`, `SetAppComputeBudget`,
`ClearAppComputeBudget`); the usage and budget-status reads went with the Game API's fields.

The live suite `e2e_operator` is gone: the operator surface left to test is a wallet credit.

## 0.49.0 ck-exec CLIENT halves (dev-tier preview)

Pinned to CrowdyJS 17.14.0 (ck-api `v2.24.0`). Additive, except that builds and listings now need
ck-api `v2.24.0`: the build fragment selects `kind` and each module's capability fields and the
listing fragment the `client*` fields, so an older API refuses `build`, `buildStatus`, `modBuild`,
`modBuildStatus`, the two waits, `modPublish` and `modListings`.

- `client.exec()`: a mod's CLIENT half, browser WASM built from a `crowdy-client-sdk` crate that the
  mod's grid serves to visitors who consent, each call with an `…Async` twin:
  `modClientBuild(appId, ExecCrate)` (the build is `kind` `client`; wait with `waitForModBuild`),
  `modClientDeploy(appId, gridId, name, buildId)`, `modClientDelete(appId, gridId, name)`,
  `gridClientMods(appId, gridId)`, `consentClientMod(appId, modId, capabilityHash)`,
  `trustAuthor(appId, gridId, authorId, capabilityHash)` and `modClientArtifact(appId, modId)`.
- `modClientArtifactBytes(appId, modId)` / `modClientArtifactBytesAsync` decode the served module
  for a native sandbox as `ExecModClientArtifactBytes` (the bytes, their lowercase `digest`,
  `fuelPerDispatch` as decimal text, `tickIntervalMs`, the parsed `capabilitySummary`, ...). Bytes
  whose SHA-256 is not the digest, a CLIENT ABI other than `kExecClientAbiVersion` (0) and a
  capability summary without a list of `hostFunctions` are refused: blocking, as a
  `graphql::CrowdyProtocolError` (an empty result built without exceptions); async, as an outcome
  of kind `Protocol`. The SDK runs no WASM; let the module call only
  `capabilitySummary.hostFunctions`.
- `parseExecClientCapabilitySummary(json)` parses a `capabilitySummaryJson`,
  `authorCapabilitySummaryJson` or a listing's `clientCapabilitySummaryJson`.
- Builds carry `kind` (`exec` or `client`) and each module's `capabilitySummaryJson`,
  `capabilityHash` and `tickIntervalMs` (null for a ck-exec module). Listings carry the CLIENT half
  the mod had when published (`clientDigest`, `clientCapabilitySummaryJson`, `clientCapabilityHash`,
  `clientTickIntervalMs`), which `modInstall` attaches to the installer's mod.
- The legacy grid-attached client mods (`marketplace().gridClientMods`, `consentGridClientMod`,
  `trustGridAuthor`, `clientArtifact`, `clientArtifactBytes`) are superseded by these (removed in
  0.50.0).
- `LocalActorStore`: a send that fails, the loop's or a manual `refresh()` / `moveTo()`, is sent
  again on the next tick even when nothing changed; it used to wait for the next keyframe.
- `SaveStateStore::save` built with `CROWDY_NO_EXCEPTIONS`: a save the API refuses keeps the blob
  cached and `dirty()`, so the next `save()` retries it; it used to be marked saved.

## 0.48.0 ck-exec observability (dev-tier preview)

Additive. Pinned to CrowdyJS 17.13.0 (ck-api `v2.22.0`).

- `client.exec()`: `endpointStats(appId, nodeType, sinceMinutes)` / `endpointStatsAsync`
  (`execEndpointStats`): per endpoint `{ nodeType, method, calls, appErrors, busy, denied,
  deadlineExceeded, otherErrors, timedCalls, latencyMsAvg, latencyMsMax, firstMinute,
  lastMinute }` over the last `sinceMinutes` (default 60, at most 10080).
- `ExecLogsQuery::flow` keeps one flow's lines (`logs` only; mod logs take none), and every
  log line, also from `modLogs`, has `flow`: 32 lowercase hex digits, null outside a call.
- `versions` lines carry `manifestJson`, the deployed manifest (a spawn seed shown as
  `seed_bytes`), null when the version's row is gone.
- `ExecReply::rateLimited()` and `retryAfterMs()`. A call over a player's limit (120 per 10 s
  per app and host) is answered `Busy` with a message starting "rate limited" and ending
  "retry in N ms"; wait that long before calling again. The SDK retries only a lost connection
  and `Moved`, never `Busy`.

## 0.47.0 ck-exec mods (dev-tier preview)

Additive. Pinned to CrowdyJS 17.12.0.

- `client.exec()`: mods, players' code on grids they own, each call with an `…Async` twin:
  `modStarter`, `modBuild(appId, ExecCrate)`, `modBuildStatus`, `modDeploy(appId, gridId, name,
  buildId)`, `modSetEnabled`, `modDelete`, `mods(appId, gridId)`, `myMods`, `modLogs`,
  `modPublish`, `modListings`, `modUnpublish`, `modInstall`, `appMods`, `modSwitches` and
  `modSetSwitch(appId, gen::ExecModScope, off, target, reason)`. `waitForModBuild` polls
  `modBuildStatus` blocking and has no twin, like `waitForBuild`.
- `execModType(name)`: the node type players call a mod by (`mod:<name>`), keyed by its grid id.

## 0.46.0 ck-exec builds (dev-tier preview)

Additive. Pinned to CrowdyJS 17.11.0.

- `client.exec()`: `starters(appId)` (`execStarters`: the four starter crates and a manifest),
  `build(appId, std::vector<ExecCrate>)` (`execBuild`, returns the build queued) and
  `buildStatus(appId, buildId)` (`execBuildStatus`), each with an `…Async` twin, and
  `waitForBuild(appId, buildId, intervalMs, timeoutMs)`, which polls `buildStatus` blocking and
  has no twin (poll `buildStatusAsync` from an event loop).
- `deploy(appId, root, types, buildId)`: with a build's id, an `ExecNodeType` may leave `wasm`
  empty and set `crate`. The `deployAsync` overload without a build id is unchanged.

## 0.45.0 ck-exec operations (dev-tier preview)

Additive. Pinned to CrowdyJS 17.10.0.

- `client.exec()`: `connectAsDeveloper(appId, ExecConnectOptions)` / `connectAsDeveloperAsync`
  and `developerEndpoint` / `developerEndpointAsync` (`execConnectAsDeveloper`: your own session
  with the org's `manage_compute`; the session calls any node type as `Caller::Developer`).
- `logs(appId, ExecLogsQuery)`, `instances`, `versions`, `status` (`view_compute_diagnostics`),
  and `activateVersion(appId, version)` and `setEnabled(appId, enabled, nodeType)`
  (`manage_compute`), each with an `…Async` twin. They return the GraphQL JSON.

## 0.44.0 ck-exec (dev-tier preview)

Additive. Pinned to CrowdyJS 17.9.0.

- `client.exec()`: `connect(appId, ExecConnectOptions)` / `connectAsync`, `endpoint` /
  `endpointAsync` (`execConnect`), `deploy` / `deployAsync` (`execDeploy`, digests computed).
- `ExecConnection`: `call` (JVal args as MessagePack), `callRaw`, `subscribe` / `unsubscribe`,
  `ping`, `onReconnect`, `host`, `connected`, `close`; `ExecConnection::open(transport,
  dispatcher, endpoint)` for a known gateway and token. Replies are `ExecReply` (`status`,
  `value()`, `message()`), pushes `ExecPush`.
- `crowdy::domains::exec_wire` encodes and decodes the client protocol (golden frames in
  `tools/parity/fixtures/exec-client-frames.json`, a copy of ck-exec's).
- `graphql::Json::toMsgpack()` and `Json::fromMsgpack()`.
- The curl WebSocket transport sends no `Sec-WebSocket-Protocol` header when no subprotocol is
  requested (the ck-exec gateway speaks none).

## 0.43.1 Circuit open cause

Additive. Pinned to CrowdyJS 17.8.0.

- `GraphQLErrorDetail::cause` is filled from `extensions.cause`. `watchdog_timeout` means the circuit opened on watchdog kills.
- `PlayerFaultCode::CIRCUIT_OPEN` is distinct from `TEMPORARILY_DISABLED`.

## 0.43.0 Grids (DN-10)

Additive. Pinned to CrowdyJS 17.7.0.

- `client.grids()`: `mintToken(appId, gridId, ttlSeconds)` (a grid-scoped
  token: an app token narrowed to one grid, deny-by-default on the server and
  refused by the binary relay), `createChannel(appId, gridId, name)` (a grid
  channel; grid owner only) and `channels(appId, gridId)`, each with an
  `...Async` twin.
- `gameModel().sessions(..., gridId)`: only sessions hosted inside a grid.
  Session JSON carries `gridId`.
- `PlayerWasmPolicy` / `SetPlayerWasmPolicyInput` gain `channelEgress`,
  `spatialMaxDistance`, `gridEventEgress` (schema only; set them through the
  raw GraphQL client).
- The browser-only CrowdyJS additions (`GridScope`, `grid-program`,
  `startGridMod`, the crowdy-dsh bridge v4 grid requests) are classified
  browser exclusions in `docs/parity-matrix.md`.

## 0.42.1 CLIENT_CAPABILITIES is sent

Bugfix. No signature changes. Same CrowdyJS 17.6.0 pin as 0.42.0.

0.42.0 built `CLIENT_CAPABILITIES` (opcode 29) and then `encodeLongSpatial` rejected
it, because `isLongSpatialLayout` did not list 29. The send returned before
`transmit()`, `housekeeping()` discarded the error, and nothing reached the server,
so `MESSAGE_BUNDLE_SIGNED` was never requested. 0.42.1 sends it. A failed advertise
is logged at Warn; the next attempt is still one `advertiseIntervalMs` later.

## 0.42.0 one HMAC per downlink bundle

Additive. Tracks Buddy v0.30.0 and CrowdyJS 17.6.0 (parity pin). No signature changes.

- `Connection` now sends `CLIENT_CAPABILITIES` (opcode 29) once the session is
  `Connected` and every `Config::advertiseIntervalMs` (15 s). Set
  `Config::advertiseCapabilities = false` to behave as 0.41.
- A Buddy at v0.30.0+ answers with `MESSAGE_BUNDLE_SIGNED` (opcode 30): one HMAC over the
  datagram, members without their own. `wire::verifySignedBundle` checks it (always;
  mismatches count in `Stats::hmacFailures`), `wire::forEachMessage` walks past the tail,
  `Stats::signedBundlesReceived` counts them. Applications that consume `Event`s see the
  same spatial notifications as before, with `containsAuth = 0`.

## 0.41.0 bulk containers

Additive. Tracks cks-game-api v2.6.0 (PRs #346–#349) and CrowdyJS 17.5.0, which
this release's parity gate pins. Every existing method keeps its signature.

- New `GameModelAPI::containerStates(appId, containerIds)` and
  `containerStatesAsync` — the bulk twin of `containerState`: same selection,
  ids as a JSON array, up to 500 per call; ids the app does not hold are
  omitted, duplicates once, input order kept.
- `GmSessionFields` carries `seededContainerCount` (populated on the create
  response only; null on every other read). Container-type selections read
  `scope` (`session` | `app`).
- Generated inputs carry `SeedContainerInput.bindingKey`,
  `SeedContainerTypeInput.scope`, `UpsertContainerTypeInput.scope` and
  `CreateSessionInput.seedFromApp { typeNames, initialState }`; `seed()`,
  `upsertContainerType()` and `createSession()` accept them unchanged.
- Server contract to know: `containers` returns 200 rows when `limit` is
  omitted and refuses above 1,000 (`BAD_REQUEST`); a caller that relied on an
  unbounded list must page. On an `app`-scoped type, `ensureContainer` /
  `createContainer` with a `sessionId` refuse with `CONTAINER_TYPE_APP_SCOPED`.
  Seeded copies of an ended session are dropped by the server after the tier's
  retention window (7 days on every tier); hand-made rows are never purged.
- Parity re-pinned to CrowdyJS 17.5.0 (`81eea4af`); the 45 "covered — CrowdyJS
  17.3.0's snapshot predates it" waivers are gone. `GameModelAPI.sessionChanged`
  joins the async-twin waivers (a subscription handle).

## 0.40.0 the game-model session system

Additive. Tracks cks-game-api PR #319 on top of ck-api v2.3.0. Every existing
`GameModelAPI` session method keeps its signature; the SDK adds what the server
now knows about a session. Numbered 0.40.0 because #100 took 0.39.0 (schema
sync to v2.3.0, parity CrowdyJS 17.3.0) while this was open.

### What is new

- **Roster, admission, capacity, host on `GmSession`:** `admission`
  (`open | locked | closed`), `maxParticipants`, `participantCount`,
  `hostUserId`, `hostTerm`, `revision`, `endedAt`, `endReason`, `createdAt`.
  `createSession` accepts `maxParticipants`, `admission`, `emptyTimeoutSec`,
  `presence`, `idempotencyKey`; `sessions(appId, status, admission,
  hostUserId, limit)` filters and limits.
- **New methods (sync + `Async` twins):** `leaveSession`, `setSessionAdmission`,
  `transferSessionHost`, `endSession` (host or app admin; all accept
  `expectedHostTerm` and `idempotencyKey`), `sessionSnapshot`, `sessionEvents`,
  `sessionInspect` (`manage_apps`), and the typed GraphQL-WS stream
  `sessionChanged(appId, sessionId, afterRevision, callbacks)` delivering
  `GameModelSessionEvent` (`revision`, `kind`, `payloadJson`, ...) in the
  `containerChanged` shape. The contract is the player-count feed's: pull the
  snapshot, apply events above its revision, re-pull on a gap.
- **Reconnection is a rejoin.** `joinSession` on a session you are in returns
  your row with `incarnation + 1` and supersedes any older client of yours; the
  join result is the full roster row. **`leaveSession` requires that
  `incarnation`** -- there is no "leave regardless" -- so a stale client can
  never remove the one that took over (`SESSION_INCARNATION_STALE`).
- **Presence is your actor -- a behaviour change every consumer inherits from
  the server.** A joined participant with no fresh Buddy actor in the app after
  the join grace window (60 s by default) is marked `left` / `presence_expired`,
  and a session nobody has been joined to for longer than its `emptyTimeoutSec`
  (5 min by default; `0` disables) is ended as `abandoned`. A client that only
  speaks GraphQL therefore drops out of a session it never replicates in. Pass
  `actorUuid` on join to bind presence to one specific actor -- your own, in
  Buddy's 32-hex form (`core::toString(WorldSession::actorUuid())`). Do not
  pass the matches kit's channel-ping uuid; it never spawns in Buddy.
- **Opting out: `presence = "none"`.** A session created with
  `sessionInput["presence"] = "none"` is never judged by actor presence
  (`GmSession.presence` reports the mode; `sessionInspect` shows its rows as
  `presence: "none"`). Its roster's only exits are `leaveSession`,
  `endSession` and the empty timeout once everyone has left. Use it for
  turn-based play that talks GraphQL and channel pings and never replicates an
  actor. The mode is fixed at creation. A GraphQL-only session that does
  **not** opt out empties after the grace window and is abandoned after the
  timeout.
- **The `sessionChanged` push is per datacenter; the events table is the
  record.** A revision committed in one region wakes subscribers on that
  region's API replicas; `sessionEvents(appId, sessionId, afterRevision)` (and
  the replay the stream performs on connect) reads the durable rows, so a
  subscriber reconnecting anywhere catches up from the revision it last saw.
- **Error codes** (on `CrowdyGraphQLError::code()`): `SESSION_FULL`,
  `SESSION_LOCKED`, `SESSION_CLOSED`, `SESSION_ENDED`,
  `SESSION_NOT_PARTICIPANT` (the caller is not joined),
  `SESSION_TARGET_NOT_PARTICIPANT` (the user named to `transferSessionHost`
  is not joined), `SESSION_INCARNATION_STALE`, `SESSION_HOST_TERM_STALE`.
- **`kit::MatchesKit` creates its session with `presence = "none"`, and now
  leaves and ends it.** A kit match is GraphQL plus channel pings; its uuid is
  only the channel-message sender id and never spawns in Buddy, so under the
  default mode every player would be expired after the grace window. Because
  nothing expires anybody, the kit owns the roster's exits: new
  **`leave(match, incarnation = nullopt)`** calls `leaveSession` with the
  incarnation the kit remembered from `create()` / `join()` on this instance
  (or the one you pass; `std::invalid_argument` when neither is known) and
  leaves the match channel; **`finish()` now ends the backing session**
  (`endSession`, reason `completed`) after a successful `end_match`, so the
  roster is cleared and the session's events become eligible for retention.
  The result is a `KitMatchFinishResult` whose `sessionEnd` says what happened
  to the session: `"ended"`, `"already_ended"` (a replayed finish), or
  `"forbidden"` (`end_match` admitted the caller but the session did not -- the
  creator who already left, or the app's elected host who is not the session
  host -- so the match is finished, the session is not, and nothing is thrown;
  an app admin can `endSession` it); any other refusal propagates. An emptied
  session that was never finished is abandoned by the empty timeout.
  Otherwise the kit is unchanged: capacity still lives in `MatchMeta` and
  join does not bind an actor. Moving it onto session capacity / admission /
  host is a later, separate change.
- **Schema and parity.** `schema.gql` is synced from the cks-game-api PR #319
  branch rebased on `dev`: the v2.3.0 SDL plus the session delta. The parity
  gate stays pinned to CrowdyJS 17.3.0 (as on `dev`); the session surface is
  CrowdyCPP-ahead and classified `covered-extension` until the pin moves to
  CrowdyJS 17.4.0, at which point those entries go stale and the gate says so.

No removals.

## 0.38.0 a bound GitHub project saves as commits

`CrowdyStudioAPI::saveProject` now follows CrowdyJS 17.0: a `STUDIO` project
still writes through `crowdyStudioProjectSave`; a `GITHUB` project commits
each changed file through `crowdyStudioGitHubPutFile` / `DeleteFile` under
`github.sha` (`expectedCommitSha`) and sends metadata as a project save with
no file bodies. A stale commit or a caller holding an older revision than
the provider last returned is the same `CrowdyStudioRevisionConflictError`
the editor already recovers from. The controller does not know which path
ran.

### What changes for you

- **Nothing if your projects stay in Studio.** Unbound saves are unchanged.
- **A bound project can no longer be saved as Studio file bodies.** Against
  ck-api v2.0 those mutations refuse with `GITHUB_BOUND_USE_CONTENTS`. If you
  were constructing `SaveCrowdyStudioProjectInput` yourself for a project
  whose `source` is `GitHub`, keep doing that — `saveProject` now issues the
  commits. Refresh a bound project that has no `github.sha` before saving.
- **New surface.** `client.crowdyStudioGitHub()` is the typed transport
  (`status`, `layout`, `tree`, `getFile`, `putFile`, `deleteFile`,
  `refresh`, `bind`, `unbind`, `repos`, `connectUrl`). Status / layout /
  tree / file / put / delete / refresh work with an app token; connect /
  repos / bind / unbind need the identity session. Path arithmetic lives in
  `crowdy/studio/github_layout.hpp` (`studioFileToRepoPath` and friends);
  the SDK does not parse `crowdy.json`.
- **Still browser-only.** The hosted Studio settings card
  (`bindGitHubRepo`, `connectGitHub` opening a tab, card busy-state) is not
  on the native controller. A native host that wants bind/unbind calls
  `crowdyStudioGitHub()` itself.
- **`saveProjectAsync` on a bound project is still blocking.** The STUDIO
  path posts one save on the async transport and delivers the callback
  from `poll()`. A GITHUB project runs the same commit loop as
  `saveProject` on the caller's thread and fires the callback before
  returning. The controller uses the sync save. A host that chose
  `*Async` to keep a frame from stalling should treat a bound save like
  `saveProject` until that path is itself async.
- **Also.** `playerComputeSetSwitch` / `playerComputeSwitches` carry
  `listingRef` (LISTING-scope kill), matching CrowdyJS 17.1.0.

Tracks CrowdyJS `17.1.0` (the 17.0 bound-save contract plus 17.1 bundling).

## 0.37.0 outbound sends are bundled by default

`replication::Connection` now packs the messages you send within a short window
into one `MESSAGE_BUNDLE` datagram (`[2]{[u16 LE len][signed message]}...`), the
framing the server has always used for its notifications. Every member is still
a complete, individually HMAC-signed message; only the datagram boundary moved.

### What changes for you

- **Nothing in the send API.** `sendActorUpdate` and friends return the sequence
  number exactly as before. A send is now *accepted* rather than *transmitted*
  when it returns: the datagram leaves when `Config::bundleWindowMs` (default
  1 ms) has passed since the bundle opened, when the next message would not fit
  in 1232 bytes, on `Connection::flushSends()`, at the end of
  `WorldSession::tick()`, before any `*AndWait` starts waiting, and on
  `disconnect()` / reassignment. A lone message is sent unwrapped, so a client
  that sends one message per window puts the same bytes on the wire it always
  did.
- **Manual pump.** With `Config::manualPump` nothing flushes between your
  `pump()` calls except capacity, `flushSends()` and `WorldSession::tick()`.
  Call `flushSends()` at the end of your frame if you pump less often than you
  want datagrams out.
- **Stats.** `Stats::datagramsSent` can now be less than `Stats::messagesSent`;
  `Stats::bundlesSent` counts datagrams that were wrappers (two or more
  members). `messagesSent` advances when a message joins the bundle; the
  datagram/byte counters when the datagram is handed to the kernel. A
  `WouldBlock` on flush keeps the bundle pending for the next attempt and moves
  `sendsDeferred`; a genuine fault drops it and moves `sendsFailed` (once, for
  the datagram) and `Stats::messagesDropped` (once per member that was in it,
  since `messagesSent` had already counted them).
- **Server requirement.** The replication server must unpack client bundles
  (Buddy v0.27.0+). Against an older server, set `Config::bundleSends = false`;
  otherwise any two messages sent within a window are dropped together.
- **Opt out.** `Config::bundleSends = false` is exactly the 0.36 behaviour: one
  datagram per message, transmitted synchronously from the calling thread.

### Also new

- `wire::BundleWriter`, `wire::kBundleHeaderSize`, `wire::kBundleLengthPrefix`,
  `wire::kMaxBundleMembers`, `wire::kMaxBundleMemberSize` (header-only, the
  writer half of `wire::forEachMessage`).
- `UdpSocket::wake()` / `canWake()`: a blocked receive on the net thread can be
  interrupted so a bundle opened mid-wait still leaves within its window.
  (POSIX only; on Windows the net thread bounds its receive wait to one window
  instead.)

## 0.34.0 the Crowdy Agent orchestrator is retired

The Agentic Crowdy Studio agent no longer runs on the server. CrowdyJS 16 docks
the DeepSeek Harness inside the browser Studio pane; it edits the player's
project, runs draft tests and takes screenshots from the page, and spends model
tokens through `POST /v1/model/chat/completions` with the player's own app
token, billed to the player's wallet by default (or the app's org wallet).
The 21 GraphQL roots this SDK drove (`crowdyStudioAgentCreateSession`,
`...SendMessage`, `...Events`, leases, approvals, tool results, history,
budget, ...) are gone from the API, so a 0.33 client loses that surface the
moment the API deploys whether or not it upgrades. There is no native
counterpart: an engine has no Studio pane to dock the harness in.

### Removed

- `crowdy/agent/*` (`CrowdyStudioAgentController`, `CrowdyStudioAgentGraphQLTransport`,
  `AgentToolRegistry`, `NativeToolDispatcherV1`, `NativeBrowserToolDispatcherAdapter`,
  `CrowdyStudioAgentControllerRuntime`, agent schema/types/errors).
- `crowdy/studio/host_adapter.hpp` (`CrowdyStudioHostAdapter`,
  `CrowdyStudioControllerHostAdapter`, the 11 native Studio tools) and
  `crowdy/studio/agent_projection.hpp`.
- `crowdy/player_host/lease_manager.hpp` (`AgentControlLeaseManager`) and
  `crowdy/player_host/control_gate.hpp` (`NativePlayerControlGate`).
- `CrowdyClient::createCrowdyStudioAgentController`.
- `CrowdyStudioIntegrationOptions::{agent, nativeTools, studioHost, controlLeases,
  controlGate, leaseManager, autoInitializeAgent}`;
  `CrowdyStudioIntegration::{agentController, leaseManager, leaseSnapshot,
  controlGate, controlSnapshot, nativeTools, initializeAgent}`;
  `CrowdyStudioIntegration::create` no longer takes an agent runtime factory.
- On `domains::CrowdyStudioAgentAPI`: `session`, `sessions`, `history`,
  `toolDescriptors`, `budget`, `createSession`, `attachClient`, `setMode`,
  `acknowledgeEvents`, `heartbeat`, `sendMessage`, `approveTool`, `rejectTool`,
  `browserToolResult`, `grantLease`, `revokeLease`, `pause`, `resume`,
  `cancelRun`, `closeSession` and their async twins.
- The generated `gen::CrowdyStudioAgentPreemptionReason` enum.
- Parity fixtures `crowdyjs-agent-tools`, `crowdyjs-descriptor-digests`,
  `crowdyjs-preemption-reasons`, `crowdyjs-player-control-gate`,
  `crowdyjs-studio-host-tools`, `crowdy-studio-runtime-sync` and their
  generators; the `${CMAKE_INSTALL_DATADIR}/crowdy/agent` install tree.

### Added

- `CrowdyStudioAgentAPI::providerConsent(appId)`, `setProviderConsent(input)`,
  `modelUsage(appId, limit?)` and async twins
  (`operations/crowdyStudioAgent/CrowdyStudioModel.graphql`).
- `player_host::PreemptionReasonV1` is now a contract-owned enum with the same
  16 wire names in the same order, plus `preemptionReasonFromName`. It used to
  be an alias of the removed generated enum; code that switched over it
  compiles unchanged, code that called `gen::toString` on it should call
  `preemptionReasonName`.
- `CrowdyStudioIntegration::schedule(task)` queues work for the maintenance
  lane (what the removed `studioHost.schedule` did), and `playerHost()` hands
  back the observation-only adapter the engine passed in.

### What to do in a game

- Delete agent wiring. `CrowdyStudioIntegration` still owns the headless Studio
  controller, layout controller and editor bridge; `poll()` pumps the platform
  and `runStudioMaintenance()` is the blocking lane, unchanged.
- Keep `PlayerHostAdapterV1` if your tooling reads observations from it;
  nothing in this SDK dispatches to it any more. The interface keeps
  `dispatch` and `clearAgentIntent` for source compatibility.
- Read agent policy and usage with `client.crowdyStudioAgent()`; record the
  player's provider-data consent with `setProviderConsent` if you drive the
  REST model endpoint yourself.

This is a breaking change in a 0.x line, so it is a minor bump (the same rule
0.20.0 used for `managementUrl`). Requires the ck-api release that carries the
metered model endpoint (first dev release after `v1.98.0`); tracks CrowdyJS
`16.0.0`.


## 0.29.0 nothing runs for an app with no player in it

The schema snapshot moves from 2026-08-28 to the current published SDL, which
carries three platform changes the SDK was documenting incorrectly. Two are
source-compatible; the third refuses a value this SDK's own docs told you to
pass.

### `alwaysOn` is refused

`compute().upsertModule()` with `alwaysOn: true` now fails with `BAD_REQUEST`.
The field is deprecated server-side, always reads `false`, and is dropped from
the module fragment this SDK selects, so `ComputeModuleFields` no longer returns
it.

`compute.hpp` told you to "set alwaysOn=true for world simulation that must run
without connected players". That is now the one thing it cannot do. Delete the
argument.

### Modules and scheduled work require a player

A compute module ticks only while its app has at least one player connected
anywhere in the fleet, and stops when the last one leaves. Scheduled automations
and `gm_timers` follow the same rule, with one difference worth knowing:

- **Automations** due while the app is empty are **skipped** and rescheduled from
  the moment a player returns. Missed runs are never made up.
- **Timers** whose deadline passes while the app is empty **wait** and fire on
  return. Late, not lost.
- **`event` and `manual` triggers** are unaffected — something already asked.
- **Synchronous `compute().invoke()`** is unaffected — a request, not a tick.

**What to change in a game.** Make scheduled work idempotent in *elapsed time*
rather than assuming a cadence: advance the world by `now - lastTick`, and store
expiries as timestamps rather than remaining-tick counters. The Game Kit headers
for `worldsim`, `combat` and `economy` said their automations run "with no client
online"; that was true and is not, and all three now say so along with how to
write against it. A blueprint that assumes a cadence stalls silently while nobody
is playing, which is the failure mode to design out.

### Reservations split, and mean something different

`App` gains `reservedUdpBytesPerSec` and `reservedGraphqlOpsPerSec`, and the app
query now selects both. `reservedEgressBytesPerSec` is deprecated and returns the
UDP value for one release. Reserving one dimension does not reserve the other.

`admin().setAppReservedThroughput()` is unchanged in shape and changed in
meaning. Three things a reservation is **not**, all of which it either was or
appeared to be before:

1. **Not a ceiling.** It obliges the platform to keep that much capacity
   provisioned for you and does not cap what you may send. Traffic above the
   reserved rate is metered, not refused.
2. **Not a data allowance.** The monthly fee buys capacity, not volume, and is
   charged *in addition to* metered usage. Reserving 5 MB/s does not make the
   first 5 MB/s free.
3. **Not the way to lift the free-tier cap.** Unfunded free apps are shaped at
   roughly 1 MB/s; funding the org wallet or enabling auto-billing lifts that.
   Until 2026-09-01 a reservation did double duty as the bypass, which is why the
   old schema description said "bypasses the ~1 MB/s rate limit".

### `Connection::Stats` byte counters are not your bill

No behaviour change; a documentation one worth reading if you have ever compared
the two. `bytesSent` and `bytesReceived` are a local diagnostic and will not
reconcile against an invoice:

- Billing counts **egress only**. `bytesSent` is traffic leaving the CLIENT,
  which is ingress to the platform and is not counted toward Aggregate Data
  Volume at all.
- The platform measures at **its** network interface, including IP and UDP
  headers. These counters count the datagram payload.

Use them for backpressure and diagnosis; use `admin().appUsageSummary()` for what
you are charged.

### Also

The rate-card field docs carried a stale worked example of 15c per GiB; the
published SDL now says 19c, matching the shipped card.


## 0.25.0 rate-limit refusals carry how long to wait

The server sends `extensions.retryAfterMs` on a `RATE_LIMITED` refusal.
`GraphQLErrorDetail` parsed a fixed set of extension keys and kept no raw
`extensions` handle, so that one was unreachable: a caller could tell that it had
been refused for asking too often and could not tell for how long. CrowdyJS
callers read `error.extensions` directly and never had this gap.

### Added

- **`GraphQLErrorDetail::retryAfterMs`** — `std::optional<std::int64_t>`,
  populated from `extensions.retryAfterMs` when it is a JSON number.

It is optional deliberately, and the `retryable` field beside it is the wrong
model to copy. `retryable` defaults to `true` because the server contract says an
absent value means "trying again is reasonable". There is no such default for a
duration: **`0` means retry now and absent means the server named no wait**, and
collapsing the two would turn silence into a busy loop. A value that is present
but not a number reads as absent.

```cpp
if (const auto& wait = error.retryAfterMs) {
  scheduleRetryIn(std::chrono::milliseconds(*wait));
} else {
  scheduleRetryWithLocalBackoff();
}
```

**Read it as a deadline, not as an interval to reuse.** On the invoke rate limit
the server computes what REMAINS of a fixed window rather than a fixed backoff,
so a second refusal inside the same window carries a smaller number, and a value
cached from an earlier refusal will be too long.

### Fixed

Subscriptions and HTTP requests were parsing the GraphQL `errors` array through
two independent copies of the same code, and they had drifted: the subscription
copy never read `blame`, so the same refusal arriving over the websocket lost its
attribution. Both call one internal `readGraphQLError` now.

**If you branched on `blame` and treated its absence on a subscription error as
"unattributed", that branch will now see the real value** — which is the
documented contract, but it is a behaviour change on the websocket path.

No wire change, and no other public type moved.

## 0.24.0 sign and verify with a pre-keyed MAC

Every datagram was signed with OpenSSL's one-shot `HMAC()`, which builds a
context and re-imports the 64-octet token for each call. The token changes only
on refresh, so that setup was repeated needlessly on every send and on every
inbound notification, and it was not a small part of the cost: it was
essentially all of it. Encoding a datagram takes 4 ns; signing it took 1225.

Measured on the builder, with the numbers and the method in
[benchmarks/README.md](benchmarks/README.md):

- encode + sign: 1225 ns -> **319 ns**
- verify a notification: 1276 ns -> **337 ns**
- a 200-entity frame through `Connection::sendActorUpdate`: about
  **1.5x** less CPU

No wire change. Same key, same message, same tag — the golden vectors are
unmoved and a test signs against them through the new path, including from
several threads at once.

### Added

- **`crowdy::core::IMac`** — a MAC bound to one key, reusable across messages,
  computing over parts without concatenating them.
- **`ICrypto::makeHmacSha256(key)`** — returns a pre-keyed `IMac`, or nullptr.

**Implementing it is optional.** The base class returns nullptr and every
caller falls back to `hmacSha256`, so an engine-injected `ICrypto` written
before this release keeps compiling and behaving identically; it simply does
not get the speedup. Engines that bind their own crypto should implement
`makeHmacSha256` to pick it up — it is the single largest CPU win available on
the replication path.

- **`spatialHmac`, `encodeLongSpatial`, `encodeChannelMessage` and
  `verifyLongSpatial`** take an optional trailing `const IMac*`. Existing calls
  compile and behave exactly as before.

### Changed

- `Connection` builds the pre-keyed MAC once per token, rebuilds it on
  rotation, and reads the token id, key and MAC under a single lock where it
  previously took the same mutex twice per send.

### Considered and rejected

Batching writes with `sendmmsg` measured between 1.00x and 1.04x, including
with loopback delivery removed from the measurement, because the kernel does
the same per-datagram work either way and only the syscall boundary is
amortised. No batch API was added. The evidence is in
[benchmarks/README.md](benchmarks/README.md) so the decision can be revisited
on hardware where syscalls cost more.

## 0.23.0 send backpressure is not a socket failure

The UDP send path treated every short write as `Errc::SocketError` and threw
the OS error code away. On a non-blocking socket a full kernel send buffer is
`WSAEWOULDBLOCK`/`EAGAIN` — ordinary backpressure — so a client emitting a large
population in one frame silently lost outbound updates and logged them as
socket faults. It was reported from a 200-entity, 10 Hz Unreal harness: several
hundred sends "failing" inside three milliseconds, which no broken socket does.

Nothing about the wire changed. No datagram, framing, or HMAC behavior is
affected.

### Added

- **`Errc::WouldBlock`** — transient: nothing was sent, the socket is healthy,
  retry shortly. Appended to the enum, so every existing value is unmoved.
- **`ReplicationConfig::socketSendBufferBytes`** (default `1 << 20`) — the
  `SO_SNDBUF` hint, matching the `socketRecvBufferBytes` that already existed.
  The send side previously ran on whatever the OS default happened to be.
- **`Connection::Stats::sendsDeferred`** and **`sendsFailed`** — saturation and
  faults counted apart. `sendsDeferred` rising under load is expected;
  `sendsFailed` rising is not.
- **`UdpSocket::nativeHandle()`** — the underlying descriptor, for diagnostics
  and reading socket options back. The socket still owns it.

### Changed

- **`UdpSocket::send` returns `Errc::WouldBlock` for a full send buffer.**
  Callers treating any non-`Ok` as fatal will now see it. Requeue and retry
  instead of dropping; a permanently full socket needs a bounded queue.
- **An exhaustive `switch` over `Errc` needs a `WouldBlock` case.** `errcName`
  handles it, and unmatched values still fall through to `"Unknown"`.
- **The POSIX send is now non-blocking** (`MSG_DONTWAIT`), matching Winsock and
  the receive path in the same file. Previously a saturated buffer blocked the
  calling thread on POSIX — a frame hitch in a game loop — where it now returns
  `WouldBlock` promptly. Portable callers must handle that return.
- **`UdpSocket::open` takes a fourth argument**, `sendBufferBytes`. Direct
  callers of the socket (rather than `Connection`) need updating; `<= 0` keeps
  the OS default.
- **`LocalActorStore` no longer records a deferred send as an error.**
  `status()` stays out of `Error` for a merely busy socket, and the update
  remains dirty so the next tick retries it.
- **A heartbeat that failed to send is no longer recorded as sent.** It
  previously advanced the heartbeat timestamp regardless of the result, so a
  heartbeat that never left the box banked the whole interval and presence
  could lapse while every counter still read healthy. This applies to any
  failure, not only backpressure.

## 0.20.0 one origin, and endpoints that can move (breaking)

Tracks CrowdyJS 14.1.0. 0.17.0 unified the two APIs behind one server but kept
the two-endpoint shape in the client; this release removes it, and adds the
machinery a client needs when the one endpoint it holds stops answering.

### Removed

- **`ClientConfig::managementUrl`** and **`ClientConfig::managementGraphqlEndpoint`**.
  Pass `httpUrl` (and `wsUrl`). For a per-game client that is the app's OWN
  datacenter endpoint from `mintAppToken`, because that is where its shards live.
- **`CrowdyClient::managementClient()`** and **`CrowdyClient::managementSubscriptions()`**.
  Use `graphqlClient()` and `subscriptions()`.
- **`MarketplaceAPI`** and **`CrowdyStudioAgentAPI`** take one GraphQL client.
  Studio moderation, policy, usage and operator roots are separated from player
  operations by the permissions they require, not by endpoint.
- **`gen::GraphQLEndpoint`** and the per-domain **`endpointFor()`**, along with
  the `schema.management.gql` / `schema.game.gql` snapshots that produced them.

CrowdyJS throws a `TypeError` when a caller passes a removed option. C++ gets
the stronger version for free: a removed struct field is a compile error, so
there is no build in which the old spelling is silently ignored.

### Added

- **`ClientConfig::discoveryUrl`** — the shared origin (multivalue DNS over
  every datacenter's balancer), returned as `discoveryUrl` by `mintAppToken`,
  `refreshAppToken`, `exchangePortalCode` and `gameClientBootstrap`. Set it and
  a client whose endpoint dies can ask where to go next.
- **`ClientConfig::rediscover`** / **`rediscoverAfterFailures`** (default 3) —
  the re-discovery hook, coalesced and never fatal.
- **`discovery()`** — `appDiscovery`, answering where an app is served BEFORE
  login, so a client can start on the shared origin and move before it
  authenticates.
- **Datacenter redirect** — `WRONG_DATACENTER` moves the client (HTTP, WS and
  UDP together) and retries once; `APP_UNAVAILABLE` raises
  `CrowdyAppUnavailableError` and carries no endpoint, on purpose.
- **`SERVER_DRAINING`** — a control-only `udpNotifications` subscription, so a
  native client gets the same advance warning a browser client already had.

### Environment variables

`CROWDY_E2E_API_URL` replaces `CROWDY_E2E_MANAGEMENT_URL` in the e2e harness.
The old name is still read: it names the same origin now, and a harness still
setting only the old name would have made every suite exit 77 — a skip, which
reads as a pass rather than as a failure.

## 0.17.0 unified API (breaking)

Tracks CrowdyJS 13.0.0: the platform merged the Management and Game APIs into
ONE server on the shared database (galaxy then; PostgreSQL + Citus since
2026-08-04). The committed schema snapshots were resynced from the unified SDL,
and the surfaces the platform retired are removed. (The two snapshots this
release kept, `schema.management.gql` and `schema.game.gql`, were themselves
retired in 0.20.0 — see below.)

- **`client.admin().environments()` removed.** Dedicated customer
  environments no longer exist; every app runs on the shared platform, and
  infrastructure provisioning moved to the separate infra-control-plane
  service. The `EnvironmentsAPI` class, its accessor, and the
  `operations/environments/` documents are gone.
- **`client.operator_()` reduced to the platform compute ceilings**
  (`computePlatformCeilings` / `setComputePlatformCeilings`). Environments,
  change orders, secrets, releases, audit, and operator-user listing moved to
  the infra-control-plane service.
- **`client.admin().usage()`**: the per-environment rollups
  (`environmentSummary` / `environmentByApp` / `orgByEnvironment`) are gone;
  `appSummary`, `appGraphqlOperations`, `playerPulse`, and the org/app
  projections stay.
- **`client.admin().billing()`**: the per-environment capacity tier catalogs
  (`buddyBillingTiers` / `graphqlBillingTiers` / `postgresBillingTiers`) are
  gone; wallets, budgets, and transactions stay.
- **Endpoints**: `managementUrl` and `httpUrl` may be the same origin;
  configuring both remains supported here, and the two-token model is
  unchanged. (0.20.0 removed `managementUrl` outright.)

Everything game-client, replication, kit, session, Crowdy Studio, agent, and
player-host is unchanged — the merged schema is a superset for those surfaces.

## 0.16.0 native Crowdy Studio integration

0.16.0 changes the public source and installed-library ABI. CrowdyCPP remains
pre-1.0: rebuild consumers and engine wrappers, and do not load 0.16 libraries
behind binaries compiled against 0.15 headers.

- Prefer `CrowdyClient::createCrowdyStudioIntegration(options)` for an
  engine-owned Studio. The returned non-copyable `CrowdyStudioIntegration`
  owns the project/runtime adapters, controller, editor bridge, concrete Studio
  host, native dispatcher, optional Agent runtime, layout controller, lease
  manager, control gate, and wallet adapter in destruction-safe order.
- Implement `ICrowdyStudioEditorAdapter` over in-memory buffers. Its callbacks
  intentionally provide no `CrowdyClient`, filesystem, shell, DOM, transport,
  or raw GraphQL authority. Existing direct-controller integrations can remain,
  but must own every borrowed provider for the controller's full lifetime.
- Persist pane state through `ICrowdyStudioLayoutStorage`, or retain
  `InMemoryCrowdyStudioLayoutStorage` for session-only state. CrowdyCPP does not
  choose a filesystem, registry, browser local storage, or process-global
  settings service. `crowdy/studio/layout.hpp` remains available in the reduced
  no-exception install.
- Replace opaque diagnostic views with `CrowdyStudioDiagnostic`. Diagnostics
  now carry target-relative path/range, severity, source, message, and optional
  rustc code. The string setter and `*DiagnosticTexts()` accessors remain
  compatibility helpers.
- Wallet state is optional read-only observation. The client integration
  installs `CrowdyStudioPlayerWalletProvider` by default; set
  `observePlayerWallet = false` or inject `walletProvider` to override it.
  Wallet failures clear the snapshot and never block editing, saving, testing,
  or deployment.
- `CrowdyStudioControllerHostAdapter` is the closed 11-tool Studio host.
  Potentially effectful calls validate session, epoch, context, lease,
  cancellation, and approval at the final boundary. Do not replace it with a
  generic host call, raw operation executor, or transport callback.
- Supply `playerHost` to let the integration own one exact
  `AgentControlLeaseManager` shared by native dispatch and
  `NativePlayerControlGate`. Forward existing keyboard, pointer, movement,
  background, death, permission, admission, context, and controlled-entity
  events through the gate. It observes takeover intent; it does not synthesize
  or consume engine input.
- `CrowdyStudioIntegration::poll()` (and compatibility spelling `tick()`) is
  nonblocking. It drains platform/Agent callbacks and native deadlines only.
  Autosave, monitoring HTTP, compile polling/sleep, and scheduled Studio host
  effects run from `runStudioMaintenance()`. Serialize that potentially
  blocking lane with all controller/editor access; never call it concurrently.
- DOM, Monaco, CSS, splitters, browser workers/VFS, renderer chrome, and
  browser input plumbing remain intentional browser exclusions. Engines own
  equivalent presentation and sandbox implementations; 0.16 adds no hidden
  DOM/filesystem authority.

Approved checkpoint restore is not implied by project-save access. It remains
available only when an independently authorized
`ICrowdyStudioSynchronizationProvider` and exact
`ICrowdyStudioApprovalGate` are both injected. The published GraphQL schema
does not expose a generic synchronization or restore root.

## 0.15.1 installed-package compatibility patch

0.15.1 changes no runtime API. It updates external-consumer verification to
request the current `0.15` CMake compatibility line.

## 0.15.0 app-scoped player counts

0.15.0 is additive. Native clients can query
`gameModel().activePlayerCount(appId)` and subscribe with
`activePlayerCountChanged(appId, callbacks)`. Only `FRESH` snapshots are
complete; deduplicate transition events by their decimal-string revision and
requery after reconnects or gaps. These methods require the matching
2026-07-24 Game API generation.

## 0.14.1 Windows replication fix

0.14.1 is a drop-in patch for 0.14.0. Winsock UDP connections are now
configured nonblocking, so `Connection::pump(0)` and manual-pump clients return
immediately when no datagram is available instead of blocking the caller.

## 0.14.0 strict portable-parity release

0.14.0 is not purely additive. It closes every portable gap against the pinned
CrowdyJS 12.0.0 target and adds typed wrappers for current platform SDL
extensions, but it also changes `SaveStateStore::patch` persistence timing and
introduces non-copyable owning runtime types.

- Prefer `client.createCrowdyStudioAgentController(options)` for production
  agent construction. It owns `CrowdyStudioAgentGraphQLTransport`, the
  GraphQL-WS event adapter, and the controller in a safe lifetime order.
- Replace custom local-tool glue with
  `NativeBrowserToolDispatcherAdapter`. It maps controller invocations to
  `NativeToolDispatcherV1`, pumps native deadlines from `controller.poll()`,
  and propagates cancellation, context, timing, output, and typed errors.
- `AgentControlLeaseManager::revoke(reason)` is an exact alias of the
  synchronous intent-first `preempt(reason)` path.
- Use `gameModel().containerChanged(...)` instead of a raw subscription for
  the typed metadata feed. The returned `SubscriptionHandle` cancels on
  destruction.
- `gameModel().ensureContainer(input)` and the `bindingKey` list filter require
  Game API 2026-07-24 or newer.
  `marketplace().appListingVersions(vars)` requires Management API 2026-07-24
  or newer. Both operations are validated against their exact published
  endpoint SDL, not the merged schema snapshot.
- Native CLIENT runtimes can use `playerCompute().artifactBytes(...)` and
  `marketplace().clientArtifactBytes(...)` for validated base64 decoding plus
  artifact hash, decimal-string fuel, nullable contract, and version metadata.
- `SaveStateStore::patch` no longer performs a network write. It updates the
  local cache and marks it dirty; code that relied on the old immediate
  persistence must call `save()` explicitly. The new `set` method has the same
  local-only behavior. `dirty()` and `lastSavedAt()` expose the save lifecycle,
  and persistence failures now occur at `save()`, not at `patch()`.
- Store revision, queue, error, local-actor, and private-avatar observability
  are real bounded snapshots/counters; lifetime totals are not reset by
  clearing retained rings.
- `AppTokenResponse::gameTokenId` and `AuthResponse::gameTokenId` are decimal
  strings. Use the checked `gameTokenIdInt64()` helper only at the native UDP
  wire boundary. Token route URLs and nullable profile fields now use
  `NullableString`, preserving GraphQL null separately from `""`.
- Native replication startup requires complete `Config::token` metadata.
  Prefer `ReplicationClient::connectWithStatus()` so assignment/socket
  failures are not discarded; the old `connect()` convenience remains.
- `ClientConfig::crypto` is the injection point for non-OpenSSL builds.
  Direct `Connection` construction still requires an `ICrypto`; with OpenSSL
  disabled, omitted crypto fails with `CryptoUnavailable` rather than leaving
  an unresolved symbol.
- `CROWDY_NO_EXCEPTIONS=ON` is a reduced package profile. It does not install
  Compute authoring, Crowdy Studio project API/models/controller,
  Agent/controller, player-host, Game Kit, or `ContainerMirror` headers; use
  the normal package for those layers.

### Runtime ownership and copyability

The new runtime objects own callbacks, protocol state, or host authority and
must not be copied:

- `graphql::SubscriptionHandle`, `graphql::GraphQLSubscriptionClient`,
  `player_host::AgentControlLeaseManager`, and
  `agent::NativeToolDispatcherV1` are move-only.
- `agent::CrowdyStudioAgentController` and
  `agent::CrowdyStudioAgentControllerRuntime` are non-copyable and
  non-movable. Keep the factory result in its returned `std::unique_ptr`
  rather than storing either object in a container that relocates values.

No provider API key or provider client was added. Agent providers remain a
server-side platform concern. See
[`docs/compatibility.md`](docs/compatibility.md) for server requirements.

## Crowdy Studio portable parity

The Crowdy Studio API surface adds
`client.crowdyStudio()` for the current CrowdyJS v12 project, personal-library,
and common-file GraphQL surface, and
`crowdy::studio::CrowdyStudioController` for the portable headless state
machine.

Key integration points:

- Keep GraphQL `BigInt` ids, revisions, sizes, and fuel values as decimal
  strings.
- Use `CrowdyStudioPatchField<T>::omitted()`, `::null()`, or `::value(...)`
  when updating nullable metadata. `std::optional` alone cannot preserve this
  wire distinction.
- Call `CrowdyClient::poll()` to deliver `*Async` API callbacks on the
  configured dispatcher/game thread.
- Call `CrowdyStudioController::tick()` from the engine loop for autosave,
  offline retry, and visible monitoring refreshes.
- Consume `CrowdyStudioDiagnostic` from the authoritative/local state vectors.
  Existing string producers can keep using the
  `setLocalDiagnostics(std::vector<std::string>)` overload; existing text-only
  views can use
  `localDiagnosticTexts()` or `authoritativeDiagnosticTexts()`.
- Inject `ICrowdyStudioSynchronizationProvider` only when the host has an
  independently authorized durable atomic-patch/checkpoint service,
  `ICrowdyStudioRuntime` (or
  `CrowdyStudioPlayerComputeRuntime`) for playerCompute execution, and
  `ICrowdyStudioApprovalGate` for exact live/restore approval.
- Optionally inject `ICrowdyStudioWalletProvider` as the last controller
  argument. `CrowdyStudioPlayerWalletProvider` adapts only the viewer-scoped
  `PlayerWalletAPI::balance()` read; failures leave authoring operational.
- Live deployment no longer has an unapproved convenience overload. Pass the
  exact plan from `makeDeploymentPlan()` plus an opaque grant issued and
  checked by the agent layer.

There are no generic checkpoint-list, atomic-patch, or approved-restore roots
in the published GraphQL SDL. Without an injected bridge those calls now fail
with `CrowdyStudioCapabilityUnavailableError` instead of suggesting that
ordinary project-save GraphQL authority is sufficient. Durable
`AgentCheckpoint` event metadata can be mapped with
`crowdyStudioCheckpointEventFromAgentV1()` and scope-checked by
`ingestCheckpointEvent()`; restore still requires a separate exact approval
grant and bridge.

The native phase intentionally excludes DOM, Monaco, CSS, browser Rust workers,
and engine rendering. It also does not add a generic GraphQL executor or any
owner/grid/source authority override. Servers must expose the current
`crowdyStudio*` Game API roots; older deployments reject only these new
operations.
