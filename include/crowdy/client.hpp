#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "crowdy/core/result.hpp"
#include "crowdy/domains/auth.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/game_model.hpp"
#include "crowdy/domains/compute.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_wallet.hpp"
#include "crowdy/domains/marketplace.hpp"
#include "crowdy/domains/player_model.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/server_status.hpp"
#include "crowdy/domains/users.hpp"
#include "crowdy/domains/world_data.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/replication/types.hpp"

namespace crowdy {

namespace domains {
class AdminAPI;
class OperatorAPI;
}  // namespace domains
namespace graphql {
class Dispatcher;
}
namespace replication {
class ReplicationClient;
}

/// Client configuration. Follows the CrowdyJS two-token model: build one
/// identity client (managementUrl + session token) and one client per game
/// (httpUrl from mintAppToken + the app-scoped token).
struct ClientConfig {
  /// Game API base URL (world data + replication assignment). Falls back to
  /// managementUrl when empty (identity-only clients).
  std::string httpUrl;
  /// Management API base URL (sign-in, portal, studio admin). Falls back to
  /// httpUrl when empty (single-endpoint deployments).
  std::string managementUrl;
  /// Game GraphQL path appended to httpUrl, or an explicitly complete custom
  /// endpoint URL. Base URLs already ending in /graphql are not duplicated.
  std::string graphqlEndpoint = "/graphql";
  long timeoutMs = 60000;
  /// Token persistence; in-memory when null.
  std::shared_ptr<graphql::ITokenStore> tokenStore;
  /// HTTP transport; the default libcurl transport when null. Engine
  /// wrappers inject their own here.
  std::shared_ptr<graphql::IHttpTransport> transport;
  /// Optional async HTTP transport for the non-blocking API path. When set,
  /// *Async calls run on it and their callbacks are delivered from poll();
  /// engines inject their own here (FHttpModule, UnityWebRequest, HTTPRequest).
  std::shared_ptr<graphql::IAsyncHttpTransport> asyncTransport;
  /// Routed Game API WebSocket base URL. CrowdyCPP does not emulate the
  /// browser UDP proxy; this normalized endpoint is retained for portable
  /// GraphQL-WebSocket consumers.
  std::string wsUrl;
  /// Optional Management GraphQL path or explicitly complete endpoint.
  /// Falls back to graphqlEndpoint for legacy single-path deployments.
  std::string managementGraphqlEndpoint;
  /// Optional explicitly complete WebSocket endpoint (or a path relative to
  /// wsUrl). When empty, wsUrl is normalized to exactly one /graphql.
  std::string wsEndpoint;
};

/// Ordered stage reached by refreshGameplayToken(). Complete is the only
/// successful terminal stage; every other value identifies where rotation
/// stopped.
enum class GameplayTokenRefreshStage : std::uint8_t {
  Quiesce = 0,
  Refresh,
  Install,
  Reconnect,
  Complete,
};

inline const char* gameplayTokenRefreshStageName(
    GameplayTokenRefreshStage stage) {
  switch (stage) {
    case GameplayTokenRefreshStage::Quiesce: return "Quiesce";
    case GameplayTokenRefreshStage::Refresh: return "Refresh";
    case GameplayTokenRefreshStage::Install: return "Install";
    case GameplayTokenRefreshStage::Reconnect: return "Reconnect";
    case GameplayTokenRefreshStage::Complete: return "Complete";
  }
  return "?";
}

/// Typed result of safe gameplay-token rotation. On a refresh-stage failure
/// the old bearer remains installed. On a reconnect-stage failure
/// tokenInstalled stays true and the fresh bearer is retained so callers can
/// retry Connection::connect() without rotating again.
struct GameplayTokenRefreshResult {
  GameplayTokenRefreshStage stage = GameplayTokenRefreshStage::Quiesce;
  Status status = Errc::Rejected;
  domains::AppTokenResponse token;
  std::string errorCode;
  std::string errorMessage;

  bool hadActiveReplication = false;
  bool tokenInstalled = false;
  bool reconnectAttempted = false;
  bool reconnected = false;
  replication::ConnState previousReplicationState =
      replication::ConnState::Idle;
  replication::Assignment previousReplicationEndpoint;
  replication::Assignment replicationEndpoint;

  bool ok() const {
    return stage == GameplayTokenRefreshStage::Complete && status.ok();
  }
};

using GameplayTokenRefreshCallback =
    std::function<void(GameplayTokenRefreshResult)>;

/// The Crowded Kingdoms client. Domain accessors mirror CrowdyJS sub-clients;
/// replication() is the native-UDP replacement for CrowdyJS's client.udp /
/// client.realtime.
class CrowdyClient {
 public:
  explicit CrowdyClient(ClientConfig config);
  ~CrowdyClient();

  CrowdyClient(const CrowdyClient&) = delete;
  CrowdyClient& operator=(const CrowdyClient&) = delete;
  CrowdyClient(CrowdyClient&&) noexcept;
  CrowdyClient& operator=(CrowdyClient&&) noexcept;

  // ----- Auth state -----------------------------------------------------------
  /// Seed a token directly (identity session token on an identity client, or
  /// an app-scoped token on a per-game client).
  void setToken(const std::string& token) { auth_->setToken(token); }
  std::string getToken() const { return auth_->token(); }
  /// Restore a persisted token from the configured token store.
  bool restoreSession() { return auth_->restore(); }
  graphql::AuthState& authState() { return *auth_; }

  // ----- Game-client surface --------------------------------------------------
  domains::AuthAPI& auth() { return *authApi_; }
  domains::UsersAPI& users() { return *users_; }
  domains::PortalAPI& portal() { return *portal_; }
  domains::ServerStatusAPI& serverStatus() { return *serverStatus_; }
  domains::ChunksAPI& chunks() { return *chunks_; }
  domains::VoxelsAPI& voxels() { return *voxels_; }
  domains::ActorsAPI& actors() { return *actors_; }
  domains::AvatarsAPI& avatars() { return *avatars_; }
  domains::StateAPI& state() { return *state_; }
  domains::HostAPI& host() { return *host_; }
  domains::TeleportAPI& teleport() { return *teleport_; }
  domains::TeamsAPI& teams() { return *teams_; }
  domains::ChannelsAPI& channels() { return *channels_; }
  domains::GameModelAPI& gameModel() { return *gameModel_; }
  domains::ComputeAPI& compute() { return *compute_; }
  domains::PlayerComputeAPI& playerCompute() { return *playerCompute_; }
  domains::PlayerWalletAPI& playerWallet() { return *playerWallet_; }
  domains::MarketplaceAPI& marketplace() { return *marketplace_; }
  domains::PlayerModelAPI& playerModel() { return *playerModel_; }
  domains::GameAppsAPI& gameApps() { return *gameApps_; }
  domains::PlatformAPI& platform() { return *platform_; }

  // ----- Gameplay-token lifecycle ----------------------------------------------
  /// Safely rotate the app-scoped bearer used by GraphQL and native UDP.
  /// Captures the active native connection, handlers, mode, and endpoint;
  /// quiesces it under the old token; refreshes through portal(); installs the
  /// new token while the socket is closed; then reconnects the same Connection
  /// so handlers remain registered exactly once. No browser UDP-proxy
  /// operations are issued.
  GameplayTokenRefreshResult refreshGameplayToken();

  /// Non-throwing async twin. The portal request uses the configured async
  /// transport and the callback is delivered from poll(). Quiesce happens
  /// before the request starts; reconnect uses the preserved native
  /// Connection.
  void refreshGameplayTokenAsync(GameplayTokenRefreshCallback cb);

  // ----- Async API completion --------------------------------------------------
  /// Drain finished async API callbacks on the calling thread. Call once per
  /// tick from the game thread so *Async callbacks fire where engine objects
  /// are safe to touch. No-op unless an async transport routes through it.
  void poll();

  // ----- Native replication ----------------------------------------------------
  /// The native UDP replication client (lazily constructed). Connect with an
  /// app-scoped token held by this client.
  replication::ReplicationClient& replication();

  // ----- Privileged surfaces ---------------------------------------------------
  /// Studio-admin surface (orgs, apps, billing, environments, ...). Drive
  /// with an org/admin token from a trusted context.
  domains::AdminAPI& admin() { return *admin_; }
  /// Operator control-plane surface (requires is_operator).
  domains::OperatorAPI& operator_() { return *operatorApi_; }

  // ----- Low-level escape hatches ------------------------------------------------
  /// Raw GraphQL against the Game API endpoint.
  graphql::GraphQLClient& graphqlClient() { return *gameGql_; }
  /// Raw GraphQL against the Management API endpoint.
  graphql::GraphQLClient& managementClient() { return *managementGql_; }
  /// Normalized routed WebSocket endpoint reserved for portable
  /// GraphQL-WebSocket consumers. Empty when neither wsUrl nor wsEndpoint was
  /// configured.
  const std::string& websocketEndpoint() const { return websocketEndpoint_; }

  const ClientConfig& config() const { return config_; }

  /// Dispose: disconnects replication and clears in-memory auth listeners.
  void close();

 private:
  ClientConfig config_;
  std::shared_ptr<graphql::IHttpTransport> transport_;
  std::shared_ptr<graphql::AuthState> auth_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
  std::shared_ptr<graphql::GraphQLClient> gameGql_;
  std::shared_ptr<graphql::GraphQLClient> managementGql_;
  std::string websocketEndpoint_;

  std::unique_ptr<domains::AuthAPI> authApi_;
  std::unique_ptr<domains::UsersAPI> users_;
  std::unique_ptr<domains::PortalAPI> portal_;
  std::unique_ptr<domains::ServerStatusAPI> serverStatus_;
  std::unique_ptr<domains::ChunksAPI> chunks_;
  std::unique_ptr<domains::VoxelsAPI> voxels_;
  std::unique_ptr<domains::ActorsAPI> actors_;
  std::unique_ptr<domains::AvatarsAPI> avatars_;
  std::unique_ptr<domains::StateAPI> state_;
  std::unique_ptr<domains::HostAPI> host_;
  std::unique_ptr<domains::TeleportAPI> teleport_;
  std::unique_ptr<domains::TeamsAPI> teams_;
  std::unique_ptr<domains::ChannelsAPI> channels_;
  std::unique_ptr<domains::GameModelAPI> gameModel_;
  std::unique_ptr<domains::ComputeAPI> compute_;
  std::unique_ptr<domains::PlayerComputeAPI> playerCompute_;
  std::unique_ptr<domains::PlayerWalletAPI> playerWallet_;
  std::unique_ptr<domains::MarketplaceAPI> marketplace_;
  std::unique_ptr<domains::PlayerModelAPI> playerModel_;
  std::unique_ptr<domains::GameAppsAPI> gameApps_;
  std::unique_ptr<domains::PlatformAPI> platform_;
  std::unique_ptr<domains::AdminAPI> admin_;
  std::unique_ptr<domains::OperatorAPI> operatorApi_;
  std::unique_ptr<replication::ReplicationClient> replication_;
};

}  // namespace crowdy
