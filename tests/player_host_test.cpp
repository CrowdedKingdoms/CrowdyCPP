#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/player_host/adapter.hpp"
#include "crowdy/player_host/schemas.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::player_host;

namespace {

constexpr std::int64_t kBaseTime = 1'753'300'800'000LL;

std::string iso(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec,
                static_cast<long long>(epoch_ms % 1'000));
  return buffer;
}

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = kBaseTime;
  std::int64_t monotonic = 10'000;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
  void advance(std::int64_t milliseconds) {
    epoch += milliseconds;
    monotonic += milliseconds;
  }
};

std::optional<LeaseScopeV1> scopeFor(CommandKindV1 kind) {
  for (const auto& schema : kGameCommandSchemasV1) {
    if (schema.kind == kind) return schema.default_scope;
  }
  return std::nullopt;
}

PlayerHostCapabilitiesV1 makeCapabilities(std::int64_t now,
                                          std::uint32_t move_rate = 100) {
  PlayerHostCapabilitiesV1 value;
  value.game_id = "native-test-game";
  value.revision = "capability-7";
  value.controlled_entity_id = "player-7";
  for (const auto kind : kCommandKindsV1) {
    CommandCapabilityV1 command;
    command.kind = kind;
    command.tool_name = std::string(toolName(kind));
    command.required_scope = scopeFor(kind);
    command.risk = gen::CrowdyStudioAgentToolRisk::WORLD_CONTROL;
    command.approval = kind == CommandKindV1::CombatAttack
                           ? ApprovalPolicyV1::Conditional
                           : ApprovalPolicyV1::None;
    command.rate_limit_per_second =
        kind == CommandKindV1::Move ? move_rate : 100;
    value.commands.push_back(std::move(command));
  }
  value.observation = {.max_age_ms = 2'000,
                       .max_nearby_actors = 16,
                       .max_nearby_voxels = 32};
  value.advertised_at = iso(now);
  return value;
}

GameObservationV1 makeObservation(
    std::int64_t now, ActorKindV1 target_kind = ActorKindV1::Mob,
    bool alive = true, std::string id = "observation-1") {
  GameObservationV1 value;
  value.observation_id = std::move(id);
  value.capability_revision = "capability-7";
  value.controlled_entity_id = "player-7";
  value.observed_at = iso(now);
  value.expires_at = iso(now + 2'000);
  value.player.position = {
      "922337203685477580812345.123456789", "64", "-12.5"};
  value.player.velocity = {"0", "0", "0"};
  value.player.look = {"0", "0"};
  value.player.health = alive ? "20" : "0";
  value.player.alive = alive;
  value.controlled_entity.kind = ControlledEntityKindV1::Player;
  value.controlled_entity.position = value.player.position;
  value.controlled_entity.velocity = {"0", "0", "0"};
  value.target = ObservationTargetV1{
      .target_id = "target-1",
      .kind = ObservationTargetKindV1::Actor,
      .distance = "2.25",
  };
  value.nearby_actors.push_back(ObservationActorV1{
      .actor_id = "target-1",
      .kind = target_kind,
      .position = {"1", "64", "1"},
      .distance = "2.25",
      .disposition = ActorDispositionV1::Hostile,
      .label = std::nullopt,
      .health = std::nullopt,
  });
  return value;
}

AgentControlLeaseV1 makeLease(std::int64_t now,
                              std::vector<LeaseScopeV1> scopes = {
                              LeaseScopeV1::Observe,
                              LeaseScopeV1::Locomotion,
                              LeaseScopeV1::Interact,
                              LeaseScopeV1::Craft,
                              LeaseScopeV1::Combat,
                              LeaseScopeV1::Communicate,
                              LeaseScopeV1::Travel,
                              LeaseScopeV1::Grid,
                              LeaseScopeV1::TrustConsent,
                              LeaseScopeV1::Commerce,
                          }) {
  AgentControlLeaseV1 value;
  value.lease_id = "lease-1";
  value.client_epoch = "1";
  value.scopes = std::move(scopes);
  value.holder = "Current player";
  value.controlled_entity_id = "player-7";
  value.host_capability_revision = "capability-7";
  value.context_version = "context-1";
  value.granted_at = iso(now);
  value.expires_at = iso(now + 60'000);
  return value;
}

PlannedCommandV1 planned(std::string observation_id = "observation-1") {
  return PlannedCommandV1{.observation_id = std::move(observation_id),
                          .capability_revision = "capability-7",
                          .controlled_entity_id = "player-7"};
}

std::vector<GameCommandV1> allCommands() {
  return {
      MoveCommandV1{.planned = planned(),
                    .direction = MoveDirectionV1::Forward,
                    .intensity = 1,
                    .duration_ms = 100},
      LookCommandV1{.planned = planned(),
                    .delta_yaw = 10,
                    .delta_pitch = -5},
      InventorySelectCommandV1{.planned = planned(), .slot = 1},
      InventoryConsumeCommandV1{
          .planned = planned(), .slot = 1, .quantity = 1},
      InventoryTransferCommandV1{
          .planned = planned(),
          .direction = InventoryTransferDirectionV1::ToContainer,
          .slot = 1,
          .quantity = 1,
          .container_ref = "target-1"},
      InteractCommandV1{.planned = planned(),
                        .action = InteractActionV1::Use,
                        .target_ref = "target-1",
                        .inventory_slot = std::nullopt},
      CraftCommandV1{
          .planned = planned(), .recipe_id = "plank", .quantity = 1},
      MountCommandV1{.planned = planned(),
                     .action = MountActionV1::Mount,
                     .mount_ref = "target-1"},
      CombatAttackCommandV1{.planned = planned(),
                            .target_ref = "target-1",
                            .attack = CombatAttackV1::Primary},
      ChatSendCommandV1{.planned = planned(),
                        .channel = ChatChannelV1::Local,
                        .text = "hello"},
      TravelTeleportCommandV1{.planned = planned(),
                              .destination_ref = "spawn"},
      StopCommandV1{},
  };
}

class FakePlayerHost final : public PlayerHostAdapterV1 {
 public:
  PlayerHostCapabilitiesV1 capability_value = makeCapabilities(kBaseTime);
  GameObservationV1 observation_value = makeObservation(kBaseTime);
  std::vector<std::string> events;
  std::vector<CommandKindV1> dispatched;
  std::vector<ValidatedGateV1> gates;
  bool hold_dispatch = false;
  bool ambiguous_dispatch = false;
  bool definitive_failure = false;
  std::optional<CommandCallbackV1> pending;

  void capabilities(CancellationTokenV1,
                    CapabilitiesCallbackV1 callback) override {
    callback(AdapterResultV1<PlayerHostCapabilitiesV1>::success(
        capability_value));
  }

  void observe(const ObserveRequestV1&, CancellationTokenV1,
               ObservationCallbackV1 callback) override {
    callback(
        AdapterResultV1<GameObservationV1>::success(observation_value));
  }

  void dispatch(const GameCommandV1& command, const ValidatedGateV1& gate,
                CancellationTokenV1, CommandCallbackV1 callback) override {
    dispatched.push_back(commandKind(command));
    gates.push_back(gate);
    events.push_back("dispatch:" + std::string(toString(commandKind(command))));
    if (hold_dispatch) {
      pending = std::move(callback);
      return;
    }
    if (ambiguous_dispatch) {
      callback(AdapterResultV1<GameCommandResultV1>::failure(
          {.code = "AGENT_TOOL_OUTCOME_UNKNOWN",
           .message = "effect may have occurred",
           .retryable = false,
           .remediation = std::nullopt,
           .field = std::nullopt,
           .required_scope = std::nullopt},
          true));
      return;
    }
    if (definitive_failure) {
      callback(AdapterResultV1<GameCommandResultV1>::failure(
          {.code = "AGENT_TOOL_FAILED",
           .message = "intent service rejected command",
           .retryable = false,
           .remediation = std::nullopt,
           .field = std::nullopt,
           .required_scope = std::nullopt}));
      return;
    }
    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = commandKind(command);
    if (const auto* value = plannedCommand(command)) {
      result.observation_id = value->observation_id;
    }
    callback(
        AdapterResultV1<GameCommandResultV1>::success(std::move(result)));
  }

  void clearAgentIntent(PreemptionReasonV1 reason) noexcept override {
    events.push_back("clear:" + std::string(preemptionReasonName(reason)));
  }

  void completePendingSuccess() {
    CHECK(pending.has_value());
    auto callback = std::move(*pending);
    pending.reset();
    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = dispatched.back();
    result.observation_id = "observation-1";
    callback(
        AdapterResultV1<GameCommandResultV1>::success(std::move(result)));
  }
};

void testSchemasAndEveryCommandMapping() {
  CHECK_EQ(kClosedPreemptionReasonsV1.size(), 16U);
  CHECK_EQ(kLeaseScopesV1.size(), 10U);
  CHECK_EQ(kMandatoryGameToolSurfacesV1.size(), 14U);
  CHECK_EQ(kGameCommandSchemasV1.size(), 12U);
  CHECK(validateDecimalStringV1(
      "922337203685477580812345.123456789", "world.x"));
  CHECK(!validateDecimalStringV1("01", "world.x"));

  auto caps = makeCapabilities(kBaseTime);
  CHECK(validatePlayerHostCapabilitiesV1(caps));
  for (const auto kind : kCommandKindsV1) {
    CHECK(commandKindForToolName(toolName(kind)).has_value());
    CHECK_EQ(*commandKindForToolName(toolName(kind)), kind);
  }
  caps.commands[0].tool_name = "game.control.look";
  CHECK(!validatePlayerHostCapabilitiesV1(caps));

  // Every command kind has a mandatory tool surface and validates as built.
  std::size_t index = 0;
  for (const auto& command : allCommands()) {
    const auto kind = commandKind(command);
    const auto surface = std::find_if(
        kMandatoryGameToolSurfacesV1.begin(),
        kMandatoryGameToolSurfacesV1.end(),
        [&](const GameToolSurfaceV1& value) {
          return value.command_kind && *value.command_kind == kind;
        });
    CHECK(surface != kMandatoryGameToolSurfacesV1.end());
    CHECK_EQ(std::string(toolName(kind)), std::string(surface->name));
    CHECK(validateGameCommandV1(command));
    ++index;
  }
  CHECK_EQ(index, 12U);
}

void testPreemptionVocabularyIsClosedAndRoundTrips() {
  // The wire strings used to come from the API's enum; the orchestrator that
  // owned it is gone and the contract carries them itself. CrowdyJS's
  // player-host/agent-types has the same 16 in the same order.
  const char* const expected[] = {
      "HUMAN_INPUT", "HUMAN_EDIT", "HUMAN_STOP", "ESCAPE", "DEATH",
      "CONTEXT_CHANGED", "PERMISSION_CHANGED", "ADMISSION_CHANGED",
      "CONTROL_TARGET_CHANGED", "DISCONNECTED", "CLIENT_REATTACHED",
      "QUOTA_FAILURE", "BUDGET_FAILURE", "OPERATOR_KILL", "LEASE_EXPIRED",
      "SESSION_CLOSED",
  };
  std::size_t index = 0;
  for (const auto reason : kClosedPreemptionReasonsV1) {
    CHECK_EQ(std::string(preemptionReasonName(reason)),
             std::string(expected[index]));
    CHECK(preemptionReasonFromName(expected[index]).has_value());
    CHECK(*preemptionReasonFromName(expected[index]) == reason);
    ++index;
  }
  CHECK(!preemptionReasonFromName("NOT_A_REASON").has_value());

  FakePlayerHost host;
  host.clearAgentIntent(PreemptionReasonV1::HUMAN_INPUT);
  CHECK_EQ(host.events.size(), 1U);
  CHECK_EQ(host.events.front(), "clear:HUMAN_INPUT");
}

void testObservationAndGateValidation() {
  const auto observation = makeObservation(kBaseTime);
  CHECK(validateGameObservationV1(observation));

  auto stale = observation;
  stale.expires_at = "not-a-timestamp";
  CHECK(!validateGameObservationV1(stale));

  auto lease = makeLease(kBaseTime);
  CHECK(validateAgentControlLeaseV1(lease));
  lease.lease_id.clear();
  CHECK(!validateAgentControlLeaseV1(lease));

  ValidatedGateV1 gate;
  gate.contract_version = std::string(kValidatedGateContractV1);
  gate.client_epoch = "1";
  gate.lease_id = "lease-1";
  gate.context_version = "context-1";
  gate.observation_id = "observation-1";
  gate.scopes = makeLease(kBaseTime).scopes;
  gate.validated_at = iso(kBaseTime);
  CHECK(validateValidatedGateV1(gate));
  gate.client_epoch.clear();
  CHECK(!validateValidatedGateV1(gate));

  // The observation adapter is what a game still implements for the
  // in-browser agent's game_observe; it answers with a valid observation.
  FakePlayerHost host;
  std::optional<AdapterResultV1<GameObservationV1>> observed;
  host.observe({.detail = ObserveDetailV1::Standard,
                .max_nearby_actors = 4,
                .max_nearby_voxels = 4},
               CancellationTokenV1{},
               [&](auto result) { observed = std::move(result); });
  CHECK(observed && observed->ok());
  CHECK(validateGameObservationV1(*observed->value));
}

}  // namespace

int main() {
  testSchemasAndEveryCommandMapping();
  testPreemptionVocabularyIsClosedAndRoundTrips();
  testObservationAndGateValidation();
  std::printf("player_host_test passed\n");
  return 0;
}
