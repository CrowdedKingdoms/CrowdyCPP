# SDK and Game API compatibility

CrowdyCPP 0.14.0 passes the strict portable-parity gate against CrowdyJS
12.0.0. The gate pins CrowdyJS commit
`a4624c9193cb943b8a922ecea5013a9e48dcc2fb`; see
[`parity-matrix.md`](parity-matrix.md) for the generated method-by-method
evidence. Native equivalents and browser exclusions remain intentional, so
this does not claim identical transports or browser behavior.

| Surface | CrowdyCPP 0.14.0 | CrowdyJS 12.0.0 | Required public API generation |
|---|---|---|---|
| Core Management and Game GraphQL | Supported | Supported | Current published Management + Game SDL |
| Native UDP replication | Direct native transport | Browser GraphQL UDP proxy | Current Replication API |
| Generic GraphQL WebSocket | `GraphQLSubscriptionClient` | `graphql-ws` | `graphql-transport-ws` endpoint |
| Game-model container feed | Typed `gameModel().containerChanged` | Typed `containerChanged` | Game API 2026-07+ |
| Keyed container ensure/filter | `ensureContainer`, `bindingKey` filter | Not present in pinned v12 | Game API 2026-07-24+ |
| App listing-version administration | `marketplace().appListingVersions` | Not present in pinned v12 | Management API 2026-07-24+ |
| Crowdy Studio projects/runtime | Headless native controller | Browser/headless controller | Game API Crowdy Studio roots |
| Agentic Studio HTTP + event stream | Typed controller, GraphQL-WS replay/gap-fill | Typed controller and transport | `crowdy.studio-agent/1` |
| Local Play/Studio tools | Native player-host + closed typed dispatcher | Browser dispatcher + player host | Agent descriptor contract v1 |

`schema.management.gql` and `schema.game.gql` are exact committed snapshots of
the published endpoint SDLs. Codegen isolates every operation with only its
transitive fragments and validates it independently against those endpoint
schemas; the merged `schema.gql` is never accepted as proof that an operation
can be sent to either plane.

Older servers reject only operations they do not know. A client can continue
using older surfaces by not calling the newer methods. There is no provider
key or provider client in CrowdyCPP: Agentic Studio provider selection and
credentials remain server-side.

The only intentional parity waivers are generated in the parity matrix:
native equivalents for browser UDP/runtime behavior and browser exclusions
for inherently browser-owned PKCE persistence and DOM/UI behavior. CLIENT
artifact-byte decoding is portable and is available through
`playerCompute().artifactBytes(...)` and
`marketplace().clientArtifactBytes(...)`.
Portable gaps, unclassified differences, and stale classifications are zero.

## CrowdyCPP 0.x source and ABI policy

Until 1.0, each minor release may contain source-incompatible or ABI-incompatible
changes. Patch releases within one minor line preserve the public source and
installed-library ABI. Consumers should review `MIGRATION.md` and rebuild when
moving between minors.

The installed `CrowdyCPPConfigVersion.cmake` uses CMake's
`SameMinorVersion` policy. A request for `0.14` may select a compatible newer
`0.14.x` package, but no `0.15.x` package is accepted as compatible merely
because both versions have major version `0`.
