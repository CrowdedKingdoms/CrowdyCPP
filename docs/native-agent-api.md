# Agentic Crowdy Studio from a native client

Since 0.34.0 the Studio agent is the **in-browser DeepSeek Harness**. CrowdyJS 16
docks it beside the Crowdy Studio editor (`dsh` mount option), and a game can host
the same harness in a Studio dock of its own: the Unreal SDK does, inside its games.
The agent edits the player's project, runs draft tests and takes screenshots
through its dock, and spends model tokens through the metered REST endpoint
`POST /v1/model/chat/completions` with the player's own app token. The GraphQL
orchestrator this SDK used to drive (sessions, runs, leases, tool approvals,
`crowdyStudioAgentEvents`) is gone from the API.

What a native client does is read and administer the policy around that agent,
and, since 0.86.0, make the platform calls a dock makes. All of it is on
`client.crowdyStudioAgent()` (`crowdy::domains::CrowdyStudioAgentAPI`), exact
generated documents and one REST route on the one API origin, gated by
permission.

## Player-facing (app token, `use_studio_agent`)

| Method | Root field or route | Purpose |
|---|---|---|
| `effectivePolicy(appId)` | `crowdyStudioAgentEffectivePolicy` | Is the agent enabled for this app, which models and modes, spend ceilings, who pays |
| `providerConsent(appId)` | `crowdyStudioProviderConsent` | Whether this player accepted that their project source may reach a model provider |
| `setProviderConsent({appId, consented})` | `crowdyStudioSetProviderConsent` | Record or revoke that consent; the model endpoint refuses `AGENT_SCOPE_DENIED` until it is recorded |
| `modelUsage(appId, limit?)` | `crowdyStudioModelUsage` | Today's request count and rate-carded charge against the ceiling, the payer (`PLAYER` wallet by default, `ORG`, or `PLATFORM`), and recent requests. Counts and charges only |
| `liveModding(appId, action)` | `crowdyStudioLiveModding` | The app's Allow Live Modding for `draft_test` or `deploy_live` (0.86.0). The mod build, deploy and switch-on calls do not check it, so a dock asks it before either action |
| `models(appId)` | `GET /v1/model/models?appId=` | The models the player may use in this app, with their rate cards (0.86.0) |

`liveModding` answers true, or a GraphQL error whose `code` says why:
`AGENT_SCOPE_DENIED` (an action other than the two, or a token for another app),
`AGENT_PERMISSION_DENIED` (no `use_studio_agent`, or the app's runtime is not
active), `AGENT_DISABLED` (the policy is off or the app's kill switch is on),
`AGENT_OPERATOR_KILLED`, or `AGENT_PROVIDER_POLICY_UNSATISFIED` (503).

`models` returns a `CrowdyStudioModelList`: per model its `id`, `name` (which may
equal the id), `contextWindow` (unset when the platform sends null),
`inputModalities` and `pricingMicrousdPerMillion` (`input`, `output`,
`reasoning`, `cachedInput`). On a refusal it carries the HTTP status and the
OpenAI-style body's `code`, `message`, `type` (`crowded_kingdoms_error`) and
`retryable`. The list refuses only with the policy's 403s (`AGENT_SCOPE_DENIED`,
`AGENT_PERMISSION_DENIED`, `AGENT_DISABLED`, `AGENT_OPERATOR_KILLED`) and
`AGENT_PROVIDER_POLICY_UNSATISFIED` (503). The request goes to the origin of the
GraphQL endpoint in use (after any datacenter move), with the bearer the client
holds when it is sent, on the client's own transports, timeout and response limit.
`crowdyStudioModelListFrom(HttpOutcome)` reads an answer your own stack fetched.

## App owner (identity session, `manage_apps`)

| Method | Root field |
|---|---|
| `policy(appId)` | `crowdyStudioAgentPolicy` |
| `setPolicy(input)` | `setCrowdyStudioAgentPolicy` (funding `payerKind` needs `manage_billing`) |
| `usage(appId, window?)` | `crowdyStudioAgentUsage` |

## Operator

Not wrapped since 0.51.0: the platform policy, the per-app operator kill and the
catalog (`cpCrowdyStudioAgentPlatformPolicy`, `cpSetCrowdyStudioAgentPlatformPolicy`,
`cpSetCrowdyStudioAgentAppKill`, `cpCrowdyStudioAgentCatalog`) are operator-only, and the
SDK wraps nothing for platform roles. Platform tooling calls them directly.

## What else a dock needs

- **The draft test and live deploy**, without blocking the game thread:
  `CrowdyStudioModRunner` (`crowdy/studio/mod_runner.hpp`,
  `client.createCrowdyStudioModRunner()`). Set `checkLiveModding` to have the run
  ask `liveModding` first.
- **A new project with both halves**: `crowdy/studio/starter_projects.hpp`
  (CrowdyJS's starters, and `createCrowdyStudioStarterProjectAsync`).
- **The tier's Studio**, the only origin a dock opens a popup to, and its wallet
  page for `AGENT_FUNDS_NEEDED`: `client.studioOrigin()` and
  `crowdy::studioWalletUrl()` (`crowdy/studio_origin.hpp`).

## The chat endpoint

A native client that calls a model on the player's behalf uses the same endpoint
the harness does; it is plain OpenAI chat completions:

```
POST {apiOrigin}/v1/model/chat/completions      Authorization: Bearer <app token>
```

Requests are narrowed to the accepted OpenAI fields (unknown, `dsh_*` and
`x_*` fields are stripped), forwarded with zero-data-retention routing, and
settled per request from the provider's usage frame; a stream the client
abandons is still charged (drained to its usage frame or estimated). Errors
come back as OpenAI-style bodies carrying the platform code
(`AGENT_MODEL_NOT_ALLOWED` 400, `AGENT_FUNDS_NEEDED` 402, `AGENT_BUDGET_EXHAUSTED`
429, `AGENT_PROVIDER_UNAVAILABLE` 503, ...). This SDK does not wrap it: the
harness is what streams it.

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
