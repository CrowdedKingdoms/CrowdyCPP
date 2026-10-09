#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

// The input log (ck-api inputLogSessions / inputLogMessages): the recorded client inputs
// of an app with replay logging on. The wrappers are thin; what they must get right is
// sending the paging and filters and returning the connection whole, because a messages
// page can be short, or empty, while hasNextPage is true.

namespace {

class InputLogTransport final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  std::string answer;
  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    return {200, answer};
  }
};

class DeferredTransport final : public graphql::IAsyncHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  std::vector<std::function<void(graphql::HttpOutcome)>> pending;
  void sendAsync(const graphql::HttpRequest& request,
                 std::function<void(graphql::HttpOutcome)> callback) override {
    requests.push_back(request);
    pending.push_back(std::move(callback));
  }
};

std::shared_ptr<InputLogTransport> transportWith(std::string answer) {
  auto transport = std::make_shared<InputLogTransport>();
  transport->answer = std::move(answer);
  return transport;
}

CrowdyClient clientOn(const std::shared_ptr<InputLogTransport>& transport) {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.transport = transport;
  return CrowdyClient(config);
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

void testSessionsSendsPagingAndFilter() {
  auto transport = transportWith(
      R"({"data":{"inputLogSessions":{"edges":[{"cursor":"c1","node":{"appId":"42","gameTokenId":"9001","userId":"7","datacenter":"or","startedAt":"2026-10-09T10:00:00.000Z","lastSeenAt":"2026-10-09T10:05:00.000Z","endedAt":null,"endReason":null,"messageCount":"1500","byteCount":"210000","messageTypes":[26,129]}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":null,"endCursor":"c1"},"totalCount":3}}})");
  auto client = clientOn(transport);
  graphql::JVal filter;
  filter["userId"] = "7";
  filter["messageType"] = std::int64_t{129};
  const graphql::Json page = client.inputLog().sessions("42", 1, "c0", filter);

  CHECK_EQ(page["totalCount"].asInt64(), 3);
  CHECK_EQ(page["pageInfo"]["endCursor"].asString(), "c1");
  CHECK_EQ(page["edges"].at(0)["node"]["gameTokenId"].asString(), "9001");
  CHECK_EQ(page["edges"].at(0)["node"]["messageTypes"].at(1).asInt64(), 129);

  CHECK_EQ(transport->requests.size(), std::size_t{1});
  const std::string& body = transport->requests[0].body;
  CHECK(contains(body, "inputLogSessions("));
  CHECK(contains(body, R"("appId":"42")"));
  CHECK(contains(body, R"("first":1)"));
  CHECK(contains(body, R"("after":"c0")"));
  CHECK(contains(body, R"("userId":"7")"));
  CHECK(contains(body, R"("messageType":129)"));
}

void testMessagesKeepsAShortPageWithHasNextPage() {
  auto transport = transportWith(
      R"({"data":{"inputLogMessages":{"edges":[],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":null,"endCursor":"aWw6MzoxMDAw"},"totalCount":null}}})");
  auto client = clientOn(transport);
  const graphql::Json page = client.inputLog().messages("42", "9001", 200);

  // An empty page that stopped at the server's scan limit is not the end of the session.
  CHECK_EQ(page["edges"].size(), std::size_t{0});
  CHECK(page["pageInfo"]["hasNextPage"].asBool());
  CHECK_EQ(page["pageInfo"]["endCursor"].asString(), "aWw6MzoxMDAw");

  const std::string& body = transport->requests[0].body;
  CHECK(contains(body, R"("gameTokenId":"9001")"));
  CHECK(contains(body, R"("first":200)"));
  CHECK(!contains(body, R"("after")"));
  for (const char* field : {"receivedAtMicros", "messageType", "seq", "fromBundle", "signed",
                            "sizeBytes", "body", "chunkX", "actorUuid", "channelId"}) {
    CHECK(contains(body, field));
  }
}

void testMessagesAsyncAnswersThroughTheCallback() {
  auto async = std::make_shared<DeferredTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.transport = transportWith("{}");
  config.asyncTransport = async;
  CrowdyClient client(config);

  graphql::JVal filter;
  graphql::JArray types;
  types.emplace_back(std::int64_t{129});
  filter["messageTypes"] = graphql::JVal(std::move(types));
  bool called = false;
  client.inputLog().messagesAsync("42", "9001", 50, "x0", filter,
                                  [&](graphql::GraphQLOutcome out) {
                                    called = true;
                                    CHECK(out.ok());
                                    CHECK_EQ(out.data["edges"].at(0)["node"]["sizeBytes"].asInt64(), 180);
                                    CHECK(!out.data["pageInfo"]["hasNextPage"].asBool());
                                  });
  CHECK_EQ(async->requests.size(), std::size_t{1});
  CHECK(contains(async->requests[0].body, R"("messageTypes":[129])"));
  CHECK(contains(async->requests[0].body, R"("after":"x0")"));

  graphql::HttpOutcome answer;
  answer.status = Errc::Ok;
  answer.response.status = 200;
  answer.response.body =
      R"({"data":{"inputLogMessages":{"edges":[{"cursor":"x","node":{"receivedAt":"2026-10-09T10:00:00.000Z","receivedAtMicros":"1791540000000000","userId":"7","gameTokenId":"9001","messageType":129,"seq":4,"fromBundle":false,"signed":true,"sizeBytes":180,"body":"gQ==","chunkX":"1","chunkY":"0","chunkZ":"-2","actorUuid":"0123456789abcdef0123456789abcdef","channelId":null}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":"x"},"totalCount":null}}})";
  auto callback = std::move(async->pending.front());
  callback(std::move(answer));
  client.poll();
  CHECK(called);
}

#ifndef CROWDY_NO_EXCEPTIONS
void testRefusalsCarryTheirCode() {
  for (const std::string code : {"INPUT_LOG_UNAVAILABLE", "NOT_FOUND"}) {
    auto transport = transportWith(R"({"errors":[{"message":"refused","extensions":{"code":")" + code +
                                   R"("}}]})");
    auto client = clientOn(transport);
    bool threw = false;
    try {
      (void)client.inputLog().messages("42", "9001");
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      CHECK_EQ(e.code(), code);
    }
    CHECK(threw);
  }
}
#endif

}  // namespace

int main() {
  testSessionsSendsPagingAndFilter();
  testMessagesKeepsAShortPageWithHasNextPage();
  testMessagesAsyncAnswersThroughTheCallback();
#ifndef CROWDY_NO_EXCEPTIONS
  testRefusalsCarryTheirCode();
#endif
  return 0;
}
