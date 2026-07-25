#include "crowdy/client.hpp"

#include <cctype>

#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/agent/client_runtime.hpp"
#include "crowdy/studio/integration.hpp"
#endif
#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/operator.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/replication/connection.hpp"

namespace crowdy {

namespace {

std::string trimUrl(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

bool isAbsoluteUrl(std::string_view value) {
  const std::size_t separator = value.find("://");
  return separator != std::string_view::npos && separator > 0;
}

void stripTrailingSlashes(std::string& path) {
  while (!path.empty() && path.back() == '/' &&
         !(path.size() >= 3 &&
           path.compare(path.size() - 3, 3, "://") == 0)) {
    path.pop_back();
  }
}

std::string normalizeGraphqlBase(std::string_view rawBase) {
  std::string base = trimUrl(rawBase);
  if (base.empty()) return {};

  const std::size_t suffixAt = base.find_first_of("?#");
  std::string path = base.substr(0, suffixAt);
  const std::string suffix =
      suffixAt == std::string::npos ? std::string() : base.substr(suffixAt);
  stripTrailingSlashes(path);

  constexpr std::string_view graphqlPath = "/graphql";
  while (path.size() >= graphqlPath.size() &&
         path.compare(path.size() - graphqlPath.size(), graphqlPath.size(),
                      graphqlPath) == 0) {
    path.resize(path.size() - graphqlPath.size());
    stripTrailingSlashes(path);
  }
  path += graphqlPath;
  return path + suffix;
}

std::string appendEndpointPath(std::string_view rawBase,
                               std::string_view rawPath) {
  std::string base = trimUrl(rawBase);
  std::string path = trimUrl(rawPath);
  if (base.empty()) return path;

  const std::size_t suffixAt = base.find_first_of("?#");
  const std::string suffix =
      suffixAt == std::string::npos ? std::string() : base.substr(suffixAt);
  if (suffixAt != std::string::npos) base.resize(suffixAt);
  stripTrailingSlashes(base);
  std::size_t first = 0;
  while (first < path.size() && path[first] == '/') ++first;
  return base + "/" + path.substr(first) + suffix;
}

std::string resolveGraphqlEndpoint(std::string_view base,
                                   std::string_view configured) {
  const std::string endpoint = trimUrl(configured);
  if (endpoint.empty()) return normalizeGraphqlBase(base);
  if (isAbsoluteUrl(endpoint)) return endpoint;
  if (endpoint == "graphql" || endpoint == "/graphql") {
    return normalizeGraphqlBase(base);
  }
  return appendEndpointPath(base, endpoint);
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
#ifndef CROWDY_NO_EXCEPTIONS
    try {
#endif
      domains::ServerAssignment a = serverStatus_.serverWithLeastClients();
      if (a.clientPort <= 0 || (a.ip4.empty() && a.ip6.empty())) {
        return Errc::Rejected;
      }
      return replication::Assignment{a.ip4, a.ip6, a.clientPort};
#ifndef CROWDY_NO_EXCEPTIONS
    } catch (const std::exception& e) {
      logger_.log(core::LogLevel::Error, std::string("assignServer failed: ") + e.what());
      return Errc::Rejected;
    }
#endif
  }

  Result<replication::TokenInfo> refreshToken() override {
#ifndef CROWDY_NO_EXCEPTIONS
    try {
#endif
      domains::AppTokenResponse t = portal_.refresh();
      if (t.token.empty()) return Errc::Rejected;
      const auto gameTokenId = t.gameTokenIdInt64();
      if (!gameTokenId) return Errc::InvalidArgument;
      replication::TokenInfo info;
      info.token = t.token;
      info.gameTokenId = *gameTokenId;
      info.expiresAtEpochMs =
          core::parseIso8601Millis(t.expiresAt.data(), t.expiresAt.size());
      return info;
#ifndef CROWDY_NO_EXCEPTIONS
    } catch (const std::exception& e) {
      logger_.log(core::LogLevel::Error, std::string("refreshToken failed: ") + e.what());
      return Errc::Rejected;
    }
#endif
  }

 private:
  domains::ServerStatusAPI& serverStatus_;
  domains::PortalAPI& portal_;
  const core::ILogger& logger_;
};

struct GameplayRefreshPreparation {
  GameplayTokenRefreshResult result;
  std::shared_ptr<replication::Connection> connection;
  replication::Connection::Snapshot snapshot;
  std::string oldToken;
  bool reconnect = false;
  bool ready = true;
};

bool wasActiveReplicationState(replication::ConnState state) {
  return state == replication::ConnState::Connecting ||
         state == replication::ConnState::Connected ||
         state == replication::ConnState::Reconnecting;
}

GameplayRefreshPreparation prepareGameplayRefresh(
    std::shared_ptr<replication::Connection> connection) {
  GameplayRefreshPreparation preparation;
  preparation.connection = std::move(connection);
  if (!preparation.connection) return preparation;

  preparation.result.hadActiveReplication = true;
  preparation.snapshot = preparation.connection->snapshot();
  preparation.result.previousReplicationState = preparation.snapshot.state;
  preparation.result.previousReplicationEndpoint =
      preparation.snapshot.endpoint;
  preparation.reconnect =
      wasActiveReplicationState(preparation.snapshot.state);

  // Native UDP has no browser proxy to disconnect. Quiescing stops the owned
  // net thread (if any), joins it, closes the socket, and leaves the handler
  // registry on the Connection object intact.
  preparation.connection->disconnect();
  // A proactive refresh may have been completing on the owned net thread
  // while snapshot() ran. Joining above establishes its final token before
  // this rotation proceeds; retain that quiesced token for rollback.
  preparation.snapshot.config.token =
      preparation.connection->snapshot().config.token;
  if (preparation.connection->state() != replication::ConnState::Closed) {
    preparation.ready = false;
    preparation.result.stage = GameplayTokenRefreshStage::Quiesce;
    preparation.result.status = Errc::Closed;
    preparation.result.errorCode = errcName(Errc::Closed);
    preparation.result.errorMessage =
        "native replication connection did not quiesce";
  }
  return preparation;
}

#ifndef CROWDY_NO_EXCEPTIONS
void setRefreshException(GameplayTokenRefreshResult& result,
                         GameplayTokenRefreshStage stage,
                         const std::exception& error) {
  result.stage = stage;
  result.status = Errc::Rejected;
  result.errorMessage = error.what();
  if (const auto* crowdyError =
          dynamic_cast<const graphql::CrowdyError*>(&error)) {
    result.errorCode = crowdyError->code();
  } else {
    result.errorCode = errcName(Errc::Rejected);
  }
}
#endif

void setRefreshOutcome(GameplayTokenRefreshResult& result,
                       const graphql::GraphQLOutcome& outcome) {
  result.stage = GameplayTokenRefreshStage::Refresh;
  result.status = outcome.status.ok() ? Status{Errc::Rejected} : outcome.status;
  if (!outcome.errors.empty()) {
    result.errorCode = outcome.errors.front().code.empty()
                           ? "GRAPHQL_ERROR"
                           : outcome.errors.front().code;
    result.errorMessage = outcome.errors.front().message;
    return;
  }
  switch (outcome.kind) {
    case graphql::GraphQLErrorKind::Http:
      result.errorCode = "HTTP_ERROR";
      result.errorMessage =
          "GraphQL endpoint returned HTTP " +
          std::to_string(outcome.httpStatus);
      break;
    case graphql::GraphQLErrorKind::Protocol:
      result.errorCode = "PROTOCOL_ERROR";
      result.errorMessage = outcome.errorMessage;
      break;
    case graphql::GraphQLErrorKind::Network:
      result.errorCode = "NETWORK_ERROR";
      result.errorMessage = outcome.errorMessage;
      break;
    case graphql::GraphQLErrorKind::Timeout:
      result.errorCode = "TIMEOUT";
      result.errorMessage = outcome.errorMessage;
      break;
    case graphql::GraphQLErrorKind::GraphQL:
      result.errorCode = "GRAPHQL_ERROR";
      result.errorMessage = "GraphQL refresh failed";
      break;
    case graphql::GraphQLErrorKind::None:
      result.errorCode = errcName(result.status.code);
      result.errorMessage = "gameplay token refresh failed";
      break;
  }
}

Result<replication::TokenInfo> replicationToken(
    const domains::AppTokenResponse& token) {
  const auto gameTokenId = token.gameTokenIdInt64();
  if (!gameTokenId) return Errc::InvalidArgument;
  replication::TokenInfo info;
  info.token = token.token;
  info.gameTokenId = *gameTokenId;
  info.expiresAtEpochMs =
      core::parseIso8601Millis(token.expiresAt.data(), token.expiresAt.size());
  return info;
}

GameplayTokenRefreshResult installGameplayRefresh(
    GameplayRefreshPreparation preparation,
    const std::shared_ptr<graphql::AuthState>& auth,
    domains::AppTokenResponse token) {
  auto result = std::move(preparation.result);
  result.token = std::move(token);
  if (result.token.token.size() != wire::kTokenOctets) {
    result.stage = GameplayTokenRefreshStage::Install;
    result.status = Errc::InvalidArgument;
    result.errorCode = errcName(Errc::InvalidArgument);
    result.errorMessage =
        "refreshAppToken returned invalid native token material";
    return result;
  }

  auto fresh = replicationToken(result.token);
  if (!fresh.ok()) {
    result.stage = GameplayTokenRefreshStage::Install;
    result.status = fresh.error();
    result.errorCode = "GAME_TOKEN_ID_OUT_OF_RANGE";
    result.errorMessage =
        "refreshAppToken returned a gameTokenId outside the native int64 wire range";
    return result;
  }
#ifndef CROWDY_NO_EXCEPTIONS
  try {
#endif
    // The socket is closed, so installing the native HMAC key first and then
    // publishing the shared GraphQL bearer is one quiesced cutover: no send
    // can observe mixed credentials.
    if (preparation.connection) {
      preparation.connection->setHandlers(preparation.snapshot.handlers);
      preparation.connection->setToken(fresh.value());
    }
    auth->setToken(result.token.token);
#ifndef CROWDY_NO_EXCEPTIONS
  } catch (const std::exception& error) {
    if (preparation.connection) {
      preparation.connection->setToken(preparation.snapshot.config.token);
    }
    try {
      auth->setToken(preparation.oldToken);
    } catch (const std::exception&) {
    }
    setRefreshException(result, GameplayTokenRefreshStage::Install, error);
    return result;
  }
#endif
  result.tokenInstalled = true;

  if (preparation.connection && preparation.reconnect) {
    result.reconnectAttempted = true;
    Status connected = preparation.connection->connect();
    if (!connected.ok()) {
      result.stage = GameplayTokenRefreshStage::Reconnect;
      result.status = connected;
      result.errorCode = errcName(connected.code);
      result.errorMessage =
          "fresh gameplay token installed; native replication reconnect failed";
      result.replicationEndpoint =
          preparation.connection->assignmentSnapshot();
      return result;
    }
    result.reconnected = true;
    result.replicationEndpoint = preparation.connection->assignmentSnapshot();
  } else {
    result.replicationEndpoint = result.previousReplicationEndpoint;
  }

  result.stage = GameplayTokenRefreshStage::Complete;
  result.status = Errc::Ok;
  return result;
}

}  // namespace

CrowdyClient::CrowdyClient(ClientConfig config) : config_(std::move(config)) {
  crypto_ = config_.crypto;
  if (!crypto_) {
#ifdef CROWDY_HAS_OPENSSL
    crypto_ = &core::opensslCrypto();
#else
    crypto_ = &core::unavailableCrypto();
#endif
  }
  transport_ = config_.transport ? config_.transport : graphql::makeCurlTransport();
  auth_ = std::make_shared<graphql::AuthState>(config_.tokenStore);
  dispatcher_ = std::make_shared<graphql::Dispatcher>();

  const std::string managementBase =
      config_.managementUrl.empty() ? config_.httpUrl : config_.managementUrl;
  const std::string gameBase = config_.httpUrl.empty() ? config_.managementUrl : config_.httpUrl;
  const std::string managementEndpoint =
      resolveGraphqlEndpoint(
          managementBase,
          config_.managementGraphqlEndpoint.empty()
              ? config_.graphqlEndpoint
              : config_.managementGraphqlEndpoint);
  const std::string gameEndpoint =
      resolveGraphqlEndpoint(gameBase, config_.graphqlEndpoint);

  managementGql_ = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{managementEndpoint, config_.timeoutMs},
      transport_, auth_);
  gameGql_ = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{gameEndpoint, config_.timeoutMs},
      transport_, auth_);
  websocketEndpoint_ = resolveGraphqlEndpoint(config_.wsUrl, config_.wsEndpoint);
  const std::string gameSubscriptionEndpoint =
      websocketEndpoint_.empty() ? gameEndpoint : websocketEndpoint_;

  // Share one completion pump across both endpoints so poll() drains every
  // async HTTP and WebSocket callback, and wire in engine transports.
  managementGql_->setDispatcher(dispatcher_);
  gameGql_->setDispatcher(dispatcher_);
  if (config_.asyncTransport) {
    managementGql_->setAsyncTransport(config_.asyncTransport);
    gameGql_->setAsyncTransport(config_.asyncTransport);
  }
  webSocketTransport_ = config_.webSocketTransport
                            ? config_.webSocketTransport
                            : graphql::makeCurlWebSocketTransport();
  managementSubscriptions_ =
      std::make_shared<graphql::GraphQLSubscriptionClient>(
          graphql::GraphQLSubscriptionClientConfig{
              managementEndpoint, config_.webSocket,
              graphql::GraphQLWebSocketEndpointKind::Complete},
          webSocketTransport_, auth_, dispatcher_);
  gameSubscriptions_ = std::make_shared<graphql::GraphQLSubscriptionClient>(
      graphql::GraphQLSubscriptionClientConfig{
          gameSubscriptionEndpoint, config_.webSocket,
          graphql::GraphQLWebSocketEndpointKind::Complete},
      webSocketTransport_, auth_, dispatcher_);

  // Management-plane domains.
  authApi_ = std::make_unique<domains::AuthAPI>(managementGql_, auth_);
  users_ = std::make_unique<domains::UsersAPI>(managementGql_);
  portal_ =
      std::make_unique<domains::PortalAPI>(managementGql_, auth_, *crypto_);
  platform_ = std::make_unique<domains::PlatformAPI>(managementGql_);
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
  gameModel_ = std::make_unique<domains::GameModelAPI>(
      gameGql_, gameSubscriptions_);
#ifndef CROWDY_NO_EXCEPTIONS
  compute_ = std::make_unique<domains::ComputeAPI>(gameGql_);
#endif
  playerCompute_ = std::make_unique<domains::PlayerComputeAPI>(gameGql_);
  playerWallet_ = std::make_unique<domains::PlayerWalletAPI>(managementGql_);
  marketplace_ = std::make_unique<domains::MarketplaceAPI>(gameGql_, managementGql_);
  playerModel_ = std::make_unique<domains::PlayerModelAPI>(gameGql_);
  gameApps_ = std::make_unique<domains::GameAppsAPI>(gameGql_);
#ifndef CROWDY_NO_EXCEPTIONS
  crowdyStudio_ = std::make_unique<domains::CrowdyStudioAPI>(gameGql_);
#endif
  crowdyStudioAgent_ = std::make_unique<domains::CrowdyStudioAgentAPI>(
      gameGql_, managementGql_, dispatcher_);

  admin_ = std::make_unique<domains::AdminAPI>(managementGql_, gameApps_.get());
}

CrowdyClient::~CrowdyClient() { close(); }

CrowdyClient::CrowdyClient(CrowdyClient&&) noexcept = default;
CrowdyClient& CrowdyClient::operator=(CrowdyClient&& other) noexcept {
  if (this == &other) return *this;
  close();

  config_ = std::move(other.config_);
  crypto_ = other.crypto_;
  transport_ = std::move(other.transport_);
  auth_ = std::move(other.auth_);
  dispatcher_ = std::move(other.dispatcher_);
  gameGql_ = std::move(other.gameGql_);
  managementGql_ = std::move(other.managementGql_);
  fallbackAsyncTransport_ =
      std::move(other.fallbackAsyncTransport_);
  websocketEndpoint_ = std::move(other.websocketEndpoint_);
  webSocketTransport_ = std::move(other.webSocketTransport_);
  gameSubscriptions_ = std::move(other.gameSubscriptions_);
  managementSubscriptions_ = std::move(other.managementSubscriptions_);
  authApi_ = std::move(other.authApi_);
  users_ = std::move(other.users_);
  portal_ = std::move(other.portal_);
  serverStatus_ = std::move(other.serverStatus_);
  chunks_ = std::move(other.chunks_);
  voxels_ = std::move(other.voxels_);
  actors_ = std::move(other.actors_);
  avatars_ = std::move(other.avatars_);
  state_ = std::move(other.state_);
  host_ = std::move(other.host_);
  teleport_ = std::move(other.teleport_);
  teams_ = std::move(other.teams_);
  channels_ = std::move(other.channels_);
  gameModel_ = std::move(other.gameModel_);
#ifndef CROWDY_NO_EXCEPTIONS
  compute_ = std::move(other.compute_);
#endif
  playerCompute_ = std::move(other.playerCompute_);
  playerWallet_ = std::move(other.playerWallet_);
  marketplace_ = std::move(other.marketplace_);
  playerModel_ = std::move(other.playerModel_);
  gameApps_ = std::move(other.gameApps_);
#ifndef CROWDY_NO_EXCEPTIONS
  crowdyStudio_ = std::move(other.crowdyStudio_);
#endif
  platform_ = std::move(other.platform_);
  crowdyStudioAgent_ = std::move(other.crowdyStudioAgent_);
  admin_ = std::move(other.admin_);
  operatorApi_ = std::move(other.operatorApi_);
  replication_ = std::move(other.replication_);
  return *this;
}

replication::ReplicationClient& CrowdyClient::replication() {
  if (!replication_) {
    auto provider = std::make_shared<ClientSessionProvider>(*serverStatus_, *portal_,
                                                            core::defaultLogger());
    replication_ = std::make_unique<replication::ReplicationClient>(
        std::move(provider), *crypto_);
  }
  return *replication_;
}

void CrowdyClient::ensureNonblockingAsyncTransport() {
  if (config_.asyncTransport || fallbackAsyncTransport_) return;
  fallbackAsyncTransport_ =
      graphql::makeThreadedAsyncTransport(transport_);
  gameGql_->setAsyncTransport(fallbackAsyncTransport_);
  managementGql_->setAsyncTransport(fallbackAsyncTransport_);
}

#ifndef CROWDY_NO_EXCEPTIONS
std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime>
CrowdyClient::createCrowdyStudioAgentController(
    agent::CrowdyStudioAgentControllerOptions options) {
  ensureNonblockingAsyncTransport();
  if (!webSocketTransport_) {
    return std::make_unique<agent::CrowdyStudioAgentControllerRuntime>(
        *crowdyStudioAgent_, std::move(options));
  }
  return std::make_unique<agent::CrowdyStudioAgentControllerRuntime>(
      *crowdyStudioAgent_, *gameSubscriptions_, std::move(options));
}

std::unique_ptr<studio::CrowdyStudioIntegration>
CrowdyClient::createCrowdyStudioIntegration(
    studio::CrowdyStudioIntegrationOptions options) {
  if (!options.crypto) {
    if (config_.crypto) {
      throw std::invalid_argument(
          "createCrowdyStudioIntegration requires options.crypto to own "
          "an externally injected crypto provider");
    }
    options.crypto = std::shared_ptr<const core::ICrypto>(
        crypto_, [](const core::ICrypto*) {});
  }

  auto projectApi =
      std::make_shared<domains::CrowdyStudioAPI>(gameGql_);
  auto playerCompute =
      std::make_shared<domains::PlayerComputeAPI>(gameGql_);
  auto runtime =
      std::make_shared<studio::CrowdyStudioPlayerComputeRuntime>(
          playerCompute, options.clientRuntime);
  if (options.observePlayerWallet && !options.walletProvider) {
    auto playerWallet =
        std::make_shared<domains::PlayerWalletAPI>(managementGql_);
    options.walletProvider =
        std::make_shared<studio::CrowdyStudioPlayerWalletProvider>(
            std::move(playerWallet));
  }

  const auto fallbackPoll = std::move(options.platformPoll);
  options.platformPoll =
      [dispatcher = dispatcher_, fallbackPoll]() mutable {
        std::size_t delivered = dispatcher ? dispatcher->drain() : 0;
        if (fallbackPoll) delivered += fallbackPoll();
        return delivered;
      };

  studio::CrowdyStudioAgentRuntimeFactory agentFactory;
  if (options.agent) {
    ensureNonblockingAsyncTransport();
    auto agentApi = std::make_shared<domains::CrowdyStudioAgentAPI>(
        gameGql_, managementGql_, dispatcher_);
    auto subscriptions = gameSubscriptions_;
    const bool realtimeAvailable =
        static_cast<bool>(webSocketTransport_);
    agentFactory =
        [agentApi = std::move(agentApi),
         subscriptions = std::move(subscriptions),
         realtimeAvailable](
            agent::CrowdyStudioAgentControllerOptions agentOptions) {
          if (!realtimeAvailable) {
            return std::make_unique<
                agent::CrowdyStudioAgentControllerRuntime>(
                agentApi, std::move(agentOptions));
          }
          return std::make_unique<
              agent::CrowdyStudioAgentControllerRuntime>(
              *agentApi, *subscriptions, std::move(agentOptions));
        };
  }
  return studio::CrowdyStudioIntegration::create(
      std::move(options), std::move(projectApi), std::move(runtime),
      std::move(agentFactory));
}
#endif

GameplayTokenRefreshResult CrowdyClient::refreshGameplayToken() {
  auto preparation = prepareGameplayRefresh(
      replication_ ? replication_->activeConnection() : nullptr);
  preparation.oldToken = auth_->token();
  if (!preparation.ready) return preparation.result;

  domains::AppTokenResponse token;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
#endif
    // Defer storage until the native token has been validated and the old
    // connection is quiescent.
    token = portal_->refresh(false);
#ifndef CROWDY_NO_EXCEPTIONS
  } catch (const std::exception& error) {
    setRefreshException(preparation.result,
                        GameplayTokenRefreshStage::Refresh, error);
    return preparation.result;
  }
#else
  if (token.token.empty()) {
    preparation.result.stage = GameplayTokenRefreshStage::Refresh;
    preparation.result.status = Errc::Rejected;
    preparation.result.errorCode = errcName(Errc::Rejected);
    preparation.result.errorMessage = "gameplay token refresh failed";
    return preparation.result;
  }
#endif
  return installGameplayRefresh(std::move(preparation), auth_,
                                std::move(token));
}

void CrowdyClient::refreshGameplayTokenAsync(
    GameplayTokenRefreshCallback cb) {
  auto preparation = prepareGameplayRefresh(
      replication_ ? replication_->activeConnection() : nullptr);
  preparation.oldToken = auth_->token();
  if (!preparation.ready) {
    dispatcher_->post(
        [cb = std::move(cb), result = std::move(preparation.result)]() mutable {
          cb(std::move(result));
        });
    return;
  }

  auto sharedPreparation =
      std::make_shared<GameplayRefreshPreparation>(std::move(preparation));
  auto sharedCallback =
      std::make_shared<GameplayTokenRefreshCallback>(std::move(cb));
#ifndef CROWDY_NO_EXCEPTIONS
  try {
#endif
    portal_->refreshAsync(
        [auth = auth_, sharedPreparation,
         sharedCallback](graphql::GraphQLOutcome outcome,
                         domains::AppTokenResponse token) mutable {
          if (!outcome.ok()) {
            setRefreshOutcome(sharedPreparation->result, outcome);
            (*sharedCallback)(std::move(sharedPreparation->result));
            return;
          }
          (*sharedCallback)(installGameplayRefresh(
              std::move(*sharedPreparation), auth, std::move(token)));
        },
        false);
#ifndef CROWDY_NO_EXCEPTIONS
  } catch (const std::exception& error) {
    setRefreshException(sharedPreparation->result,
                        GameplayTokenRefreshStage::Refresh, error);
    dispatcher_->post(
        [sharedCallback,
         result = std::move(sharedPreparation->result)]() mutable {
          (*sharedCallback)(std::move(result));
        });
  }
#endif
}

void CrowdyClient::poll() {
  if (dispatcher_) dispatcher_->drain();
}

void CrowdyClient::close() {
  if (dispatcher_) dispatcher_->close();
  if (gameGql_) gameGql_->close();
  if (managementGql_) managementGql_->close();
  replication_.reset();
  if (gameSubscriptions_) gameSubscriptions_->close();
  if (managementSubscriptions_) managementSubscriptions_->close();
}

}  // namespace crowdy
