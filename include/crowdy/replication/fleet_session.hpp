#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/replication/connection.hpp"

/// A fleet session: one realtime session an organization's game server opens for itself,
/// carrying the players who are on that server.
///
/// THIS IS FOR AN ORGANIZATION'S OWN SERVERS. A fleet token signs for every player the
/// session carries, so it stays where the organization's org token stays and never reaches a
/// player's device. A game on a player's device uses Connection with that player's own token.
///
/// The game server signs each player in once (`mintManagedPlayerToken`), opens one fleet
/// session (the Application API's `openFleetSession`, which answers the fleet token, its
/// gameTokenId and a realtime server; client.admin().fleetSessions() in
/// crowdy/domains/fleet_sessions.hpp wraps it and builds this class's FleetConfig from the
/// answer), and then:
///
///   FleetSession fleet(config, crypto);
///   fleet.open();
///   fleet.carry({playerToken, playerGameTokenId}, [](const FleetCarryResult& r) { ... });
///   fleet.sendActorUpdate(playerGameTokenId, update);   // once the carry is confirmed
///   fleet.poll();                                       // every tick: handlers fire here
///
/// A player who moves to another of the organization's servers is carried by that server's
/// session; this one hears FleetHandlers::released with FleetReleaseReason::CarriedElsewhere.
///
/// On the wire a message sent for a carried player is the message that player's own session
/// would send, with the tail's game token id the player's and the HMAC keyed with the fleet
/// token. The session's own messages (its capabilities, the carry and release messages, its
/// own heartbeat) carry the fleet session's gameTokenId.
///
/// This class makes no Application API call. It takes the token and the server it is given,
/// so a server with its own HTTP client uses it as it is; refreshDue() says when to call
/// `refreshFleetSession`, and applyRefresh() takes the answer.
///
/// Threading is Connection's: by default the session owns one network thread and you call
/// poll() from your game thread; with FleetConfig::transport.manualPump you call pump()
/// yourself. Every send, carry() and release() may be called from any thread and never
/// throws. Every callback and handler fires on the thread that calls poll().
namespace crowdy::replication {

/// A player's app token, as `mintManagedPlayerToken` or `refreshAppToken` answered it.
struct PlayerToken {
  std::string token;  ///< 64 characters
  std::int64_t gameTokenId = 0;
};

/// How a carry ended. 0 to 4 are FLEET_CARRY_RESULT's status byte.
enum class FleetCarryStatus : std::uint8_t {
  /// The session carries the player; FleetCarryResult::userId says who they are.
  Carried = 0,
  /// The realtime servers know no live token with that gameTokenId (expired, or the
  /// player's sign-in ended).
  NoLiveToken = 1,
  /// The token belongs to another organization's player, or to another app.
  OtherOrganizationOrApp = 2,
  /// The proof did not hold, or this machine's clock is more than 30 seconds from the
  /// server's.
  ProofFailed = 3,
  /// The session already carries as many players as it may (`openFleetSession`'s
  /// `maxPlayers`).
  SessionFull = 4,
  /// No answer came before FleetConfig::carryTimeoutMs. Never on the wire.
  TimedOut = 254,
  /// A refusal this build does not know; FleetCarryResult::wireStatus has the byte.
  Refused = 255,
};

inline const char* fleetCarryStatusName(FleetCarryStatus s) {
  switch (s) {
    case FleetCarryStatus::Carried: return "Carried";
    case FleetCarryStatus::NoLiveToken: return "NoLiveToken";
    case FleetCarryStatus::OtherOrganizationOrApp: return "OtherOrganizationOrApp";
    case FleetCarryStatus::ProofFailed: return "ProofFailed";
    case FleetCarryStatus::SessionFull: return "SessionFull";
    case FleetCarryStatus::TimedOut: return "TimedOut";
    case FleetCarryStatus::Refused: return "Refused";
  }
  return "?";
}

struct FleetCarryResult {
  std::int64_t playerGameTokenId = 0;
  /// The player's user id when carried; 0 otherwise.
  std::int64_t userId = 0;
  FleetCarryStatus status = FleetCarryStatus::TimedOut;
  /// The status byte the server sent; 255 when none came (TimedOut).
  std::uint8_t wireStatus = 255;

  bool carried() const { return status == FleetCarryStatus::Carried; }
};

/// Why a carry ended (FLEET_RELEASED's reason byte). Treat a value not listed as the carry
/// having ended.
enum class FleetReleaseReason : std::uint8_t {
  /// Another session carries the player now: they moved to another of your servers.
  CarriedElsewhere = 1,
  /// The player's sign-in ended (deauthorized, expired, blocked or deleted) and no carry
  /// with a live token followed.
  SignInEnded = 2,
  /// No gameplay input from the player for the app's idle timeout.
  Idle = 5,
};

struct FleetReleased {
  std::int64_t playerGameTokenId = 0;
  /// The player's user id when this session knew it; 0 otherwise.
  std::int64_t userId = 0;
  FleetReleaseReason reason = FleetReleaseReason::SignInEnded;
};

/// One row of the carried-players table.
struct FleetPlayer {
  std::int64_t gameTokenId = 0;
  /// 0 until the first carry is confirmed.
  std::int64_t userId = 0;
  /// The server confirmed a carry: sends for the player are accepted.
  bool carried = false;
  /// A FLEET_CARRY is waiting for its result (the first carry, a carry again after the
  /// session moved to another realtime server, or a carry with a refreshed token).
  bool pending = false;
};

/// The carried players who may receive a notification, by gameTokenId. Empty for state (an
/// actor update, a voxel edit, an event), which a fleet session is sent once however many
/// of its players are in range. Valid only during the callback.
using FleetRecipients = std::span<const std::int64_t>;

/// Connection's Handlers, each with the recipients after its own arguments. State arrives
/// with no recipients; a communication (spatial voice, text and video, direct messages,
/// channel messages and channel audio) arrives with the carried players the platform's
/// rules let receive it, and the game server delivers it to those players and no others.
/// All callbacks fire on the thread that calls poll().
struct FleetHandlers {
  std::function<void(const SpatialNotification&, FleetRecipients)> actorUpdate;
  std::function<void(const SpatialNotification&, const wire::VoxelPayloadView&, FleetRecipients)>
      voxelUpdate;
  std::function<void(const SpatialNotification&, FleetRecipients)> audio;
  std::function<void(const SpatialNotification&, FleetRecipients)> video;
  std::function<void(const SpatialNotification&, std::uint8_t reason, FleetRecipients)> actorLeft;
  std::function<void(const SpatialNotification&, std::int64_t updatedAtMs, FleetRecipients)>
      chunkChanged;
  std::function<void(const SpatialNotification&, FleetRecipients)> text;
  std::function<void(const SpatialNotification&, const wire::EventPayloadView&, FleetRecipients)>
      clientEvent;
  std::function<void(const SpatialNotification&, const wire::EventPayloadView&, FleetRecipients)>
      serverEvent;
  std::function<void(const SpatialNotification&, FleetRecipients)> genericSpatial;
  std::function<void(const SpatialNotification&, FleetRecipients)> singleActorMessage;
  std::function<void(const ChannelNotification&, FleetRecipients)> channelMessage;
  std::function<void(const ChannelNotification&, FleetRecipients)> channelAudio;
  /// A refusal of one send, by its sequence number (wire::ErrorCode::Unauthorized for a
  /// player the server does not count as carried by this session).
  std::function<void(const GenericError&)> genericError;
  /// Every spatial notification, before the typed handler.
  std::function<void(const SpatialNotification&, FleetRecipients)> any;
  std::function<void(ConnState)> status;
  /// Every carry that ends, after the callback given to carry(): the first carry of a
  /// player, a carry with a refreshed token, and the carry again after the session moved.
  std::function<void(const FleetCarryResult&)> carryResult;
  /// The server ended a carry: this session no longer carries the player when this fires.
  std::function<void(const FleetReleased&)> released;
};

struct FleetConfig {
  std::int64_t appId = 0;
  /// The fleet token (64 characters), its gameTokenId and when it expires, as
  /// `openFleetSession` answered them. The expiry is kept for token(); the game server
  /// renews with `refreshFleetSession` and calls setToken().
  TokenInfo token;
  /// The realtime server `openFleetSession` answered (`ip4`, `ip6`, `clientPort`).
  Assignment server;

  /// Ask for the state this session's own carried players sent, when another of its
  /// carried players is in range (ClientCapability::kFleetOwnState). Off by default: a game
  /// server simulates its own players. setFleetOwnState() changes it while connected.
  bool fleetOwnState = false;

  /// A FLEET_CARRY is sent again this often, with a fresh time and proof, until its result
  /// arrives.
  int carryResendMs = 250;
  /// A carry with no result this long after it was first sent ends TimedOut.
  int carryTimeoutMs = 5000;

  /// Take FLEET_CARRY_RESULT, FLEET_RELEASED and FLEET_DELIVERY only from inside a verified
  /// MESSAGE_BUNDLE_SIGNED, and count any other as Stats::hmacFailures. None of the three
  /// carries an HMAC of its own. Off by default, as Connection takes a channel notification
  /// or an error frame either way.
  bool requireSignedControl = false;

  /// Called when the session needs a realtime server again after open(): the server asked
  /// it to move (COMMAND_RECONNECT), answered TOKEN_EXPIRED, or the silence watchdog fired.
  /// Return the server `refreshFleetSession` answers. It runs on the network thread (the
  /// pump() caller with manualPump) and may block. Unset, the session opens again at the
  /// server it has; setServer() moves it later.
  std::function<Result<Assignment>()> reassign;

  /// Socket, bundling, pump and watchdog options, with the meanings Connection gives them.
  /// `appId` and `token` here are ignored (the two above are used), and so is
  /// `refreshLeadMs`: the game server refreshes its fleet token itself.
  Config transport;
};

class FleetSession {
 public:
  using Stats = Connection::Stats;
  using CarryCallback = std::function<void(const FleetCarryResult&)>;

  FleetSession(FleetConfig config, const core::ICrypto& crypto,
               const core::IClock& clock = core::systemClock(),
               const core::ILogger& logger = core::defaultLogger());
  ~FleetSession();

  FleetSession(const FleetSession&) = delete;
  FleetSession& operator=(const FleetSession&) = delete;

  /// Open the socket to FleetConfig::server. After it returns the state is Connecting until
  /// the session-ready wait elapses, then Connected (the status handler fires from poll()).
  /// InvalidArgument for a token that is not 64 characters.
  Status open();
  /// Stop the network thread and close the socket; state -> Closed. The carried table is
  /// emptied and a carry still waiting reports nothing. This does not end the session on
  /// the platform: that is the Application API's `closeFleetSession`.
  void close();

  ConnState state() const { return connection_->state(); }

  /// Replace the handler set (call before open() or between polls).
  void setHandlers(FleetHandlers handlers);

  /// Take a refreshed fleet token (`refreshFleetSession`: the same token and id, a new
  /// expiry). A different token or id is taken too, and every carried player is then
  /// carried again, since a proof names the fleet session's gameTokenId. InvalidArgument
  /// for a token that is not 64 characters.
  Status setToken(const TokenInfo& token);
  TokenInfo token() const;

  /// Move to another realtime server (`refreshFleetSession` answered a different one).
  /// The session reconnects there and carries its players again. Naming the server it is
  /// on does nothing. InvalidArgument without an address or a port.
  Status setServer(const Assignment& server);
  /// The server the session is on, or will open at.
  Assignment server() const;
  /// The app the session is in (FleetConfig::appId).
  std::int64_t appId() const { return config_.appId; }

  /// Take a `refreshFleetSession` answer in one call: setToken with the new expiry, then
  /// setServer with the answer's server, which moves the session only when it differs. It
  /// answers a move order (moveOrdered() is false afterwards). InvalidArgument, with nothing
  /// changed, for a token that is not 64 characters or a server without an address or a port.
  /// domains::applyFleetRefresh() calls this with the Application API's answer.
  Status applyRefresh(const TokenInfo& token, const Assignment& server);

  /// True when the realtime server told the session to move (COMMAND_RECONNECT), answered
  /// TOKEN_EXPIRED, or the silence watchdog fired, and no FleetConfig::reassign answered it:
  /// the session opened again at the server it has. Call `refreshFleetSession` without a
  /// `currentServer` and apply the answer; setServer and applyRefresh clear it.
  bool moveOrdered() const;
  /// True when it is time to call `refreshFleetSession`: a move was ordered (moveOrdered()),
  /// or less than half of the token's lifetime is left, counted from when this session was
  /// given the token to its `expiresAtEpochMs` (12 hours into a 24-hour token). False for a
  /// token with no expiry. The library makes no call itself; ask this from your own loop.
  bool refreshDue() const;

  /// Start or stop asking for the state this session's own carried players sent
  /// (ClientCapability::kFleetOwnState). Advertised at once when connected and it changed;
  /// the starting value is FleetConfig::fleetOwnState. Any thread.
  void setFleetOwnState(bool on);
  bool fleetOwnState() const noexcept;
  /// Connection::setOwnMediaEcho for this session; the starting value is
  /// FleetConfig::transport.ownMediaEcho.
  void setOwnMediaEcho(bool on) { connection_->setOwnMediaEcho(on); }

  // ----- Carrying -------------------------------------------------------------

  /// Start carrying a player. Sends FLEET_CARRY now when connected (once connected
  /// otherwise) and again every FleetConfig::carryResendMs until FLEET_CARRY_RESULT arrives
  /// or FleetConfig::carryTimeoutMs passes. `done` then fires once from poll(), followed by
  /// FleetHandlers::carryResult. Sends for the player are accepted from the moment the
  /// result is Carried.
  ///
  /// Call it again with the player's refreshed token: when that carry is confirmed it
  /// replaces the entry the same user had under the older token, whose gameTokenId this
  /// session then refuses. A carry that is refused or times out leaves the player not
  /// carried under that gameTokenId. A carry repeated before its result reports once, to
  /// the newer callback.
  ///
  /// InvalidArgument for a token that is not 64 characters or a gameTokenId that is the
  /// session's own; NotConnected before open() and after close().
  Status carry(const PlayerToken& player, CarryCallback done = {});

  /// The player left the game: sends FLEET_RELEASE and takes them out of the carried table
  /// at once. Their actors leave as when a session ends. A carry still waiting for the
  /// player reports nothing. The message is sent whether or not the table had the player;
  /// it has no acknowledgement.
  Status release(std::int64_t playerGameTokenId);

  /// True when the server confirmed a carry of this gameTokenId and nothing ended it.
  bool carries(std::int64_t playerGameTokenId) const;
  std::optional<FleetPlayer> player(std::int64_t playerGameTokenId) const;
  /// Every row of the table: carried players and carries still waiting.
  std::vector<FleetPlayer> players() const;
  /// How many players the server confirmed and nothing ended.
  std::size_t carriedCount() const;

  // ----- Sends for a carried player -------------------------------------------
  //
  // Connection's sends with the carried player first. Each returns Errc::NotCarried, with
  // nothing sent, for a gameTokenId this session does not carry now.

  Result<std::uint8_t> sendActorUpdate(std::int64_t player, const SpatialSend& p);
  Result<std::uint8_t> sendVoxelUpdate(std::int64_t player, const wire::ChunkCoord& chunk,
                                       const core::ActorUuid& uuid, std::int16_t x,
                                       std::int16_t y, std::int16_t z, std::int16_t voxelType,
                                       Bytes voxelState, std::uint8_t distance = 8,
                                       wire::DecayRate decay = wire::DecayRate::None);
  Result<std::uint8_t> sendAudio(std::int64_t player, const SpatialSend& p);
  Result<std::uint8_t> sendVideo(std::int64_t player, const SpatialSend& p);
  Result<std::size_t> sendVideoFrame(std::int64_t player, const wire::ChunkCoord& chunk,
                                     const core::ActorUuid& uuid, Bytes frame,
                                     std::uint16_t frameId, std::uint8_t codec = 0,
                                     std::uint8_t distance = 1,
                                     wire::DecayRate decay = wire::DecayRate::None);
  Result<std::uint8_t> sendText(std::int64_t player, const SpatialSend& p);
  Result<std::uint8_t> sendClientEvent(std::int64_t player, const wire::ChunkCoord& chunk,
                                       const core::ActorUuid& uuid, std::uint16_t eventType,
                                       Bytes state, std::uint8_t distance = 8,
                                       wire::DecayRate decay = wire::DecayRate::None);
  Result<std::uint8_t> sendGenericSpatial(std::int64_t player, const SpatialSend& p);
  Result<std::uint8_t> sendSingleActorMessage(std::int64_t player,
                                              const wire::ChunkCoord& targetChunk,
                                              const core::ActorUuid& targetUuid, Bytes payload);
  Result<std::uint8_t> sendChannelMessage(std::int64_t player, std::int64_t channelId,
                                          const core::ActorUuid& uuid, Bytes payload);
  Result<std::uint8_t> sendChannelAudio(std::int64_t player, std::int64_t channelId,
                                        const core::ActorUuid& uuid, Bytes payload);
  Result<std::uint8_t> sendRangedChannelMessage(std::int64_t player, std::int64_t channelId,
                                                const core::ActorUuid& uuid, Bytes payload,
                                                const wire::ChunkCoord& origin,
                                                std::uint32_t maxDistance);
  /// Idle keep-alive for one of the player's actors. Send every 2 seconds or so while the
  /// actor is idle, so its presence never lapses.
  Result<std::uint8_t> sendHeartbeat(std::int64_t player, const wire::ChunkCoord& chunk,
                                     const core::ActorUuid& uuid);
  /// The speakers whose spatial voice and video the player receives (Connection::setSpeakers).
  /// Each carried player has their own lists. A changed kind is sent at once when connected,
  /// and every list again with each capabilities advertisement and after the player is
  /// carried again. InvalidArgument over wire::kMaxSpeakers names.
  Status setSpeakers(std::int64_t player, SpeakerList voice, SpeakerList video);

  // ----- The session's own sends ----------------------------------------------

  /// A keep-alive of the session itself: no actor, chunk (0,0,0), the fleet session's own
  /// gameTokenId. The capabilities advertisement already goes out every
  /// FleetConfig::transport.advertiseIntervalMs; this is for a server that wants one sooner.
  Result<std::uint8_t> sendHeartbeat();
  /// CLIENT_CAPABILITIES now. Sent automatically when connected and every
  /// advertiseIntervalMs; public for tests.
  Result<std::uint8_t> sendCapabilities() { return connection_->sendCapabilities(); }
  /// Connection::flushSends: put the pending outbound bundle on the wire now.
  Status flushSends() { return connection_->flushSends(); }

  // ----- Receive-side ----------------------------------------------------------

  /// Drain pending notifications and dispatch them to the handlers on the calling thread,
  /// then report the carries whose deadline passed. Returns the number of events dispatched.
  std::size_t poll(std::size_t maxEvents = SIZE_MAX);
  /// Manual-pump mode only: receive datagrams, resend waiting carries and run lifecycle
  /// housekeeping without blocking longer than timeoutMs. Returns the datagrams processed.
  std::size_t pump(int timeoutMs = 0) { return connection_->pump(timeoutMs); }

  /// Connection's counters for this session's socket. messagesSentByType[49] and [50] count
  /// carries and releases; messagesReceivedByType[51], [52] and [53] count their answers.
  Stats stats() const { return connection_->stats(); }
  /// How many times this session has started: open() and every reconnection count one.
  std::uint64_t sessionEpoch() const { return connection_->sessionEpoch(); }

 private:
  class Endpoint;

  struct Player {
    wire::Token64 token;  ///< the player's key, for the proof of a resend or a carry again
    std::int64_t userId = 0;
    bool carried = false;
    bool attempt = false;         ///< a FLEET_CARRY waits for its result
    std::int64_t nextSendMs = 0;  ///< monotonic; 0: send at the next pass
    std::int64_t deadlineMs = 0;  ///< monotonic; 0 until the attempt's first send
    std::uint64_t order = 0;      ///< which carry() call made or last renewed the entry
    CarryCallback done;
    std::optional<std::vector<core::ActorUuid>> voiceSpeakers;
    std::optional<std::vector<core::ActorUuid>> videoSpeakers;
  };

  void sendCarryLocked(std::int64_t id, Player& player, std::int64_t nowMono);
  /// Send every FLEET_CARRY that is due. playersMutex_ held.
  void driveCarriesLocked(std::int64_t nowMono);
  void sendSpeakersLocked(std::int64_t id, const Player& player);
  void recarryAllLocked();
  void onTick();
  void onAdvertised();
  void onFleetMessage(Bytes message);
  void onCarryResult(const wire::FleetCarryResultView& result);
  void onReleased(const wire::FleetReleasedView& released);
  void onDelivery(const wire::FleetDeliveryView& delivery);
  void expireCarries();
  void report(const FleetCarryResult& result, const CarryCallback& done);

  FleetConfig config_;
  const core::ICrypto& crypto_;
  const core::IClock& clock_;
  std::shared_ptr<Endpoint> endpoint_;

  mutable std::mutex handlersMutex_;
  FleetHandlers handlers_;
  /// The handler set of the poll() in progress (poll thread only).
  FleetHandlers active_;

  mutable std::mutex playersMutex_;
  std::unordered_map<std::int64_t, Player> players_;
  std::uint64_t carryOrder_ = 0;
  /// False when no row waits for a carry result, so a pass need not walk the table.
  bool attemptsPending_ = false;
  /// The session epoch whose carries the table reflects; a later epoch carries again.
  std::uint64_t carriedEpoch_ = 0;
  /// Epoch millis when the session was last given its token: where refreshDue() counts the
  /// token's lifetime from.
  std::atomic<std::int64_t> tokenSinceEpochMs_{0};

  /// Declared last: destroyed first, so its network thread has stopped before anything
  /// its hooks touch goes.
  std::unique_ptr<Connection> connection_;
};

}  // namespace crowdy::replication
