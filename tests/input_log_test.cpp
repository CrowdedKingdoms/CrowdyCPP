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

// CrowdyJS 18.13.0: every sessions read selects missingRecords (an older Game API refuses
// the selection, so this needs the release after v2.40.2).
void testSessionsSelectMissingRecords() {
  auto transport = transportWith(
      R"({"data":{"inputLogSessions":{"edges":[{"cursor":"c1","node":{"appId":"42","gameTokenId":"9001","userId":"7","datacenter":"or","startedAt":"2026-10-09T10:00:00.000Z","lastSeenAt":"2026-10-09T10:05:00.000Z","endedAt":"2026-10-09T10:06:00.000Z","endReason":"paused","messageCount":"1500","byteCount":"210000","missingRecords":"3","messageTypes":[26]}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":"c1"},"totalCount":1}}})");
  auto client = clientOn(transport);
  const graphql::Json page = client.inputLog().sessions("42");

  CHECK(contains(transport->requests[0].body, "missingRecords"));
  CHECK_EQ(page["edges"].at(0)["node"]["missingRecords"].asString(), "3");
  CHECK_EQ(page["edges"].at(0)["node"]["endReason"].asString(), "paused");
}

graphql::HttpOutcome okAnswer(std::string body) {
  graphql::HttpOutcome answer;
  answer.status = Errc::Ok;
  answer.response.status = 200;
  answer.response.body = std::move(body);
  return answer;
}

CrowdyClient asyncClient(const std::shared_ptr<DeferredTransport>& async) {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.transport = transportWith("{}");
  config.asyncTransport = async;
  return CrowdyClient(config);
}

// The token rides the request to the transport, and a read cancelled while in flight
// reports cancelled even when its transport ignored the token and answered.
void testCancelInFlightReportsCancelled() {
  auto async = std::make_shared<DeferredTransport>();
  auto client = asyncClient(async);
  graphql::CancelToken reading;
  bool called = false;
  client.inputLog().messagesAsync("42", "9001", 50, {}, {}, reading,
                                  [&](graphql::GraphQLOutcome out) {
                                    called = true;
                                    CHECK(!out.ok());
                                    CHECK(out.kind == graphql::GraphQLErrorKind::Cancelled);
                                    CHECK(out.status.code == Errc::Cancelled);
                                  });
  CHECK_EQ(async->requests.size(), std::size_t{1});
  CHECK(async->requests[0].cancel.has_value());
  CHECK(!async->requests[0].cancel->cancelled());

  reading.cancel();
  CHECK(async->requests[0].cancel->cancelled());
  auto callback = std::move(async->pending.front());
  callback(okAnswer(
      R"({"data":{"inputLogMessages":{"edges":[],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":null},"totalCount":null}}})"));
  client.poll();
  CHECK(called);
}

// A token cancelled before the call sends nothing, and an uncancelled token changes nothing.
void testTokenBeforeTheCall() {
  auto async = std::make_shared<DeferredTransport>();
  auto client = asyncClient(async);
  graphql::CancelToken cancelled;
  cancelled.cancel();
  bool refused = false;
  client.inputLog().sessionsAsync("42", 10, {}, {}, cancelled, [&](graphql::GraphQLOutcome out) {
    refused = true;
    CHECK(out.kind == graphql::GraphQLErrorKind::Cancelled);
  });
  client.poll();
  CHECK(refused);
  CHECK_EQ(async->requests.size(), std::size_t{0});

  graphql::CancelToken idle;
  bool answered = false;
  client.inputLog().sessionsAsync("42", 10, {}, {}, idle, [&](graphql::GraphQLOutcome out) {
    answered = true;
    CHECK(out.ok());
  });
  CHECK_EQ(async->requests.size(), std::size_t{1});
  auto callback = std::move(async->pending.front());
  callback(okAnswer(
      R"({"data":{"inputLogSessions":{"edges":[],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":null},"totalCount":0}}})"));
  client.poll();
  CHECK(answered);
}

std::vector<graphql::GraphQLErrorDetail> refusalOf(const std::string& extensions) {
  auto async = std::make_shared<DeferredTransport>();
  auto client = asyncClient(async);
  std::vector<graphql::GraphQLErrorDetail> errors;
  client.inputLog().messagesAsync("42", "9001", 50, {}, {}, [&](graphql::GraphQLOutcome out) {
    errors = out.errors;
  });
  auto callback = std::move(async->pending.front());
  callback(okAnswer(R"({"errors":[{"message":"refused","extensions":)" + extensions + "}]}"));
  client.poll();
  return errors;
}

// The two read codes worth repeating with the same cursor, and the funds refusal's figures.
void testRetryableCodesAndFunds() {
  CHECK(graphql::inputLogReadRetryable(
      refusalOf(R"({"code":"INPUT_LOG_RATE_LIMITED","retryable":true})")));
  CHECK(graphql::inputLogReadRetryable(
      refusalOf(R"({"code":"INPUT_LOG_TEMPORARILY_UNAVAILABLE","retryable":true})")));
  CHECK(!graphql::inputLogReadRetryable(
      refusalOf(R"({"code":"INPUT_LOG_TEMPORARILY_UNAVAILABLE","retryable":false})")));
  CHECK(!graphql::inputLogReadRetryable(refusalOf(R"({"code":"BAD_USER_INPUT"})")));
  CHECK(!graphql::inputLogReadRetryable(refusalOf(R"({"code":"INPUT_LOG_UNAVAILABLE"})")));

  const auto funds = graphql::inputLogFundsNeededOf(refusalOf(
      R"({"code":"INPUT_LOG_FUNDS_NEEDED","requiredMicrousd":"2500000","spendableMicrousd":"0"})"));
  CHECK(funds.has_value());
  CHECK_EQ(funds->requiredMicrousd, "2500000");
  CHECK_EQ(funds->spendableMicrousd, "0");
  CHECK(!graphql::inputLogFundsNeededOf(refusalOf(R"({"code":"FORBIDDEN"})")).has_value());
}

#ifndef CROWDY_NO_EXCEPTIONS
// The blocking reads throw CrowdyCancelledError: before the call (nothing sent), and when
// the token is cancelled while the transport is still working.
void testBlockingReadsThrowWhenCancelled() {
  auto transport = transportWith("{}");
  auto client = clientOn(transport);
  graphql::CancelToken cancelled;
  cancelled.cancel();
  bool threw = false;
  try {
    (void)client.inputLog().messages("42", "9001", 50, {}, {}, cancelled);
  } catch (const graphql::CrowdyCancelledError& e) {
    threw = true;
    CHECK_EQ(e.code(), "CANCELLED");
  }
  CHECK(threw);
  CHECK_EQ(transport->requests.size(), std::size_t{0});

  class CancellingTransport final : public graphql::IHttpTransport {
   public:
    graphql::HttpResponse send(const graphql::HttpRequest& request) override {
      if (request.cancel) request.cancel->cancel();
      return {200,
              R"({"data":{"inputLogSessions":{"edges":[],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":null},"totalCount":0}}})"};
    }
  };
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.transport = std::make_shared<CancellingTransport>();
  CrowdyClient midFlight(config);
  threw = false;
  try {
    (void)midFlight.inputLog().sessions("42", 10, {}, {}, graphql::CancelToken{});
  } catch (const graphql::CrowdyCancelledError&) {
    threw = true;
  }
  CHECK(threw);
}

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
  testSessionsSelectMissingRecords();
  testCancelInFlightReportsCancelled();
  testTokenBeforeTheCall();
  testRetryableCodesAndFunds();
#ifndef CROWDY_NO_EXCEPTIONS
  testBlockingReadsThrowWhenCancelled();
  testRefusalsCarryTheirCode();
#endif
  return 0;
}
