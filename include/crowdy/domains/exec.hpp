#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/core/result.hpp"
#include "crowdy/domains/domain_base.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/graphql/websocket.hpp"

/// client.exec() — ck-exec (dev-tier preview): a player's connection to the
/// execution host that runs an app's hubs (stateful, one instance per key) and
/// spokes (stateless, replicated), and a developer's deploys.
///
///   auto exec = client.exec().connect(appId, {.nodeType = "arena", .key = "m1"});
///   exec->call("arena", "m1", "state", {}, [](crowdy::domains::ExecReply r) {
///     if (r.ok()) render(r.value());
///   });
///   client.poll();  // callbacks run here, as for every async surface
///
/// `connect` asks the Game API for a host (`execConnect`, with the app-scoped
/// token of `appId` as the session token) and opens a WebSocket to its gateway.
/// Frames are ck-exec's client protocol; payloads are MessagePack
/// (graphql::Json::toMsgpack / fromMsgpack). The connection recovers by itself:
/// a closed socket or a `Moved` reply asks for a host again, renews every
/// subscription, and retries the call once. Mirrors CrowdyJS `client.exec`.
/// See https://docs.dev.crowdedkingdoms.com/exec/intro.
namespace crowdy::domains {

/// Call statuses as the gateway sends them, by wire value.
enum class ExecStatus : std::uint8_t {
  Ok = 0,
  AppError = 1,
  Busy = 2,
  Moved = 3,
  NotFound = 4,
  DeadlineExceeded = 5,
  Denied = 6,
  RateLimited = 7,
  Unavailable = 8,
  Internal = 9,
  Trapped = 10,
  BadRequest = 11,
  Unknown = 255,
};

std::string_view execStatusName(ExecStatus status) noexcept;
ExecStatus execStatusFromWire(std::uint8_t value) noexcept;
/// Busy, Moved, Unavailable and RateLimited: trying again later can succeed.
bool execStatusRetryable(ExecStatus status) noexcept;

/// ck-exec's client protocol, one WebSocket binary message per frame.
namespace exec_wire {

struct ClientFrame {
  enum class Kind { Call, Subscribe, Unsubscribe, Ping };
  Kind kind = Kind::Call;
  std::uint32_t rid = 0;  // the ping nonce for Ping
  std::string nodeType;
  std::string key;
  std::string method;  // the topic for Subscribe / Unsubscribe
  std::string payload;
};

struct ServerFrame {
  enum class Kind { Reply, Push, Pong };
  Kind kind = Kind::Reply;
  std::uint32_t rid = 0;  // the pong nonce for Pong
  std::uint8_t status = 0;
  std::string nodeType;
  std::string key;
  std::string topic;
  std::string payload;
};

/// InvalidArgument when a name does not fit its length prefix.
Result<std::string> encode(const ClientFrame& frame);
/// Malformed for a truncated frame or an unknown type.
Result<ServerFrame> decode(std::string_view bytes);

}  // namespace exec_wire

/// Where a player connects, and the connect token for it (about a minute).
struct ExecEndpoint {
  std::string gatewayUrl;
  std::string token;
  std::string host;
};

struct ExecReply {
  ExecStatus status = ExecStatus::Unavailable;
  /// MessagePack for Ok; the refusal's message (UTF-8) otherwise.
  std::string payload;

  bool ok() const { return status == ExecStatus::Ok; }
  bool retryable() const { return execStatusRetryable(status); }
  /// The payload decoded from MessagePack (a null Json when it is not).
  graphql::Json value() const { return graphql::Json::fromMsgpack(payload); }
  std::string message() const { return ok() ? std::string() : payload; }
};

/// A published message on a topic this connection subscribes to.
struct ExecPush {
  std::string nodeType;
  std::string key;
  std::string topic;
  std::string payload;

  graphql::Json value() const { return graphql::Json::fromMsgpack(payload); }
};

using ExecReplyCallback = std::function<void(ExecReply)>;
using ExecPushHandler = std::function<void(const ExecPush&)>;
/// Finds a host: calls `found` once, from any thread.
using ExecDial = std::function<void(std::function<void(Result<ExecEndpoint>)> found)>;

struct ExecConnectOptions {
  /// Put the player on the host running this node type (with `key`).
  std::string nodeType;
  /// The instance key within `nodeType`; empty for the root hub or a spoke.
  std::string key;
  /// How long a call waits for its reply.
  long callTimeoutMs = 10000;
  long openTimeoutMs = 10000;
  /// Connect again when the socket closes unexpectedly.
  bool reconnect = true;
  long initialReconnectDelayMs = 250;
  long maxReconnectDelayMs = 5000;
};

/// Filters for `ExecAPI::logs`. Empty strings and negative numbers mean "not set".
struct ExecLogsQuery {
  std::string nodeType;
  std::string key;
  /// The least severe level included: 0 errors only ... 3 everything (the default).
  int maxLevel = -1;
  /// Only lines older than this line id, to page back.
  std::string before;
  /// At most this many lines (default 100, at most 500).
  int limit = -1;
};

/// One player's connection to a ck-exec host. Thread-safe. Every callback runs
/// through the dispatcher (CrowdyClient::poll()); without one, on the
/// transport's thread.
class ExecConnection {
 public:
  ExecConnection(std::shared_ptr<graphql::IWebSocketTransport> transport,
                 std::shared_ptr<graphql::Dispatcher> dispatcher, ExecDial dial,
                 ExecConnectOptions options = {});
  ~ExecConnection();

  ExecConnection(const ExecConnection&) = delete;
  ExecConnection& operator=(const ExecConnection&) = delete;

  /// A connection to a known gateway with a connect token you already have
  /// (tools and tests). It does not reconnect.
  static std::shared_ptr<ExecConnection> open(
      std::shared_ptr<graphql::IWebSocketTransport> transport,
      std::shared_ptr<graphql::Dispatcher> dispatcher, ExecEndpoint endpoint,
      ExecConnectOptions options = {});

  /// Start connecting. `done` fires once: Ok when open, the failure otherwise.
  /// Calls made before then wait for the connection.
  void connect(std::function<void(Status)> done = {});

  /// Call a node's endpoint with raw bytes. `done` fires once.
  void callRaw(std::string nodeType, std::string key, std::string method,
               std::string payload, ExecReplyCallback done);
  /// Call with `args` as MessagePack; decode the reply with ExecReply::value().
  void call(std::string nodeType, std::string key, std::string method,
            const graphql::JVal& args, ExecReplyCallback done);
  /// Receive what a node publishes on `topic`. Returns a handle for
  /// unsubscribe(); `done` fires with the gateway's answer to the first one.
  std::uint64_t subscribe(std::string nodeType, std::string key, std::string topic,
                          ExecPushHandler onPush, ExecReplyCallback done = {});
  /// Stop one handler (and the subscription, when it was the last one).
  void unsubscribe(std::uint64_t handle);
  void ping(ExecReplyCallback done);

  /// Called with the new host after every reconnect.
  void onReconnect(std::function<void(std::string host)> listener);

  std::string host() const;
  bool connected() const;
  /// Close; nothing reconnects and pending calls fail as Unavailable.
  void close();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

/// One node type of a deploy: its compiled module and its manifest settings.
struct ExecNodeType {
  std::string name;
  /// "hub" or "spoke".
  std::string kind;
  /// The compiled module (`wasm32-unknown-unknown`, built with ckx-sdk).
  std::string wasm;
  /// The type that owns this one; empty for the root.
  std::string parent;
  bool client = false;
  /// Types it may call and subscribe to; "*" for any.
  std::vector<std::string> calls;
  /// Any other manifest fields (`persist_every_ms`, `replicas`, `seed_b64`, ...).
  graphql::JVal extra;
};

class ExecAPI : public DomainBase {
 public:
  ExecAPI(std::shared_ptr<graphql::GraphQLClient> gql,
          std::shared_ptr<graphql::IWebSocketTransport> transport);

  /// A host for the signed-in player and its connect token (`execConnect`),
  /// blocking. The session token must be the app-scoped token of `appId`.
  Result<ExecEndpoint> endpoint(std::string appId, std::string nodeType = {}, std::string key = {}) const;
  void endpointAsync(std::string appId, std::string nodeType, std::string key,
                     std::function<void(Result<ExecEndpoint>)> done) const;

  /// Connect the signed-in player to ck-exec for `appId`. Returns at once; the
  /// connection opens in the background and calls made meanwhile wait for it.
  std::shared_ptr<ExecConnection> connect(std::string appId, ExecConnectOptions options = {}) const;
  /// The same, calling back once the connection is open (or with the failure).
  void connectAsync(std::string appId, ExecConnectOptions options,
                    std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const;

  /// Deploy a new version of the app's nodes and make it active (`execDeploy`),
  /// blocking: the manifest with each module's SHA-256, and each distinct module
  /// once. Returns `{ version }`. Requires the org `manage_compute` permission.
  graphql::Json deploy(std::string appId, std::string root, const std::vector<ExecNodeType>& types) const;
  void deployAsync(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                   graphql::GraphQLCallback done) const;

  // ---- operations (dev-tier preview) ----

  /// A host and a developer connect token for `appId` (`execConnectAsDeveloper`),
  /// blocking. The session's calls arrive as `Caller::Developer` with your user id and
  /// may reach any node type, not only `client` ones. Requires the org
  /// `manage_compute` permission and your own session token, not an app token.
  Result<ExecEndpoint> developerEndpoint(std::string appId, std::string nodeType = {}, std::string key = {}) const;
  void developerEndpointAsync(std::string appId, std::string nodeType, std::string key,
                              std::function<void(Result<ExecEndpoint>)> done) const;
  /// Connect as one of the app's developers (studio tools, manual runs, admin
  /// endpoints). The same connection as `connect`, reconnecting with a fresh
  /// developer token.
  std::shared_ptr<ExecConnection> connectAsDeveloper(std::string appId, ExecConnectOptions options = {}) const;
  void connectAsDeveloperAsync(std::string appId, ExecConnectOptions options,
                               std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const;

  /// Guest log lines (`execLogs`), newest first, kept for 24 hours: an array of
  /// `{ id, nodeType, key, level, host, at, text }`. Requires `view_compute_diagnostics`.
  graphql::Json logs(std::string appId, const ExecLogsQuery& query = {}) const;
  void logsAsync(std::string appId, const ExecLogsQuery& query, graphql::GraphQLCallback done) const;
  /// What the manager has placed (`execInstances`). Requires `view_compute_diagnostics`.
  graphql::Json instances(std::string appId) const;
  void instancesAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// The app's versions, newest first (`execVersions`). Requires `view_compute_diagnostics`.
  graphql::Json versions(std::string appId) const;
  void versionsAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// `{ activeVersion, disabled, disabledTypes, budgetPaused }` (`execAppStatus`).
  /// Requires `view_compute_diagnostics`.
  graphql::Json status(std::string appId) const;
  void statusAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Make an earlier version active again, a rollback (`execActivateVersion`);
  /// instances pick it up when they next start. Requires `manage_compute`.
  graphql::Json activateVersion(std::string appId, int version) const;
  void activateVersionAsync(std::string appId, int version, graphql::GraphQLCallback done) const;
  /// The kill switch, for the whole app or one node type (`execSetEnabled`). Off:
  /// nothing of it is placed, what runs is persisted and stopped, calls are refused
  /// with `Denied`. Requires `manage_compute`.
  graphql::Json setEnabled(std::string appId, bool enabled, std::string nodeType = {}) const;
  void setEnabledAsync(std::string appId, bool enabled, std::string nodeType, graphql::GraphQLCallback done) const;

 private:
  ExecDial dialer(std::string appId, std::string nodeType, std::string key, bool developer = false) const;
  static graphql::JVal deployVariables(std::string appId, std::string root, const std::vector<ExecNodeType>& types);

  std::shared_ptr<graphql::IWebSocketTransport> transport_;
};

/// The SHA-256 of `bytes` as lowercase hex, as a deploy names modules.
std::string execSha256Hex(std::string_view bytes);

}  // namespace crowdy::domains
