#pragma once

#include <string>

#include "crowdy/core/base64.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"

/// Overworld portals & app-scoped tokens — client.portal(). Gameplay requires
/// a short-lived APP-SCOPED token per app; the identity session token is
/// management-plane only. See
/// https://docs.crowdedkingdoms.com/management-api/portals-and-app-tokens.
/// Targets the Management API.
namespace crowdy::domains {

/// PKCE material for the cross-origin portal flow. Keep the verifier secret
/// on the game side; only the challenge travels to the Overworld.
struct PkcePair {
  std::string verifier;
  std::string challenge;  ///< base64url(SHA256(verifier))
  std::string method = "S256";
};

class PortalAPI : public DomainBase {
 public:
  PortalAPI(std::shared_ptr<graphql::GraphQLClient> gql, std::shared_ptr<graphql::AuthState> auth)
      : DomainBase(std::move(gql)), auth_(std::move(auth)) {}

  static constexpr std::string_view kAppTokenFields =
      "token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl";

  /// Native/direct mint: exchange the identity session token for an
  /// app-scoped gameplay token. The token is NOT stored on this client —
  /// build a per-game client with it. Free/open apps auto-grant access; paid
  /// apps require an existing entitlement (FORBIDDEN otherwise).
  AppTokenResponse mintAppToken(std::string_view appId) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    return AppTokenResponse::fromJson(execUnwrap(
        "mutation MintAppToken($input: MintAppTokenInput!) { mintAppToken(input: $input) {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }",
        vars));
  }

  /// Same-app refresh: rotate the CURRENT app token (send it as the bearer on
  /// this client) for a fresh one and store it. Call before expiresAt to keep
  /// playing without re-portaling.
  AppTokenResponse refresh() const {
    auto r = AppTokenResponse::fromJson(execUnwrap(
        "mutation RefreshAppToken { refreshAppToken {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }"));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// Overworld side: mint a one-time authorization code bound to the game's
  /// PKCE challenge + redirect URI. Returns { code, redirectUri, expiresAt }.
  graphql::Json createAuthorizationCode(std::string_view appId, std::string_view codeChallenge,
                                        std::string_view redirectUri,
                                        std::string_view codeChallengeMethod = "S256") const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["codeChallenge"] = codeChallenge;
    vars["input"]["codeChallengeMethod"] = codeChallengeMethod;
    vars["input"]["redirectUri"] = redirectUri;
    return execUnwrap(
        "mutation CreatePortalAuthorizationCode($input: CreatePortalAuthorizationCodeInput!) {"
        " createPortalAuthorizationCode(input: $input) { code redirectUri expiresAt } }",
        vars);
  }

  /// Game side: exchange a one-time code (+ PKCE verifier) for an app token
  /// and store it on this client. Public — no session token required.
  AppTokenResponse exchangeCode(std::string_view code, std::string_view codeVerifier = {}) const {
    graphql::JVal vars;
    vars["input"]["code"] = code;
    if (!codeVerifier.empty()) vars["input"]["codeVerifier"] = codeVerifier;
    auto r = AppTokenResponse::fromJson(execUnwrap(
        "mutation ExchangePortalCode($input: ExchangePortalCodeInput!) {"
        " exchangePortalCode(input: $input) {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// Generate a PKCE verifier + S256 challenge for the portal flow.
  static PkcePair generatePkce(const core::ICrypto& crypto = core::opensslCrypto()) {
    std::uint8_t raw[32];
    crypto.randomBytes(raw, sizeof(raw));
    PkcePair p;
    p.verifier = core::base64UrlEncode(Bytes(raw, sizeof(raw)));
    std::uint8_t digest[32];
    crypto.sha256(asBytes(p.verifier), digest);
    p.challenge = core::base64UrlEncode(Bytes(digest, sizeof(digest)));
    return p;
  }

  // ----- Consent + connected apps -------------------------------------------

  /// Whether portaling into an app needs a consent prompt (Overworld side).
  graphql::Json getConsent(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(
        "query PortalConsent($appId: BigInt!) { portalConsent(appId: $appId) {"
        " appId appName trusted alreadyGranted consentRequired } }",
        vars);
  }

  /// Record the user's consent for an app (call from the consent screen).
  graphql::Json authorizeApp(std::string_view appId) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    return execUnwrap(
        "mutation AuthorizeApp($input: AuthorizeAppInput!) { authorizeApp(input: $input) {"
        " grantId appId status scopes } }",
        vars);
  }

  /// Revoke a prior authorization; also revokes the user's live tokens for it.
  bool revokeAppAuthorization(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(
               "mutation RevokeAppAuthorization($appId: BigInt!) { revokeAppAuthorization(appId: $appId) }",
               vars)
        .asBool();
  }

  /// The user's active app authorizations ("connected apps").
  graphql::Json myAuthorizedApps() const {
    return execUnwrap(
        "query MyAuthorizedApps { myAuthorizedApps {"
        " grantId appId appName scopes status grantedAt revokedAt } }");
  }

  /// Register/update an app's portal client settings (requires manage_apps).
  graphql::Json setAppClientSettings(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(
        "mutation SetAppClientSettings($input: SetAppClientSettingsInput!) {"
        " setAppClientSettings(input: $input) { appId appName trusted consentRequired } }",
        vars);
  }

 private:
  std::shared_ptr<graphql::AuthState> auth_;
};

}  // namespace crowdy::domains
