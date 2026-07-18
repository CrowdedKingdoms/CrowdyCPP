#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.users() — profile reads + account admin. Targets the Management
/// API with the identity session token (admin methods additionally require
/// the relevant platform flag; the server enforces them).
namespace crowdy::domains {

class UsersAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// The signed-in user's profile.
  graphql::Json me() const { return execUnwrap(gen::users::kMeDocument); }

  graphql::Json updateGamertag(std::string_view gamertag) const {
    graphql::JVal vars;
    vars["input"]["gamertag"] = gamertag;
    return execUnwrap(gen::users::kUpdateGamertagDocument, vars);
  }

  bool deleteMyAccount() const {
    return execUnwrap(gen::users::kDeleteMyAccountDocument).asBool();
  }

  graphql::Json freePlayWindow() const {
    return execUnwrap(gen::users::kFreePlayWindowDocument);
  }

  graphql::Json get(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return execUnwrap(gen::users::kUserDocument, vars);
  }

  graphql::Json paginated(std::string_view query = {}, int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    if (!query.empty()) vars["query"] = query;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::users::kUsersPaginatedDocument, vars, "UsersPaginated");
  }

  /// Relay cursor pagination variant.
  graphql::Json listConnection(int first = 50, std::string_view after = {},
                               std::string_view query = {}) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!query.empty()) vars["query"] = query;
    return execUnwrap(gen::users::kUsersPaginatedDocument, vars, "UsersConnection");
  }

  graphql::Json setSuperAdmin(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetSuperAdminDocument, vars);
  }

  graphql::Json setOperator(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetOperatorDocument, vars);
  }

  graphql::Json setEarlyAccessOverride(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetEarlyAccessOverrideDocument, vars);
  }

  graphql::Json updateType(std::string_view userId, std::string_view value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kUpdateUserTypeDocument, vars);
  }

  graphql::Json forceLogout(std::string_view userId) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    return execUnwrap(gen::users::kForceLogoutUserDocument, vars);
  }

  graphql::Json updateState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::users::kUpdateUserStateDocument, vars);
  }
};

}  // namespace crowdy::domains
