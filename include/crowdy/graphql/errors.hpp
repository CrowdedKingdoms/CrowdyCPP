#pragma once

#include <stdexcept>
#include <string>
#include <vector>

/// Structured error classes for the GraphQL layer, mirroring CrowdyJS
/// (https://github.com/CrowdedKingdoms/CrowdyJS): branch on `code()` /
/// `extensions.code`, never on message text.
///
/// The replication (UDP) layer never throws — see crowdy/core/result.hpp.
namespace crowdy::graphql {

/// Base class for every SDK error.
class CrowdyError : public std::runtime_error {
 public:
  CrowdyError(std::string code, const std::string& message)
      : std::runtime_error(message), code_(std::move(code)) {}
  /// Stable machine-readable code (e.g. "UNAUTHENTICATED", "HTTP_ERROR").
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

/// Non-2xx HTTP response from a GraphQL endpoint.
class CrowdyHttpError : public CrowdyError {
 public:
  CrowdyHttpError(int status, std::string body)
      : CrowdyError("HTTP_ERROR", "GraphQL endpoint returned HTTP " + std::to_string(status)),
        status_(status),
        body_(std::move(body)) {}
  int status() const { return status_; }
  const std::string& body() const { return body_; }

 private:
  int status_;
  std::string body_;
};

/// One entry of a GraphQL `errors` array.
struct GraphQLErrorDetail {
  std::string message;
  std::string code;         ///< extensions.code (stable, enumerated)
  std::string remediation;  ///< extensions.remediation when provided
  std::string path;         ///< dotted path, e.g. "mintAppToken"
};

/// The server returned GraphQL errors. Preserves every error including
/// extensions.
class CrowdyGraphQLError : public CrowdyError {
 public:
  explicit CrowdyGraphQLError(std::vector<GraphQLErrorDetail> errors)
      : CrowdyError(errors.empty() ? "GRAPHQL_ERROR" : firstCode(errors),
                    errors.empty() ? "GraphQL error" : errors.front().message),
        errors_(std::move(errors)) {}
  const std::vector<GraphQLErrorDetail>& errors() const { return errors_; }

 private:
  static std::string firstCode(const std::vector<GraphQLErrorDetail>& errors) {
    return errors.front().code.empty() ? "GRAPHQL_ERROR" : errors.front().code;
  }
  std::vector<GraphQLErrorDetail> errors_;
};

/// Network-level failure (DNS, TLS, connection refused).
class CrowdyNetworkError : public CrowdyError {
 public:
  explicit CrowdyNetworkError(const std::string& message)
      : CrowdyError("NETWORK_ERROR", message) {}
};

/// Request timed out.
class CrowdyTimeoutError : public CrowdyError {
 public:
  explicit CrowdyTimeoutError(const std::string& message)
      : CrowdyError("TIMEOUT", message) {}
};

/// Server response failed validation (not JSON, missing data, wrong shape).
class CrowdyProtocolError : public CrowdyError {
 public:
  explicit CrowdyProtocolError(const std::string& message)
      : CrowdyError("PROTOCOL_ERROR", message) {}
};

}  // namespace crowdy::graphql
