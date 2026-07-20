#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/generated/operations.hpp"

/// Studio-admin surface — client.admin().*. Privileged organization/app
/// administration: drive from a trusted context (studio backend / tooling)
/// with an org-scoped or admin token; the server enforces the relevant org or
/// app permission on every call. Targets the Management API.
namespace crowdy::domains {

namespace detail {
/// Shared helpers for the admin wrappers.
class AdminDomain : public DomainBase {
 public:
  using DomainBase::DomainBase;

 protected:
  graphql::JVal one(std::string_view k, const graphql::JVal& v) const {
    graphql::JVal vars;
    vars[k] = v;
    return vars;
  }
  graphql::JVal two(std::string_view k1, const graphql::JVal& v1, std::string_view k2,
                    const graphql::JVal& v2) const {
    graphql::JVal vars;
    vars[k1] = v1;
    vars[k2] = v2;
    return vars;
  }
};
}  // namespace detail

/// client.admin().organizations() — orgs, members, RBAC roles, org tokens.
class OrganizationsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json mine() const { return execUnwrap(gen::organizations::kMyOrganizationsDocument); }
  graphql::Json get(std::string_view id) const {
    return execUnwrap(gen::organizations::kOrganizationDocument, one("id", graphql::JVal(id)));
  }
  graphql::Json getBySlug(std::string_view slug) const {
    return execUnwrap(gen::organizations::kOrganizationBySlugDocument,
                      one("slug", graphql::JVal(slug)));
  }
  graphql::Json create(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrganizationDocument, one("input", input));
  }
  graphql::Json setStatus(std::string_view orgId, std::string_view status) const {
    return execUnwrap(gen::organizations::kSetOrgStatusDocument,
                      two("orgId", graphql::JVal(orgId), "status", graphql::JVal(status)));
  }
  graphql::Json members(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgMembersDocument, one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json inviteMember(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kInviteOrgMemberDocument, one("input", input));
  }
  graphql::Json removeMember(std::string_view orgId, std::string_view userId) const {
    return execUnwrap(gen::organizations::kRemoveOrgMemberDocument,
                      two("orgId", graphql::JVal(orgId), "userId", graphql::JVal(userId)));
  }
  graphql::Json memberRoles(std::string_view orgMemberId) const {
    return execUnwrap(gen::organizations::kMemberRolesDocument,
                      one("orgMemberId", graphql::JVal(orgMemberId)));
  }
  graphql::Json updateMemberRoles(const graphql::JVal& vars) const {
    return execUnwrap(gen::organizations::kUpdateOrgMemberRolesDocument, vars);
  }
  graphql::Json permissions() const {
    return execUnwrap(gen::organizations::kOrgPermissionsDocument);
  }
  graphql::Json roles(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgRolesDocument, one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json createRole(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrgRoleDocument, one("input", input));
  }
  graphql::Json updateRole(std::string_view orgRoleId, const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kUpdateOrgRoleDocument,
                      two("orgRoleId", graphql::JVal(orgRoleId), "input", input));
  }
  graphql::Json deleteRole(std::string_view orgRoleId) const {
    return execUnwrap(gen::organizations::kDeleteOrgRoleDocument,
                      one("orgRoleId", graphql::JVal(orgRoleId)));
  }
  graphql::Json tokens(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgTokensDocument, one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json createToken(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrgTokenDocument, one("input", input));
  }
  graphql::Json updateToken(std::string_view orgTokenId, const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kUpdateOrgTokenDocument,
                      two("orgTokenId", graphql::JVal(orgTokenId), "input", input));
  }
  graphql::Json revokeToken(std::string_view orgTokenId) const {
    return execUnwrap(gen::organizations::kRevokeOrgTokenDocument,
                      one("orgTokenId", graphql::JVal(orgTokenId)));
  }
};

/// client.admin().apps() — app registry + routing.
class AppsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json mine() const { return execUnwrap(gen::apps::kMyAppsDocument); }
  graphql::Json get(std::string_view appId) const {
    return execUnwrap(gen::apps::kAppDocument, one("appId", graphql::JVal(appId)));
  }
  graphql::Json getBySlug(std::string_view orgSlug, std::string_view appSlug) const {
    return execUnwrap(gen::apps::kAppBySlugDocument,
                      two("orgSlug", graphql::JVal(orgSlug), "appSlug", graphql::JVal(appSlug)));
  }
  graphql::Json forOrg(std::string_view orgSlug) const {
    return execUnwrap(gen::apps::kAppsForOrgDocument, one("orgSlug", graphql::JVal(orgSlug)));
  }
  graphql::Json marketplace() const {
    return execUnwrap(gen::apps::kMarketplaceAppsDocument, graphql::JVal(), "MarketplaceApps");
  }
  /// Relay cursor pagination variant of marketplace().
  graphql::Json marketplaceConnection(const graphql::JVal& vars = graphql::JVal()) const {
    return execUnwrap(gen::apps::kMarketplaceAppsDocument, vars, "AppsConnection");
  }
  /// Routing tuple for an app: which game-api endpoint should serve it.
  /// Returns { appId, splitMode, deploymentTarget, gameApiUrl } fields from
  /// the app row; route gameplay to gameApiUrl when non-null.
  graphql::Json routeFor(std::string_view appId) const { return get(appId); }
  graphql::Json create(const graphql::JVal& input) const {
    return execUnwrap(gen::apps::kCreateAppDocument, one("input", input));
  }
  graphql::Json update(std::string_view appId, const graphql::JVal& input) const {
    return execUnwrap(gen::apps::kUpdateAppDocument,
                      two("appId", graphql::JVal(appId), "input", input));
  }
  graphql::Json archive(std::string_view appId) const {
    return execUnwrap(gen::apps::kArchiveAppDocument, one("appId", graphql::JVal(appId)));
  }
  /// visibility: an AppVisibility enum value string.
  graphql::Json setVisibility(std::string_view appId, std::string_view visibility) const {
    return execUnwrap(gen::apps::kSetAppVisibilityDocument,
                      two("appId", graphql::JVal(appId), "visibility", graphql::JVal(visibility)));
  }
};

/// client.admin().appAccess() — access tiers + per-user grants.
class AppAccessAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json tiers(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kAppAccessTiersDocument, one("appId", graphql::JVal(appId)));
  }
  graphql::Json myAccess(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kMyAppAccessDocument, one("appId", graphql::JVal(appId)));
  }
  graphql::Json userAccessByApp(const graphql::JVal& vars) const {
    return execUnwrap(gen::appAccess::kAppUserAccessByAppDocument, vars, "AppUserAccessByApp");
  }
  /// Relay cursor pagination variant of userAccessByApp().
  graphql::Json userAccessConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::appAccess::kAppUserAccessByAppDocument, vars,
                      "AppUserAccessConnection");
  }
  graphql::Json grantMemberCandidates(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kAppGrantMemberCandidatesDocument,
                      one("appId", graphql::JVal(appId)));
  }
  graphql::Json runtimePermissions() const {
    return execUnwrap(gen::appAccess::kRuntimePermissionsDocument);
  }
  graphql::Json createTier(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kCreateAccessTierDocument, one("input", input));
  }
  graphql::Json updateTier(std::string_view tierId, const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kUpdateAccessTierDocument,
                      two("tierId", graphql::JVal(tierId), "input", input));
  }
  graphql::Json archiveTier(std::string_view tierId) const {
    return execUnwrap(gen::appAccess::kArchiveAccessTierDocument,
                      one("tierId", graphql::JVal(tierId)));
  }
  graphql::Json grant(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kGrantAppAccessDocument, one("input", input));
  }
  graphql::Json grantMine(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kGrantMyAppAccessDocument,
                      one("appId", graphql::JVal(appId)));
  }
  graphql::Json claimFree(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kClaimFreeAppAccessDocument,
                      one("appId", graphql::JVal(appId)));
  }
  graphql::Json revoke(std::string_view appId, std::string_view userId) const {
    return execUnwrap(gen::appAccess::kRevokeAppAccessDocument,
                      two("appId", graphql::JVal(appId), "userId", graphql::JVal(userId)));
  }
};

/// client.admin().billing() — org wallet + per-app budgets.
class BillingAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json walletBalance(std::string_view orgId) const {
    return execUnwrap(gen::billing::kWalletBalanceDocument, one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json walletTransactions(std::string_view orgId, int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::billing::kWalletTransactionsDocument, vars, "WalletTransactions");
  }
  /// Relay cursor pagination variant of walletTransactions().
  graphql::Json walletTransactionsConnection(std::string_view orgId, int first = 50,
                                             std::string_view after = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    return execUnwrap(gen::billing::kWalletTransactionsDocument, vars,
                      "WalletTransactionsConnection");
  }
  graphql::Json appBudget(std::string_view orgId, std::string_view appId) const {
    return execUnwrap(gen::billing::kAppBudgetDocument,
                      two("orgId", graphql::JVal(orgId), "appId", graphql::JVal(appId)));
  }
  graphql::Json appBudgets(std::string_view orgId) const {
    return execUnwrap(gen::billing::kAppBudgetsDocument, one("orgId", graphql::JVal(orgId)));
  }
  /// monthlyLimitCents is a BigInt string of minor currency units.
  graphql::Json setAppBudget(std::string_view orgId, std::string_view appId,
                             std::string_view monthlyLimitCents) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    vars["monthlyLimitCents"] = monthlyLimitCents;
    return execUnwrap(gen::billing::kSetAppBudgetDocument, vars);
  }
  graphql::Json buddyBillingTiers() const {
    return execUnwrap(gen::billing::kBillingTiersDocument, graphql::JVal(), "BuddyBillingTiers");
  }
  graphql::Json graphqlBillingTiers() const {
    return execUnwrap(gen::billing::kBillingTiersDocument, graphql::JVal(), "GraphqlBillingTiers");
  }
  graphql::Json postgresBillingTiers() const {
    return execUnwrap(gen::billing::kBillingTiersDocument, graphql::JVal(),
                      "PostgresBillingTiers");
  }
};

/// client.admin().payments() — checkouts (wallet top-ups, plan purchases).
class PaymentsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  /// Economy-sensitive: pass an idempotencyKey inside the input.
  graphql::Json createCheckout(const graphql::JVal& input) const {
    return execUnwrap(gen::payments::kCreateCheckoutDocument, one("input", input));
  }
  graphql::Json capturePaypalCheckout(std::string_view orderId,
                                      std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["orderId"] = orderId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::payments::kCapturePaypalCheckoutDocument, vars);
  }
  graphql::Json checkouts(const graphql::JVal& vars = graphql::JVal()) const {
    return execUnwrap(gen::payments::kCheckoutsDocument, vars, "Checkouts");
  }
  graphql::Json checkoutsConnection(const graphql::JVal& vars = graphql::JVal()) const {
    return execUnwrap(gen::payments::kCheckoutsDocument, vars, "CheckoutsConnection");
  }
  graphql::Json myCheckouts(int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::payments::kMyCheckoutsDocument, vars, "MyCheckouts");
  }
  graphql::Json myCheckoutsConnection(int first = 50, std::string_view after = {}) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    return execUnwrap(gen::payments::kMyCheckoutsDocument, vars, "MyCheckoutsConnection");
  }
  graphql::Json paymentEvents(int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::payments::kPaymentEventsDocument, vars, "PaymentEvents");
  }
  graphql::Json paymentEventsConnection(int first = 50, std::string_view after = {}) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    return execUnwrap(gen::payments::kPaymentEventsDocument, vars, "PaymentEventsConnection");
  }
};

/// client.admin().quotas() — usage quotas at org/app scope.
class QuotasAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json forOrg(std::string_view orgId) const {
    return execUnwrap(gen::quotas::kQuotasForOrgDocument, one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json forApp(std::string_view appId) const {
    return execUnwrap(gen::quotas::kQuotasForAppDocument, one("appId", graphql::JVal(appId)));
  }
  graphql::Json effective(const graphql::JVal& vars) const {
    return execUnwrap(gen::quotas::kEffectiveQuotaDocument, vars);
  }
  graphql::Json set(const graphql::JVal& input) const {
    return execUnwrap(gen::quotas::kSetQuotaDocument, one("input", input));
  }
  graphql::Json remove(std::string_view quotaId) const {
    return execUnwrap(gen::quotas::kDeleteQuotaDocument, one("quotaId", graphql::JVal(quotaId)));
  }
};

/// client.admin().environments() — dedicated environments.
class EnvironmentsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json datacenters() const {
    return execUnwrap(gen::environments::kEnvironmentDatacentersDocument);
  }
  graphql::Json flavors(std::string_view datacenter) const {
    return execUnwrap(gen::environments::kEnvironmentFlavorsDocument,
                      one("datacenter", graphql::JVal(datacenter)));
  }
  graphql::Json versions() const {
    return execUnwrap(gen::environments::kEnvironmentVersionsDocument);
  }
  graphql::Json forwardVersions(std::string_view orgId, std::string_view slug) const {
    return execUnwrap(gen::environments::kEnvironmentForwardVersionsDocument,
                      two("orgId", graphql::JVal(orgId), "slug", graphql::JVal(slug)));
  }
  graphql::Json quote(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kEnvironmentQuoteDocument, one("input", input));
  }
  graphql::Json forOrg(std::string_view orgId) const {
    return execUnwrap(gen::environments::kOrgEnvironmentsDocument,
                      one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json get(std::string_view orgId, std::string_view slug) const {
    return execUnwrap(gen::environments::kOrgEnvironmentDocument,
                      two("orgId", graphql::JVal(orgId), "slug", graphql::JVal(slug)));
  }
  graphql::Json create(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kCreateEnvironmentDocument, one("input", input));
  }
  /// DRY RUN of redeploy(): component version diffs, schema apply/skip and
  /// the pipeline tasks a redeploy would run — read-only, no change order.
  /// Would-fail conditions come back in `blockers`. Needs view_environments.
  graphql::Json redeployPlan(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kEnvironmentRedeployPlanDocument, one("input", input));
  }
  graphql::Json redeploy(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kRedeployEnvironmentDocument, one("input", input));
  }
  graphql::Json restartServices(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kRestartEnvironmentServicesDocument, one("input", input));
  }
  graphql::Json resume(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kResumeEnvironmentDocument, one("input", input));
  }
  graphql::Json updateScaling(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kUpdateEnvironmentScalingDocument, one("input", input));
  }
  graphql::Json updateBillingTiers(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kUpdateEnvironmentBillingTiersDocument,
                      one("input", input));
  }
  graphql::Json linkApp(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kLinkAppToEnvironmentDocument, one("input", input));
  }
  /// Destructive.
  graphql::Json destroy(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kDestroyEnvironmentDocument, one("input", input));
  }
  /// Destructive and unrecoverable.
  graphql::Json purge(const graphql::JVal& input) const {
    return execUnwrap(gen::environments::kPurgeEnvironmentDocument, one("input", input));
  }
};

/// client.admin().usage() — replication + GraphQL usage reporting.
class UsageAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json appSummary(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kAppUsageSummaryDocument, vars);
  }
  graphql::Json appGraphqlOperations(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kAppGraphqlOperationsDocument, vars);
  }
  graphql::Json environmentSummary(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kEnvironmentUsageSummaryDocument, vars);
  }
  graphql::Json environmentByApp(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kEnvironmentUsageByAppDocument, vars);
  }
  graphql::Json orgByEnvironment(std::string_view orgId, std::string_view sinceIso) const {
    return execUnwrap(gen::usage::kOrgUsageByEnvironmentDocument,
                      two("orgId", graphql::JVal(orgId), "since", graphql::JVal(sinceIso)));
  }
  graphql::Json playerPulse(std::string_view orgId) const {
    return execUnwrap(gen::usage::kPlayerPulseDocument, one("orgId", graphql::JVal(orgId)));
  }
  /// Org-wide usage rollup for a window ("since" = ISO-8601 or relative like
  /// "7d"; defaults server-side to 7d).
  graphql::Json orgSummary(std::string_view orgId, std::string_view since = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!since.empty()) vars["since"] = since;
    return execUnwrap(
        "query OrgUsageSummary($orgId: BigInt!, $since: DateTime) {"
        " orgUsageSummary(orgId: $orgId, since: $since) {"
        " orgId replicationSendBytes replicationRecvBytes graphqlSendBytes graphqlRecvBytes"
        " totalOps } }",
        vars);
  }
  /// Projected month-end egress for one shared app vs its free allowance.
  graphql::Json appProjection(std::string_view orgId, std::string_view appId) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    return execUnwrap(
        "query AppUsageProjection($orgId: BigInt!, $appId: BigInt!) {"
        " appUsageProjection(orgId: $orgId, appId: $appId) {"
        " appId currentEgressBytes sufficientData daysElapsed projectedBytes"
        " freeAllowanceBytes projectedPctOfFree onTrackToExceed } }",
        vars);
  }
  /// Org-level rollup of per-app usage projections.
  graphql::Json orgProjection(std::string_view orgId) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    return execUnwrap(
        "query OrgUsageProjection($orgId: BigInt!) {"
        " orgUsageProjection(orgId: $orgId) {"
        " sufficientData daysElapsed totalProjectedBytes totalFreeAllowanceBytes"
        " apps { appId appName currentEgressBytes projectedBytes onTrackToExceed } } }",
        vars);
  }
};

/// client.admin().sharedEnvironment() — publish-to-shared, runtime gating,
/// spend caps, auto-billing.
class SharedEnvironmentAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json plans() const { return run("SharedEnvPlans", graphql::JVal()); }
  graphql::Json orgFreeAppQuota(std::string_view orgId) const {
    return run("OrgFreeAppQuota", one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json appSubscription(std::string_view appId) const {
    return run("AppSharedSubscription", one("appId", graphql::JVal(appId)));
  }
  graphql::Json appRuntimeState(std::string_view appId) const {
    return run("AppRuntimeState", one("appId", graphql::JVal(appId)));
  }
  graphql::Json orgAutoBilling(std::string_view orgId) const {
    return run("OrgAutoBilling", one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json orgPaymentMethods(std::string_view orgId) const {
    return run("OrgPaymentMethods", one("orgId", graphql::JVal(orgId)));
  }
  graphql::Json publishApp(const graphql::JVal& input) const {
    return run("PublishAppToShared", one("input", input));
  }
  graphql::Json cancelSubscription(std::string_view appId,
                                   std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return run("CancelSharedSubscription", vars);
  }
  graphql::Json setSpendCaps(const graphql::JVal& input) const {
    return run("SetAppSpendCaps", one("input", input));
  }
  graphql::Json setAutoBilling(const graphql::JVal& input) const {
    return run("SetAutoBilling", one("input", input));
  }
  graphql::Json setupPaymentMethod(std::string_view orgId,
                                   std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return run("SetupSharedPaymentMethod", vars);
  }
  graphql::Json removePaymentMethod(const graphql::JVal& vars) const {
    return run("RemoveSharedPaymentMethod", vars);
  }
  /// ECONOMY-SENSITIVE: reserve sustained egress throughput for a shared app
  /// (bytes/s; 0 clears back to the free tier). Pass an idempotencyKey.
  graphql::Json setAppReservedThroughput(const graphql::JVal& input,
                                         std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["input"] = input;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(
        "mutation SetAppReservedThroughput($input: SetAppReservedThroughputInput!,"
        " $idempotencyKey: String) {"
        " setAppReservedThroughput(input: $input, idempotencyKey: $idempotencyKey) {"
        " app { appId } chargedCents } }",
        vars);
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::sharedEnvironment::kSharedEnvironmentDocument, vars, op);
  }
};

/// Aggregate accessor: client.admin().
class AdminAPI {
 public:
  /// `gameApps` points at the Game-API-plane grids domain (same instance as
  /// client.gameApps()), grouped here for discoverability like CrowdyJS's
  /// admin.grids.
  explicit AdminAPI(std::shared_ptr<graphql::GraphQLClient> management,
                    GameAppsAPI* gameApps = nullptr)
      : organizations_(management),
        apps_(management),
        appAccess_(management),
        billing_(management),
        payments_(management),
        quotas_(management),
        environments_(management),
        usage_(management),
        sharedEnvironment_(management),
        grids_(gameApps) {}

  OrganizationsAPI& organizations() { return organizations_; }
  AppsAPI& apps() { return apps_; }
  AppAccessAPI& appAccess() { return appAccess_; }
  BillingAPI& billing() { return billing_; }
  PaymentsAPI& payments() { return payments_; }
  QuotasAPI& quotas() { return quotas_; }
  EnvironmentsAPI& environments() { return environments_; }
  UsageAPI& usage() { return usage_; }
  SharedEnvironmentAPI& sharedEnvironment() { return sharedEnvironment_; }
  /// App grids + grid runtime-permission administration (same instance as
  /// client.gameApps(); requires a per-game client / app token).
  GameAppsAPI& grids() { return *grids_; }

 private:
  OrganizationsAPI organizations_;
  AppsAPI apps_;
  AppAccessAPI appAccess_;
  BillingAPI billing_;
  PaymentsAPI payments_;
  QuotasAPI quotas_;
  EnvironmentsAPI environments_;
  UsageAPI usage_;
  SharedEnvironmentAPI sharedEnvironment_;
  GameAppsAPI* grids_;
};

}  // namespace crowdy::domains
