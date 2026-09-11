# Agentic Crowdy Studio from a native client

Since 0.34.0 the Studio agent is the **in-browser DeepSeek Harness** that
CrowdyJS 16 docks beside the Crowdy Studio editor (`dsh` mount option). It
edits the player's project, runs draft tests and takes screenshots from the
page, and spends model tokens through the metered REST endpoint
`POST /v1/model/chat/completions` with the player's own app token. There is no
native counterpart: a C++ engine has no Studio pane to dock it in, and the
GraphQL orchestrator this SDK used to drive (sessions, runs, leases, tool
approvals, `crowdyStudioAgentEvents`) is gone from the API.

What a native client still does is read and administer the policy around that
agent. All of it is on `client.crowdyStudioAgent()`
(`crowdy::domains::CrowdyStudioAgentAPI`), exact generated documents on the
one API origin, gated by permission.

## Player-facing (app token, `use_studio_agent`)

| Method | Root field | Purpose |
|---|---|---|
| `effectivePolicy(appId)` | `crowdyStudioAgentEffectivePolicy` | Is the agent enabled for this app, which models and modes, spend ceilings, who pays |
| `providerConsent(appId)` | `crowdyStudioProviderConsent` | Whether this player accepted that their project source may reach a model provider |
| `setProviderConsent({appId, consented})` | `crowdyStudioSetProviderConsent` | Record or revoke that consent; the model endpoint refuses `AGENT_SCOPE_DENIED` until it is recorded |
| `modelUsage(appId, limit?)` | `crowdyStudioModelUsage` | Today's request count and rate-carded charge against the ceiling, the payer (`PLAYER` wallet by default, `ORG`, or `PLATFORM`), and recent requests. Counts and charges only |

## App owner (identity session, `manage_apps`)

| Method | Root field |
|---|---|
| `policy(appId)` | `crowdyStudioAgentPolicy` |
| `setPolicy(input)` | `setCrowdyStudioAgentPolicy` (funding `payerKind` needs `manage_billing`) |
| `usage(appId, window?)` | `crowdyStudioAgentUsage` |

## Operator

`platformPolicy()`, `setPlatformPolicy(input)`, `setOperatorAppKill(input)`
and the catalog (`cpCrowdyStudioAgentCatalog`) are unchanged.

## The REST endpoint itself

A native client that wants to call a model on the player's behalf can use the
same endpoint the browser agent does; it is plain OpenAI chat completions:

```
GET  {apiOrigin}/v1/model/models                Authorization: Bearer <app token>
POST {apiOrigin}/v1/model/chat/completions      Authorization: Bearer <app token>
```

Requests are narrowed to the accepted OpenAI fields (unknown, `dsh_*` and
`x_*` fields are stripped), forwarded with zero-data-retention routing, and
settled per request from the provider's usage frame; a stream the client
abandons is still charged (drained to its usage frame or estimated). Errors
come back as OpenAI-style bodies carrying the platform code
(`AGENT_MODEL_NOT_ALLOWED`, `AGENT_BUDGET_EXHAUSTED`, `AGENT_FUNDS_NEEDED`,
`AGENT_SCOPE_DENIED`, ...). This SDK does not wrap that endpoint; use any
HTTP client.

## What was removed in 0.34.0

`crowdy/agent/*` (controller, transport, registry, native tool dispatchers,
schema), `crowdy/studio/host_adapter.hpp`, `crowdy/studio/agent_projection.hpp`,
`crowdy/player_host/control_gate.hpp`, `crowdy/player_host/lease_manager.hpp`,
`CrowdyClient::createCrowdyStudioAgentController`, the `agent`, `nativeTools`,
`studioHost`, `controlLeases`, `controlGate` and `leaseManager` fields of
`CrowdyStudioIntegrationOptions`, and the 21 session/run/lease/tool operations
on `CrowdyStudioAgentAPI`. See [MIGRATION.md](../MIGRATION.md).

`crowdy/player_host/` keeps the observation contract (`PlayerHostAdapterV1`,
typed observations, schemas, the preemption vocabulary) for games that expose
their world to tooling; nothing in this SDK dispatches commands to it any more.
