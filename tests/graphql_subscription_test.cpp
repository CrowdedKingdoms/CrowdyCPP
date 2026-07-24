#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/graphql/subscription_client.hpp"
#include "crowdy/graphql/websocket.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::graphql;

namespace {

class FakeConnection final : public IWebSocketConnection {
 public:
  void start(WebSocketEventCallback callback) override {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    started_ = true;
  }

  Status send(WebSocketFrame frame) override {
    std::lock_guard lock(mutex_);
    if (!sendStatus_.ok()) return sendStatus_;
    sent_.push_back(std::move(frame));
    return Errc::Ok;
  }

  void close(std::uint16_t code, std::string_view reason) override {
    std::lock_guard lock(mutex_);
    closes_.push_back(WebSocketCloseInfo{code, std::string(reason), true});
  }

  void emitOpen() {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Open;
    emit(std::move(event));
  }

  void emitText(std::string text) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Frame;
    event.frame = WebSocketFrame{WebSocketFrameKind::Text, std::move(text)};
    emit(std::move(event));
  }

  void emitFrame(WebSocketFrameKind kind, std::string payload) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Frame;
    event.frame = WebSocketFrame{kind, std::move(payload)};
    emit(std::move(event));
  }

  void emitClose(std::uint16_t code, std::string reason = {},
                 bool clean = false) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Close;
    event.close = WebSocketCloseInfo{code, std::move(reason), clean};
    emit(std::move(event));
  }

  void emitError(WebSocketErrorKind kind, bool retryable,
                 std::string message = "transport failed") {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Error;
    event.error.kind = kind;
    event.error.status =
        kind == WebSocketErrorKind::Timeout ? Errc::Timeout : Errc::SocketError;
    event.error.message = std::move(message);
    event.error.retryable = retryable;
    emit(std::move(event));
  }

  std::vector<WebSocketFrame> sent() const {
    std::lock_guard lock(mutex_);
    return sent_;
  }

  std::vector<WebSocketCloseInfo> closes() const {
    std::lock_guard lock(mutex_);
    return closes_;
  }

  bool started() const {
    std::lock_guard lock(mutex_);
    return started_;
  }

 private:
  void emit(WebSocketEvent event) {
    WebSocketEventCallback callback;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
    }
    CHECK(static_cast<bool>(callback));
    callback(std::move(event));
  }

  mutable std::mutex mutex_;
  WebSocketEventCallback callback_;
  std::vector<WebSocketFrame> sent_;
  std::vector<WebSocketCloseInfo> closes_;
  Status sendStatus_ = Errc::Ok;
  bool started_ = false;
};

class FakeTransport final : public IWebSocketTransport {
 public:
  std::shared_ptr<IWebSocketConnection> createConnection(
      const WebSocketConnectRequest& request) override {
    auto connection = std::make_shared<FakeConnection>();
    {
      std::lock_guard lock(mutex_);
      requests_.push_back(request);
      connections_.push_back(connection);
    }
    return connection;
  }

  std::size_t connectionCount() const {
    std::lock_guard lock(mutex_);
    return connections_.size();
  }

  std::shared_ptr<FakeConnection> connection(std::size_t index) const {
    std::lock_guard lock(mutex_);
    CHECK(index < connections_.size());
    return connections_[index];
  }

  WebSocketConnectRequest request(std::size_t index) const {
    std::lock_guard lock(mutex_);
    CHECK(index < requests_.size());
    return requests_[index];
  }

 private:
  mutable std::mutex mutex_;
  std::vector<WebSocketConnectRequest> requests_;
  std::vector<std::shared_ptr<FakeConnection>> connections_;
};

class FakeHttpTransport final : public IHttpTransport {
 public:
  HttpResponse send(const HttpRequest&) override {
    return HttpResponse{200, R"({"data":{"ok":true}})"};
  }
};

template <typename Predicate>
void waitFor(Predicate predicate, long timeoutMs = 1000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      CHECK(false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::unique_ptr<GraphQLSubscriptionClient> makeClient(
    const std::shared_ptr<FakeTransport>& transport,
    const std::shared_ptr<AuthState>& auth,
    const std::shared_ptr<Dispatcher>& dispatcher,
    GraphQLSubscriptionOptions options = {}) {
  return std::make_unique<GraphQLSubscriptionClient>(
      GraphQLSubscriptionClientConfig{"https://api.example.test/base/graphql",
                                      options},
      transport, auth, dispatcher);
}

Json parseSentText(const std::shared_ptr<FakeConnection>& connection,
                   std::size_t index) {
  const auto sent = connection->sent();
  CHECK(index < sent.size());
  CHECK(sent[index].kind == WebSocketFrameKind::Text);
  const Json parsed = Json::parse(sent[index].payload);
  CHECK(parsed.ok());
  return parsed;
}

void acknowledge(const std::shared_ptr<FakeConnection>& connection) {
  connection->emitOpen();
  connection->emitText(R"({"type":"connection_ack"})");
}

void testEndpointNormalization() {
  auto check = [](std::string_view input, std::string_view expected) {
    auto result = normalizeGraphQLWebSocketUrl(input);
    CHECK(result.ok());
    CHECK(result.value() == expected);
  };
  check("http://example.test", "ws://example.test/graphql");
  check("https://example.test/", "wss://example.test/graphql");
  check("ws://example.test/api", "ws://example.test/api/graphql");
  check("wss://example.test/api/graphql",
        "wss://example.test/api/graphql");
  check("https://example.test/graphql/graphql/?region=us",
        "wss://example.test/graphql?region=us");
  CHECK(!normalizeGraphQLWebSocketUrl("ftp://example.test").ok());
  CHECK(!normalizeGraphQLWebSocketUrl("https://example.test/#fragment").ok());
}

void testDefaultTransportAvailability() {
#ifdef CROWDY_HAS_CURL_WEBSOCKETS
  CHECK(static_cast<bool>(makeCurlWebSocketTransport()) ==
        curlWebSocketTransportAvailable());
#else
  CHECK(!curlWebSocketTransportAvailable());
  CHECK(!makeCurlWebSocketTransport());
#endif
}

void testReuseHttpAuthAndDispatcher() {
  auto auth = std::make_shared<AuthState>();
  auth->setToken("shared-token");
  auto http = std::make_shared<FakeHttpTransport>();
  GraphQLClient httpClient(GraphQLClientConfig{"https://api.example/graphql", 1000},
                           http, auth);
  auto webSocket = std::make_shared<FakeTransport>();
  GraphQLSubscriptionClient subscriptions(
      GraphQLSubscriptionClientConfig{"https://api.example/graphql", {}},
      webSocket, httpClient);
  CHECK(static_cast<bool>(httpClient.dispatcher()));
  CHECK(httpClient.dispatcher() == subscriptions.dispatcher());

  GraphQLSubscriptionCallbacks callbacks;
  auto handle = subscriptions.subscribe(
      "subscription { watch { value } }", JVal(), {}, std::move(callbacks));
  const auto connection = webSocket->connection(0);
  connection->emitOpen();
  Json init = parseSentText(connection, 0);
  CHECK(init["payload"]["Authorization"].asString() ==
        "Bearer shared-token");
  CHECK(handle.active());
}

void testHandshakeAuthParsePingAndDispatcher() {
  auto transport = std::make_shared<FakeTransport>();
  auto auth = std::make_shared<AuthState>();
  auth->setToken("test-token");
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(transport, auth, dispatcher);

  bool nextCalled = false;
  GraphQLSubscriptionOutcome outcome;
  const std::thread::id gameThread = std::this_thread::get_id();
  std::thread::id callbackThread;
  JVal variables;
  variables["appId"] = "42";
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext = [&](GraphQLSubscriptionOutcome value) {
    nextCalled = true;
    callbackThread = std::this_thread::get_id();
    outcome = std::move(value);
  };
  auto handle = client->subscribe(
      "subscription Watch($appId: BigInt!) { watch(appId: $appId) { value } }",
      variables, "Watch", std::move(callbacks));
  CHECK(handle.active());
  CHECK_EQ(transport->connectionCount(), std::size_t{1});
  const auto connection = transport->connection(0);
  CHECK(connection->started());
  CHECK(transport->request(0).url ==
        "wss://api.example.test/base/graphql");
  CHECK(transport->request(0).subprotocol == "graphql-transport-ws");

  connection->emitOpen();
  Json init = parseSentText(connection, 0);
  CHECK(init["type"].asString() == "connection_init");
  CHECK(init["payload"]["Authorization"].asString() ==
        "Bearer test-token");

  connection->emitText(R"({"type":"connection_ack"})");
  Json subscribe = parseSentText(connection, 1);
  CHECK(subscribe["type"].asString() == "subscribe");
  CHECK(subscribe["id"].asString() == "1");
  CHECK(subscribe["payload"]["operationName"].asString() == "Watch");
  CHECK(subscribe["payload"]["variables"]["appId"].asString() == "42");

  connection->emitText(R"({"type":"ping","payload":{"nonce":"abc"}})");
  Json pong = parseSentText(connection, 2);
  CHECK(pong["type"].asString() == "pong");
  CHECK(pong["payload"]["nonce"].asString() == "abc");

  connection->emitFrame(WebSocketFrameKind::Ping, "wire-ping");
  const auto sent = connection->sent();
  CHECK_EQ(sent.size(), std::size_t{4});
  CHECK(sent[3].kind == WebSocketFrameKind::Pong);
  CHECK(sent[3].payload == "wire-ping");

  std::thread engineTransportThread([connection] {
    connection->emitText(
        R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":7}}}})");
  });
  engineTransportThread.join();
  CHECK(!nextCalled);
  CHECK_EQ(dispatcher->drain(), std::size_t{1});
  CHECK(nextCalled);
  CHECK(callbackThread == gameThread);
  CHECK(outcome.ok());
  CHECK_EQ(outcome.data["watch"]["value"].asInt64(), std::int64_t{7});
}

void testCancellationSuppressesQueuedDelivery() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client =
      makeClient(transport, std::make_shared<AuthState>(), dispatcher);
  bool called = false;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext =
      [&](GraphQLSubscriptionOutcome) { called = true; };
  auto handle = client->subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto connection = transport->connection(0);
  acknowledge(connection);
  connection->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":1}}}})");
  CHECK(!called);

  handle.cancel();
  CHECK(!handle.active());
  dispatcher->drain();
  CHECK(!called);

  const auto sent = connection->sent();
  CHECK_EQ(sent.size(), std::size_t{3});
  Json complete = Json::parse(sent[2].payload);
  CHECK(complete["type"].asString() == "complete");
  CHECK(complete["id"].asString() == "1");
  CHECK(!connection->closes().empty());
}

void testServerCompleteAndCloseMapping() {
  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    bool completed = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onComplete = [&] { completed = true; };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(R"({"id":"1","type":"complete"})");
    CHECK(!handle.active());
    CHECK(!completed);
    dispatcher->drain();
    CHECK(completed);
  }

  {
    GraphQLSubscriptionOptions options;
    options.maxReconnectAttempts = 3;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitClose(4403, "forbidden");
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::Authorization);
    CHECK_EQ(got.closeCode, std::uint16_t{4403});
    CHECK(got.terminal);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{1});
  }
}

void testReconnectReplayAndStaleConnectionFencing() {
  GraphQLSubscriptionOptions options;
  options.initialReconnectDelayMs = 1;
  options.maxReconnectDelayMs = 1;
  options.reconnectJitter = 0;
  options.reconnectRandomSeed = 7;
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(transport, std::make_shared<AuthState>(),
                           dispatcher, options);

  std::size_t reconnects = 0;
  std::size_t nextCount = 0;
  GraphQLReconnectInfo reconnectInfo;
  GraphQLSubscriptionError terminalError;
  bool errored = false;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext =
      [&](GraphQLSubscriptionOutcome) { ++nextCount; };
  callbacks.onError = [&](GraphQLSubscriptionError error) {
    terminalError = std::move(error);
    errored = true;
  };
  callbacks.onReconnect = [&](GraphQLReconnectInfo info) {
    ++reconnects;
    reconnectInfo = std::move(info);
  };
  auto handle = client->subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto first = transport->connection(0);
  acknowledge(first);
  first->emitClose(1012, "service restart");

  waitFor([&] { return transport->connectionCount() == 2; });
  const auto second = transport->connection(1);
  acknowledge(second);
  CHECK_EQ(second->sent().size(), std::size_t{2});
  CHECK_EQ(reconnects, std::size_t{0});

  // A late terminal event from the replaced connection must not kill the
  // healthy replayed operation.
  first->emitClose(4401, "UNAUTHENTICATED");
  first->emitError(WebSocketErrorKind::Protocol, false, "late old error");
  CHECK(handle.active());
  CHECK(!errored);

  second->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":2}}}})");
  CHECK_EQ(nextCount, std::size_t{0});
  dispatcher->drain();
  CHECK_EQ(reconnects, std::size_t{1});
  CHECK_EQ(reconnectInfo.attempt, std::size_t{1});
  CHECK(reconnectInfo.operationId == "1");
  CHECK_EQ(nextCount, std::size_t{1});
  CHECK(!errored);
  (void)terminalError;
}

void testTerminalGraphqlErrors() {
  struct Case {
    const char* code;
    GraphQLSubscriptionErrorKind kind;
  };
  const Case cases[] = {
      {"AUTH_REQUIRED", GraphQLSubscriptionErrorKind::Authentication},
      {"APP_ID_REQUIRED", GraphQLSubscriptionErrorKind::AppScope},
      {"AGENT_CLIENT_EPOCH_STALE",
       GraphQLSubscriptionErrorKind::StaleClientEpoch},
  };

  for (const auto& testCase : cases) {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    GraphQLSubscriptionError got;
    bool called = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError = [&](GraphQLSubscriptionError error) {
      got = std::move(error);
      called = true;
    };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(
        std::string(
            R"({"id":"1","type":"error","payload":[{"message":"terminal","extensions":{"code":")") +
        testCase.code + R"("}}]})");
    CHECK(!handle.active());
    CHECK(!called);
    dispatcher->drain();
    CHECK(called);
    CHECK(got.kind == testCase.kind);
    CHECK(got.code == testCase.code);
    CHECK(got.terminal);
    CHECK(!got.retryable);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{1});
    CHECK(isTerminalSubscriptionErrorCode(testCase.code));
  }
}

void testFrameAndUtf8Limits() {
  GraphQLSubscriptionOptions options;
  options.maxFrameBytes = 256;
  options.maxReconnectAttempts = 0;

  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(std::string(257, 'x'));
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::FrameTooLarge);
    CHECK_EQ(got.closeCode, std::uint16_t{1009});
  }

  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    std::string invalid = R"({"type":"ping","payload":")";
    invalid.push_back(static_cast<char>(0xc3));
    invalid += R"("})";
    connection->emitText(std::move(invalid));
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::InvalidUtf8);
    CHECK_EQ(got.closeCode, std::uint16_t{1007});
  }
}

void testAcknowledgementTimeoutAndReconnectCap() {
  {
    GraphQLSubscriptionOptions options;
    options.acknowledgementTimeoutMs = 5;
    options.maxReconnectAttempts = 0;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    transport->connection(0)->emitOpen();
    waitFor([&] { return !handle.active(); });
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::Timeout);
    CHECK(got.status.code == Errc::Timeout);
  }

  {
    GraphQLSubscriptionOptions options;
    options.initialReconnectDelayMs = 0;
    options.maxReconnectDelayMs = 0;
    options.reconnectJitter = 0;
    options.maxReconnectAttempts = 1;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    transport->connection(0)->emitError(WebSocketErrorKind::Connection, true);
    waitFor([&] { return transport->connectionCount() == 2; });
    transport->connection(1)->emitError(WebSocketErrorKind::Connection, true);
    waitFor([&] { return !handle.active(); });
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::ReconnectExhausted);
    CHECK(got.code == "RECONNECT_EXHAUSTED");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{2});
  }
}

void testCrowdyClientInjectionAndPoll() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test/api";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  config.webSocket.initialReconnectDelayMs = 0;
  CrowdyClient client(std::move(config));
  client.setToken("game-token");

  bool called = false;
  std::thread::id callbackThread;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext = [&](GraphQLSubscriptionOutcome) {
    called = true;
    callbackThread = std::this_thread::get_id();
  };
  auto handle = client.subscriptions().subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto connection = webSocket->connection(0);
  acknowledge(connection);
  std::thread transportThread([connection] {
    connection->emitText(
        R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":9}}}})");
  });
  transportThread.join();
  CHECK(!called);
  client.poll();
  CHECK(called);
  CHECK(callbackThread == std::this_thread::get_id());
  CHECK(client.subscriptions().endpoint() ==
        "wss://game.example.test/api/graphql");
  CHECK(handle.active());
}

}  // namespace

int main() {
  testEndpointNormalization();
  testDefaultTransportAvailability();
  testReuseHttpAuthAndDispatcher();
  testHandshakeAuthParsePingAndDispatcher();
  testCancellationSuppressesQueuedDelivery();
  testServerCompleteAndCloseMapping();
  testReconnectReplayAndStaleConnectionFencing();
  testTerminalGraphqlErrors();
  testFrameAndUtf8Limits();
  testAcknowledgementTimeoutAndReconnectCap();
  testCrowdyClientInjectionAndPoll();
  std::printf("graphql_subscription_test passed\n");
  return 0;
}
