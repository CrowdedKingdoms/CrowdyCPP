// A Studio dock's platform calls (0.86.0): the live-modding check, the model list, the
// Studio origin, the starter projects and the async draft test / deploy runner.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio_origin.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include <chrono>
#include <map>
#include <thread>

#include "crowdy/domains/crowdy_studio.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/studio/mod_runner.hpp"
#include "crowdy/studio/starter_projects.hpp"
#endif
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::domains;

namespace {

struct Sent {
  std::string method;
  std::string url;
  std::string authorization;
  std::string accept;
  graphql::Json body;
};

/// Records each request and answers it with `answer` (`{"data":{}}` when unset).
class ScriptedHttp final : public graphql::IHttpTransport {
 public:
  std::vector<Sent> sent;
  std::function<graphql::HttpResponse(const Sent&)> answer;

  graphql::HttpResponse send(const graphql::HttpRequest& r) override {
    Sent s;
    s.method = r.method;
    s.url = r.url;
    for (const auto& [name, value] : r.headers) {
      if (name == "Authorization") s.authorization = value;
      if (name == "Accept") s.accept = value;
    }
    s.body = graphql::Json::parse(r.body);
    sent.push_back(s);
    return answer ? answer(sent.back()) : graphql::HttpResponse{200, R"({"data":{}})"};
  }
};

graphql::HttpResponse ok(std::string body) { return {200, std::move(body)}; }

/// A client on the scripted transport: async requests complete inline and their callbacks
/// wait in the dispatcher, as a game's do until it polls.
struct Harness {
  std::shared_ptr<ScriptedHttp> http = std::make_shared<ScriptedHttp>();
  std::shared_ptr<graphql::AuthState> auth = std::make_shared<graphql::AuthState>();
  std::shared_ptr<graphql::Dispatcher> dispatcher = std::make_shared<graphql::Dispatcher>();
  std::shared_ptr<graphql::GraphQLClient> gql;

  explicit Harness(std::size_t maxBytes = graphql::kDefaultMaxResponseBytes) {
    graphql::GraphQLClientConfig config{"http://test/graphql", 1000, maxBytes};
    gql = std::make_shared<graphql::GraphQLClient>(config, http, auth);
    gql->setDispatcher(dispatcher);
    gql->setAsyncTransport(graphql::makeInlineAsyncTransport(http));
    auth->setToken("tok");
  }
};

void testLiveModding() {
  CHECK_EQ(toString(CrowdyStudioLiveModdingAction::DraftTest), std::string_view("draft_test"));
  CHECK_EQ(toString(CrowdyStudioLiveModdingAction::DeployLive), std::string_view("deploy_live"));

  Harness h;
  CrowdyStudioAgentAPI agent(h.gql, h.dispatcher);
  h.http->answer = [](const Sent&) { return ok(R"({"data":{"crowdyStudioLiveModding":true}})"); };
  CHECK(agent.liveModding("77", CrowdyStudioLiveModdingAction::DraftTest));
  const Sent& sent = h.http->sent.back();
  CHECK_EQ(sent.method, std::string("POST"));
  CHECK_EQ(sent.authorization, std::string("Bearer tok"));
  CHECK_EQ(sent.body["operationName"].asString(), std::string("CrowdyStudioLiveModding"));
  CHECK_EQ(sent.body["variables"]["appId"].asString(), std::string("77"));
  CHECK_EQ(sent.body["variables"]["action"].asString(), std::string("draft_test"));
  CHECK(sent.body["query"].asString().find(
            "crowdyStudioLiveModding(appId: $appId, action: $action)") != std::string::npos);

  // Async: the outcome's data is the Boolean, delivered when the game drains its dispatcher.
  std::optional<graphql::GraphQLOutcome> got;
  agent.liveModdingAsync("77", CrowdyStudioLiveModdingAction::DeployLive,
                         [&](graphql::GraphQLOutcome outcome) { got = std::move(outcome); });
  CHECK(!got);
  h.dispatcher->drain();
  CHECK(got && got->ok());
  CHECK(got->data.isBool() && got->data.asBool());
  CHECK_EQ(h.http->sent.back().body["variables"]["action"].asString(), std::string("deploy_live"));

  // A refusal is a GraphQL error carrying its code.
  h.http->answer = [](const Sent&) {
    return ok(R"({"errors":[{"message":"Agentic Crowdy Studio is disabled by Management policy.","extensions":{"code":"AGENT_DISABLED","retryable":false}}],"data":null})");
  };
  got.reset();
  agent.liveModdingAsync("77", "draft_test", [&](graphql::GraphQLOutcome outcome) { got = std::move(outcome); });
  h.dispatcher->drain();
  CHECK(got && got->kind == graphql::GraphQLErrorKind::GraphQL);
  CHECK_EQ(got->errors.at(0).code, std::string("AGENT_DISABLED"));
  CHECK(!got->errors.at(0).retryable);
#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    (void)agent.liveModding("77", CrowdyStudioLiveModdingAction::DraftTest);
  } catch (const graphql::CrowdyGraphQLError& error) {
    threw = error.code() == "AGENT_DISABLED";
  }
  CHECK(threw);
#else
  CHECK(!agent.liveModding("77", CrowdyStudioLiveModdingAction::DraftTest));
#endif
}

constexpr const char* kModelList =
    R"({"object":"list","data":[)"
    R"({"id":"openai/gpt-oss-120b","object":"model","owned_by":"crowded-kingdoms","name":"openai/gpt-oss-120b","context_window":null,"input_modalities":["text"],"pricing_microusd_per_million":{"input":195000,"output":780000,"reasoning":780000,"cached_input":97500}},)"
    R"({"id":"x/vision","object":"model","owned_by":"crowded-kingdoms","name":"Vision","context_window":131072,"input_modalities":["text","image"],"pricing_microusd_per_million":{"input":1,"output":2,"reasoning":3,"cached_input":4}},)"
    R"({"object":"model","name":"no id"}]})";

void testModels() {
  Harness h;
  CrowdyStudioAgentAPI agent(h.gql, h.dispatcher);
  h.http->answer = [](const Sent&) { return ok(kModelList); };
  CrowdyStudioModelList list = agent.models("77");
  CHECK(list.ok());
  CHECK_EQ(list.httpStatus, 200);
  const Sent& sent = h.http->sent.back();
  CHECK_EQ(sent.method, std::string("GET"));
  CHECK_EQ(sent.url, std::string("http://test/v1/model/models?appId=77"));
  CHECK_EQ(sent.authorization, std::string("Bearer tok"));
  CHECK_EQ(sent.accept, std::string("application/json"));
  // An entry without an id is not a model.
  CHECK_EQ(list.models.size(), 2u);
  const CrowdyStudioModel& first = list.models.at(0);
  CHECK_EQ(first.id, std::string("openai/gpt-oss-120b"));
  CHECK_EQ(first.name, first.id);
  CHECK(!first.contextWindow);
  CHECK(first.inputModalities == std::vector<std::string>{"text"});
  CHECK_EQ(first.pricingMicrousdPerMillion.input, 195000);
  CHECK_EQ(first.pricingMicrousdPerMillion.output, 780000);
  CHECK_EQ(first.pricingMicrousdPerMillion.reasoning, 780000);
  CHECK_EQ(first.pricingMicrousdPerMillion.cachedInput, 97500);
  const CrowdyStudioModel& second = list.models.at(1);
  CHECK_EQ(second.name, std::string("Vision"));
  CHECK(second.contextWindow == std::optional<std::int64_t>(131072));
  CHECK(second.inputModalities == (std::vector<std::string>{"text", "image"}));

  // An origin has no userinfo.
  CHECK(h.gql->setEndpoint("https://user:secret@ck-va.example.com:8443/graphql"));
  CHECK_EQ(h.gql->origin(), std::string("https://ck-va.example.com:8443"));

  // The origin moves with the endpoint (a datacenter move), and the bearer is the one held
  // when the request is sent.
  CHECK(h.gql->setEndpoint("https://ck-va.example.com:8443/graphql"));
  CHECK_EQ(h.gql->origin(), std::string("https://ck-va.example.com:8443"));
  h.auth->setToken("tok2");
  std::optional<CrowdyStudioModelList> got;
  agent.modelsAsync("7 7", [&](CrowdyStudioModelList answer) { got = std::move(answer); });
  CHECK(!got);
  h.dispatcher->drain();
  CHECK(got && got->ok());
  CHECK_EQ(h.http->sent.back().url,
           std::string("https://ck-va.example.com:8443/v1/model/models?appId=7%207"));
  CHECK_EQ(h.http->sent.back().authorization, std::string("Bearer tok2"));

  // The endpoint's refusals carry the status, code, message, type and retryable flag.
  h.http->answer = [](const Sent&) {
    return graphql::HttpResponse{
        403,
        R"({"error":{"message":"Agentic Crowdy Studio is disabled by Management policy.","type":"crowded_kingdoms_error","code":"AGENT_DISABLED","retryable":false}})"};
  };
  list = agent.models("77");
  CHECK(list.status.code == Errc::Rejected);
  CHECK_EQ(list.httpStatus, 403);
  CHECK(list.error && list.error->code == "AGENT_DISABLED");
  CHECK_EQ(list.error->type, std::string("crowded_kingdoms_error"));
  CHECK_EQ(list.error->message, std::string("Agentic Crowdy Studio is disabled by Management policy."));
  CHECK(!list.error->retryable);
  CHECK(list.models.empty());
  // The body's flag wins over the status.
  h.http->answer = [](const Sent&) {
    return graphql::HttpResponse{
        503,
        R"({"error":{"message":"stale","type":"crowded_kingdoms_error","code":"AGENT_PROVIDER_POLICY_UNSATISFIED","retryable":false}})"};
  };
  list = agent.models("77");
  CHECK(list.error && list.error->code == "AGENT_PROVIDER_POLICY_UNSATISFIED" && !list.error->retryable);
  // A body that is not the envelope: a guard's default, or a gateway's page.
  h.http->answer = [](const Sent&) {
    return graphql::HttpResponse{401, R"({"statusCode":401,"message":"Unauthorized"})"};
  };
  list = agent.models("77");
  CHECK(list.error && list.error->message == "Unauthorized");
  CHECK(list.error->code.empty() && !list.error->retryable);
  h.http->answer = [](const Sent&) {
    return graphql::HttpResponse{502, "<html>" + std::string(2000, 'x')};
  };
  list = agent.models("77");
  CHECK(list.error && list.error->retryable);
  CHECK_EQ(list.error->message.size(), 512u);
  // A 200 that is not a list.
  h.http->answer = [](const Sent&) { return ok(R"({"object":"list"})"); };
  list = agent.models("77");
  CHECK(list.status.code == Errc::Malformed);
  // A list keeps at most kCrowdyStudioModelListMax models.
  h.http->answer = [](const Sent&) {
    std::string many = R"({"object":"list","data":[)";
    for (int i = 0; i < 1100; ++i) many += (i ? "," : "") + std::string(R"({"id":"m)") + std::to_string(i) + "\"}";
    return ok(many + "]}");
  };
  list = agent.models("77");
  CHECK(list.ok());
  CHECK_EQ(list.models.size(), kCrowdyStudioModelListMax);

  // Cancelled before it is sent: nothing goes out, sync or async.
  graphql::CancelToken cancel;
  cancel.cancel();
  const std::size_t before = h.http->sent.size();
  list = agent.models("77", graphql::RequestOptions{cancel});
  CHECK(list.status.code == Errc::Cancelled);
  got.reset();
  agent.modelsAsync("77", graphql::RequestOptions{cancel},
                    [&](CrowdyStudioModelList answer) { got = std::move(answer); });
  h.dispatcher->drain();
  CHECK(got && got->status.code == Errc::Cancelled);
  CHECK_EQ(h.http->sent.size(), before);

  // A body over the client's limit is refused unparsed.
  Harness small(64);
  CrowdyStudioAgentAPI limited(small.gql, small.dispatcher);
  small.http->answer = [](const Sent&) { return ok(kModelList); };
  list = limited.models("77");
  CHECK(list.status.code == Errc::ResponseTooLarge);
  CHECK(list.models.empty());

  // A transport failure is the transport's.
  graphql::HttpOutcome timedOut;
  timedOut.status = Errc::Timeout;
  timedOut.errorMessage = "timed out";
  list = crowdyStudioModelListFrom(timedOut);
  CHECK(list.status.code == Errc::Timeout);
  CHECK_EQ(list.errorMessage, std::string("timed out"));
}

// The tier's Studio from the API in use: `ck.<root>` serves `studio.<root>` (CrowdyJS's
// defaultHostedSignInUrl), a local API's Studio is on :3001, anything else has no convention.
void testStudioOrigin() {
  using Origin = std::optional<std::string>;
  CHECK(studioOriginFor("https://ck.dev.crowdedkingdoms.com") == Origin("https://studio.dev.crowdedkingdoms.com"));
  CHECK(studioOriginFor("https://ck.prod.crowdedkingdoms.com/graphql") ==
        Origin("https://studio.prod.crowdedkingdoms.com"));
  CHECK(studioOriginFor("HTTPS://CK.Test.CrowdedKingdoms.com:443/graphql?x=1") ==
        Origin("https://studio.test.crowdedkingdoms.com"));
  // A datacenter's or an instance's host, as a token's game API URL names one.
  CHECK(studioOriginFor("https://ck-va.dev.crowdedkingdoms.com") == Origin("https://studio.dev.crowdedkingdoms.com"));
  CHECK(studioOriginFor("https://ck-api-va-1.prod.crowdedkingdoms.com/graphql") ==
        Origin("https://studio.prod.crowdedkingdoms.com"));
  CHECK(studioOriginFor("http://localhost:3000/graphql") == Origin("http://localhost:3001"));
  CHECK(studioOriginFor("http://127.0.0.1:3000") == Origin("http://127.0.0.1:3001"));
  CHECK(!studioOriginFor("https://game.example.com/graphql"));
  CHECK(!studioOriginFor("https://cka.example.com/graphql"));
  CHECK(!studioOriginFor("https://ck."));
  CHECK(!studioOriginFor("https://ck"));
  CHECK(!studioOriginFor(""));
  CHECK(defaultStudioOrigin() == studioOriginFor(kDefaultHttpOrigin).value_or(""));
  CHECK_EQ(defaultStudioOrigin().rfind("https://studio.", 0), 0u);
  CHECK_EQ(studioWalletUrl("https://studio.dev.crowdedkingdoms.com/"),
           std::string("https://studio.dev.crowdedkingdoms.com/account/wallet"));
  CHECK_EQ(kStudioWalletPath, std::string_view("/account/wallet"));

  // The client's follows its endpoint, a move included.
  ClientConfig config;
  config.httpUrl = "https://ck.test.crowdedkingdoms.com";
  config.transport = std::make_shared<ScriptedHttp>();
  CrowdyClient client(std::move(config));
  CHECK_EQ(client.studioOrigin(), std::string("https://studio.test.crowdedkingdoms.com"));
  CHECK(client.graphqlClient().setEndpoint("http://localhost:3000/graphql"));
  CHECK_EQ(client.studioOrigin(), std::string("http://localhost:3001"));
  CHECK(client.graphqlClient().setEndpoint("https://game.example.com/graphql"));
  CHECK(client.studioOrigin().empty());
}

#ifndef CROWDY_NO_EXCEPTIONS
graphql::Json readFixture(const std::string& name) {
  std::ifstream in(std::string(CROWDY_PARITY_FIXTURE_DIR) + "/" + name, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return graphql::Json::parse(text.str());
}

studio::CrowdyStudioProjectKind kindOf(std::string_view kind) {
  if (kind == "SERVER") return studio::CrowdyStudioProjectKind::Server;
  if (kind == "CLIENT") return studio::CrowdyStudioProjectKind::Client;
  return studio::CrowdyStudioProjectKind::FullStack;
}

void expectSame(const std::string& fixtureCase, const char* field, const std::string& got,
                const std::string& want) {
  if (got == want) return;
  std::fprintf(stderr, "starter case %s: %s is\n%s\nnot\n%s\n", fixtureCase.c_str(), field,
               got.c_str(), want.c_str());
  CHECK(false);
}

std::string optionalText(const std::optional<std::string>& value) {
  return value ? "=" + *value : "(none)";
}

std::string optionalText(const graphql::Json& value) {
  return value.isString() ? "=" + value.asString() : "(none)";
}

// CrowdyJS's createCrowdyStudioStarterProject, replayed case by case from the shared fixture.
void testStarterFixture() {
  const graphql::Json fixture = readFixture("crowdy-studio-starter.v1.json");
  CHECK(fixture.ok());
  CHECK_EQ(fixture["contractVersion"].asString(), std::string("crowdy.studio-starter/1"));
  std::size_t cases = 0;
  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const std::string name = fixtureCase["name"].asString();
    const graphql::Json given = fixtureCase["options"];
    studio::CrowdyStudioNewProjectOptions options;
    options.appId = given["appId"].asString();
    options.gridId = given["gridId"].asString();
    options.name = given["name"].asString();
    options.kind = kindOf(given["kind"].asStringView());
    if (given["description"].isString()) options.description = given["description"].asString();
    if (given["modStarter"].isString()) {
      options.modStarter =
          studio::crowdyStudioModStarterOf(fixture["modStarters"][given["modStarter"].asStringView()]);
    }
    const studio::CreateCrowdyStudioProjectInput input = studio::createCrowdyStudioStarterProject(options);
    const graphql::Json want = fixtureCase["expected"];
    const graphql::Json metadata = want["metadata"];
    expectSame(name, "appId", input.appId, want["appId"].asString());
    expectSame(name, "gridId", optionalText(input.gridId), optionalText(want["gridId"]));
    expectSame(name, "kind", input.kind == studio::CrowdyStudioProjectKind::Server ? "SERVER"
                             : input.kind == studio::CrowdyStudioProjectKind::Client ? "CLIENT"
                                                                                      : "FULL_STACK",
               want["kind"].asString());
    expectSame(name, "name", input.metadata.name, metadata["name"].asString());
    expectSame(name, "description", optionalText(input.metadata.description),
               optionalText(metadata["description"]));
    expectSame(name, "serverModuleName", optionalText(input.metadata.serverModuleName),
               optionalText(metadata["serverModuleName"]));
    expectSame(name, "clientModuleName", optionalText(input.metadata.clientModuleName),
               optionalText(metadata["clientModuleName"]));
    expectSame(name, "pairingPreference", std::string(studio::toString(input.metadata.pairingPreference)),
               metadata["pairingPreference"].asString());
    CHECK_EQ(input.files.size(), want["files"].size());
    std::size_t index = 0;
    want["files"].forEach([&](const graphql::Json& file) {
      const studio::CrowdyStudioProjectFile& made = input.files.at(index++);
      expectSame(name, "file target", std::string(studio::toString(made.target)), file["target"].asString());
      expectSame(name, "file path", made.path, file["path"].asString());
      expectSame(name, "file content", made.content, file["content"].asString());
    });
    ++cases;
  });
  CHECK_EQ(cases, 10u);

  // The CLIENT starter on its own, and the two refusals CrowdyJS makes.
  const auto client = studio::crowdyStudioClientStarterFiles("x-client");
  CHECK_EQ(client.size(), 2u);
  CHECK(client.at(0).content.find("name = \"x-client\"") != std::string::npos);
  CHECK(client.at(0).content.find("crowdy-client-sdk = \"0.1.0\"") != std::string::npos);
  studio::CrowdyStudioNewProjectOptions server;
  server.name = "s";
  server.kind = studio::CrowdyStudioProjectKind::Server;
  std::string refused;
  try {
    (void)studio::createCrowdyStudioStarterProject(server);
  } catch (const std::invalid_argument& error) {
    refused = error.what();
  }
  CHECK_EQ(refused, std::string("A SERVER target starts from the mod starter (client.exec().modStarter)"));
  server.modStarter = studio::crowdyStudioModStarterOf(fixture["modStarters"]["no-library"]);
  refused.clear();
  try {
    (void)studio::createCrowdyStudioStarterProject(server);
  } catch (const std::invalid_argument& error) {
    refused = error.what();
  }
  CHECK_EQ(refused, std::string("The mod starter has no Cargo.toml or src/lib.rs"));
}

constexpr const char* kModStarterAnswer =
    R"({"data":{"execModStarter":{"crate":"grid-mod","nodeType":"mod:grid-mod","description":"d","files":[{"path":"Cargo.toml","content":"[package]\nname = \"grid-mod\"\n"},{"path":"src/lib.rs","content":"fn grid() {}\n"}]}}})";

std::string createdProject(std::string_view name) {
  return R"({"data":{"crowdyStudioProjectCreate":{"projectId":"p9","appId":"77","ownerUserId":"42","gridId":"5","name":")" +
         std::string(name) +
         R"(","description":null,"serverModuleName":"turret-server","clientModuleName":"turret-client","pairingPreference":"INDEPENDENT","sdkVersion":"0.1.8","abiVersion":0,"revision":"1","archived":false,"archivedAt":null,"fileCount":4,"totalBytes":"10","createdAt":"t","updatedAt":"t","files":[]}}})";
}

constexpr const char* kClientStarterAnswer =
    R"({"data":{"execModClientStarter":{"crate":"client-half","nodeType":"client","description":"d","files":[{"path":"Cargo.toml","content":"[package]\nname = \"client-half\"\n"},{"path":"src/lib.rs","content":"fn platform() {}\n"}]}}})";

constexpr const char* kUnavailable =
    R"({"errors":[{"message":"no starter","extensions":{"code":"INTERNAL_SERVER_ERROR","httpStatus":503}}],"data":null})";

// The async create: the mod starter for a SERVER target and the CLIENT half starter for a
// CLIENT one, then the project, every answer from the dispatcher.
void testStarterProjectAsync() {
  Harness h;
  domains::ExecAPI exec(h.gql, nullptr);
  domains::CrowdyStudioAPI projects(h.gql);
  std::string clientStarter = kClientStarterAnswer;
  h.http->answer = [&clientStarter](const Sent& sent) {
    const std::string op = sent.body["operationName"].asString();
    if (op == "ExecModStarter") return ok(kModStarterAnswer);
    if (op == "ExecModClientStarter") return ok(clientStarter);
    if (op == "CrowdyStudioProjectCreate") return ok(createdProject("Turret"));
    return ok(R"({"data":{}})");
  };
  studio::CrowdyStudioNewProjectOptions options;
  options.appId = "77";
  options.gridId = "5";
  options.name = "Turret";
  options.kind = studio::CrowdyStudioProjectKind::FullStack;
  std::optional<graphql::GraphQLOutcome> outcome;
  studio::CrowdyStudioProject created;
  const auto create = [&] {
    outcome.reset();
    studio::createCrowdyStudioStarterProjectAsync(
        exec, projects, options, [&](graphql::GraphQLOutcome out, studio::CrowdyStudioProject project) {
          outcome = std::move(out);
          created = std::move(project);
        });
    CHECK(!outcome);
    for (int i = 0; i < 6 && !outcome; ++i) h.dispatcher->drain();
  };
  const auto sentInput = [&] { return h.http->sent.back().body["variables"]["input"]; };

  create();
  CHECK(outcome && outcome->ok());
  CHECK_EQ(created.projectId, std::string("p9"));
  CHECK_EQ(h.http->sent.size(), 3u);
  CHECK_EQ(h.http->sent.at(0).body["operationName"].asString(), std::string("ExecModStarter"));
  CHECK_EQ(h.http->sent.at(1).body["operationName"].asString(), std::string("ExecModClientStarter"));
  graphql::Json input = sentInput();
  CHECK_EQ(input["serverModuleName"].asString(), std::string("turret-server"));
  CHECK_EQ(input["clientModuleName"].asString(), std::string("turret-client"));
  CHECK_EQ(input["sdkVersion"].asString(), std::string("0.1.8"));
  CHECK_EQ(input["initialFiles"].size(), 4u);
  CHECK(input["initialFiles"].at(0)["content"].asString().find("name = \"turret-server\"") != std::string::npos);
  // The platform's CLIENT half starter, its package renamed to the CLIENT module.
  CHECK_EQ(input["initialFiles"].at(2)["target"].asString(), std::string("CLIENT"));
  CHECK_EQ(input["initialFiles"].at(2)["content"].asString(), std::string("[package]\nname = \"turret-client\"\n"));
  CHECK_EQ(input["initialFiles"].at(3)["content"].asString(), std::string("fn platform() {}\n"));

  // An API host whose toolchain predates the starter (503): the SDK's copy, the same crate.
  clientStarter = kUnavailable;
  options.kind = studio::CrowdyStudioProjectKind::Client;
  create();
  CHECK(outcome && outcome->ok());
  CHECK_EQ(h.http->sent.size(), 5u);
  CHECK_EQ(h.http->sent.at(3).body["operationName"].asString(), std::string("ExecModClientStarter"));
  input = sentInput();
  CHECK_EQ(input["initialFiles"].size(), 2u);
  CHECK_EQ(input["initialFiles"].at(0)["content"].asString(),
           studio::crowdyStudioClientStarterFiles("turret-client").at(0).content);

  // Any other refusal of the CLIENT half starter is the callback's outcome.
  clientStarter = R"({"errors":[{"message":"no access","extensions":{"code":"FORBIDDEN"}}],"data":null})";
  create();
  CHECK(outcome && outcome->kind == graphql::GraphQLErrorKind::GraphQL);
  CHECK_EQ(outcome->errors.at(0).code, std::string("FORBIDDEN"));
  CHECK_EQ(h.http->sent.size(), 6u);

  // A starter passed in is not fetched; a refused mod starter is the callback's outcome.
  options.clientStarter = studio::CrowdyStudioModStarter{};
  options.clientStarter->files = {{"Cargo.toml", "[package]\nname = \"x\"\n"}, {"src/lib.rs", "fn x() {}\n"}};
  create();
  CHECK(outcome && outcome->ok());
  CHECK_EQ(h.http->sent.size(), 7u);
  h.http->answer = [](const Sent&) { return ok(kUnavailable); };
  options.kind = studio::CrowdyStudioProjectKind::Server;
  create();
  CHECK(outcome && outcome->kind == graphql::GraphQLErrorKind::GraphQL);
  CHECK_EQ(outcome->errors.at(0).httpStatus, 503);
  CHECK_EQ(h.http->sent.size(), 8u);
}

// Game API v2.73.0: a build's Studio source, the CLIENT half starter, and the Studio links.
void testPlatformCalls() {
  Harness h;
  domains::ExecAPI exec(h.gql, nullptr);
  domains::CrowdyStudioAPI projects(h.gql);
  std::string links =
      R"({"data":{"crowdyStudioLinks":{"origin":"https://studio.prod.crowdedkingdoms.com","walletUrl":"https://studio.prod.crowdedkingdoms.com/account/wallet"}}})";
  h.http->answer = [&links](const Sent& sent) {
    const std::string op = sent.body["operationName"].asString();
    const std::string build =
        R"({"buildId":"b1","status":"queued","kind":"exec","log":null,"sdkVersion":null,"createdAt":"t","startedAt":null,"finishedAt":null,"artifacts":[],"studioProjectId":"p1","studioRevision":"3"})";
    if (op == "ExecModBuild") return ok(R"({"data":{"execModBuild":)" + build + "}}");
    if (op == "ExecModClientBuild") return ok(R"({"data":{"execModClientBuild":)" + build + "}}");
    if (op == "ExecModClientStarter") return ok(kClientStarterAnswer);
    if (op == "CrowdyStudioLinks") return ok(links);
    return ok(R"({"data":{}})");
  };
  const domains::ExecCrate crate{"turret", {{"Cargo.toml", "c"}, {"src/lib.rs", "l"}}};
  const domains::ExecStudioSource source{"p1", "3"};

  const graphql::Json built = exec.modBuild("77", crate, source);
  CHECK_EQ(built["studioProjectId"].asString(), std::string("p1"));
  CHECK_EQ(built["studioRevision"].asString(), std::string("3"));
  graphql::Json studio = h.http->sent.back().body["variables"]["studio"];
  CHECK_EQ(studio["projectId"].asString(), std::string("p1"));
  CHECK_EQ(studio["revision"].asString(), std::string("3"));
  (void)exec.modBuild("77", crate);
  CHECK(h.http->sent.back().body["variables"]["studio"].isNull());

  std::optional<graphql::GraphQLOutcome> outcome;
  exec.modClientBuildAsync("77", crate, source, [&](graphql::GraphQLOutcome out) { outcome = std::move(out); });
  for (int i = 0; i < 4 && !outcome; ++i) h.dispatcher->drain();
  CHECK(outcome && outcome->ok());
  CHECK_EQ(h.http->sent.back().body["operationName"].asString(), std::string("ExecModClientBuild"));
  CHECK_EQ(h.http->sent.back().body["variables"]["studio"]["revision"].asString(), std::string("3"));

  const graphql::Json starter = exec.modClientStarter("77");
  CHECK_EQ(starter["crate"].asString(), std::string("client-half"));
  CHECK_EQ(studio::crowdyStudioModStarterOf(starter).files.size(), 2u);

  const CrowdyStudioLinks platform = projects.links();
  CHECK_EQ(platform.origin, std::string("https://studio.prod.crowdedkingdoms.com"));
  CHECK_EQ(platform.walletUrl, std::string("https://studio.prod.crowdedkingdoms.com/account/wallet"));
  CHECK_EQ(h.http->sent.back().body["operationName"].asString(), std::string("CrowdyStudioLinks"));
  // An answer without an origin is malformed, not an empty link.
  links = R"({"data":{"crowdyStudioLinks":{"origin":"","walletUrl":""}}})";
  outcome.reset();
  CrowdyStudioLinks empty;
  projects.linksAsync([&](graphql::GraphQLOutcome out, CrowdyStudioLinks got) {
    outcome = std::move(out);
    empty = std::move(got);
  });
  for (int i = 0; i < 4 && !outcome; ++i) h.dispatcher->drain();
  CHECK(outcome && !outcome->ok() && outcome->status.code == Errc::Malformed);
  CHECK(empty.origin.empty());
}

graphql::HttpResponse refusalOf(std::string_view code, int httpStatus = 0) {
  std::string extensions = R"("code":")" + std::string(code) + "\"";
  if (httpStatus) extensions += R"(,"httpStatus":)" + std::to_string(httpStatus);
  return ok(R"({"errors":[{"message":"refused )" + std::string(code) + R"(","extensions":{)" + extensions +
            R"(}}],"data":null})");
}

/// A read that drops: nothing comes back, or a gateway's page, or the API says it is busy.
std::function<graphql::HttpResponse()> networkDrop() {
  return []() -> graphql::HttpResponse { throw graphql::CrowdyNetworkError("connection reset by peer"); };
}
std::function<graphql::HttpResponse()> timeoutDrop() {
  return []() -> graphql::HttpResponse { throw graphql::CrowdyTimeoutError("timed out after 1000 ms"); };
}
std::function<graphql::HttpResponse()> answerDrop(graphql::HttpResponse response) {
  return [response] { return response; };
}
graphql::HttpResponse busyRefusal(int retryAfterMs = 0) {
  return ok(R"({"errors":[{"message":"the platform is busy; retry shortly","extensions":{"code":"PLATFORM_BUSY","blame":"PLATFORM","retryable":true)" +
            (retryAfterMs ? R"(,"retryAfterMs":)" + std::to_string(retryAfterMs) : std::string()) +
            R"(}}],"data":null})");
}

/// The Game API a run talks to: one project, its builds (building, then succeeded unless told
/// otherwise), the player's mods and the CLIENT half's attachment.
struct Backend {
  std::string kind = "FULL_STACK";
  std::string revision = "7";
  std::optional<std::string> githubSha;
  bool legacyClient = false;
  std::map<std::string, std::string> buildEnds{{"cb1", "succeeded"}, {"sb1", "succeeded"}, {"rb1", "succeeded"}};
  std::map<std::string, int> statusCalls;
  std::map<std::string, graphql::HttpResponse> refuse;
  /// What an operation answers first, in turn, before answering normally: each throws (no
  /// answer comes back) or returns a response.
  std::map<std::string, std::vector<std::function<graphql::HttpResponse()>>> drops;
  std::string riderMods = "[]";
  bool liveModdingAllowed = true;
  /// The deployed and switched-on mod's `blocked` and `running`.
  std::optional<std::string> blocked;
  bool running = true;

  std::string project() const {
    const bool server = kind != "CLIENT";
    const bool client = kind != "SERVER";
    std::string files;
    const auto add = [&](const char* target, const char* path, const std::string& content) {
      if (!files.empty()) files += ",";
      files += R"({"target":")" + std::string(target) + R"(","path":")" + path + R"(","content":)" +
               graphql::JVal(content).dump() + R"(,"revision":"1","provenance":"AUTHORED"})";
    };
    if (server) {
      add("SERVER", "Cargo.toml", "[package]\nname = \"turret-server\"\n[dependencies]\nckx-sdk = \"0.14.1\"\n");
      add("SERVER", "src/lib.rs", "fn server() {}\n");
      add("SERVER", "notes.txt", "not part of the crate");
    }
    if (client) {
      add("CLIENT", "Cargo.toml",
          legacyClient ? "[dependencies]\ncrowdy-compute-sdk = \"0.2\"\n"
                       : "[package]\nname = \"turret-client\"\n[dependencies]\ncrowdy-client-sdk = \"0.1.0\"\n");
      add("CLIENT", "src/lib.rs", "fn client() {}\n");
    }
    const std::string pairing = kind == "SERVER" ? "SERVER_ONLY" : kind == "CLIENT" ? "CLIENT_ONLY" : "INDEPENDENT";
    std::string github;
    if (githubSha) {
      github = R"(,"source":"GITHUB","githubOwner":"o","githubRepo":"r","githubBranch":"main","githubSha":")" +
               *githubSha + "\"";
    }
    return R"({"data":{"crowdyStudioProject":{"projectId":"p1","appId":"77","ownerUserId":"42","gridId":"5","name":"Turret","description":null,"serverModuleName":)" +
           std::string(server ? R"("turret-server")" : "null") + R"(,"clientModuleName":)" +
           (client ? (kind == "CLIENT" ? R"("hud-client")" : R"("turret-client")") : "null") +
           R"(,"pairingPreference":")" + pairing + R"(","sdkVersion":"0.1.8","abiVersion":0,"revision":")" + revision +
           R"(","archived":false,"archivedAt":null,"fileCount":5,"totalBytes":"100","createdAt":"t","updatedAt":"t")" +
           github + R"(,"files":[)" + files + "]}}}";
  }

  static std::string build(const std::string& id, const std::string& status, const std::string& log) {
    return R"({"buildId":")" + id + R"(","status":")" + status + R"(","kind":"exec","log":)" +
           (log.empty() ? std::string("null") : graphql::JVal(log).dump()) +
           R"(,"sdkVersion":null,"createdAt":"t","startedAt":null,"finishedAt":null,"artifacts":[]})";
  }

  graphql::HttpResponse answer(const Sent& sent) {
    const std::string op = sent.body["operationName"].asString();
    const auto dropped = drops.find(op);
    if (dropped != drops.end() && !dropped->second.empty()) {
      auto next = std::move(dropped->second.front());
      dropped->second.erase(dropped->second.begin());
      return next();
    }
    const auto refused = refuse.find(op);
    if (refused != refuse.end()) return refused->second;
    const graphql::Json vars = sent.body["variables"];
    if (op == "CrowdyStudioProject") return ok(project());
    if (op == "CrowdyStudioLiveModding") {
      return liveModdingAllowed ? ok(R"({"data":{"crowdyStudioLiveModding":true}})")
                                : refusalOf("AGENT_DISABLED");
    }
    if (op == "ExecModClientBuild") return ok(R"({"data":{"execModClientBuild":)" + build("cb1", "queued", "") + "}}");
    if (op == "ExecModBuild") {
      const bool rider = vars["crate"]["name"].asString() == "hud-client";
      return ok(R"({"data":{"execModBuild":)" + build(rider ? "rb1" : "sb1", "queued", "") + "}}");
    }
    if (op == "ExecModBuildStatus") {
      const std::string id = vars["buildId"].asString();
      const bool finished = ++statusCalls[id] >= 2;
      const std::string end = buildEnds[id];
      const std::string log = end == "failed" ? "error[E0308]: mismatched types\n --> src/lib.rs:3:5\n"
                                              : "warning: unused variable: `x`\n --> src/lib.rs:2:9\n";
      return ok(R"({"data":{"execModBuildStatus":)" + build(id, finished ? end : "building", finished ? log : "") +
                "}}");
    }
    const std::string mod = R"({"modId":"900","gridId":"5","name":")" + vars["name"].asString() +
                            R"(","ownerId":"42","version":3,"digest":"sd1","enabled":true,"listingId":null,"blocked":)" +
                            (blocked ? graphql::JVal(*blocked).dump() : std::string("null")) + R"(,"running":)" +
                            (running ? "true" : "false") + R"(,"updatedAt":"t"})";
    if (op == "ExecModDeploy") return ok(R"({"data":{"execModDeploy":)" + mod + "}}");
    if (op == "ExecModSetEnabled") return ok(R"({"data":{"execModSetEnabled":)" + mod + "}}");
    if (op == "ExecMyMods") return ok(R"({"data":{"execMyMods":)" + riderMods + "}}");
    if (op == "ExecModStarter") return ok(kModStarterAnswer);
    if (op == "ExecModClientDeploy") {
      return ok(R"({"data":{"execModClientDeploy":{"modId":"900","gridId":"5","name":")" + vars["name"].asString() +
                R"(","ownerId":"42","clientVersion":2,"digest":"cd1","sizeBytes":8,"capabilitySummaryJson":"{}","capabilityHash":"h1","tickIntervalMs":1000,"updatedAt":"t"}}})");
    }
    if (op == "ExecConsentClientMod") return ok(R"({"data":{"execConsentClientMod":true}})");
    return ok(R"({"data":{}})");
  }
};

/// A runner over the scripted API; runs advance as the dispatcher is drained.
struct RunnerHarness {
  Harness h;
  Backend backend;
  std::unique_ptr<studio::CrowdyStudioModRunner> runner;
  std::vector<std::string> steps;

  RunnerHarness() {
    h.http->answer = [this](const Sent& sent) { return backend.answer(sent); };
    runner = std::make_unique<studio::CrowdyStudioModRunner>(
        std::make_shared<domains::ExecAPI>(h.gql, nullptr), std::make_shared<domains::CrowdyStudioAPI>(h.gql),
        std::make_shared<CrowdyStudioAgentAPI>(h.gql, h.dispatcher), h.dispatcher);
  }

  static studio::CrowdyStudioRunRequest request() {
    studio::CrowdyStudioRunRequest request;
    request.scope = {"77", "5"};
    request.projectId = "p1";
    request.expectedRevisionId = "7";
    request.pollIntervalMs = 0;
    return request;
  }

  studio::CrowdyStudioRunHandle start(studio::CrowdyStudioRunRequest request,
                                      std::optional<studio::CrowdyStudioRunResult>& result) {
    return runner->run(
        std::move(request), [&result](studio::CrowdyStudioRunResult done) { result = std::move(done); },
        [this](const studio::CrowdyStudioRunProgress& progress) {
          std::string step(studio::toString(progress.step));
          if (progress.target) step += ":" + std::string(studio::toString(*progress.target));
          steps.push_back(step);
        });
  }

  /// Drain until `done` holds or two seconds pass.
  template <typename Predicate>
  void drainUntil(Predicate done) {
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done() && std::chrono::steady_clock::now() < giveUp) {
      if (h.dispatcher->drain() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::vector<std::string> operations() const {
    std::vector<std::string> names;
    for (const auto& sent : h.http->sent) names.push_back(sent.body["operationName"].asString());
    return names;
  }

  const Sent& first(std::string_view operation) const {
    for (const auto& sent : h.http->sent) {
      if (sent.body["operationName"].asString() == operation) return sent;
    }
    std::fprintf(stderr, "no %s was sent\n", std::string(operation).c_str());
    std::exit(1);
  }
};

void expectOperations(const RunnerHarness& r, const std::vector<std::string>& want) {
  const auto got = r.operations();
  if (got == want) return;
  std::fprintf(stderr, "operations:\n");
  for (const auto& name : got) std::fprintf(stderr, "  %s\n", name.c_str());
  CHECK(false);
}

// The full sequence: the CLIENT half built first, then the SERVER half built, deployed and
// switched on, then the CLIENT half attached and consented to as its author.
void testRunFullStack() {
  RunnerHarness r;
  std::optional<studio::CrowdyStudioRunResult> result;
  auto handle = r.start(RunnerHarness::request(), result);
  CHECK(handle && !handle.done());
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  CHECK(handle.done());
  CHECK_EQ(result->message, std::string("Draft test is running"));
  CHECK_EQ(result->projectRevisionId, std::string("7"));
  expectOperations(r, {"CrowdyStudioProject", "ExecModClientBuild", "ExecModBuildStatus", "ExecModBuildStatus",
                       "ExecModBuild", "ExecModBuildStatus", "ExecModBuildStatus", "ExecModDeploy",
                       "ExecModSetEnabled", "ExecModClientDeploy", "ExecConsentClientMod"});
  // Each build takes only its target's crate files.
  const graphql::Json clientCrate = r.first("ExecModClientBuild").body["variables"]["crate"];
  CHECK_EQ(clientCrate["name"].asString(), std::string("turret-client"));
  CHECK_EQ(clientCrate["files"].size(), 2u);
  const graphql::Json serverCrate = r.first("ExecModBuild").body["variables"]["crate"];
  CHECK_EQ(serverCrate["name"].asString(), std::string("turret-server"));
  CHECK_EQ(serverCrate["files"].size(), 2u);
  // Both builds name the project and the revision they are, which what is deployed reports.
  for (const char* build : {"ExecModClientBuild", "ExecModBuild"}) {
    const graphql::Json studio = r.first(build).body["variables"]["studio"];
    CHECK_EQ(studio["projectId"].asString(), std::string("p1"));
    CHECK_EQ(studio["revision"].asString(), std::string("7"));
  }
  CHECK_EQ(r.first("ExecModDeploy").body["variables"]["buildId"].asString(), std::string("sb1"));
  CHECK(r.first("ExecModSetEnabled").body["variables"]["enabled"].asBool());
  const graphql::Json attach = r.first("ExecModClientDeploy").body["variables"];
  CHECK_EQ(attach["name"].asString(), std::string("turret-server"));
  CHECK_EQ(attach["buildId"].asString(), std::string("cb1"));
  CHECK_EQ(r.first("ExecConsentClientMod").body["variables"]["capabilityHash"].asString(), std::string("h1"));

  CHECK_EQ(result->targets.size(), 2u);
  const studio::CrowdyStudioRunTarget& client = result->targets.at(0);
  CHECK(client.target == studio::CrowdyStudioTarget::Client);
  CHECK(client.phase == studio::CrowdyStudioRunStep::Done);
  CHECK_EQ(client.moduleName, std::string("turret-client"));
  CHECK_EQ(client.modName, std::string("turret-server"));
  CHECK_EQ(client.buildId, std::string("cb1"));
  CHECK_EQ(client.compileStatus, std::string("succeeded"));
  CHECK_EQ(client.digest, std::string("cd1"));
  CHECK_EQ(client.version, 2);
  CHECK_EQ(client.capabilityHash, std::string("h1"));
  CHECK_EQ(client.diagnostics.size(), 1u);
  CHECK(client.buildLog.find("unused variable") != std::string::npos);
  const studio::CrowdyStudioRunTarget& server = result->targets.at(1);
  CHECK(server.target == studio::CrowdyStudioTarget::Server);
  CHECK_EQ(server.buildId, std::string("sb1"));
  CHECK_EQ(server.digest, std::string("sd1"));
  CHECK_EQ(server.version, 3);
  CHECK(r.steps == (std::vector<std::string>{"ReadingProject", "Building:CLIENT", "Building:SERVER",
                                             "Deploying:SERVER", "Enabling:SERVER", "Attaching:CLIENT",
                                             "Consenting:CLIENT", "Done"}));
}

// A failed compile is a result, and a failed CLIENT build publishes nothing.
void testRunCompileFailed() {
  RunnerHarness r;
  r.backend.buildEnds["cb1"] = "failed";
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(RunnerHarness::request(), result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::CompileFailed);
  expectOperations(r, {"CrowdyStudioProject", "ExecModClientBuild", "ExecModBuildStatus", "ExecModBuildStatus"});
  const studio::CrowdyStudioRunTarget& client = result->targets.at(0);
  CHECK_EQ(client.compileStatus, std::string("failed"));
  CHECK_EQ(client.message, std::string("turret-client failed to compile"));
  CHECK_EQ(client.diagnostics.size(), 1u);
  CHECK(client.diagnostics.at(0).severity == studio::CrowdyStudioDiagnosticSeverity::Error);
  CHECK(result->targets.at(1).buildId.empty());

  // A crate on legacy player compute fails before anything is built.
  RunnerHarness legacy;
  legacy.backend.legacyClient = true;
  result.reset();
  legacy.start(RunnerHarness::request(), result);
  legacy.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::CompileFailed);
  CHECK_EQ(result->targets.at(0).buildLog, std::string(studio::kCrowdyStudioLegacyClientCrate));
  expectOperations(legacy, {"CrowdyStudioProject"});
}

// The pin: a project that moved since the caller read it is refused before anything is built.
void testRunRevisionPin() {
  RunnerHarness r;
  r.backend.revision = "8";
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(RunnerHarness::request(), result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::RevisionConflict);
  CHECK_EQ(result->projectRevisionId, std::string("8"));
  expectOperations(r, {"CrowdyStudioProject"});

  // A GitHub-bound project pins by its mirror commit.
  RunnerHarness bound;
  bound.backend.revision = "9";
  bound.backend.githubSha = "def";
  auto request = RunnerHarness::request();
  request.expectedRevisionId.clear();
  request.expectedGithubSha = "abc";
  result.reset();
  bound.start(request, result);
  bound.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::RevisionConflict);
  request.expectedGithubSha = "def";
  result.reset();
  bound.start(request, result);
  bound.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  CHECK(result->githubSha == std::optional<std::string>("def"));

  // A request without a pin is refused at once.
  request.expectedGithubSha.reset();
  bool refused = false;
  try {
    bound.start(request, result);
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  CHECK(refused);
}

// RATE_LIMITED and a full build queue are their own outcomes; other refusals carry their code.
void testRunRefusals() {
  RunnerHarness limited;
  limited.backend.refuse["ExecModClientBuild"] = refusalOf("RATE_LIMITED", 429);
  std::optional<studio::CrowdyStudioRunResult> result;
  limited.start(RunnerHarness::request(), result);
  limited.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::RateLimited);
  CHECK(result->error && result->error->code == "RATE_LIMITED");

  RunnerHarness full;
  full.backend.refuse["ExecModBuild"] = refusalOf("INTERNAL_SERVER_ERROR", 503);
  result.reset();
  full.start(RunnerHarness::request(), result);
  full.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::QueueFull);
  CHECK(result->error && result->error->httpStatus == 503);
  // The CLIENT half built, but nothing was published.
  CHECK_EQ(result->targets.at(0).compileStatus, std::string("succeeded"));
  CHECK(result->targets.at(0).digest.empty());

  RunnerHarness forbidden;
  forbidden.backend.refuse["ExecModDeploy"] = refusalOf("FORBIDDEN", 403);
  result.reset();
  forbidden.start(RunnerHarness::request(), result);
  forbidden.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Refused);
  CHECK(result->error && result->error->code == "FORBIDDEN");

  // The live-modding check, when asked, comes first and its refusal stops the run.
  RunnerHarness dock;
  dock.backend.liveModdingAllowed = false;
  auto request = RunnerHarness::request();
  request.checkLiveModding = true;
  request.deployment = studio::CrowdyStudioDeployment::Live;
  result.reset();
  dock.start(request, result);
  dock.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Refused);
  CHECK(result->error && result->error->code == "AGENT_DISABLED");
  expectOperations(dock, {"CrowdyStudioProject", "CrowdyStudioLiveModding"});
  CHECK_EQ(dock.first("CrowdyStudioLiveModding").body["variables"]["action"].asString(), std::string("deploy_live"));

  // A build that never finishes within the poll limit fails.
  RunnerHarness slow;
  slow.backend.buildEnds["cb1"] = "building";
  request = RunnerHarness::request();
  request.pollLimit = 3;
  result.reset();
  slow.start(request, result);
  slow.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Failed);
  CHECK_EQ(result->message, std::string("Build cb1 did not finish within 3 polls"));
}

// A CLIENT-only project's CLIENT half rides the mod named for its CLIENT module.
void testRunClientOnly() {
  RunnerHarness fresh;
  fresh.backend.kind = "CLIENT";
  std::optional<studio::CrowdyStudioRunResult> result;
  fresh.start(RunnerHarness::request(), result);
  fresh.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  expectOperations(fresh, {"CrowdyStudioProject", "ExecModClientBuild", "ExecModBuildStatus", "ExecModBuildStatus",
                           "ExecMyMods", "ExecModStarter", "ExecModBuild", "ExecModBuildStatus",
                           "ExecModBuildStatus", "ExecModDeploy", "ExecModSetEnabled", "ExecModClientDeploy",
                           "ExecConsentClientMod"});
  CHECK_EQ(fresh.first("ExecModBuild").body["variables"]["crate"]["name"].asString(), std::string("hud-client"));
  // The rider mod is the mod starter, not the project's code: it names no Studio source.
  CHECK(fresh.first("ExecModBuild").body["variables"]["studio"].isNull());
  CHECK_EQ(fresh.first("ExecModDeploy").body["variables"]["name"].asString(), std::string("hud-client"));
  CHECK_EQ(fresh.first("ExecModClientDeploy").body["variables"]["name"].asString(), std::string("hud-client"));
  CHECK_EQ(result->targets.size(), 1u);
  CHECK_EQ(result->targets.at(0).modName, std::string("hud-client"));

  // A rider already switched on is used as it is.
  RunnerHarness running;
  running.backend.kind = "CLIENT";
  running.backend.riderMods = R"([{"modId":"901","gridId":"5","name":"hud-client","enabled":true}])";
  result.reset();
  running.start(RunnerHarness::request(), result);
  running.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  expectOperations(running, {"CrowdyStudioProject", "ExecModClientBuild", "ExecModBuildStatus",
                             "ExecModBuildStatus", "ExecMyMods", "ExecModClientDeploy", "ExecConsentClientMod"});
}

// Cancelling stops at the next step; destroying the runner ends its runs without calling back.
void testRunCancelAndDestroy() {
  RunnerHarness r;
  auto request = RunnerHarness::request();
  request.pollIntervalMs = 30;
  std::optional<studio::CrowdyStudioRunResult> result;
  auto handle = r.start(request, result);
  r.drainUntil([&] { return r.h.http->sent.size() >= 2; });
  handle.cancel();
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Cancelled);
  CHECK(handle.done());
  for (const auto& name : r.operations()) CHECK(name != "ExecModBuild" && name != "ExecModDeploy");

  RunnerHarness gone;
  bool called = false;
  gone.runner->run(RunnerHarness::request(), [&](studio::CrowdyStudioRunResult) { called = true; });
  gone.h.dispatcher->drain();
  gone.runner.reset();
  for (int i = 0; i < 20; ++i) {
    gone.h.dispatcher->drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!called);
}

std::size_t countOf(const RunnerHarness& r, std::string_view operation) {
  const auto names = r.operations();
  return static_cast<std::size_t>(std::count(names.begin(), names.end(), operation));
}

// A dropped status read (no answer, a gateway's 5xx, a body that does not parse, PLATFORM_BUSY)
// is a missed poll; a refusal still ends the run, and a limit reached after misses names the
// last one.
void testRunDroppedStatusReads() {
  RunnerHarness r;
  r.backend.drops["ExecModBuildStatus"] = {networkDrop(), timeoutDrop(), answerDrop({502, "<html>Bad Gateway</html>"}),
                                           answerDrop(ok("not json")), answerDrop(busyRefusal(1))};
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(RunnerHarness::request(), result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  // Five misses, then the two reads each build takes.
  CHECK_EQ(countOf(r, "ExecModBuildStatus"), 9u);
  CHECK_EQ(countOf(r, "ExecModDeploy"), 1u);

  // A refusal is an answer.
  RunnerHarness refused;
  refused.backend.refuse["ExecModBuildStatus"] = refusalOf("NOT_FOUND", 404);
  result.reset();
  refused.start(RunnerHarness::request(), result);
  refused.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Refused);
  CHECK(result->error && result->error->code == "NOT_FOUND");
  CHECK_EQ(countOf(refused, "ExecModBuildStatus"), 1u);

  // So is a 4xx without a GraphQL body, and a busy answer that is not the platform's.
  for (graphql::HttpResponse answer :
       {graphql::HttpResponse{401, "Unauthorized"},
        ok(R"({"errors":[{"message":"too slow","extensions":{"code":"USER_CODE_TOO_SLOW","blame":"AUTHOR","retryable":false}}],"data":null})")}) {
    RunnerHarness once;
    once.backend.refuse["ExecModBuildStatus"] = answer;
    result.reset();
    once.start(RunnerHarness::request(), result);
    once.drainUntil([&] { return result.has_value(); });
    CHECK(result && !result->ok());
    CHECK_EQ(countOf(once, "ExecModBuildStatus"), 1u);
  }

  RunnerHarness down;
  for (int i = 0; i < 3; ++i) down.backend.drops["ExecModBuildStatus"].push_back(networkDrop());
  auto request = RunnerHarness::request();
  request.pollLimit = 3;
  result.reset();
  down.start(request, result);
  down.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Failed);
  CHECK_EQ(result->message,
           std::string("Build cb1 did not finish within 3 polls; the last status read that failed: "
                       "connection reset by peer"));
  CHECK(result->targets.at(0).state == studio::CrowdyStudioRunTargetState::Building);
}

// The project, live-modding, mod-listing and mod-starter reads are read once more after a
// dropped read; a second drop ends the run, and a mutation is never repeated.
void testRunReadsOnceMore() {
  RunnerHarness r;
  r.backend.drops["CrowdyStudioProject"] = {networkDrop()};
  r.backend.drops["CrowdyStudioLiveModding"] = {answerDrop(busyRefusal())};
  auto request = RunnerHarness::request();
  request.checkLiveModding = true;
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(request, result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  CHECK_EQ(countOf(r, "CrowdyStudioProject"), 2u);
  CHECK_EQ(countOf(r, "CrowdyStudioLiveModding"), 2u);

  RunnerHarness twice;
  twice.backend.drops["CrowdyStudioProject"] = {networkDrop(), answerDrop({503, "Service Unavailable"})};
  result.reset();
  twice.start(RunnerHarness::request(), result);
  twice.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Failed);
  CHECK_EQ(result->message, std::string("Reading the project failed: HTTP 503"));
  expectOperations(twice, {"CrowdyStudioProject", "CrowdyStudioProject"});

  RunnerHarness rider;
  rider.backend.kind = "CLIENT";
  rider.backend.drops["ExecMyMods"] = {timeoutDrop()};
  rider.backend.drops["ExecModStarter"] = {answerDrop({504, "Gateway Timeout"})};
  result.reset();
  rider.start(RunnerHarness::request(), result);
  rider.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  CHECK_EQ(countOf(rider, "ExecMyMods"), 2u);
  CHECK_EQ(countOf(rider, "ExecModStarter"), 2u);

  RunnerHarness deploy;
  deploy.backend.drops["ExecModDeploy"] = {networkDrop()};
  result.reset();
  deploy.start(RunnerHarness::request(), result);
  deploy.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Failed);
  CHECK_EQ(countOf(deploy, "ExecModDeploy"), 1u);
  CHECK(result->targets.at(1).state == studio::CrowdyStudioRunTargetState::Built);
}

// Each target reports what the platform holds at the end, and the mod it is or rides.
void testRunTargetStates() {
  using State = studio::CrowdyStudioRunTargetState;
  RunnerHarness r;
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(RunnerHarness::request(), result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  const studio::CrowdyStudioRunTarget& client = result->targets.at(0);
  const studio::CrowdyStudioRunTarget& server = result->targets.at(1);
  CHECK(client.state == State::Consented);
  CHECK(server.state == State::SwitchedOn);
  for (const auto* entry : {&client, &server}) {
    CHECK_EQ(entry->modId, std::string("900"));
    CHECK(entry->enabled == std::optional<bool>(true));
    CHECK(entry->running == std::optional<bool>(true));
    CHECK(!entry->blocked);
  }
  CHECK_EQ(studio::toString(State::Consented), std::string_view("Consented"));

  // A refusal settles each target where the platform left it.
  const auto settled = [](const std::string& refusedOp) {
    RunnerHarness one;
    one.backend.refuse[refusedOp] = refusalOf("FORBIDDEN", 403);
    std::optional<studio::CrowdyStudioRunResult> done;
    one.start(RunnerHarness::request(), done);
    one.drainUntil([&] { return done.has_value(); });
    CHECK(done && done->outcome == studio::CrowdyStudioRunOutcome::Refused);
    return std::make_pair(done->targets.at(0).state, done->targets.at(1).state);
  };
  CHECK(settled("ExecModClientBuild") == std::make_pair(State::NotStarted, State::NotStarted));
  CHECK(settled("ExecModBuild") == std::make_pair(State::Built, State::NotStarted));
  CHECK(settled("ExecModDeploy") == std::make_pair(State::Built, State::Built));
  CHECK(settled("ExecModSetEnabled") == std::make_pair(State::Built, State::Deployed));
  CHECK(settled("ExecModClientDeploy") == std::make_pair(State::Built, State::SwitchedOn));
  CHECK(settled("ExecConsentClientMod") == std::make_pair(State::Attached, State::SwitchedOn));

  RunnerHarness failed;
  failed.backend.buildEnds["cb1"] = "failed";
  result.reset();
  failed.start(RunnerHarness::request(), result);
  failed.drainUntil([&] { return result.has_value(); });
  CHECK(result->targets.at(0).state == State::BuildFailed);
  CHECK(result->targets.at(1).state == State::NotStarted);

  // A CLIENT-only project's CLIENT target carries the rider mod's facts.
  RunnerHarness rider;
  rider.backend.kind = "CLIENT";
  rider.backend.riderMods = R"([{"modId":"901","gridId":"5","name":"hud-client","enabled":true,"running":false,"blocked":null}])";
  result.reset();
  rider.start(RunnerHarness::request(), result);
  rider.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->ok());
  CHECK_EQ(result->targets.at(0).modId, std::string("900"));  // the attach answer names it
  CHECK(result->targets.at(0).running == std::optional<bool>(false));
  CHECK(result->targets.at(0).state == State::Consented);
}

// A switch-on answer that says the mod is blocked: the run still attaches and consents, then
// ends Blocked with the reason.
void testRunBlocked() {
  RunnerHarness r;
  r.backend.blocked = "app 77 is switched off";
  r.backend.running = false;
  std::optional<studio::CrowdyStudioRunResult> result;
  r.start(RunnerHarness::request(), result);
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Blocked);
  CHECK(!result->ok());
  CHECK_EQ(result->message, std::string("Mod 'turret-server' is switched on but blocked: app 77 is switched off"));
  CHECK_EQ(countOf(r, "ExecModClientDeploy"), 1u);
  CHECK_EQ(countOf(r, "ExecConsentClientMod"), 1u);
  const studio::CrowdyStudioRunTarget& server = result->targets.at(1);
  CHECK(server.blocked == std::optional<std::string>("app 77 is switched off"));
  CHECK(server.running == std::optional<bool>(false));
  CHECK(server.enabled == std::optional<bool>(true));
  CHECK_EQ(server.message, result->message);
  CHECK(result->targets.at(0).blocked == server.blocked);
  CHECK(result->targets.at(0).state == studio::CrowdyStudioRunTargetState::Consented);
  CHECK_EQ(studio::toString(studio::CrowdyStudioRunOutcome::Blocked), std::string_view("Blocked"));

  // A CLIENT-only project's rider that is on already, but blocked.
  RunnerHarness rider;
  rider.backend.kind = "CLIENT";
  rider.backend.riderMods =
      R"([{"modId":"901","gridId":"5","name":"hud-client","enabled":true,"running":false,"blocked":"mods are switched off for grid 5"}])";
  result.reset();
  rider.start(RunnerHarness::request(), result);
  rider.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Blocked);
  CHECK(result->targets.at(0).blocked == std::optional<std::string>("mods are switched off for grid 5"));
}

// The module names the player saw pin the run as the revision does.
void testRunModulePins() {
  const auto outcomeOf = [](const std::string& kind, std::optional<std::string> server,
                            std::optional<std::string> client, std::string* message = nullptr) {
    RunnerHarness r;
    r.backend.kind = kind;
    auto request = RunnerHarness::request();
    request.expectedServerModuleName = std::move(server);
    request.expectedClientModuleName = std::move(client);
    std::optional<studio::CrowdyStudioRunResult> result;
    r.start(request, result);
    r.drainUntil([&] { return result.has_value(); });
    if (message) *message = result->message;
    return result->outcome;
  };
  using Outcome = studio::CrowdyStudioRunOutcome;
  CHECK(outcomeOf("FULL_STACK", "turret-server", "turret-client") == Outcome::Running);
  std::string message;
  CHECK(outcomeOf("FULL_STACK", "turret", std::nullopt, &message) == Outcome::RevisionConflict);
  CHECK_EQ(message, std::string("The project's SERVER module is 'turret-server', not 'turret'"));
  CHECK(outcomeOf("FULL_STACK", std::nullopt, "hud", &message) == Outcome::RevisionConflict);
  CHECK_EQ(message, std::string("The project's CLIENT module is 'turret-client', not 'hud'"));
  CHECK(outcomeOf("SERVER", "turret-server", "turret-client", &message) == Outcome::RevisionConflict);
  CHECK_EQ(message, std::string("The project has no CLIENT module, not 'turret-client'"));
}

// Whatever throws inside a run ends it Failed through onDone, never out of poll().
void testRunExceptions() {
  RunnerHarness r;
  std::optional<studio::CrowdyStudioRunResult> result;
  int called = 0;
  r.runner->run(
      RunnerHarness::request(),
      [&](studio::CrowdyStudioRunResult done) {
        ++called;
        result = std::move(done);
      },
      [](const studio::CrowdyStudioRunProgress& progress) {
        if (progress.step == studio::CrowdyStudioRunStep::Deploying) throw std::runtime_error("the HUD is gone");
      });
  r.drainUntil([&] { return result.has_value(); });
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Failed);
  CHECK_EQ(result->message, std::string("The run stopped: the HUD is gone"));
  CHECK_EQ(result->targets.at(1).state, studio::CrowdyStudioRunTargetState::Built);

  // A throw at the first step, inside run() itself, ends the run the same way.
  RunnerHarness first;
  result.reset();
  bool threw = false;
  try {
    first.runner->run(
        RunnerHarness::request(), [&](studio::CrowdyStudioRunResult done) { result = std::move(done); },
        [](const studio::CrowdyStudioRunProgress&) { throw 42; });
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(result && result->message == "The run stopped on an exception");

  // What onDone throws stays inside the run.
  RunnerHarness loud;
  bool done = false;
  loud.runner->run(RunnerHarness::request(), [&](studio::CrowdyStudioRunResult) {
    done = true;
    throw std::runtime_error("the game's own bug");
  });
  loud.drainUntil([&] { return done; });
  CHECK(done);
  for (int i = 0; i < 5; ++i) loud.h.dispatcher->drain();
}

// A runner over a client of its own: its requests carry the app token it was given whatever the
// shared client holds, it follows WRONG_DATACENTER within the estate on its own, and
// setAppToken rotates the token.
void testRunAppTokenRunner() {
  auto http = std::make_shared<ScriptedHttp>();
  Backend backend;
  std::string moveTo = "https://ck-va2.dev.crowdedkingdoms.com";
  int redirects = 1;
  http->answer = [&](const Sent& sent) {
    if (redirects > 0 && sent.body["operationName"].asString() == "CrowdyStudioProject") {
      --redirects;
      return ok(R"({"errors":[{"message":"App 77 is served elsewhere","extensions":{"code":"WRONG_DATACENTER","appId":"77","appDatacenter":"va2","applicationApiUrl":")" +
                moveTo + R"("}}],"data":null})");
    }
    return backend.answer(sent);
  };
  ClientConfig config;
  config.httpUrl = "https://ck-va.dev.crowdedkingdoms.com";
  config.transport = http;
  config.asyncTransport = graphql::makeInlineAsyncTransport(http);
  CrowdyClient client(std::move(config));
  client.setToken("management-session");

  auto runner = client.createCrowdyStudioModRunner("app-token-1");
  std::optional<studio::CrowdyStudioRunResult> result;
  const auto drain = [&] {
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!result && std::chrono::steady_clock::now() < giveUp) {
      client.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  bool rotated = false;
  runner->run(
      RunnerHarness::request(), [&](studio::CrowdyStudioRunResult done) { result = std::move(done); },
      [&](const studio::CrowdyStudioRunProgress& progress) {
        // The engine switches the shared client to another plane mid-run, and the app token rotates.
        if (progress.step == studio::CrowdyStudioRunStep::Deploying && !rotated) {
          rotated = true;
          client.setToken("someone-else");
          runner->setAppToken("app-token-2");
        }
      });
  drain();
  CHECK(result && result->ok());
  CHECK(rotated);
  bool sawSecond = false;
  for (const auto& sent : http->sent) {
    CHECK(sent.authorization == "Bearer app-token-1" || sent.authorization == "Bearer app-token-2");
    if (sent.authorization == "Bearer app-token-2") sawSecond = true;
  }
  CHECK(sawSecond);
  CHECK_EQ(http->sent.back().authorization, std::string("Bearer app-token-2"));
  // The redirect moved the runner's client, and the retry went to the new endpoint; the shared
  // client stayed where it was.
  CHECK_EQ(http->sent.at(0).url, std::string("https://ck-va.dev.crowdedkingdoms.com/graphql"));
  CHECK_EQ(http->sent.at(1).url, std::string("https://ck-va2.dev.crowdedkingdoms.com/graphql"));
  CHECK_EQ(http->sent.back().url, std::string("https://ck-va2.dev.crowdedkingdoms.com/graphql"));
  CHECK_EQ(client.graphqlClient().endpoint(), std::string("https://ck-va.dev.crowdedkingdoms.com/graphql"));
  CHECK_EQ(client.getToken(), std::string("someone-else"));

  // A move out of the estate is refused, and the run sees the server's refusal.
  moveTo = "https://ck.elsewhere.example";
  redirects = 1;
  http->sent.clear();
  auto stranger = client.createCrowdyStudioModRunner("app-token-3");
  result.reset();
  stranger->run(RunnerHarness::request(), [&](studio::CrowdyStudioRunResult done) { result = std::move(done); });
  drain();
  CHECK(result && result->outcome == studio::CrowdyStudioRunOutcome::Refused);
  CHECK(result->error && result->error->code == "WRONG_DATACENTER");
  CHECK_EQ(http->sent.size(), 1u);

  // A runner on the client's own session has no token of its own to set.
  auto shared = client.createCrowdyStudioModRunner();
  bool refused = false;
  try {
    shared->setAppToken("app-token-4");
  } catch (const std::logic_error&) {
    refused = true;
  }
  CHECK(refused);
  bool empty = false;
  try {
    (void)client.createCrowdyStudioModRunner(std::string());
  } catch (const std::invalid_argument&) {
    empty = true;
  }
  CHECK(empty);
  bool cleared = false;
  try {
    runner->setAppToken(std::string());
  } catch (const std::invalid_argument&) {
    cleared = true;
  }
  CHECK(cleared);
}
#endif

}  // namespace

int main() {
  testLiveModding();
  testModels();
  testStudioOrigin();
#ifndef CROWDY_NO_EXCEPTIONS
  testStarterFixture();
  testStarterProjectAsync();
  testPlatformCalls();
  testRunFullStack();
  testRunCompileFailed();
  testRunRevisionPin();
  testRunRefusals();
  testRunClientOnly();
  testRunCancelAndDestroy();
  testRunDroppedStatusReads();
  testRunReadsOnceMore();
  testRunTargetStates();
  testRunBlocked();
  testRunModulePins();
  testRunExceptions();
  testRunAppTokenRunner();
#endif
  std::printf("studio_dock_test: OK\n");
  return 0;
}
