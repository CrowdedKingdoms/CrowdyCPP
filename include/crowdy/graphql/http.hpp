#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/core/result.hpp"

/// Pluggable HTTP transport. The default implementation (curlTransport())
/// wraps libcurl; engine wrappers substitute their own HTTP stack (e.g. an
/// engine HTTP module) by implementing IHttpTransport.
///
/// Transports must be safe to call from multiple threads.
namespace crowdy::graphql {

struct HttpRequest {
  std::string url;
  std::string method = "POST";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  long timeoutMs = 60000;
};

struct HttpResponse {
  int status = 0;
  std::string body;
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  /// Throws CrowdyNetworkError / CrowdyTimeoutError on transport failure.
  /// Non-2xx statuses are returned, not thrown (the GraphQL client decides).
  virtual HttpResponse send(const HttpRequest& request) = 0;
};

/// Default libcurl transport. Maintains a per-instance cookie jar so sticky
/// load-balancer cookies persist across requests. Only available when built
/// with CROWDY_WITH_CURL (the default).
std::shared_ptr<IHttpTransport> makeCurlTransport();

/// Result of an async HTTP round trip. status.ok() means the request completed
/// and `response` holds whatever the server returned (any HTTP status code);
/// a non-ok status is a transport-level failure (DNS, TLS, connection, timeout)
/// with detail in `errorMessage`.
struct HttpOutcome {
  Status status;
  HttpResponse response;
  std::string errorMessage;
};

/// Async, non-blocking, non-throwing HTTP transport. This is the interface a
/// game engine implements over its own async HTTP stack (Unreal FHttpModule,
/// Unity UnityWebRequest, Godot HTTPRequest) so API calls never block a frame
/// and never throw. sendAsync starts the request and invokes `cb` exactly once
/// when it finishes; the callback may run on any thread, so route it through a
/// Dispatcher to land on the game thread.
class IAsyncHttpTransport {
 public:
  virtual ~IAsyncHttpTransport() = default;
  virtual void sendAsync(const HttpRequest& request,
                         std::function<void(HttpOutcome)> cb) = 0;
};

/// Adapts a synchronous IHttpTransport to the async interface by running the
/// blocking send inline and invoking the callback before returning. Useful as
/// a fallback and for tests; a real engine transport avoids the block.
std::shared_ptr<IAsyncHttpTransport> makeInlineAsyncTransport(
    std::shared_ptr<IHttpTransport> sync);

}  // namespace crowdy::graphql
