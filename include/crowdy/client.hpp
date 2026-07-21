#pragma once

#include <memory>
#include <string>

#include "crowdy/domains/auth.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/game_model.hpp"
#include "crowdy/domains/compute.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_model.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/server_status.hpp"
#include "crowdy/domains/users.hpp"
#include "crowdy/domains/world_data.hpp"
#include "crowdy/graphql/graphql_client.hpp"

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
  /// Path appended to the base URLs.
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
};

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
  domains::PlayerModelAPI& playerModel() { return *playerModel_; }
  domains::GameAppsAPI& gameApps() { return *gameApps_; }
  domains::PlatformAPI& platform() { return *platform_; }

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
  std::unique_ptr<domains::PlayerModelAPI> playerModel_;
  std::unique_ptr<domains::GameAppsAPI> gameApps_;
  std::unique_ptr<domains::PlatformAPI> platform_;
  std::unique_ptr<domains::AdminAPI> admin_;
  std::unique_ptr<domains::OperatorAPI> operatorApi_;
  std::unique_ptr<replication::ReplicationClient> replication_;
};

}  // namespace crowdy
