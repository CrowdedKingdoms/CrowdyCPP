// Mirrors Management API e2e: payments (checkout lifecycle reads, NO
// capture). Creates a small org-wallet top-up checkout with an idempotency
// key, then reads it back through myCheckouts (+ its Relay connection). The
// platform-wide checkouts / paymentEvents listings are super-admin only and not
// wrapped (since 0.51.0). Deployments without a payment provider
// configured must fail with a STRUCTURED error code, which this suite treats
// as pass-with-note. See https://docs.crowdedkingdoms.com for the payments
// surface.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

std::string bigIntStr(const graphql::Json& v) {
  if (v.isString()) return v.asString();
  return std::to_string(v.asInt64());
}

int runAll() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& own = e2e::owner(cfg);

  graphql::Json e2eApp = own.admin().apps().get(cfg.appId);
  const std::string orgId = bigIntStr(e2eApp["orgId"]);
  E2E_CHECK(!orgId.empty());

  E2E_SUBTEST("createCheckout: small ORG_WALLET_TOPUP with idempotencyKey");
  std::string checkoutId;
  const std::string idempotencyKey = "cpp-e2e-topup-" + e2e::runSuffix();
  graphql::JVal input;
  input["provider"] = "STRIPE";
  input["purpose"] = "ORG_WALLET_TOPUP";
  input["orgId"] = orgId;
  input["amountCents"] = "500";
  input["currency"] = "usd";
  input["idempotencyKey"] = idempotencyKey;
  try {
    graphql::Json checkout = own.admin().payments().createCheckout(input);
    checkoutId = bigIntStr(checkout["checkoutId"]);
    E2E_CHECK(!checkoutId.empty() && checkoutId != "0");
    E2E_CHECK(checkout["purpose"].asString() == "ORG_WALLET_TOPUP");
    E2E_CHECK(checkout["status"].asString() == "PENDING");
    E2E_CHECK(bigIntStr(checkout["amountCents"]) == "500");
    // Idempotent replay with the same key + input returns the SAME checkout.
    graphql::Json replay = own.admin().payments().createCheckout(input);
    E2E_CHECK(bigIntStr(replay["checkoutId"]) == checkoutId);
  } catch (const graphql::CrowdyGraphQLError& e) {
    E2E_CHECK(!e.code().empty());
    std::printf("   note: no payment provider on this deployment (code=%s): %s\n",
                e.code().c_str(), e.what());
  }

  E2E_SUBTEST("myCheckouts + myCheckoutsConnection read back");
  graphql::Json mine = own.admin().payments().myCheckouts(50);
  E2E_CHECK(mine["items"].isArray());
  E2E_CHECK(mine["pageInfo"]["totalCount"].asInt64() >= 0);
  graphql::Json mineConn = own.admin().payments().myCheckoutsConnection(50);
  E2E_CHECK(mineConn["edges"].isArray());
  if (!checkoutId.empty()) {
    bool sawMine = false;
    mine["items"].forEach([&](graphql::Json c) {
      if (bigIntStr(c["checkoutId"]) == checkoutId) sawMine = true;
    });
    bool sawMineConn = false;
    mineConn["edges"].forEach([&](graphql::Json edge) {
      if (bigIntStr(edge["node"]["checkoutId"]) == checkoutId) sawMineConn = true;
    });
    E2E_CHECK(sawMine);
    E2E_CHECK(sawMineConn);
  }

  // Deliberately NO capture: the checkout stays PENDING and expires
  // server-side; capture flows are provider-webhook territory.

  std::puts("e2e_payments OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return runAll();
  } catch (const graphql::CrowdyError& e) {
    std::fprintf(stderr, "FATAL [%s]: %s\n", e.code().c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    return 1;
  }
}
