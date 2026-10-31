// The 13+ teen-safety surface against a live tier (ck-api v2.60.0): org-managed players signed in
// with their age and country (and the refusals a blocked country and a too-young player get),
// crowdy::startGameSession from those app tokens and from a Crowded Kingdoms account's own mint,
// the player's voice and video settings, contacts, blocks, a report and its resolution by the
// app's moderator, the app's recording state, the documents, the territory list, a CK account's
// age, and the Minecraft sign-in's refusal outside Crowded Kingdoms' own apps.
//
// Needs, beside CROWDY_E2E_API_URL, CROWDY_E2E_EMAIL and CROWDY_E2E_APP_ID:
//   CROWDY_E2E_MANAGED_FILE   a JSON file with the app's organization: {"orgId", "orgToken"}
//                             (an org token with players:mint and players:manage for the app).
//                             Read by the suite, never printed.
//   CROWDY_E2E_OWNER_EMAIL / CROWDY_E2E_OWNER_PASSWORD
//                             an admin of that organization (moderate_safety): resolves the
//                             report the suite files, and entitles the CK account if needed.
//   CROWDY_E2E_PROVISIONING_TOKEN on dev and test, for the CK accounts it registers.
// The managed players it signs in are deleted at the end; reports use reason OTHER.

#include <cstdio>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/game_session.hpp"
#include "e2e_util.hpp"

using namespace e2e;

namespace {

struct ManagedOrg {
  std::string orgId;
  std::string orgToken;
};

ManagedOrg requireManagedOrg() {
  const std::string path = envOr("CROWDY_E2E_MANAGED_FILE");
  if (path.empty()) {
    std::puts("CROWDY_E2E_MANAGED_FILE not configured; skipping");
    std::exit(77);
  }
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  const crowdy::graphql::Json file = crowdy::graphql::Json::parse(text.str());
  ManagedOrg org;
  org.orgId = file["orgId"].asBigIntString();
  org.orgToken = file["orgToken"].asString();
  E2E_CHECK(!org.orgId.empty() && org.orgToken.rfind("cko_", 0) == 0);
  return org;
}

/// An ISO-8601 UTC time `secondsAgo` seconds before now.
std::string isoAgo(int secondsAgo) {
  const std::time_t at = std::time(nullptr) - secondsAgo;
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &at);
#else
  gmtime_r(&at, &utc);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

std::string firstCode(const crowdy::graphql::GraphQLOutcome& out) {
  return out.errors.empty() ? std::string() : out.errors.front().code;
}

/// The organization's server: the org token, and no Origin header (libcurl sends none).
class OrgServer {
 public:
  OrgServer(const E2eConfig& cfg, ManagedOrg org) : org_(std::move(org)) {
    crowdy::ClientConfig c;
    c.httpUrl = cfg.apiUrl;
    client_ = std::make_unique<crowdy::CrowdyClient>(std::move(c));
    client_->setToken(org_.orgToken);
  }

  /// mintManagedPlayerToken, which neither SDK wraps (an organization's server call).
  crowdy::graphql::GraphQLOutcome mint(const std::string& appId, const std::string& externalId,
                                       std::optional<int> ageYears,
                                       std::optional<std::string> country) {
    crowdy::graphql::JVal vars;
    auto& input = vars["input"];
    input["appId"] = appId;
    input["externalId"] = externalId;
    if (ageYears) input["ageYears"] = *ageYears;
    if (country) input["country"] = *country;
    return client_->graphqlClient().requestOutcome(
        "mutation Mint($input: MintManagedPlayerTokenInput!) {"
        " mintManagedPlayerToken(input: $input) {"
        " token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl discoveryUrl launchUrl"
        " runtimeGate { status reason } } }",
        vars);
  }

  crowdy::domains::AppTokenResponse mintOk(const std::string& appId, const std::string& externalId,
                                           int ageYears, const std::string& country) {
    const auto out = mint(appId, externalId, ageYears, country);
    if (!out.ok()) {
      std::fprintf(stderr, "mint %s failed: %s\n", externalId.c_str(), firstCode(out).c_str());
    }
    E2E_CHECK(out.ok());
    minted_.push_back(externalId);
    return crowdy::domains::AppTokenResponse::fromJson(out.data["mintManagedPlayerToken"]);
  }

  /// Delete every managed player this run signed in; true when each was deleted.
  bool cleanUp() {
    bool all = true;
    for (const auto& externalId : minted_) {
      try {
        all = client_->admin().managedPlayers().remove(org_.orgId, externalId).asBool() && all;
      } catch (const crowdy::graphql::CrowdyGraphQLError& e) {
        printGraphQLError(e, "deleteManagedPlayer");
        all = false;
      }
    }
    return all;
  }

 private:
  ManagedOrg org_;
  std::unique_ptr<crowdy::CrowdyClient> client_;
  std::vector<std::string> minted_;
};

crowdy::GameSession start(const E2eConfig& cfg, const crowdy::domains::AppTokenResponse& token) {
  crowdy::GameSessionOptions options;
  options.fallbackOrigin = cfg.apiUrl;
  // Small deployments can briefly answer "no available servers"; the harness retries ~30 s.
  options.connectAttempts = 10;
  options.connectRetryDelayMs = 3000;
  crowdy::GameSession session = crowdy::startGameSession(token, std::move(options));
  if (!session.ok()) {
    std::fprintf(stderr, "startGameSession: %s\n", crowdy::errcName(session.status.code));
  }
  E2E_CHECK(session.ok());
  E2E_CHECK(session.connection->state() == crowdy::replication::ConnState::Connected);
  return session;
}

template <typename Fn>
std::string refusalCode(Fn&& call) {
  try {
    call();
  } catch (const crowdy::graphql::CrowdyGraphQLError& e) {
    return e.code();
  }
  return {};
}

}  // namespace

int main() {
  const E2eConfig cfg = requireConfig();
  const ManagedOrg managedOrg = requireManagedOrg();
  const std::string& appId = cfg.appId;
  OrgServer org(cfg, managedOrg);

  crowdy::ClientConfig publicConfig;
  publicConfig.httpUrl = cfg.apiUrl;
  crowdy::CrowdyClient publicClient(std::move(publicConfig));

  E2E_SUBTEST("public: territory policies and the player documents");
  {
    const auto policies = publicClient.platform().territoryPolicies();
    bool deBlocked = false;
    for (const auto& p : policies) {
      if (p.countryCode == "DE") deBlocked = p.blocked();
    }
    E2E_CHECK(deBlocked);
    const auto documents = publicClient.auth().playerLegalDocuments();
    E2E_CHECK(!documents.empty());
    for (const auto& d : documents) {
      E2E_CHECK(!d.documentId.empty() && !d.version.empty() && d.appId.isNull());
    }
    // An app that requires no documents of its own answers an empty list.
    for (const auto& d : publicClient.auth().appRequiredDocuments(appId)) {
      E2E_CHECK(d.appId.valueOrEmpty() == appId);
    }
  }

  E2E_SUBTEST("managed mint: a blocked country and a player under the minimum are refused");
  {
    const std::string suffix = runSuffix();
    const auto blocked = org.mint(appId, "e2e-cpp-13p-de-" + suffix, 30, std::string("DE"));
    E2E_CHECK(!blocked.ok());
    const auto territory = crowdy::graphql::territoryNotSupportedOf(blocked.errors);
    E2E_CHECK(territory.has_value());
    E2E_CHECK(territory->country == "DE");
    const auto young = org.mint(appId, "e2e-cpp-13p-young-" + suffix, 15, std::string("US"));
    E2E_CHECK(!young.ok());
    E2E_CHECK(crowdy::graphql::ageRequirementNotMetOf(young.errors));
  }

  E2E_SUBTEST("managed mint with ageYears and country, then startGameSession from the token");
  const std::string externalA = "e2e-cpp-13p-a-" + runSuffix();
  const std::string externalB = "e2e-cpp-13p-b-" + runSuffix();
  crowdy::GameSession a = start(cfg, org.mintOk(appId, externalA, 30, "US"));
  crowdy::GameSession b = start(cfg, org.mintOk(appId, externalB, 41, "KR"));
  const crowdy::graphql::Json meA = a.client->users().me();
  const crowdy::graphql::Json meB = b.client->users().me();
  E2E_CHECK(meA["ageBracket"].asString() == "18_plus");
  E2E_CHECK(meA["country"].asString() == "US");
  E2E_CHECK(meB["country"].asString() == "KR");
  const std::string userA = meA["userId"].asBigIntString();
  const std::string userB = meB["userId"].asBigIntString();
  E2E_CHECK(!userA.empty() && !userB.empty() && userA != userB);

  E2E_SUBTEST("voice and video settings: an adult's defaults, then a choice");
  {
    auto& safety = a.client->safety();
    const auto initial = safety.commSettings(appId);
    E2E_CHECK(!initial.isMinor);
    E2E_CHECK(initial.voiceAllowed == crowdy::gen::CommScope::EVERYONE);
    E2E_CHECK(initial.voiceSend == crowdy::gen::CommScope::EVERYONE);
    crowdy::domains::CommChoicesInput lower;
    lower.voiceSend = crowdy::gen::CommScope::CONTACTS;
    lower.videoReceive = crowdy::gen::CommScope::OFF;
    const auto lowered = safety.setCommSettings(appId, lower);
    E2E_CHECK(lowered.voiceSend == crowdy::gen::CommScope::CONTACTS);
    E2E_CHECK(lowered.videoReceive == crowdy::gen::CommScope::OFF);
    E2E_CHECK(lowered.choices.voiceSend == crowdy::gen::CommScope::CONTACTS);
    E2E_CHECK(lowered.videoSend == crowdy::gen::CommScope::EVERYONE);
    const auto consent = safety.requestParentalConsent(
        appId, crowdy::gen::ParentalConsentFeature::VOICE, "parent@test.invalid");
    E2E_CHECK(consent.status == crowdy::gen::ParentalConsentRequestStatus::NOT_REQUIRED);
  }

  E2E_SUBTEST("contacts: request, accept, list, remove");
  {
    const auto asked = a.client->safety().requestContact(appId, userB);
    E2E_CHECK(asked.state == crowdy::gen::ContactState::PENDING);
    bool incoming = false;
    for (const auto& r : b.client->safety().contactRequests(appId)) {
      if (r.userId == userA) incoming = r.direction == crowdy::gen::ContactRequestDirection::INCOMING;
    }
    E2E_CHECK(incoming);
    const auto accepted = b.client->safety().acceptContact(appId, userA);
    E2E_CHECK(accepted.state == crowdy::gen::ContactState::ACCEPTED);
    bool listed = false;
    for (const auto& c : a.client->safety().contacts(appId)) listed = listed || c.userId == userB;
    E2E_CHECK(listed);
    E2E_CHECK(a.client->safety().removeContact(appId, userB));
    for (const auto& c : a.client->safety().contacts(appId)) E2E_CHECK(c.userId != userB);
  }

  E2E_SUBTEST("blocks: block, list, unblock");
  {
    const auto blocked = a.client->safety().block(appId, userB);
    E2E_CHECK(blocked.userId == userB);
    bool listed = false;
    for (const auto& x : a.client->safety().blocks(appId)) listed = listed || x.userId == userB;
    E2E_CHECK(listed);
    // A blocked player's request is answered as usual and goes nowhere.
    b.client->safety().requestContact(appId, userA);
    bool reached = false;
    for (const auto& r : a.client->safety().contactRequests(appId)) reached = reached || r.userId == userB;
    E2E_CHECK(!reached);
    E2E_CHECK(a.client->safety().unblock(appId, userB));
    E2E_CHECK(!a.client->safety().unblock(appId, userB));
  }

  E2E_SUBTEST("the app's recording state, read with the app token");
  {
    const auto recording = a.client->admin().apps().recording(appId);
    E2E_CHECK(recording.has_value());
    if (!recording->enabled) E2E_CHECK(!recording->voice && !recording->video);
  }

  E2E_SUBTEST("a report, resolved by the app's moderator");
  if (!cfg.ownerEmail.empty() && !envOr("CROWDY_E2E_OWNER_PASSWORD").empty()) {
    crowdy::domains::SafetyReportFiling filing;
    filing.appId = appId;
    filing.reportedUserId = userB;
    filing.reason = crowdy::gen::SafetyReportReason::OTHER;
    filing.details = "CrowdyCPP e2e: an automated report, resolved by the same run";
    filing.occurredAt = isoAgo(60);
    const auto receipt = a.client->safety().fileReport(filing);
    E2E_CHECK(!receipt.reportId.empty());
    auto& moderation = owner(cfg).moderation();
    bool open = false;
    moderation.reports(appId, crowdy::gen::SafetyReportStatus::OPEN, 200)
        .forEach([&](crowdy::graphql::Json r) {
          open = open || r["reportId"].asBigIntString() == receipt.reportId;
        });
    E2E_CHECK(open);
    const auto resolved =
        moderation.resolveReport(appId, receipt.reportId, "CrowdyCPP e2e: no action, test report");
    E2E_CHECK(resolved["status"].asString() == "RESOLVED");
  } else {
    std::puts("   (no owner configured: no report filed, so the managed players can be deleted)");
  }

  E2E_SUBTEST("Minecraft sign-in: a challenge outside Crowded Kingdoms' own apps is refused");
  {
    const std::string code = refusalCode([&] { publicClient.portal().minecraftChallenge(appId); });
    if (code.empty()) {
      // This tier lists the app's organization as Crowded Kingdoms' own.
      E2E_CHECK(publicClient.portal().minecraftChallenge(appId).serverId.size() == 40);
    } else {
      E2E_CHECK(code == "FORBIDDEN" || code == "FEATURE_DISABLED");
    }
  }

  E2E_SUBTEST("a CK account gives its age; a sign-up from a blocked country makes no account");
  {
    crowdy::ClientConfig c;
    c.httpUrl = cfg.apiUrl;
    applyProvisioningToken(c);
    crowdy::CrowdyClient identity(std::move(c));
    const std::string email = deriveEmail(cfg, "13p-age");
    crowdy::domains::RegisterUserInput input;
    input.email = email;
    input.password = derivePassword(email);
    E2E_CHECK(!identity.auth().registerUser(input).token.empty());
    E2E_CHECK(identity.users().me()["ageBracket"].asString() == "unknown");
    const auto age = identity.auth().recordPlayerAge({"1990-05-15", "US", std::nullopt});
    E2E_CHECK(age.ageBracket.valueOrEmpty() == "18_plus");
    E2E_CHECK(age.country.valueOrEmpty() == "US");
    E2E_CHECK(refusalCode([&] {
                identity.auth().recordPlayerAge({"1990-05-15", "US", std::nullopt});
              }) == "CONFLICT");

    // The SDK's own sign-in path ends in the same entry point.
    E2E_CHECK(identity.auth().recordPlayerConsents(true, true));
    crowdy::domains::AppTokenResponse minted;
    try {
      minted = identity.portal().mintAppToken(appId);
    } catch (const crowdy::graphql::CrowdyGraphQLError& e) {
      // A paid or invite-only app: the owner entitles the account on the e2e tier.
      E2E_CHECK(crowdy::graphql::accessRefusalOf(e.errors()).has_value());
      requireOwner(cfg);
      std::puts("   (the app grants no free access: entitled through the owner)");
      crowdy::graphql::JVal grant;
      grant["appId"] = appId;
      grant["userId"] = age.userId;
      grant["tierId"] = ensureEntitledTier(cfg, appId);
      owner(cfg).admin().appAccess().grant(grant);
      minted = identity.portal().mintAppToken(appId);
    }
    crowdy::GameSession own = start(cfg, minted);
    E2E_CHECK(own.client->users().me()["userId"].asBigIntString() == age.userId);
    own = crowdy::GameSession{};
    E2E_CHECK(identity.users().deleteMyAccount());

    crowdy::ClientConfig c2;
    c2.httpUrl = cfg.apiUrl;
    applyProvisioningToken(c2);
    crowdy::CrowdyClient abroad(std::move(c2));
    const std::string abroadEmail = deriveEmail(cfg, "13p-fr");
    crowdy::domains::RegisterUserInput fr;
    fr.email = abroadEmail;
    fr.password = derivePassword(abroadEmail);
    fr.acceptLegal = true;
    fr.attestAgeOfMajority = true;
    fr.birthDate = "1990-05-15";
    fr.country = "FR";
    std::optional<std::string> country;
    try {
      abroad.auth().registerUser(fr);
    } catch (const crowdy::graphql::CrowdyGraphQLError& e) {
      if (const auto t = crowdy::graphql::territoryNotSupportedOf(e.errors())) country = t->country;
    }
    E2E_CHECK(country.value_or("") == "FR");
    E2E_CHECK(refusalCode([&] { abroad.auth().login(abroadEmail, fr.password); }) != "");
  }

  a = crowdy::GameSession{};
  b = crowdy::GameSession{};
  E2E_CHECK(org.cleanUp());
  std::puts("e2e_teen_safety: ok");
  return 0;
}
