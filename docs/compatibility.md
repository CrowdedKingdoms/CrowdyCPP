# SDK and Game API compatibility

CrowdyCPP 0.14.0 is the strict portable/native counterpart to CrowdyJS 12.0.0.
The parity gate pins CrowdyJS commit
`a9c620c021c83c39f630dca4bb3e46b76691ac2d`; see
[`parity-matrix.md`](parity-matrix.md) for the generated method-by-method
evidence.

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

Older servers reject only operations they do not know. A client can continue
using older surfaces by not calling the newer methods. There is no provider
key or provider client in CrowdyCPP: Agentic Studio provider selection and
credentials remain server-side.

The only intentional parity waivers are generated in the parity matrix:
native equivalents for browser UDP/runtime behavior and browser exclusions
for inherently browser-owned PKCE persistence or byte-buffer conveniences.
Portable gaps, unclassified differences, and stale classifications are zero.
