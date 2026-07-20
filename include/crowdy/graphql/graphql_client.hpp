#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/core/result.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"

/// GraphQL-over-HTTP client. One instance per endpoint (Management API or
/// Game API); both share a CrowdyClient's AuthState so HTTP auth never
/// drifts.
namespace crowdy::graphql {

struct GraphQLClientConfig {
  std::string endpoint;  ///< full URL of the /graphql endpoint
  long timeoutMs = 60000;
};

/// Which failure a GraphQLOutcome represents (None when it succeeded). The
/// blocking request() maps each of these onto the matching CrowdyError; the
/// async path leaves them as data so callers branch on the code.
enum class GraphQLErrorKind { None, Http, GraphQL, Protocol, Network, Timeout };

/// Non-throwing result of an operation, the async twin of request(). ok() means
/// the server returned data with no GraphQL errors; otherwise `kind` and the
/// matching fields describe the failure.
struct GraphQLOutcome {
  Status status;                            ///< Ok, or an Errc describing the failure
  GraphQLErrorKind kind = GraphQLErrorKind::None;
  Json data;                                ///< the `data` value when ok()
  std::vector<GraphQLErrorDetail> errors;   ///< server GraphQL errors (kind == GraphQL)
  int httpStatus = 0;                       ///< HTTP status code of the response
  std::string errorMessage;                 ///< protocol/network/timeout detail
  std::string body;                         ///< raw response body (kind == Http)

  bool ok() const { return status.ok() && kind == GraphQLErrorKind::None; }
};

using GraphQLCallback = std::function<void(GraphQLOutcome)>;

class GraphQLClient {
 public:
  GraphQLClient(GraphQLClientConfig config, std::shared_ptr<IHttpTransport> transport,
                std::shared_ptr<AuthState> auth)
      : config_(std::move(config)), transport_(std::move(transport)), auth_(std::move(auth)) {}

#ifndef CROWDY_NO_EXCEPTIONS
  /// Execute an operation and return the `data` value. Blocking.
  /// Throws CrowdyHttpError / CrowdyGraphQLError / CrowdyNetworkError /
  /// CrowdyTimeoutError / CrowdyProtocolError. Prefer requestAsync in engines.
  Json request(std::string_view document, const JVal& variables = JVal(),
               std::string_view operationName = {});
#endif

  /// Execute an operation without blocking or throwing. `cb` is invoked once
  /// with the outcome. When an async transport is set the request runs on it;
  /// otherwise it falls back to the synchronous transport inline. When a
  /// Dispatcher is set the callback is delivered from Dispatcher::drain(),
  /// otherwise inline on whatever thread the transport completed on.
  void requestAsync(std::string_view document, const JVal& variables,
                    std::string_view operationName, GraphQLCallback cb);

  /// Inject the engine's async HTTP transport (FHttpModule, etc.).
  void setAsyncTransport(std::shared_ptr<IAsyncHttpTransport> transport) {
    asyncTransport_ = std::move(transport);
  }
  /// Route async callbacks through this dispatcher so they fire on drain().
  void setDispatcher(std::shared_ptr<Dispatcher> dispatcher) {
    dispatcher_ = std::move(dispatcher);
  }

  const std::string& endpoint() const { return config_.endpoint; }
  AuthState& auth() { return *auth_; }

 private:
  HttpRequest buildHttpRequest(std::string_view document, const JVal& variables,
                               std::string_view operationName) const;
  HttpOutcome sendInline(const HttpRequest& request);

  GraphQLClientConfig config_;
  std::shared_ptr<IHttpTransport> transport_;
  std::shared_ptr<AuthState> auth_;
  std::shared_ptr<IAsyncHttpTransport> asyncTransport_;
  std::shared_ptr<Dispatcher> dispatcher_;
};

}  // namespace crowdy::graphql
