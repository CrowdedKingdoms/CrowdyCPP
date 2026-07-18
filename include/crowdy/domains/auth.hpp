#pragma once

#include <string>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// Authentication and account lifecycle — client.auth(). Passwordless:
/// magic link, social/OIDC, or (dev only) the dev bypass. Every path returns
/// an identity SESSION token (management-plane), stored on the shared auth
/// state automatically. Gameplay tokens are minted separately via
/// client.portal(). Targets the Management API.
namespace crowdy::domains {

class AuthAPI : public DomainBase {
 public:
  AuthAPI(std::shared_ptr<graphql::GraphQLClient> gql, std::shared_ptr<graphql::AuthState> auth)
      : DomainBase(std::move(gql)), auth_(std::move(auth)) {}

  /// The federated sign-in providers currently enabled (e.g. ["google"]).
  std::vector<std::string> availableLoginProviders() const {
    graphql::Json r = execUnwrap("query AvailableLoginProviders { availableLoginProviders }");
    std::vector<std::string> out;
    r.forEach([&](graphql::Json p) { out.push_back(p.asString()); });
    return out;
  }

  /// Email a one-time magic sign-in link (creates the account on first
  /// sign-in). In development (DEV_AUTH_BYPASS) the response carries
  /// `devToken` to pass straight to completeLoginLink without an inbox.
  graphql::Json requestLoginLink(std::string_view email, std::string_view redirectUri = {}) const {
    graphql::JVal input;
    input["email"] = email;
    if (!redirectUri.empty()) input["redirectUri"] = redirectUri;
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(
        "mutation RequestLoginLink($input: RequestLoginLinkInput!) {"
        " requestLoginLink(input: $input) { sent devToken } }",
        vars);
  }

  /// Complete a magic-link sign-in; stores the session token on success.
  AuthResponse completeLoginLink(std::string_view token) const {
    graphql::JVal vars;
    vars["input"]["token"] = token;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation CompleteLoginLink($input: CompleteLoginLinkInput!) {"
        " completeLoginLink(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// Begin a federated (social) sign-in: returns { authorizeUrl, state }.
  graphql::Json socialLoginStart(std::string_view provider, std::string_view redirectUri) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["redirectUri"] = redirectUri;
    return execUnwrap(
        "mutation SocialLoginStart($input: SocialLoginStartInput!) {"
        " socialLoginStart(input: $input) { authorizeUrl state } }",
        vars);
  }

  /// Complete a federated sign-in from the provider callback; stores token.
  AuthResponse socialLoginComplete(std::string_view provider, std::string_view code,
                                   std::string_view state) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation SocialLoginComplete($input: SocialLoginCompleteInput!) {"
        " socialLoginComplete(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// DEV ONLY bypass sign-in (server must have DEV_AUTH_BYPASS). Stores the
  /// session token; throws FORBIDDEN when the bypass is disabled.
  AuthResponse devLogin(std::string_view email) const {
    graphql::JVal vars;
    vars["input"]["email"] = email;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation DevLogin($input: DevLoginInput!) {"
        " devLogin(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// The signed-in user's linked sign-in identities.
  graphql::Json myIdentities() const {
    return execUnwrap(
        "query MyIdentities { myIdentities {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }");
  }

  /// Link an additional federated identity (from a social callback).
  graphql::Json linkIdentity(std::string_view provider, std::string_view code,
                             std::string_view state) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    return execUnwrap(
        "mutation LinkIdentity($input: LinkIdentityInput!) { linkIdentity(input: $input) {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }",
        vars);
  }

  /// Unlink a federated identity (cannot remove the last sign-in method).
  bool unlinkIdentity(std::string_view identityId) const {
    graphql::JVal vars;
    vars["identityId"] = identityId;
    return execUnwrap(
               "mutation UnlinkIdentity($identityId: String!) { unlinkIdentity(identityId: $identityId) }",
               vars)
        .asBool();
  }

  /// Single-device logout; clears the stored token.
  bool logout() const {
    bool ok = execUnwrap(gen::auth::kLogoutDocument).asBool();
    auth_->clearToken();
    return ok;
  }

  /// Revoke every active session for the user.
  bool logoutAllDevices() const {
    return execUnwrap(gen::auth::kLogoutAllDevicesDocument).asBool();
  }

  void setToken(const std::string& token) const { auth_->setToken(token); }
  std::string getToken() const { return auth_->token(); }

 private:
  std::shared_ptr<graphql::AuthState> auth_;
};

}  // namespace crowdy::domains
