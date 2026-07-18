#pragma once

#include <memory>
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

  std::shared_ptr<graphql::GraphQLClient> gql_;
};

}  // namespace crowdy::domains
