#include <cctype>
#include <chrono>
#include <thread>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/domains/admin.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

// The 13+ teen-safety surface (ck-api v2.60.0, CrowdyJS's client.safety, client.moderation and
// the identity additions): each wrapper's root field, variables and parsed result, against a
// transport that answers by operation name.

namespace {

/// The operation a request runs: its operationName, or the first operation the document names.
std::string opName(const graphql::Json& body) {
  if (!body["operationName"].asString().empty()) return body["operationName"].asString();
  const std::string query = body["query"].asString();
  for (const std::string kind : {"query ", "mutation "}) {
    const auto at = query.find(kind);
    if (at == std::string::npos) continue;
    const auto start = at + kind.size();
    auto end = start;
    while (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) ||
                                  query[end] == '_')) {
      ++end;
    }
    return query.substr(start, end - start);
  }
  return {};
}

class RoutingHttp final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  std::map<std::string, std::string> replies;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    const graphql::Json body = graphql::Json::parse(request.body);
    const std::string name = opName(body);
    const auto it = replies.find(name);
    if (it != replies.end()) return {200, it->second};
    return {200, R"({"data":{"ok":true}})"};
  }

  graphql::Json last() const { return graphql::Json::parse(requests.back().body); }
};

struct Rig {
  std::shared_ptr<RoutingHttp> http = std::make_shared<RoutingHttp>();
  std::unique_ptr<CrowdyClient> client;

  Rig() {
    ClientConfig config;
    config.httpUrl = "https://game.invalid";
    config.transport = http;
    client = std::make_unique<CrowdyClient>(std::move(config));
  }
};

/// CrowdyClient delivers async completions from poll(); wait for `done` while polling.
template <typename Done>
void drain(CrowdyClient& client, Done done) {
  for (int i = 0; i < 400 && !done(); ++i) {
    client.poll();
    if (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(done());
}

bool sent(const graphql::Json& body, const std::string& rootField) {
  return body["query"].asString().find(rootField + "(") != std::string::npos ||
         body["query"].asString().find(rootField + " ") != std::string::npos ||
         body["query"].asString().find(rootField + "}") != std::string::npos;
}

const char* kCommSettings =
    R"({"data":{"myCommSettings":{"appId":"7","isMinor":true,"voiceSend":"CONTACTS",)"
    R"("voiceReceive":"OFF","videoSend":"OFF","videoReceive":"SOMETHING_NEW",)"
    R"("choices":{"voiceSend":"EVERYONE","voiceReceive":null,"videoSend":null,"videoReceive":"OFF"},)"
    R"("voiceAllowed":"CONTACTS","videoAllowed":"OFF","voiceConsent":true,"videoConsent":false,)"
    R"("updatedAt":"2026-10-30T22:00:00.000Z"}}})";

void testCommSettings() {
  Rig rig;
  rig.http->replies["MyCommSettings"] = kCommSettings;
  const domains::CommSettings s = rig.client->safety().commSettings("7");
  const graphql::Json body = rig.http->last();
  CHECK(sent(body, "myCommSettings"));
  CHECK_EQ(body["variables"]["appId"].asString(), std::string("7"));
  CHECK_EQ(s.appId, std::string("7"));
  CHECK(s.isMinor);
  CHECK(s.voiceSend == gen::CommScope::CONTACTS);
  CHECK(s.voiceReceive == gen::CommScope::OFF);
  // A scope this SDK does not know reads as the most restrictive one.
  CHECK(s.videoReceive == gen::CommScope::OFF);
  CHECK(s.choices.voiceSend == gen::CommScope::EVERYONE);
  CHECK(!s.choices.voiceReceive.has_value());
  CHECK(s.choices.videoReceive == gen::CommScope::OFF);
  CHECK(s.voiceAllowed == gen::CommScope::CONTACTS);
  CHECK(s.voiceConsent);
  CHECK(!s.videoConsent);
  CHECK_EQ(s.updatedAt.valueOrEmpty(), std::string("2026-10-30T22:00:00.000Z"));

  rig.http->replies["SetMyCommSettings"] =
      R"({"data":{"setMyCommSettings":{"appId":"7","isMinor":false,"voiceSend":"OFF",)"
      R"("voiceReceive":"EVERYONE","videoSend":"EVERYONE","videoReceive":"EVERYONE",)"
      R"("choices":{"voiceSend":"OFF"},"voiceAllowed":"EVERYONE","videoAllowed":"EVERYONE",)"
      R"("voiceConsent":false,"videoConsent":false,"updatedAt":null}}})";
  domains::CommChoicesInput choices;
  choices.voiceSend = gen::CommScope::OFF;
  choices.videoReceive = gen::CommScope::CONTACTS;
  const domains::CommSettings changed = rig.client->safety().setCommSettings("7", choices);
  const graphql::Json set = rig.http->last();
  CHECK(sent(set, "setMyCommSettings"));
  CHECK_EQ(set["variables"]["input"]["voiceSend"].asString(), std::string("OFF"));
  CHECK_EQ(set["variables"]["input"]["videoReceive"].asString(), std::string("CONTACTS"));
  // Unset choices are not sent: the server keeps them.
  CHECK(!set["variables"]["input"]["voiceReceive"].ok());
  CHECK(!set["variables"]["input"]["videoSend"].ok());
  CHECK(changed.voiceSend == gen::CommScope::OFF);
  CHECK(changed.updatedAt.isNull());

  // An empty change still sends an input object, which the schema requires.
  rig.client->safety().setCommSettings("7", {});
  CHECK(rig.http->last()["variables"]["input"].isObject());

  bool called = false;
  rig.client->safety().commSettingsAsync(
      "7", [&](graphql::GraphQLOutcome out, domains::CommSettings got) {
        called = true;
        CHECK(out.ok());
        CHECK(got.voiceSend == gen::CommScope::CONTACTS);
      });
  drain(*rig.client, [&] { return called; });
}

void testContactsAndBlocks() {
  Rig rig;
  rig.http->replies["MyContacts"] =
      R"({"data":{"myContacts":[{"userId":"11","state":"ACCEPTED","createdAt":"a","acceptedAt":"b"}]}})";
  rig.http->replies["ContactRequests"] =
      R"({"data":{"contactRequests":[{"userId":"12","direction":"OUTGOING","createdAt":"c"}]}})";
  rig.http->replies["RequestContact"] =
      R"({"data":{"requestContact":{"userId":"13","state":"PENDING","createdAt":"d","acceptedAt":null}}})";
  rig.http->replies["AcceptContact"] =
      R"({"data":{"acceptContact":{"userId":"13","state":"ACCEPTED","createdAt":"d","acceptedAt":"e"}}})";
  rig.http->replies["RemoveContact"] = R"({"data":{"removeContact":true}})";
  rig.http->replies["MyBlocks"] = R"({"data":{"myBlocks":[{"userId":"14","createdAt":"f"}]}})";
  rig.http->replies["BlockPlayer"] = R"({"data":{"blockPlayer":{"userId":"14","createdAt":"f"}}})";
  rig.http->replies["UnblockPlayer"] = R"({"data":{"unblockPlayer":false}})";
  auto& safety = rig.client->safety();

  const auto contacts = safety.contacts("7");
  CHECK(sent(rig.http->last(), "myContacts"));
  CHECK_EQ(contacts.size(), std::size_t{1});
  CHECK_EQ(contacts.at(0).userId, std::string("11"));
  CHECK(contacts.at(0).state == gen::ContactState::ACCEPTED);
  CHECK_EQ(contacts.at(0).acceptedAt.valueOrEmpty(), std::string("b"));

  const auto requests = safety.contactRequests("7");
  CHECK(sent(rig.http->last(), "contactRequests"));
  CHECK(requests.at(0).direction == gen::ContactRequestDirection::OUTGOING);

  const auto pending = safety.requestContact("7", "13");
  const graphql::Json req = rig.http->last();
  CHECK(sent(req, "requestContact"));
  CHECK_EQ(req["variables"]["appId"].asString(), std::string("7"));
  CHECK_EQ(req["variables"]["userId"].asString(), std::string("13"));
  CHECK(pending.state == gen::ContactState::PENDING);
  CHECK(pending.acceptedAt.isNull());

  CHECK(safety.acceptContact("7", "13").state == gen::ContactState::ACCEPTED);
  CHECK(sent(rig.http->last(), "acceptContact"));
  CHECK(safety.removeContact("7", "13"));
  CHECK(sent(rig.http->last(), "removeContact"));

  const auto blocks = safety.blocks("7");
  CHECK(sent(rig.http->last(), "myBlocks"));
  CHECK_EQ(blocks.at(0).userId, std::string("14"));
  CHECK_EQ(safety.block("7", "14").userId, std::string("14"));
  CHECK(sent(rig.http->last(), "blockPlayer"));
  CHECK(!safety.unblock("7", "14"));
  CHECK(sent(rig.http->last(), "unblockPlayer"));

  int calls = 0;
  safety.contactsAsync("7", [&](graphql::GraphQLOutcome out, std::vector<domains::PlayerContact> got) {
    ++calls;
    CHECK(out.ok());
    CHECK_EQ(got.size(), std::size_t{1});
  });
  safety.unblockAsync("7", "14", [&](graphql::GraphQLOutcome out, bool got) {
    ++calls;
    CHECK(out.ok());
    CHECK(!got);
  });
  safety.blockAsync("7", "14", [&](graphql::GraphQLOutcome out, domains::PlayerBlock got) {
    ++calls;
    CHECK(out.ok());
    CHECK_EQ(got.createdAt, std::string("f"));
  });
  drain(*rig.client, [&] { return calls == 3; });
}

void testParentalConsentAndReports() {
  Rig rig;
  rig.http->replies["RequestParentalConsent"] =
      R"({"data":{"requestParentalConsent":{"appId":"7","feature":"VIDEO","status":"NOT_REQUIRED"}}})";
  rig.http->replies["FileSafetyReport"] =
      R"({"data":{"fileSafetyReport":{"reportId":"9001","filedAt":"2026-10-30T23:00:00.000Z"}}})";
  auto& safety = rig.client->safety();

  const auto consent =
      safety.requestParentalConsent("7", gen::ParentalConsentFeature::VIDEO, "parent@test.invalid");
  const graphql::Json body = rig.http->last();
  CHECK(sent(body, "requestParentalConsent"));
  CHECK_EQ(body["variables"]["feature"].asString(), std::string("VIDEO"));
  CHECK_EQ(body["variables"]["parentEmail"].asString(), std::string("parent@test.invalid"));
  CHECK(consent.feature == gen::ParentalConsentFeature::VIDEO);
  CHECK(consent.status == gen::ParentalConsentRequestStatus::NOT_REQUIRED);

  domains::SafetyReportFiling report;
  report.appId = "7";
  report.reportedUserId = "14";
  report.reason = gen::SafetyReportReason::HARASSMENT;
  report.occurredAt = "2026-10-30T22:55:00.000Z";
  const auto receipt = safety.fileReport(report);
  const graphql::Json filed = rig.http->last();
  CHECK(sent(filed, "fileSafetyReport"));
  CHECK_EQ(filed["variables"]["input"]["reason"].asString(), std::string("HARASSMENT"));
  CHECK_EQ(filed["variables"]["input"]["reportedUserId"].asString(), std::string("14"));
  CHECK(!filed["variables"]["input"]["details"].ok());
  CHECK(!filed["variables"]["input"]["reportedActorUuid"].ok());
  CHECK_EQ(receipt.reportId, std::string("9001"));

  report.details = "spoke to me after I asked them to stop";
  report.reportedActorUuid = std::string(32, 'a');
  safety.fileReport(report);
  CHECK_EQ(rig.http->last()["variables"]["input"]["details"].asString(),
           std::string("spoke to me after I asked them to stop"));
  CHECK_EQ(rig.http->last()["variables"]["input"]["reportedActorUuid"].asString(),
           std::string(32, 'a'));
}

void testModeration() {
  Rig rig;
  rig.http->replies["SafetyReports"] =
      R"({"data":{"safetyReports":[{"reportId":"9001","status":"OPEN","sessions":["5"]}]}})";
  rig.http->replies["ResolveSafetyReport"] =
      R"({"data":{"resolveSafetyReport":{"reportId":"9001","status":"RESOLVED"}}})";
  rig.http->replies["EscalateSafetyReport"] =
      R"({"data":{"escalateSafetyReport":{"reportId":"9001","status":"ESCALATED"}}})";
  CHECK(&rig.client->moderation() == &rig.client->admin().moderation());
  auto& moderation = rig.client->moderation();

  const graphql::Json all = moderation.reports("7");
  graphql::Json body = rig.http->last();
  CHECK(sent(body, "safetyReports"));
  CHECK(!body["variables"]["status"].ok());
  CHECK(!body["variables"]["first"].ok());
  CHECK_EQ(all.at(0)["reportId"].asString(), std::string("9001"));

  moderation.reports("7", gen::SafetyReportStatus::OPEN, 20);
  body = rig.http->last();
  CHECK_EQ(body["variables"]["status"].asString(), std::string("OPEN"));
  CHECK_EQ(body["variables"]["first"].asInt64(), std::int64_t{20});

  CHECK_EQ(moderation.resolveReport("7", "9001", "warned")["status"].asString(),
           std::string("RESOLVED"));
  body = rig.http->last();
  CHECK(sent(body, "resolveSafetyReport"));
  CHECK_EQ(body["variables"]["outcome"].asString(), std::string("warned"));
  CHECK_EQ(moderation.escalateReport("7", "9001", "NCMEC-1")["status"].asString(),
           std::string("ESCALATED"));
  CHECK_EQ(rig.http->last()["variables"]["ncmecReportId"].asString(), std::string("NCMEC-1"));
}

void testIdentity() {
  Rig rig;
  rig.http->replies["Register"] =
      R"({"data":{"register":{"token":"s","gameTokenId":"1","user":{"userId":"21","email":"a@test.invalid","gamertag":null}}}})";
  rig.http->replies["RecordPlayerAge"] =
      R"({"data":{"recordPlayerAge":{"userId":"21","ageBracket":"16_17","country":"US"}}})";
  rig.http->replies["RecordPlayerConsentsFor"] = R"({"data":{"recordPlayerConsents":true}})";
  rig.http->replies["RecordPlayerConsents"] = R"({"data":{"recordPlayerConsents":true}})";
  rig.http->replies["PlayerLegalAcceptanceFor"] = R"({"data":{"playerLegalAcceptance":false}})";
  rig.http->replies["PlayerLegalDocuments"] =
      R"({"data":{"playerLegalDocuments":[{"documentId":"player-terms","version":"player-terms-2026-11-01","title":"Player Terms","url":"https://crowdedkingdoms.com/player-terms.html","appId":null}]}})";
  rig.http->replies["AppRequiredDocuments"] =
      R"({"data":{"appRequiredDocuments":[{"documentId":"rules","version":"rules-1","title":"Rules","url":"https://game.invalid/rules","appId":"7"}]}})";
  rig.http->replies["DeveloperLegalDocuments"] = R"({"data":{"developerLegalDocuments":[]}})";
  rig.http->replies["DeveloperLegalAcceptance"] = R"({"data":{"developerLegalAcceptance":true}})";
  rig.http->replies["RecordDeveloperConsents"] = R"({"data":{"recordDeveloperConsents":true}})";
  rig.http->replies["TerritoryPolicies"] =
      R"({"data":{"territoryPolicies":[{"countryCode":"DE","status":"blocked","minAge":13},{"countryCode":"KR","status":"open","minAge":14}]}})";
  auto& auth = rig.client->auth();

  domains::RegisterUserInput input;
  input.email = "a@test.invalid";
  input.password = "pw-123456";
  input.birthDate = "2010-04-01";
  input.country = "US";
  input.parentalConfirmation = true;
  input.acceptLegal = true;
  const auto registered = auth.registerUser(input);
  graphql::Json body = rig.http->last();
  const graphql::Json sentInput = body["variables"]["registerUserInput"];
  CHECK_EQ(sentInput["birthDate"].asString(), std::string("2010-04-01"));
  CHECK_EQ(sentInput["country"].asString(), std::string("US"));
  CHECK(sentInput["parentalConfirmation"].asBool());
  CHECK(sentInput["acceptLegal"].asBool());
  CHECK(!sentInput["attestAgeOfMajority"].ok());
  CHECK(!sentInput["gamertag"].ok());
  CHECK_EQ(registered.userId, std::string("21"));
  CHECK_EQ(auth.getToken(), std::string("s"));

  const domains::PlayerAge age = auth.recordPlayerAge({"2010-04", "US", true});
  body = rig.http->last();
  CHECK(sent(body, "recordPlayerAge"));
  CHECK_EQ(body["variables"]["input"]["birthDate"].asString(), std::string("2010-04"));
  CHECK(body["variables"]["input"]["parentalConfirmation"].asBool());
  CHECK_EQ(age.ageBracket.valueOrEmpty(), std::string("16_17"));
  CHECK_EQ(age.country.valueOrEmpty(), std::string("US"));
  auth.recordPlayerAge({"1990-05-02", "CA", std::nullopt});
  CHECK(!rig.http->last()["variables"]["input"]["parentalConfirmation"].ok());

  domains::PlayerConsents consents;
  consents.acceptLegal = true;
  consents.appId = "7";
  consents.method = gen::LegalAcceptanceMethod::IN_GAME;
  CHECK(auth.recordPlayerConsents(consents));
  body = rig.http->last();
  CHECK_EQ(opName(body), std::string("RecordPlayerConsentsFor"));
  CHECK_EQ(body["variables"]["appId"].asString(), std::string("7"));
  CHECK_EQ(body["variables"]["method"].asString(), std::string("IN_GAME"));
  CHECK(!body["variables"]["attestAgeOfMajority"].ok());
  // The two-flag form keeps the document every older API accepts.
  CHECK(auth.recordPlayerConsents(true, true));
  CHECK_EQ(opName(rig.http->last()), std::string("RecordPlayerConsents"));

  CHECK(!auth.playerLegalAcceptance(std::string_view("7")));
  CHECK_EQ(rig.http->last()["variables"]["appId"].asString(), std::string("7"));

  const auto platformDocs = auth.playerLegalDocuments();
  CHECK_EQ(platformDocs.size(), std::size_t{1});
  CHECK_EQ(platformDocs.at(0).documentId, std::string("player-terms"));
  CHECK(platformDocs.at(0).appId.isNull());
  const auto appDocs = auth.appRequiredDocuments("7");
  CHECK_EQ(appDocs.at(0).appId.valueOrEmpty(), std::string("7"));
  CHECK_EQ(rig.http->last()["variables"]["appId"].asString(), std::string("7"));
  CHECK(auth.developerLegalDocuments().empty());
  CHECK(auth.developerLegalAcceptance());
  CHECK(auth.recordDeveloperConsents(true));
  CHECK(rig.http->last()["variables"]["acceptDeveloperTerms"].asBool());

  const auto policies = rig.client->platform().territoryPolicies();
  CHECK_EQ(policies.size(), std::size_t{2});
  CHECK(policies.at(0).blocked());
  CHECK(!policies.at(1).blocked());
  CHECK_EQ(policies.at(1).minAge, 14);

  int calls = 0;
  auth.recordPlayerAgeAsync({"2010-04", "US", true},
                            [&](graphql::GraphQLOutcome out, domains::PlayerAge got) {
                              ++calls;
                              CHECK(out.ok());
                              CHECK_EQ(got.userId, std::string("21"));
                            });
  auth.appRequiredDocumentsAsync(
      "7", [&](graphql::GraphQLOutcome out, std::vector<domains::LegalDocument> got) {
        ++calls;
        CHECK(out.ok());
        CHECK_EQ(got.size(), std::size_t{1});
      });
  rig.client->platform().territoryPoliciesAsync(
      [&](graphql::GraphQLOutcome out, std::vector<domains::TerritoryPolicy> got) {
        ++calls;
        CHECK(out.ok());
        CHECK_EQ(got.size(), std::size_t{2});
      });
  drain(*rig.client, [&] { return calls == 3; });
}

void testMinecraftSignIn() {
  Rig rig;
  const std::string token(64, 'm');
  rig.http->replies["MinecraftChallenge"] =
      R"({"data":{"minecraftChallenge":{"serverId":"0123456789abcdef0123456789abcdef01234567","expiresAt":"2026-10-30T23:02:00.000Z"}}})";
  rig.http->replies["SignInWithMinecraft"] =
      R"({"data":{"signInWithMinecraft":{"token":")" + token +
      R"(","gameTokenId":"77","appId":"7","expiresAt":"2026-10-31T00:00:00.000Z","gameApiUrl":"https://ck-or-1.game.invalid","gameApiWsUrl":"wss://ck-or-1.game.invalid","discoveryUrl":"https://ck.game.invalid","launchUrl":null,"runtimeGate":{"status":"ACTIVE","reason":null}}}})";
  auto& portal = rig.client->portal();

  const domains::MinecraftChallenge challenge = portal.minecraftChallenge("7");
  CHECK(sent(rig.http->last(), "minecraftChallenge"));
  CHECK_EQ(rig.http->last()["variables"]["appId"].asString(), std::string("7"));
  CHECK_EQ(challenge.serverId.size(), std::size_t{40});

  domains::MinecraftSignInParams params;
  params.appId = "7";
  params.username = "Steve_1";
  params.serverId = challenge.serverId;
  params.birthDate = "1990-05";
  params.country = "US";
  params.acceptedDocuments = {{"player-terms", "player-terms-2026-11-01"},
                              {"minecraft-addendum", "minecraft-addendum-2026-11-01"}};
  const domains::AppTokenResponse signedIn = portal.signInWithMinecraft(params);
  const graphql::Json body = rig.http->last();
  const graphql::Json input = body["variables"]["input"];
  CHECK(sent(body, "signInWithMinecraft"));
  CHECK_EQ(input["username"].asString(), std::string("Steve_1"));
  CHECK_EQ(input["serverId"].asString(), challenge.serverId);
  CHECK(!input["parentalConfirmation"].ok());
  CHECK_EQ(input["acceptedDocuments"].size(), std::size_t{2});
  CHECK_EQ(input["acceptedDocuments"].at(1)["documentId"].asString(),
           std::string("minecraft-addendum"));
  CHECK_EQ(input["acceptedDocuments"].at(1)["version"].asString(),
           std::string("minecraft-addendum-2026-11-01"));
  CHECK_EQ(signedIn.token, token);
  CHECK_EQ(signedIn.gameApiUrl.valueOrEmpty(), std::string("https://ck-or-1.game.invalid"));
  // Stored like exchangeManagedPlayerAssertion's.
  CHECK_EQ(rig.client->getToken(), token);

  bool called = false;
  portal.minecraftChallengeAsync("7", [&](graphql::GraphQLOutcome out, domains::MinecraftChallenge got) {
    called = true;
    CHECK(out.ok());
    CHECK_EQ(got.expiresAt, std::string("2026-10-30T23:02:00.000Z"));
  });
  drain(*rig.client, [&] { return called; });
}

void testAppsRecordingAndDocuments() {
  Rig rig;
  rig.http->replies["AppRecording"] =
      R"({"data":{"app":{"appId":"7","recording":{"enabled":true,"voice":true,"video":false}}}})";
  rig.http->replies["SetAppRequiredDocuments"] =
      R"({"data":{"setAppRequiredDocuments":[{"documentId":"rules","version":"rules-2","title":"Rules","url":"https://game.invalid/rules","appId":"7"}]}})";
  auto& apps = rig.client->admin().apps();

  const auto recording = apps.recording("7");
  CHECK(sent(rig.http->last(), "app"));
  CHECK(recording.has_value());
  CHECK(recording->enabled);
  CHECK(recording->voice);
  CHECK(!recording->video);

  rig.http->replies["AppRecording"] = R"({"data":{"app":null}})";
  CHECK(!apps.recording("8").has_value());

  domains::LegalDocument rules;
  rules.documentId = "rules";
  rules.version = "rules-2";
  rules.title = "Rules";
  rules.url = "https://game.invalid/rules";
  const auto stored = apps.setRequiredDocuments("7", {rules});
  const graphql::Json body = rig.http->last();
  CHECK(sent(body, "setAppRequiredDocuments"));
  CHECK_EQ(body["variables"]["documents"].size(), std::size_t{1});
  CHECK_EQ(body["variables"]["documents"].at(0)["version"].asString(), std::string("rules-2"));
  CHECK(!body["variables"]["documents"].at(0)["appId"].ok());
  CHECK_EQ(stored.at(0).appId.valueOrEmpty(), std::string("7"));

  apps.setRequiredDocuments("7", {});
  CHECK(rig.http->last()["variables"]["documents"].isArray());
  CHECK_EQ(rig.http->last()["variables"]["documents"].size(), std::size_t{0});
}

graphql::GraphQLOutcome refusal(const std::string& extensions) {
  RoutingHttp http;
  http.replies["Probe"] = R"({"errors":[{"message":"refused","extensions":)" + extensions + "}]}";
  auto auth = std::make_shared<graphql::AuthState>();
  graphql::GraphQLClient client(graphql::GraphQLClientConfig{"http://test/graphql", 1000},
                                std::shared_ptr<graphql::IHttpTransport>(&http, [](auto*) {}),
                                auth);
  return client.requestOutcome("query Probe { ok }");
}

void testRefusals() {
  const auto legal = refusal(
      R"({"code":"LEGAL_ACCEPTANCE_REQUIRED","missingDocuments":[{"documentId":"player-terms","version":"player-terms-2026-11-01","appId":null},{"documentId":"rules","version":"rules-2","appId":"7"}]})");
  const auto missing = graphql::legalAcceptanceRequiredOf(legal.errors);
  CHECK(missing.has_value());
  CHECK_EQ(missing->missingDocuments.size(), std::size_t{2});
  CHECK_EQ(missing->missingDocuments.at(0).documentId, std::string("player-terms"));
  CHECK(missing->missingDocuments.at(0).appId.empty());
  CHECK_EQ(missing->missingDocuments.at(1).appId, std::string("7"));
  CHECK(!graphql::ageRequiredOf(legal.errors));

  // An older API names no documents; the refusal still reads.
  const auto older = refusal(R"({"code":"LEGAL_ACCEPTANCE_REQUIRED"})");
  CHECK(graphql::legalAcceptanceRequiredOf(older.errors).has_value());
  CHECK(graphql::legalAcceptanceRequiredOf(older.errors)->missingDocuments.empty());

  const auto territory = refusal(R"({"code":"TERRITORY_NOT_SUPPORTED","country":"DE"})");
  CHECK_EQ(graphql::territoryNotSupportedOf(territory.errors)->country, std::string("DE"));
  CHECK(!graphql::territoryNotSupportedOf(older.errors).has_value());

  CHECK(graphql::ageRequiredOf(refusal(R"({"code":"AGE_REQUIRED"})").errors));
  CHECK(graphql::ageRequirementNotMetOf(refusal(R"({"code":"AGE_REQUIREMENT_NOT_MET"})").errors));
  CHECK(graphql::safetyLogRequiredOf(refusal(R"({"code":"SAFETY_LOG_REQUIRED"})").errors));
  CHECK(graphql::parentalConsentUnavailableOf(
      refusal(R"({"code":"PARENTAL_CONSENT_UNAVAILABLE"})").errors));
  CHECK(graphql::socialLimitReachedOf(refusal(R"({"code":"SOCIAL_LIMIT_REACHED"})").errors));
  CHECK(graphql::minecraftVerificationUnavailableOf(
      refusal(R"({"code":"MINECRAFT_VERIFICATION_UNAVAILABLE"})").errors));
  const auto failed =
      refusal(R"({"code":"MINECRAFT_VERIFICATION_FAILED","reason":"not_joined"})");
  CHECK_EQ(graphql::minecraftVerificationFailedOf(failed.errors)->reason, std::string("not_joined"));
  CHECK(!graphql::minecraftVerificationFailedOf(territory.errors).has_value());

#ifndef CROWDY_NO_EXCEPTIONS
  // The blocking wrappers throw the same details.
  Rig rig;
  rig.http->replies["MyCommSettings"] =
      R"({"errors":[{"message":"no","extensions":{"code":"TERRITORY_NOT_SUPPORTED","country":"GB"}}]})";
  bool threw = false;
  try {
    rig.client->safety().commSettings("7");
  } catch (const graphql::CrowdyGraphQLError& e) {
    threw = true;
    CHECK_EQ(graphql::territoryNotSupportedOf(e.errors())->country, std::string("GB"));
  }
  CHECK(threw);
#endif
}

void testStudioGitHubBatch() {
#ifndef CROWDY_NO_EXCEPTIONS
  Rig rig;
  rig.http->replies["CrowdyStudioGitHubFiles"] =
      R"({"data":{"crowdyStudioGitHubFiles":{"commitSha":"abc","missing":["gone.rs"],"files":[{"path":"a.txt","sha":"s1","size":3,"binary":false,"content":"hey"},{"path":"b.png","sha":"s2","size":9,"binary":true,"content":null}]}}})";
  rig.http->replies["CrowdyStudioGitHubCommitFiles"] =
      R"({"data":{"crowdyStudioGitHubCommitFiles":{"commitSha":"def","parentSha":"abc","written":["a.txt"],"deleted":["b.png"]}}})";
  rig.http->replies["CrowdyStudioGitHubLayout"] =
      R"({"data":{"crowdyStudioGitHubLayout":{"commitSha":"abc","server":".","client":null,"assets":"assets","fromFile":false,"kind":"CONSTRUCT_GAME"}}})";
  auto& github = rig.client->crowdyStudioGitHub();

  const auto files = github.getFiles({"7", "p1", {"a.txt", "b.png", "gone.rs"}, std::nullopt});
  graphql::Json body = rig.http->last();
  CHECK(sent(body, "crowdyStudioGitHubFiles"));
  CHECK_EQ(body["variables"]["input"]["paths"].size(), std::size_t{3});
  CHECK(!body["variables"]["input"]["commitSha"].ok());
  CHECK_EQ(files.files.size(), std::size_t{2});
  CHECK_EQ(files.files.at(0).content.value_or(""), std::string("hey"));
  CHECK(files.files.at(1).binary);
  CHECK(!files.files.at(1).content.has_value());
  CHECK_EQ(files.missing.at(0), std::string("gone.rs"));

  studio::CrowdyStudioGitHubCommitFilesInput commit;
  commit.appId = "7";
  commit.projectId = "p1";
  commit.message = "edit";
  commit.expectedCommitSha = "abc";
  commit.changes = {{"a.txt", std::string("new"), false}, {"b.png", std::nullopt, true}};
  const auto made = github.commitFiles(commit);
  body = rig.http->last();
  CHECK(sent(body, "crowdyStudioGitHubCommitFiles"));
  CHECK_EQ(body["variables"]["input"]["changes"].at(0)["content"].asString(), std::string("new"));
  CHECK(!body["variables"]["input"]["changes"].at(0)["delete"].ok());
  CHECK(body["variables"]["input"]["changes"].at(1)["delete"].asBool());
  CHECK(!body["variables"]["input"]["changes"].at(1)["content"].ok());
  CHECK_EQ(made.commitSha, std::string("def"));
  CHECK_EQ(made.deleted.at(0), std::string("b.png"));

  const auto layout = github.layout({"7", "p1"});
  CHECK(layout.kind == gen::CrowdyStudioGitHubLayoutKind::CONSTRUCT_GAME);
#endif
}

}  // namespace

int main() {
  testCommSettings();
  testContactsAndBlocks();
  testParentalConsentAndReports();
  testModeration();
  testIdentity();
  testMinecraftSignIn();
  testAppsRecordingAndDocuments();
  testRefusals();
  testStudioGitHubBatch();
  std::puts("teen_safety_test: ok");
  return 0;
}
