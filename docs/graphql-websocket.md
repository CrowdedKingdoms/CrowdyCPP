# GraphQL WebSocket examples

CrowdyCPP implements `graphql-transport-ws`. The client authenticates in
`connection_init`, replays active operations after bounded reconnects, and
delivers every callback through the shared `Dispatcher`.

## Generic operations

`client.subscriptions()` runs any GraphQL subscription. The published Game API
schema's only subscription roots are the legacy game model's feeds, which go at
ck-exec P4 (CrowdyCPP 0.48.0 removed their typed wrappers); ck-exec pushes
arrive on an `ExecConnection` subscription instead.

```cpp
crowdy::graphql::GraphQLSubscriptionCallbacks callbacks;
callbacks.onNext = [](crowdy::graphql::GraphQLSubscriptionOutcome outcome) {
  if (outcome.ok()) consume(outcome.data);
};

auto handle = game.subscriptions().subscribe(
    "subscription Watch($appId: BigInt!) { customFeed(appId: $appId) { id } }",
    crowdy::graphql::JVal::object({{"appId", appId}}),
    "Watch", std::move(callbacks));
```

Direct `GraphQLSubscriptionClient` construction treats its URL as an API base
by default. Set `endpointKind` to
`GraphQLWebSocketEndpointKind::Complete` for a custom route such as
`wss://host/subscriptions/`; its path, query, and trailing slash are preserved.
`CrowdyClient::wsEndpoint` selects this complete-endpoint behavior
automatically.

Engine integrations may inject `IWebSocketTransport`; libcurl 8.13+ WebSockets
are optional. Older curl versions are deliberately not enabled because their
fragmented-message metadata cannot satisfy this backend's reassembly contract.
A curl-free build retains the same API and reports a typed transport
unavailable error until an engine transport is supplied.
