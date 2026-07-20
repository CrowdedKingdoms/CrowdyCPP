#pragma once

#include <cctype>
#include <functional>
#include <string>
#include <utility>

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

  void mintAppTokenAsync(std::string_view appId,
                         std::function<void(graphql::GraphQLOutcome, AppTokenResponse)> cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    execUnwrapAsync(
        "mutation MintAppToken($input: MintAppTokenInput!) { mintAppToken(input: $input) {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          AppTokenResponse value{};
          if (out.ok()) value = AppTokenResponse::fromJson(out.data);
          cb(std::move(out), value);
        });
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

  void refreshAsync(std::function<void(graphql::GraphQLOutcome, AppTokenResponse)> cb) const {
    execUnwrapAsync(
        "mutation RefreshAppToken { refreshAppToken {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }",
        graphql::JVal(), {}, [this, cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          AppTokenResponse r{};
          if (out.ok()) {
            r = AppTokenResponse::fromJson(out.data);
            if (!r.token.empty()) auth_->setToken(r.token);
          }
          cb(std::move(out), r);
        });
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

  void createAuthorizationCodeAsync(std::string_view appId, std::string_view codeChallenge,
                                    std::string_view redirectUri,
                                    std::string_view codeChallengeMethod,
                                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["codeChallenge"] = codeChallenge;
    vars["input"]["codeChallengeMethod"] = codeChallengeMethod;
    vars["input"]["redirectUri"] = redirectUri;
    execUnwrapAsync(
        "mutation CreatePortalAuthorizationCode($input: CreatePortalAuthorizationCodeInput!) {"
        " createPortalAuthorizationCode(input: $input) { code redirectUri expiresAt } }",
        vars, {}, std::move(cb));
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

  void exchangeCodeAsync(std::string_view code, std::string_view codeVerifier,
                         std::function<void(graphql::GraphQLOutcome, AppTokenResponse)> cb) const {
    graphql::JVal vars;
    vars["input"]["code"] = code;
    if (!codeVerifier.empty()) vars["input"]["codeVerifier"] = codeVerifier;
    execUnwrapAsync(
        "mutation ExchangePortalCode($input: ExchangePortalCodeInput!) {"
        " exchangePortalCode(input: $input) {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl launchUrl } }",
        vars, {}, [this, cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          AppTokenResponse r{};
          if (out.ok()) {
            r = AppTokenResponse::fromJson(out.data);
            if (!r.token.empty()) auth_->setToken(r.token);
          }
          cb(std::move(out), r);
        });
  }

  /// A begun portal entry: send the player to `url`; keep `verifier` (and
  /// `state` for correlation) to pass to completeEntry() on the callback.
  struct PortalEntry {
    std::string url;
    std::string state;
    std::string verifier;
  };

  /// Game side: begin a cross-origin portal entry. Generates PKCE material
  /// and assembles the authorize URL on the identity origin
  /// (`authorizeUrl?app_id=...&code_challenge=...&code_challenge_method=S256&
  /// redirect_uri=...&state=...`). Native flows that own both sides can skip
  /// this and call createAuthorizationCode + exchangeCode directly.
  PortalEntry beginEntry(std::string_view appId, std::string_view authorizeUrl,
                         std::string_view redirectUri, std::string_view state = {}) const {
    PkcePair pkce = generatePkce();
    PortalEntry entry;
    entry.verifier = pkce.verifier;
    if (state.empty()) {
      std::uint8_t raw[16];
      core::opensslCrypto().randomBytes(raw, sizeof(raw));
      entry.state = core::base64UrlEncode(Bytes(raw, sizeof(raw)));
    } else {
      entry.state = std::string(state);
    }
    entry.url = std::string(authorizeUrl);
    entry.url += (entry.url.find('?') == std::string::npos) ? '?' : '&';
    entry.url += "app_id=" + urlEncode(appId);
    entry.url += "&code_challenge=" + urlEncode(pkce.challenge);
    entry.url += "&code_challenge_method=S256";
    entry.url += "&redirect_uri=" + urlEncode(redirectUri);
    entry.url += "&state=" + urlEncode(entry.state);
    return entry;
  }

  /// Game side: complete a portal entry from the callback's code + the
  /// verifier saved by beginEntry(). Stores the app token on this client.
  AppTokenResponse completeEntry(std::string_view code, std::string_view verifier) const {
    return exchangeCode(code, verifier);
  }

  void completeEntryAsync(std::string_view code, std::string_view verifier,
                          std::function<void(graphql::GraphQLOutcome, AppTokenResponse)> cb) const {
    exchangeCodeAsync(code, verifier, std::move(cb));
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

  void getConsentAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(
        "query PortalConsent($appId: BigInt!) { portalConsent(appId: $appId) {"
        " appId appName trusted alreadyGranted consentRequired } }",
        vars, {}, std::move(cb));
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

  void authorizeAppAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    execUnwrapAsync(
        "mutation AuthorizeApp($input: AuthorizeAppInput!) { authorizeApp(input: $input) {"
        " grantId appId status scopes } }",
        vars, {}, std::move(cb));
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

  void revokeAppAuthorizationAsync(std::string_view appId,
                                   std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(
        "mutation RevokeAppAuthorization($appId: BigInt!) { revokeAppAuthorization(appId: $appId) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  /// The user's active app authorizations ("connected apps").
  graphql::Json myAuthorizedApps() const {
    return execUnwrap(
        "query MyAuthorizedApps { myAuthorizedApps {"
        " grantId appId appName scopes status grantedAt revokedAt } }");
  }

  void myAuthorizedAppsAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(
        "query MyAuthorizedApps { myAuthorizedApps {"
        " grantId appId appName scopes status grantedAt revokedAt } }",
        graphql::JVal(), {}, std::move(cb));
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

  void setAppClientSettingsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(
        "mutation SetAppClientSettings($input: SetAppClientSettingsInput!) {"
        " setAppClientSettings(input: $input) { appId appName trusted consentRequired } }",
        vars, {}, std::move(cb));
  }

 private:
  static std::string urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
      }
    }
    return out;
  }

  std::shared_ptr<graphql::AuthState> auth_;
};

}  // namespace crowdy::domains
