#include "crowdy/client.hpp"

#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/operator.hpp"
#include "crowdy/replication/connection.hpp"

namespace crowdy {

namespace {

std::string joinUrl(const std::string& base, const std::string& path) {
  if (base.empty()) return base;
  // If the base already names the endpoint (ends with the path), keep it.
  if (base.size() >= path.size() && base.compare(base.size() - path.size(), path.size(), path) == 0)
    return base;
  if (!base.empty() && base.back() == '/') return base.substr(0, base.size() - 1) + path;
  return base + path;
}

/// Bridges the replication client to this CrowdyClient's GraphQL plane:
/// serverWithLeastClients (Game API) for assignment and refreshAppToken
/// (Management API, current app token as bearer) for rotation.
class ClientSessionProvider final : public replication::ISessionProvider {
 public:
  ClientSessionProvider(domains::ServerStatusAPI& serverStatus, domains::PortalAPI& portal,
                        const core::ILogger& logger)
      : serverStatus_(serverStatus), portal_(portal), logger_(logger) {}

  Result<replication::Assignment> assignServer() override {
    try {
      domains::ServerAssignment a = serverStatus_.serverWithLeastClients();
      return replication::Assignment{a.ip4, a.ip6, a.clientPort};
    } catch (const std::exception& e) {
      logger_.log(core::LogLevel::Error, std::string("assignServer failed: ") + e.what());
      return Errc::Rejected;
    }
  }

  Result<replication::TokenInfo> refreshToken() override {
    try {
      domains::AppTokenResponse t = portal_.refresh();
      replication::TokenInfo info;
      info.token = t.token;
      info.gameTokenId = t.gameTokenId;
      info.expiresAtEpochMs =
          core::parseIso8601Millis(t.expiresAt.data(), t.expiresAt.size());
      return info;
    } catch (const std::exception& e) {
      logger_.log(core::LogLevel::Error, std::string("refreshToken failed: ") + e.what());
      return Errc::Rejected;
    }
  }

 private:
  domains::ServerStatusAPI& serverStatus_;
  domains::PortalAPI& portal_;
  const core::ILogger& logger_;
};

}  // namespace

CrowdyClient::CrowdyClient(ClientConfig config) : config_(std::move(config)) {
  transport_ = config_.transport ? config_.transport : graphql::makeCurlTransport();
  auth_ = std::make_shared<graphql::AuthState>(config_.tokenStore);

  const std::string managementBase =
      config_.managementUrl.empty() ? config_.httpUrl : config_.managementUrl;
  const std::string gameBase = config_.httpUrl.empty() ? config_.managementUrl : config_.httpUrl;

  managementGql_ = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{joinUrl(managementBase, config_.graphqlEndpoint),
                                   config_.timeoutMs},
      transport_, auth_);
  gameGql_ = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{joinUrl(gameBase, config_.graphqlEndpoint), config_.timeoutMs},
      transport_, auth_);

  // Management-plane domains.
  authApi_ = std::make_unique<domains::AuthAPI>(managementGql_, auth_);
  users_ = std::make_unique<domains::UsersAPI>(managementGql_);
  portal_ = std::make_unique<domains::PortalAPI>(managementGql_, auth_);
  platform_ = std::make_unique<domains::PlatformAPI>(managementGql_);
  admin_ = std::make_unique<domains::AdminAPI>(managementGql_);
  operatorApi_ = std::make_unique<domains::OperatorAPI>(managementGql_);

  // Game-plane domains (app-scoped token).
  serverStatus_ = std::make_unique<domains::ServerStatusAPI>(gameGql_);
  chunks_ = std::make_unique<domains::ChunksAPI>(gameGql_);
  voxels_ = std::make_unique<domains::VoxelsAPI>(gameGql_);
  actors_ = std::make_unique<domains::ActorsAPI>(gameGql_);
  avatars_ = std::make_unique<domains::AvatarsAPI>(gameGql_);
  state_ = std::make_unique<domains::StateAPI>(gameGql_);
  host_ = std::make_unique<domains::HostAPI>(gameGql_);
  teleport_ = std::make_unique<domains::TeleportAPI>(gameGql_);
  teams_ = std::make_unique<domains::TeamsAPI>(gameGql_);
  channels_ = std::make_unique<domains::ChannelsAPI>(gameGql_);
  gameModel_ = std::make_unique<domains::GameModelAPI>(gameGql_);
  gameApps_ = std::make_unique<domains::GameAppsAPI>(gameGql_);
}

CrowdyClient::~CrowdyClient() { close(); }

replication::ReplicationClient& CrowdyClient::replication() {
  if (!replication_) {
    auto provider = std::make_shared<ClientSessionProvider>(*serverStatus_, *portal_,
                                                            core::defaultLogger());
    replication_ = std::make_unique<replication::ReplicationClient>(std::move(provider));
  }
  return *replication_;
}

void CrowdyClient::close() { replication_.reset(); }

}  // namespace crowdy
