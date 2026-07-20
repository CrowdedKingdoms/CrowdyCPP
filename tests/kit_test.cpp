#include <cstring>

#include "crowdy/kit/inventory.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

namespace {

void testPolicyJson() {
  CHECK_EQ(kitPolicyJson(ownerOfSelfPolicy()), R"({"type":"owner_of_self"})");
  CHECK_EQ(kitPolicyJson(conditionPolicy("$amount > 0")),
           R"({"expression":"$amount > 0","type":"condition"})");
  CHECK_EQ(kitPolicyJson(featureGate("land_owner")),
           R"({"feature":"land_owner","type":"tier_feature"})");

  // andPolicies composition skips nulls and collapses single rules.
  JVal solo = andPolicies(ownerOfSelfPolicy(), {JVal(), JVal()});
  CHECK_EQ(kitPolicyJson(solo), R"({"type":"owner_of_self"})");
  JVal combined = andPolicies(ownerOfSelfPolicy(), {featureGate("vip")});
  graphql::Json parsed = graphql::Json::parse(kitPolicyJson(combined));
  CHECK_EQ(parsed["type"].asString(), "and");
  CHECK_EQ(parsed["rules"].size(), 2u);
}

void testToSnakeCase() {
  CHECK_EQ(toSnakeCase("Bank"), "bank");
  CHECK_EQ(toSnakeCase("GuildBank"), "guild_bank");
  CHECK_EQ(toSnakeCase("NPCSpawner"), "npcspawner");  // matches the TS regex behavior
  CHECK_EQ(toSnakeCase("wave2Boss"), "wave2_boss");
  CHECK_EQ(toSnakeCase("with space-dash"), "with_space_dash");
}

void testTrustedAuthority() {
  JVal fn;
  applyTrustedAuthority(fn, TrustedAuthority::server());
  graphql::Json parsed = graphql::Json::parse(fn.dump());
  CHECK_EQ(parsed["invokeScope"].asString(), "server");
  CHECK_EQ(graphql::Json::parse(parsed["invokePolicyJson"].asStringView())["type"].asString(),
           "allow");

  JVal fn2;
  applyTrustedAuthority(fn2, TrustedAuthority::automation(), "self.hp > 0");
  graphql::Json parsed2 = graphql::Json::parse(fn2.dump());
  CHECK(parsed2["autonomousInvocable"].asBool());
  graphql::Json policy2 = graphql::Json::parse(parsed2["invokePolicyJson"].asStringView());
  CHECK_EQ(policy2["type"].asString(), "and");
  CHECK_EQ(policy2["rules"].at(0)["type"].asString(), "is_automation");
  CHECK_EQ(policy2["rules"].at(1)["expression"].asString(), "self.hp > 0");
}

void testInventoryBlueprintShape() {
  KitBlueprint bp = inventoryBlueprint(
      {.typePrefix = "Bank", .maxSlots = 10, .slotCount = 8, .recipes = {}, .barters = {}});
  CHECK_EQ(bp.name, "BankInventory");
  CHECK_EQ(bp.containerTypes.size(), 2u);
  CHECK_EQ(bp.propertyDefinitions.size(), 5u);
  CHECK_EQ(bp.functions.size(), 4u);

  const auto names = inventoryNames("Bank");
  CHECK_EQ(names.stackType, "BankItemStack");
  CHECK_EQ(names.grantFn, "bank_grant_stack");
  CHECK_EQ(names.transferFn, "bank_transfer_stack");

  // The consume function's guard must match the TS blueprint exactly.
  graphql::Json consume = graphql::Json::parse(bp.functions[1].dump());
  CHECK_EQ(consume["name"].asString(), "bank_consume_stack");
  graphql::Json consumePolicy =
      graphql::Json::parse(consume["invokePolicyJson"].asStringView());
  CHECK_EQ(consumePolicy["rules"].at(1)["expression"].asString(),
           "$amount > 0 && self.quantity >= $amount");

  // move clamps to slotCount - 1.
  graphql::Json move = graphql::Json::parse(bp.functions[2].dump());
  CHECK_EQ(move["mutations"].at(0)["expression"].asString(), "clamp($to_slot, 0, 7)");

  // max_slots default carries the option.
  graphql::Json maxSlots = graphql::Json::parse(bp.propertyDefinitions[0].dump());
  CHECK_EQ(maxSlots["defaultValueJson"].asString(), "10");
}

void testAtomicInventoryTransactions() {
  InventoryRecipeSpec recipe;
  recipe.recipeId = "wood-planks";
  recipe.inputs = {{"wood", 2}};
  recipe.output = {"plank", 4};
  InventoryBarterSpec barter;
  barter.barterId = "wheat-for-emerald";
  barter.pay = {"wheat", 5};
  barter.receive = {"emerald", 1};
  KitBlueprint bp = inventoryBlueprint(
      {.typePrefix = "", .maxSlots = 24, .slotCount = 64,
       .recipes = {recipe}, .barters = {barter}});
  CHECK_EQ(bp.functions.size(), 6u);
  graphql::Json craft = graphql::Json::parse(bp.functions[4].dump());
  CHECK_EQ(craft["name"].asString(), "craft_wood_planks");
  CHECK(craft["autonomousInvocable"].asBool());
  CHECK_EQ(craft["mutations"].size(), 2u);
  graphql::Json craftPolicy =
      graphql::Json::parse(craft["invokePolicyJson"].asStringView());
  CHECK(std::strstr(craftPolicy["rules"].at(1)["expression"].asString().c_str(),
                    "item_id == \"wood\"") != nullptr);
  graphql::Json trade = graphql::Json::parse(bp.functions[5].dump());
  CHECK_EQ(trade["name"].asString(), "barter_wheat_for_emerald");
  CHECK(trade["autonomousInvocable"].asBool());
}

void testMergeBlueprints() {
  KitBlueprint a = inventoryBlueprint();
  KitBlueprint b = inventoryBlueprint(
      {.typePrefix = "Bank", .maxSlots = 24, .slotCount = 64, .recipes = {}, .barters = {}});
  MergedBlueprints merged = mergeBlueprints("42", {a, b}, "sess-1");

  graphql::Json seed = graphql::Json::parse(merged.seedInput.dump());
  CHECK_EQ(seed["appId"].asString(), "42");
  CHECK_EQ(seed["sessionId"].asString(), "sess-1");
  CHECK_EQ(seed["containerTypes"].size(), 4u);
  CHECK_EQ(seed["functions"].size(), 8u);

  // Duplicate names across blueprints must throw.
  bool threw = false;
  try {
    mergeBlueprints("42", {a, inventoryBlueprint()});
  } catch (const std::invalid_argument& e) {
    threw = true;
    CHECK(std::strstr(e.what(), "container type") != nullptr);
  }
  CHECK(threw);
}

void testComposeBlueprints() {
  KitBlueprint composite =
      composeBlueprints("bundle",
                        {inventoryBlueprint(),
                         inventoryBlueprint({.typePrefix = "X",
                                             .maxSlots = 24,
                                             .slotCount = 64,
                                             .recipes = {},
                                             .barters = {}})});
  CHECK_EQ(composite.name, "bundle");
  CHECK_EQ(composite.containerTypes.size(), 4u);
  CHECK_EQ(composite.functions.size(), 8u);
}

void testOwnerHelpers() {
  CHECK_EQ(ownerEqualsCaller("self.owner_user_id"), "self.owner_user_id == $caller_user_id");
  CHECK_EQ(ownerEqualsCaller("self.owner_user_id", OwnerIdKind::String),
           "self.owner_user_id == to_string($caller_user_id)");
  graphql::Json mirror = graphql::Json::parse(ownerMirrorProperty("Plot").dump());
  CHECK_EQ(mirror["key"].asString(), "owner_user_id");
  CHECK_EQ(mirror["valueType"].asString(), "int");
  CHECK_EQ(mirror["defaultValueJson"].asString(), "0");
}

}  // namespace

int main() {
  testPolicyJson();
  testToSnakeCase();
  testTrustedAuthority();
  testInventoryBlueprintShape();
  testAtomicInventoryTransactions();
  testMergeBlueprints();
  testComposeBlueprints();
  testOwnerHelpers();
  std::puts("kit_test OK");
  return 0;
}
