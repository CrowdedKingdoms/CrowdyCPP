#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "crowdy/graphql/auth_state.hpp"
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

class GraphQLClient {
 public:
  GraphQLClient(GraphQLClientConfig config, std::shared_ptr<IHttpTransport> transport,
                std::shared_ptr<AuthState> auth)
      : config_(std::move(config)), transport_(std::move(transport)), auth_(std::move(auth)) {}

  /// Execute an operation. Returns the `data` value.
  /// Throws CrowdyHttpError / CrowdyGraphQLError / CrowdyNetworkError /
  /// CrowdyTimeoutError / CrowdyProtocolError.
  Json request(std::string_view document, const JVal& variables = JVal(),
               std::string_view operationName = {});

  const std::string& endpoint() const { return config_.endpoint; }
  AuthState& auth() { return *auth_; }

 private:
  GraphQLClientConfig config_;
  std::shared_ptr<IHttpTransport> transport_;
  std::shared_ptr<AuthState> auth_;
};

}  // namespace crowdy::graphql
