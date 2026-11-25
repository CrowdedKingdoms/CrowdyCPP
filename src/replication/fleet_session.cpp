#include "crowdy/replication/fleet_session.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

#include "dispatch.hpp"

namespace crowdy::replication {

using wire::MessageType;

namespace {

bool sameServer(const Assignment& a, const Assignment& b) {
  return a.ip4 == b.ip4 && a.ip6 == b.ip6 && a.clientPort == b.clientPort;
}

FleetCarryStatus carryStatusOf(std::uint8_t wireStatus) {
  return wireStatus <= static_cast<std::uint8_t>(FleetCarryStatus::SessionFull)
             ? static_cast<FleetCarryStatus>(wireStatus)
             : FleetCarryStatus::Refused;
}

/// The long-spatial frame of a message that names no place and no actor: chunk (0,0,0),
/// distance 0, a zero UUID. CLIENT_CAPABILITIES is sent this way.
SpatialSend controlSend(Bytes payload) {
  SpatialSend p;
  p.chunk = wire::ChunkCoord{};
  p.distance = 0;
  p.decay = wire::DecayRate::None;
  p.payload = payload;
  return p;
}

}  // namespace

/// Where the session's connection finds its realtime server. A fleet session is given its
/// server (openFleetSession's answer) and keeps it; only a move the server asks for goes
/// back to the game server, through FleetConfig::reassign.
class FleetSession::Endpoint final : public ISessionProvider {
 public:
  Endpoint(Assignment server, std::function<Result<Assignment>()> reassign)
      : server_(std::move(server)), reassign_(std::move(reassign)) {}

  Result<Assignment> assignServer() override {
    if (!useHeld_.exchange(false, std::memory_order_acq_rel) && reassign_) {
      Result<Assignment> next = reassign_();
      if (!next.ok()) return next.error();
      std::lock_guard lock(mutex_);
      server_ = next.value();
      return server_;
    }
    std::lock_guard lock(mutex_);
    return server_;
  }

  /// The fleet token is the game server's to refresh (refreshFleetSession); the connection
  /// is configured never to ask.
  Result<TokenInfo> refreshToken() override { return Errc::Rejected; }

  /// The next assignment opens at the server held, without asking the game server.
  void useHeldNext() { useHeld_.store(true, std::memory_order_release); }

  void set(const Assignment& server) {
    std::lock_guard lock(mutex_);
    server_ = server;
  }
  Assignment get() const {
    std::lock_guard lock(mutex_);
    return server_;
  }

 private:
  mutable std::mutex mutex_;
  Assignment server_;
  std::function<Result<Assignment>()> reassign_;
  std::atomic<bool> useHeld_{true};
};

FleetSession::FleetSession(FleetConfig config, const core::ICrypto& crypto,
                           const core::IClock& clock, const core::ILogger& logger)
    : config_(std::move(config)),
      crypto_(crypto),
      clock_(clock),
      endpoint_(std::make_shared<Endpoint>(config_.server, config_.reassign)) {
  Config transport = config_.transport;
  transport.appId = config_.appId;
  transport.token = config_.token;
  transport.refreshLeadMs = 0;
  connection_ = std::make_unique<Connection>(std::move(transport), endpoint_, crypto_, clock_,
                                             logger);

  auto hooks = std::make_unique<Connection::FleetHooks>();
  hooks->message = [this](Bytes message) { onFleetMessage(message); };
  hooks->tick = [this] { onTick(); };
  hooks->advertised = [this] { onAdvertised(); };
  hooks->capabilities.store(config_.fleetOwnState ? wire::ClientCapability::kFleetOwnState : 0u,
                            std::memory_order_release);
  hooks->requireSignedBundle = config_.requireSignedControl;
  connection_->fleet_ = std::move(hooks);

  // Whatever arrives outside a FLEET_DELIVERY is for the session as a whole: no recipients.
  Handlers plain;
  plain.any = [this](const SpatialNotification& n) {
    detail::dispatchSpatial(active_, n, FleetRecipients{});
  };
  plain.channelMessage = [this](const ChannelNotification& n) {
    if (active_.channelMessage) active_.channelMessage(n, FleetRecipients{});
  };
  plain.channelAudio = [this](const ChannelNotification& n) {
    if (active_.channelAudio) active_.channelAudio(n, FleetRecipients{});
  };
  plain.genericError = [this](const GenericError& e) {
    if (active_.genericError) active_.genericError(e);
  };
  plain.status = [this](ConnState s) {
    if (active_.status) active_.status(s);
  };
  connection_->setHandlers(std::move(plain));
}

FleetSession::~FleetSession() { connection_->disconnect(); }

Status FleetSession::open() {
  const ConnState s = state();
  if (s != ConnState::Connected && s != ConnState::Connecting) endpoint_->useHeldNext();
  return connection_->connect();
}

void FleetSession::close() {
  connection_->disconnect();
  std::lock_guard lock(playersMutex_);
  players_.clear();
  attemptsPending_ = false;
}

void FleetSession::setHandlers(FleetHandlers handlers) {
  std::lock_guard lock(handlersMutex_);
  handlers_ = std::move(handlers);
}

Status FleetSession::setToken(const TokenInfo& token) {
  const auto key = wire::Token64::fromString(token.token);
  if (!key) return Errc::InvalidArgument;
  const Connection::Credentials before = connection_->credentials();
  connection_->setToken(token);
  const bool changed = before.gameTokenId != token.gameTokenId ||
                       std::memcmp(before.token.octets, key->octets, wire::kTokenOctets) != 0;
  if (changed) {
    // A proof names the fleet session's gameTokenId, and the server knows the carries by
    // the session that made them.
    std::lock_guard lock(playersMutex_);
    recarryAllLocked();
    if (state() == ConnState::Connected) driveCarriesLocked(clock_.monotonicMillis());
  }
  return Errc::Ok;
}

TokenInfo FleetSession::token() const {
  std::lock_guard lock(connection_->tokenMutex_);
  return connection_->token_;
}

Status FleetSession::setServer(const Assignment& server) {
  const std::string& host =
      (config_.transport.preferIpv6 && !server.ip6.empty()) ? server.ip6 : server.ip4;
  if (host.empty() || server.clientPort <= 0) return Errc::InvalidArgument;
  if (sameServer(endpoint_->get(), server)) return Errc::Ok;
  endpoint_->set(server);
  const ConnState s = state();
  if (s == ConnState::Connected || s == ConnState::Connecting || s == ConnState::Reconnecting) {
    endpoint_->useHeldNext();
    connection_->requestReassignment();
  }
  return Errc::Ok;
}

Assignment FleetSession::server() const { return endpoint_->get(); }

void FleetSession::setFleetOwnState(bool on) {
  constexpr std::uint32_t bit = wire::ClientCapability::kFleetOwnState;
  auto& capabilities = connection_->fleet_->capabilities;
  const std::uint32_t before = on ? capabilities.fetch_or(bit, std::memory_order_acq_rel)
                                  : capabilities.fetch_and(~bit, std::memory_order_acq_rel);
  if (((before & bit) != 0) == on) return;
  if (config_.transport.advertiseCapabilities && state() == ConnState::Connected)
    (void)connection_->sendCapabilities();
}

bool FleetSession::fleetOwnState() const noexcept {
  return (connection_->fleet_->capabilities.load(std::memory_order_acquire) &
          wire::ClientCapability::kFleetOwnState) != 0;
}

// ---------------------------------------------------------------------------
// Carrying
// ---------------------------------------------------------------------------

Status FleetSession::carry(const PlayerToken& player, CarryCallback done) {
  const auto key = wire::Token64::fromString(player.token);
  if (!key) return Errc::InvalidArgument;
  if (player.gameTokenId == connection_->credentials().gameTokenId) return Errc::InvalidArgument;
  const ConnState s = state();
  if (s != ConnState::Connected && s != ConnState::Connecting && s != ConnState::Reconnecting)
    return Errc::NotConnected;

  std::lock_guard lock(playersMutex_);
  Player& entry = players_[player.gameTokenId];
  entry.token = *key;
  entry.attempt = true;
  entry.nextSendMs = 0;
  entry.deadlineMs = 0;
  entry.order = ++carryOrder_;
  entry.done = std::move(done);
  attemptsPending_ = true;
  // Before the session is connected the carry waits for the pass that finds it so.
  if (s == ConnState::Connected) sendCarryLocked(player.gameTokenId, entry, clock_.monotonicMillis());
  return Errc::Ok;
}

void FleetSession::sendCarryLocked(std::int64_t id, Player& player, std::int64_t nowMono) {
  // One read of the session's id for the proof and the tail, so a token change between
  // them cannot make the two disagree.
  const std::int64_t fleetId = connection_->credentials().gameTokenId;
  std::uint8_t payload[wire::fleet::kCarryPayloadSize];
  const auto n = wire::encodeFleetCarryPayload(crypto_, player.token, fleetId, id,
                                               clock_.epochMillis(),
                                               MutableBytes(payload, sizeof(payload)));
  // A send that fails (a full send buffer, a socket between two servers) is tried again at
  // the next interval like one that was lost.
  if (n.ok())
    (void)connection_->sendLongSpatial(MessageType::FleetCarry,
                                       controlSend(Bytes(payload, n.value())), &fleetId);
  player.nextSendMs = nowMono + std::max(1, config_.carryResendMs);
  if (player.deadlineMs == 0) player.deadlineMs = nowMono + std::max(0, config_.carryTimeoutMs);
}

void FleetSession::driveCarriesLocked(std::int64_t nowMono) {
  if (!attemptsPending_) return;
  bool any = false;
  for (auto& [id, player] : players_) {
    if (!player.attempt) continue;
    any = true;
    // Past its deadline a carry is poll()'s to report; it is not sent again.
    if (player.deadlineMs != 0 && nowMono >= player.deadlineMs) continue;
    if (nowMono >= player.nextSendMs) sendCarryLocked(id, player, nowMono);
  }
  attemptsPending_ = any;
}

void FleetSession::recarryAllLocked() {
  for (auto& [id, player] : players_) {
    (void)id;
    if (!player.attempt && !player.carried) continue;
    player.attempt = true;
    player.nextSendMs = 0;
    player.deadlineMs = 0;  // a fresh window on the new session
    attemptsPending_ = true;
  }
}

void FleetSession::onTick() {
  if (connection_->state() != ConnState::Connected) return;
  const std::uint64_t epoch = connection_->sessionEpoch();
  std::lock_guard lock(playersMutex_);
  if (epoch != carriedEpoch_) {
    // A new session, on this realtime server or another: it knows nothing of the carries
    // the last one held, as it knows nothing of its capabilities.
    recarryAllLocked();
    carriedEpoch_ = epoch;
  }
  driveCarriesLocked(clock_.monotonicMillis());
}

Status FleetSession::release(std::int64_t playerGameTokenId) {
  {
    std::lock_guard lock(playersMutex_);
    players_.erase(playerGameTokenId);
  }
  std::uint8_t payload[wire::fleet::kReleasePayloadSize];
  const auto n = wire::encodeFleetReleasePayload(playerGameTokenId,
                                                 MutableBytes(payload, sizeof(payload)));
  if (!n.ok()) return n.error();
  const auto sent = connection_->sendLongSpatial(MessageType::FleetRelease,
                                                 controlSend(Bytes(payload, n.value())));
  return sent.ok() ? Status(Errc::Ok) : Status(sent.error());
}

bool FleetSession::carries(std::int64_t playerGameTokenId) const {
  std::lock_guard lock(playersMutex_);
  const auto it = players_.find(playerGameTokenId);
  return it != players_.end() && it->second.carried;
}

std::optional<FleetPlayer> FleetSession::player(std::int64_t playerGameTokenId) const {
  std::lock_guard lock(playersMutex_);
  const auto it = players_.find(playerGameTokenId);
  if (it == players_.end()) return std::nullopt;
  return FleetPlayer{it->first, it->second.userId, it->second.carried, it->second.attempt};
}

std::vector<FleetPlayer> FleetSession::players() const {
  std::lock_guard lock(playersMutex_);
  std::vector<FleetPlayer> out;
  out.reserve(players_.size());
  for (const auto& [id, player] : players_)
    out.push_back(FleetPlayer{id, player.userId, player.carried, player.attempt});
  return out;
}

std::size_t FleetSession::carriedCount() const {
  std::lock_guard lock(playersMutex_);
  std::size_t n = 0;
  for (const auto& [id, player] : players_) {
    (void)id;
    if (player.carried) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Sends for a carried player
// ---------------------------------------------------------------------------

Result<std::uint8_t> FleetSession::sendActorUpdate(std::int64_t player, const SpatialSend& p) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendLongSpatial(MessageType::ActorUpdateRequest, p, &player);
}

Result<std::uint8_t> FleetSession::sendVoxelUpdate(std::int64_t player,
                                                   const wire::ChunkCoord& chunk,
                                                   const core::ActorUuid& uuid, std::int16_t x,
                                                   std::int16_t y, std::int16_t z,
                                                   std::int16_t voxelType, Bytes voxelState,
                                                   std::uint8_t distance, wire::DecayRate decay) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendVoxelUpdateAs(&player, chunk, uuid, x, y, z, voxelType, voxelState,
                                        distance, decay);
}

Result<std::uint8_t> FleetSession::sendAudio(std::int64_t player, const SpatialSend& p) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendLongSpatial(MessageType::ClientAudioPacket, p, &player);
}

Result<std::uint8_t> FleetSession::sendVideo(std::int64_t player, const SpatialSend& p) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendLongSpatial(MessageType::ClientVideoPacket, p, &player);
}

Result<std::size_t> FleetSession::sendVideoFrame(std::int64_t player,
                                                 const wire::ChunkCoord& chunk,
                                                 const core::ActorUuid& uuid, Bytes frame,
                                                 std::uint16_t frameId, std::uint8_t codec,
                                                 std::uint8_t distance, wire::DecayRate decay) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendVideoFrameAs(&player, chunk, uuid, frame, frameId, codec, distance,
                                       decay);
}

Result<std::uint8_t> FleetSession::sendText(std::int64_t player, const SpatialSend& p) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendLongSpatial(MessageType::ClientTextPacket, p, &player);
}

Result<std::uint8_t> FleetSession::sendClientEvent(std::int64_t player,
                                                   const wire::ChunkCoord& chunk,
                                                   const core::ActorUuid& uuid,
                                                   std::uint16_t eventType, Bytes state,
                                                   std::uint8_t distance, wire::DecayRate decay) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendClientEventAs(&player, chunk, uuid, eventType, state, distance, decay);
}

Result<std::uint8_t> FleetSession::sendGenericSpatial(std::int64_t player, const SpatialSend& p) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendLongSpatial(MessageType::GenericSpatial1, p, &player);
}

Result<std::uint8_t> FleetSession::sendSingleActorMessage(std::int64_t player,
                                                          const wire::ChunkCoord& targetChunk,
                                                          const core::ActorUuid& targetUuid,
                                                          Bytes payload) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendSingleActorMessageAs(&player, targetChunk, targetUuid, payload);
}

Result<std::uint8_t> FleetSession::sendChannelMessage(std::int64_t player,
                                                      std::int64_t channelId,
                                                      const core::ActorUuid& uuid,
                                                      Bytes payload) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendChannelRequest(MessageType::ChannelMessageRequest, channelId, uuid,
                                         payload, &player);
}

Result<std::uint8_t> FleetSession::sendChannelAudio(std::int64_t player, std::int64_t channelId,
                                                    const core::ActorUuid& uuid, Bytes payload) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendChannelRequest(MessageType::ChannelAudioRequest, channelId, uuid,
                                         payload, &player);
}

Result<std::uint8_t> FleetSession::sendRangedChannelMessage(std::int64_t player,
                                                            std::int64_t channelId,
                                                            const core::ActorUuid& uuid,
                                                            Bytes payload,
                                                            const wire::ChunkCoord& origin,
                                                            std::uint32_t maxDistance) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendRangedChannelMessageAs(&player, channelId, uuid, payload, origin,
                                                 maxDistance);
}

Result<std::uint8_t> FleetSession::sendHeartbeat(std::int64_t player,
                                                 const wire::ChunkCoord& chunk,
                                                 const core::ActorUuid& uuid) {
  if (!carries(player)) return Errc::NotCarried;
  return connection_->sendHeartbeatAs(&player, chunk, uuid);
}

Result<std::uint8_t> FleetSession::sendHeartbeat() {
  return connection_->sendLongSpatial(MessageType::ClientActorHeartbeat, controlSend(Bytes()));
}

Status FleetSession::setSpeakers(std::int64_t player, SpeakerList voice, SpeakerList video) {
  if (voice.names.size() > wire::kMaxSpeakers || video.names.size() > wire::kMaxSpeakers)
    return Errc::InvalidArgument;
  std::lock_guard lock(playersMutex_);
  const auto it = players_.find(player);
  if (it == players_.end() || !it->second.carried) return Errc::NotCarried;
  Player& entry = it->second;
  if (voice.mode != wire::SpeakerMode::Keep)
    entry.voiceSpeakers = voice.mode == wire::SpeakerMode::Only
                              ? std::optional<std::vector<core::ActorUuid>>(voice.names)
                              : std::nullopt;
  if (video.mode != wire::SpeakerMode::Keep)
    entry.videoSpeakers = video.mode == wire::SpeakerMode::Only
                              ? std::optional<std::vector<core::ActorUuid>>(video.names)
                              : std::nullopt;
  if (state() != ConnState::Connected) return Errc::Ok;
  // One kind a message, as Connection sends them: 32 names of each would not fit one datagram.
  if (voice.mode != wire::SpeakerMode::Keep) {
    const auto sent = connection_->sendSpeakerLists(voice, SpeakerList::keep(), &player);
    if (!sent.ok()) return sent.error();
  }
  if (video.mode != wire::SpeakerMode::Keep) {
    const auto sent = connection_->sendSpeakerLists(SpeakerList::keep(), video, &player);
    if (!sent.ok()) return sent.error();
  }
  return Errc::Ok;
}

void FleetSession::sendSpeakersLocked(std::int64_t id, const Player& player) {
  if (player.voiceSpeakers)
    (void)connection_->sendSpeakerLists(SpeakerList::only(*player.voiceSpeakers),
                                        SpeakerList::keep(), &id);
  if (player.videoSpeakers)
    (void)connection_->sendSpeakerLists(SpeakerList::keep(),
                                        SpeakerList::only(*player.videoSpeakers), &id);
}

void FleetSession::onAdvertised() {
  // The lists live with the session on the server, as the capabilities do. A player being
  // carried again gets theirs when that carry is confirmed.
  std::lock_guard lock(playersMutex_);
  for (const auto& [id, player] : players_) {
    if (player.carried && !player.attempt) sendSpeakersLocked(id, player);
  }
}

// ---------------------------------------------------------------------------
// Receive path (the poll() thread)
// ---------------------------------------------------------------------------

std::size_t FleetSession::poll(std::size_t maxEvents) {
  {
    std::lock_guard lock(handlersMutex_);
    active_ = handlers_;
  }
  const std::size_t dispatched = connection_->poll(maxEvents);
  expireCarries();
  return dispatched;
}

void FleetSession::report(const FleetCarryResult& result, const CarryCallback& done) {
  if (done) done(result);
  if (active_.carryResult) active_.carryResult(result);
}

void FleetSession::onFleetMessage(Bytes message) {
  if (message.empty()) return;
  switch (static_cast<MessageType>(message[0])) {
    case MessageType::FleetCarryResult:
      if (const auto result = wire::parseFleetCarryResult(message); result.ok())
        onCarryResult(result.value());
      break;
    case MessageType::FleetReleased:
      if (const auto released = wire::parseFleetReleased(message); released.ok())
        onReleased(released.value());
      break;
    case MessageType::FleetDelivery:
      if (const auto delivery = wire::parseFleetDelivery(message); delivery.ok())
        onDelivery(delivery.value());
      break;
    default:
      break;
  }
}

void FleetSession::onCarryResult(const wire::FleetCarryResultView& view) {
  FleetCarryResult result;
  result.playerGameTokenId = view.playerGameTokenId;
  result.wireStatus = view.status;
  result.status = carryStatusOf(view.status);
  CarryCallback done;
  {
    std::lock_guard lock(playersMutex_);
    const auto it = players_.find(view.playerGameTokenId);
    // Nothing waits: a second answer to a carry that was sent again, or an answer for a
    // player released since.
    if (it == players_.end() || !it->second.attempt) return;
    Player& entry = it->second;
    done = std::move(entry.done);
    entry.done = nullptr;
    if (!result.carried()) {
      players_.erase(it);
    } else {
      result.userId = view.userId;
      entry.attempt = false;
      entry.carried = true;
      entry.userId = view.userId;
      entry.nextSendMs = 0;
      entry.deadlineMs = 0;
      // The same user under a token carried earlier: this carry replaced that one on the
      // server, so its entry goes and its speaker lists come along.
      for (auto other = players_.begin(); other != players_.end();) {
        Player& old = other->second;
        if (other->first == view.playerGameTokenId || view.userId == 0 ||
            old.userId != view.userId || old.order >= entry.order) {
          ++other;
          continue;
        }
        if (!entry.voiceSpeakers) entry.voiceSpeakers = std::move(old.voiceSpeakers);
        if (!entry.videoSpeakers) entry.videoSpeakers = std::move(old.videoSpeakers);
        other = players_.erase(other);
      }
      if (state() == ConnState::Connected) sendSpeakersLocked(view.playerGameTokenId, entry);
    }
  }
  report(result, done);
}

void FleetSession::onReleased(const wire::FleetReleasedView& view) {
  FleetReleased released;
  released.playerGameTokenId = view.playerGameTokenId;
  released.reason = static_cast<FleetReleaseReason>(view.reason);
  {
    std::lock_guard lock(playersMutex_);
    const auto it = players_.find(view.playerGameTokenId);
    if (it != players_.end()) {
      released.userId = it->second.userId;
      // A carry still waiting keeps its row until its own result or deadline; the player
      // is not carried meanwhile.
      if (it->second.attempt) it->second.carried = false;
      else players_.erase(it);
    }
  }
  if (active_.released) active_.released(released);
}

void FleetSession::onDelivery(const wire::FleetDeliveryView& view) {
  std::int64_t ids[wire::fleet::kMaxDeliveryRecipients];
  for (std::size_t i = 0; i < view.count; ++i) ids[i] = view.recipient(i);
  const FleetRecipients recipients(ids, view.count);

  const Bytes inner = view.notification;
  const std::uint8_t type = inner[0];
  if (type == static_cast<std::uint8_t>(MessageType::ChannelMessageNotification) ||
      type == static_cast<std::uint8_t>(MessageType::ChannelAudioNotification)) {
    const auto ch = wire::parseChannelNotification(inner);
    if (!ch.ok()) return;
    const ChannelNotification n{ch->channelId, ch->senderUuid, ch->payload, ch->epochMillis,
                                ch->sequence};
    const bool audio = type == static_cast<std::uint8_t>(MessageType::ChannelAudioNotification);
    if (audio && active_.channelAudio) active_.channelAudio(n, recipients);
    if (!audio && active_.channelMessage) active_.channelMessage(n, recipients);
    return;
  }
  const auto v = wire::parseLongSpatial(inner);
  if (!v.ok()) return;
  const SpatialNotification n{v->type,    v->appId,                v->chunk,   v->uuid,
                              v->payload, v->epochMillisOrTokenId, v->sequence};
  detail::dispatchSpatial(active_, n, recipients);
}

void FleetSession::expireCarries() {
  std::vector<std::pair<FleetCarryResult, CarryCallback>> expired;
  {
    std::lock_guard lock(playersMutex_);
    if (!attemptsPending_) return;
    const std::int64_t now = clock_.monotonicMillis();
    for (auto it = players_.begin(); it != players_.end();) {
      Player& player = it->second;
      if (!player.attempt || player.deadlineMs == 0 || now < player.deadlineMs) {
        ++it;
        continue;
      }
      FleetCarryResult result;
      result.playerGameTokenId = it->first;
      result.status = FleetCarryStatus::TimedOut;
      expired.emplace_back(result, std::move(player.done));
      it = players_.erase(it);
    }
  }
  for (const auto& [result, done] : expired) report(result, done);
}

}  // namespace crowdy::replication
