// Mirrors Management API e2e: operator control plane (READ-ONLY). Gated on
// CROWDY_E2E_OPERATOR_EMAIL (an is_operator account); skips (77) otherwise.
// Exercises the read-side control-plane queries only — environments list +
// detail, environment versions, change orders, audit, operator users. NO
// mutations whatsoever. See https://docs.crowdedkingdoms.com for the operator
// surface.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

int runAll() {
  auto cfg = e2e::requireConfig();
  if (cfg.operatorEmail.empty()) {
    std::puts("CROWDY_E2E_OPERATOR_EMAIL not configured; skipping");
    return 77;
  }
  auto op = e2e::identityClient(cfg, cfg.operatorEmail);

  E2E_SUBTEST("environments list + detail");
  graphql::Json envs = op->operator_().environments(1, 20);
  E2E_CHECK(envs["rows"].isArray());
  E2E_CHECK(envs["total"].asInt64() >= 0);
  E2E_CHECK(envs["page"].asInt64() == 1);
  std::string firstSlug;
  envs["rows"].forEach([&](graphql::Json row) {
    E2E_CHECK(!row["id"].asString().empty());
    E2E_CHECK(!row["slug"].asString().empty());
    if (firstSlug.empty()) firstSlug = row["slug"].asString();
  });
  if (!firstSlug.empty()) {
    graphql::Json env = op->operator_().environment(firstSlug);
    E2E_CHECK(env["slug"].asString() == firstSlug);
    E2E_CHECK(!env["status"].asString().empty());
  } else {
    std::puts("   note: no environments provisioned on this deployment");
  }

  E2E_SUBTEST("environmentVersions");
  graphql::Json versions = op->operator_().environmentVersions();
  E2E_CHECK(versions["rows"].isArray());
  versions["rows"].forEach([&](graphql::Json row) {
    E2E_CHECK(!row["version"].asString().empty());
  });

  E2E_SUBTEST("changeOrders list");
  graphql::Json orders = op->operator_().changeOrders();
  E2E_CHECK(orders["rows"].isArray());
  E2E_CHECK(orders["total"].asInt64() >= 0);

  E2E_SUBTEST("audit");
  graphql::JVal auditVars;
  auditVars["limit"] = std::int64_t{20};
  graphql::Json audit = op->operator_().audit(auditVars);
  E2E_CHECK(audit.isArray());
  audit.forEach([&](graphql::Json row) {
    E2E_CHECK(!row["action"].asString().empty());
  });

  E2E_SUBTEST("operatorUsers");
  graphql::Json operators = op->operator_().operatorUsers();
  E2E_CHECK(operators.size() >= 1);
  bool sawSelf = false;
  operators.forEach([&](graphql::Json u) {
    E2E_CHECK(u["isOperator"].asBool());
    if (u["email"].asString() == cfg.operatorEmail) sawSelf = true;
  });
  E2E_CHECK(sawSelf);

  std::puts("e2e_operator OK");
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
