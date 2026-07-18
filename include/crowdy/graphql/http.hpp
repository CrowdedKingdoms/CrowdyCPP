#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

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

}  // namespace crowdy::graphql
