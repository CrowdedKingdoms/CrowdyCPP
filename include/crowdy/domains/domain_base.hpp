#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/graphql/graphql_client.hpp"

namespace crowdy::domains {

/// Base for domain sub-clients: holds the GraphQL client for the endpoint the
/// domain targets (management or game) and provides the exec helper.
class DomainBase {
 public:
  explicit DomainBase(std::shared_ptr<graphql::GraphQLClient> gql) : gql_(std::move(gql)) {}

 protected:
  /// Execute `document` and return data[rootField].
  graphql::Json exec(std::string_view document, std::string_view rootField,
                     const graphql::JVal& variables = graphql::JVal(),
                     std::string_view operationName = {}) const {
    graphql::Json data = gql_->request(document, variables, operationName);
    return rootField.empty() ? data : data[rootField];
  }

  /// Execute and unwrap: our operations select exactly one root field, so
  /// return data's single member (or data itself for multi-field documents).
  graphql::Json execUnwrap(std::string_view document,
                           const graphql::JVal& variables = graphql::JVal(),
                           std::string_view operationName = {}) const {
    graphql::Json data = gql_->request(document, variables, operationName);
    if (data.isObject() && data.size() == 1) {
      graphql::Json single;
      data.forEachMember([&](std::string_view, graphql::Json v) { single = v; });
      return single;
    }
    return data;
  }

  /// Async twin of exec(): on success the outcome's data is data[rootField].
  void execAsync(std::string_view document, std::string_view rootField,
                 const graphql::JVal& variables, std::string_view operationName,
                 graphql::GraphQLCallback cb) const {
    std::string root(rootField);
    gql_->requestAsync(document, variables, operationName,
                       [root = std::move(root), cb = std::move(cb)](
                           graphql::GraphQLOutcome out) mutable {
                         if (out.ok() && !root.empty()) out.data = out.data[root];
                         cb(std::move(out));
                       });
  }

  /// Async twin of execUnwrap(): on success the outcome's data is unwrapped to
  /// the single selected root field.
  void execUnwrapAsync(std::string_view document, const graphql::JVal& variables,
                       std::string_view operationName, graphql::GraphQLCallback cb) const {
    gql_->requestAsync(document, variables, operationName,
                       [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                         if (out.ok() && out.data.isObject() && out.data.size() == 1) {
                           graphql::Json single;
                           out.data.forEachMember(
                               [&](std::string_view, graphql::Json v) { single = v; });
                           out.data = single;
                         }
                         cb(std::move(out));
                       });
  }

  std::shared_ptr<graphql::GraphQLClient> gql_;
};

}  // namespace crowdy::domains
