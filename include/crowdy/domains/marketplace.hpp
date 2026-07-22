#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.marketplace() — the P4a player-code marketplace (free mode).
///
/// Player-facing browse/publish/acquire/install/consent and the D4 grid
/// claim flows target the Game API; studio moderation (admission queue,
/// catalog administration, ownership transfer, claim-policy config) targets
/// the Management API — this domain holds both clients.
///
/// No money moves through any call here: every listing is free in P4a, an
/// acquisition is an entitlement write, and the paid modes ship with P4b.
/// Publishing snapshots artifact hashes and the DERIVED capability summary,
/// never source; installs consent to the summary's hash. The browser-side
/// artifact-byte decode + broker handoff is CrowdyJS-only (04 §7); native
/// clients use clientArtifact() and decode the base64 payload themselves.
namespace crowdy::domains {

class MarketplaceAPI {
  /// One endpoint's executor over the shared marketplace document.
  class Endpoint : public DomainBase {
   public:
    using DomainBase::DomainBase;
    graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
      return execUnwrap(gen::marketplace::kMarketplaceDocument, vars, op);
    }
    void runAsync(std::string_view op, const graphql::JVal& vars,
                  graphql::GraphQLCallback cb) const {
      execUnwrapAsync(gen::marketplace::kMarketplaceDocument, vars, op,
                      std::move(cb));
    }
  };

 public:
  MarketplaceAPI(std::shared_ptr<graphql::GraphQLClient> game,
                 std::shared_ptr<graphql::GraphQLClient> management)
      : game_(std::move(game)), management_(std::move(management)) {}

  // -- Store (Game API) -------------------------------------------------------

  /// Browse the app's active listings with per-listing admission standing.
  graphql::Json listings(const graphql::JVal& vars) const {
    return game_.run("MarketplaceListings", vars);
  }
  void listingsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceListings", vars, std::move(cb));
  }

  /// Published versions of one listing (capability summaries + consent hashes).
  graphql::Json versions(const graphql::JVal& vars) const {
    return game_.run("MarketplaceListingVersions", vars);
  }
  void versionsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceListingVersions", vars, std::move(cb));
  }

  /// The caller's entitlements in this app.
  graphql::Json myAcquisitions(const graphql::JVal& vars) const {
    return game_.run("MarketplaceMyAcquisitions", vars);
  }
  void myAcquisitionsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceMyAcquisitions", vars, std::move(cb));
  }

  /// The caller's active installs in this app.
  graphql::Json myInstalls(const graphql::JVal& vars) const {
    return game_.run("MarketplaceMyInstalls", vars);
  }
  void myInstallsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceMyInstalls", vars, std::move(cb));
  }

  /// Create a listing (personal, or org-owned via input.ownerOrgId).
  graphql::Json publishListing(const graphql::JVal& vars) const {
    return game_.run("MarketplacePublishListing", vars);
  }
  void publishListingAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplacePublishListing", vars, std::move(cb));
  }

  /// Publish an immutable version from compiled module versions (hashes only).
  graphql::Json publishVersion(const graphql::JVal& vars) const {
    return game_.run("MarketplacePublishVersion", vars);
  }
  void publishVersionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplacePublishVersion", vars, std::move(cb));
  }

  /// Free acquisition (entitlement write; idempotent per listing+caller).
  graphql::Json acquire(const graphql::JVal& vars) const {
    return game_.run("MarketplaceAcquire", vars);
  }
  void acquireAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceAcquire", vars, std::move(cb));
  }

  /// Install after consenting to the version's capability hash.
  graphql::Json install(const graphql::JVal& vars) const {
    return game_.run("MarketplaceInstall", vars);
  }
  void installAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceInstall", vars, std::move(cb));
  }

  /// Remove instances/attachments/fetch rights; the acquisition is retained.
  graphql::Json uninstall(const graphql::JVal& vars) const {
    return game_.run("MarketplaceUninstall", vars);
  }
  void uninstallAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceUninstall", vars, std::move(cb));
  }

  // -- Grid-attached client mods (D2) -------------------------------------------

  /// Client mods attached to a grid, with the caller's consent state.
  graphql::Json gridClientMods(const graphql::JVal& vars) const {
    return game_.run("MarketplaceGridClientMods", vars);
  }
  void gridClientModsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceGridClientMods", vars, std::move(cb));
  }

  /// Consent to one attachment's exact capability hash (per player).
  graphql::Json consentGridClientMod(const graphql::JVal& vars) const {
    return game_.run("MarketplaceConsentGridClientMod", vars);
  }
  void consentGridClientModAsync(const graphql::JVal& vars,
                                 graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceConsentGridClientMod", vars, std::move(cb));
  }

  /// Trust one author's active attachments at gridClientMods'
  /// authorCapabilityHash. Widening requires another explicit call.
  graphql::Json trustGridAuthor(const graphql::JVal& vars) const {
    return game_.run("MarketplaceTrustGridAuthor", vars);
  }
  void trustGridAuthorAsync(const graphql::JVal& vars,
                            graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceTrustGridAuthor", vars, std::move(cb));
  }

  /// Fetch an acquired/attached listing's client artifact (base64 + metadata).
  graphql::Json clientArtifact(const graphql::JVal& vars) const {
    return game_.run("MarketplaceClientArtifact", vars);
  }
  void clientArtifactAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceClientArtifact", vars, std::move(cb));
  }

  // -- D4 grid claim flows (Game API) --------------------------------------------

  /// The app's claim policy (self_claim / approval / invite / marketplace_only).
  graphql::Json gridClaimPolicy(const graphql::JVal& vars) const {
    return game_.run("MarketplaceGridClaimPolicy", vars);
  }
  void gridClaimPolicyAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceGridClaimPolicy", vars, std::move(cb));
  }

  /// Pending claim requests (approvers see the app queue; players their own).
  graphql::Json gridClaimRequests(const graphql::JVal& vars) const {
    return game_.run("MarketplaceGridClaimRequests", vars);
  }
  void gridClaimRequestsAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceGridClaimRequests", vars, std::move(cb));
  }

  /// Claim grid ownership under the app policy (server-authorized, D4).
  graphql::Json claimGridOwnership(const graphql::JVal& vars) const {
    return game_.run("MarketplaceClaimGridOwnership", vars);
  }
  void claimGridOwnershipAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceClaimGridOwnership", vars, std::move(cb));
  }

  /// Approve or deny a pending claim request (approvers/staff).
  graphql::Json decideGridClaim(const graphql::JVal& vars) const {
    return game_.run("MarketplaceDecideGridClaim", vars);
  }
  void decideGridClaimAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceDecideGridClaim", vars, std::move(cb));
  }

  /// Issue a standing claim invite (approvers/staff; INVITE mode).
  graphql::Json issueGridClaimInvite(const graphql::JVal& vars) const {
    return game_.run("MarketplaceIssueGridClaimInvite", vars);
  }
  void issueGridClaimInviteAsync(const graphql::JVal& vars,
                                 graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceIssueGridClaimInvite", vars, std::move(cb));
  }

  // -- Studio moderation (Management API) -----------------------------------------

  /// The admission queue: listings joined with allow-list standing.
  graphql::Json admissionQueue(const graphql::JVal& vars) const {
    return management_.run("MarketplaceAdmissionQueue", vars);
  }
  void admissionQueueAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceAdmissionQueue", vars, std::move(cb));
  }

  /// Studio catalog administration view (includes delisted/killed on request).
  graphql::Json appListings(const graphql::JVal& vars) const {
    return management_.run("MarketplaceAppListings", vars);
  }
  void appListingsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceAppListings", vars, std::move(cb));
  }

  /// All acquisitions in the app (studio audit view).
  graphql::Json appAcquisitions(const graphql::JVal& vars) const {
    return management_.run("MarketplaceAppAcquisitions", vars);
  }
  void appAcquisitionsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceAppAcquisitions", vars, std::move(cb));
  }

  /// Audited personal<->org listing transfer (DN-9).
  graphql::Json transferListing(const graphql::JVal& vars) const {
    return management_.run("MarketplaceTransferListing", vars);
  }
  void transferListingAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceTransferListing", vars, std::move(cb));
  }

  /// Catalog status (owner delist/relist; studio KILLED — pair with the
  /// game-side listing kill switch to stop running installs).
  graphql::Json setListingStatus(const graphql::JVal& vars) const {
    return management_.run("MarketplaceSetListingStatus", vars);
  }
  void setListingStatusAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceSetListingStatus", vars, std::move(cb));
  }

  /// Configure the app's D4 grid claim policy (manage_apps).
  graphql::Json setGridClaimPolicy(const graphql::JVal& vars) const {
    return management_.run("MarketplaceSetGridClaimPolicy", vars);
  }
  void setGridClaimPolicyAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceSetGridClaimPolicy", vars, std::move(cb));
  }

  // -- P4b: paid modes + grid commerce (Game API) -----------------------------

  /// Renew a RENT / extend a TIME_LIMITED acquisition (a wallet charge).
  graphql::Json renewAcquisition(const graphql::JVal& vars) const {
    return game_.run("MarketplaceRenewAcquisition", vars);
  }
  void renewAcquisitionAsync(const graphql::JVal& vars,
                             graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceRenewAcquisition", vars, std::move(cb));
  }

  /// Top up a COST_LIMITED acquisition's unit budget (a wallet charge).
  graphql::Json topUpAcquisition(const graphql::JVal& vars) const {
    return game_.run("MarketplaceTopUpAcquisition", vars);
  }
  void topUpAcquisitionAsync(const graphql::JVal& vars,
                             graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceTopUpAcquisition", vars, std::move(cb));
  }

  /// Refund a paid acquisition (window + void-on-use rules apply). Cents back.
  graphql::Json refundAcquisition(const graphql::JVal& vars) const {
    return game_.run("MarketplaceRefundAcquisition", vars);
  }
  void refundAcquisitionAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceRefundAcquisition", vars, std::move(cb));
  }

  /// Browse the app's grid listings.
  graphql::Json gridListings(const graphql::JVal& vars) const {
    return game_.run("MarketplaceGridListings", vars);
  }
  void gridListingsAsync(const graphql::JVal& vars,
                         graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplaceGridListings", vars, std::move(cb));
  }

  /// Buy a grid listing (wallet debit + atomic ownership; refund on failure).
  graphql::Json purchaseGrid(const graphql::JVal& vars) const {
    return game_.run("MarketplacePurchaseGrid", vars);
  }
  void purchaseGridAsync(const graphql::JVal& vars,
                         graphql::GraphQLCallback cb) const {
    game_.runAsync("MarketplacePurchaseGrid", vars, std::move(cb));
  }

  // -- P4b: pricing, payouts, risk (Management API) ---------------------------

  /// Author-only: set a listing's acquisition mode + price.
  graphql::Json setListingPricing(const graphql::JVal& vars) const {
    return management_.run("MarketplaceSetListingPricing", vars);
  }
  void setListingPricingAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceSetListingPricing", vars, std::move(cb));
  }

  /// Set the app's marketplace org revenue share in bps (manage_billing).
  graphql::Json setOrgShare(const graphql::JVal& vars) const {
    return management_.run("MarketplaceSetOrgShare", vars);
  }
  void setOrgShareAsync(const graphql::JVal& vars,
                        graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceSetOrgShare", vars, std::move(cb));
  }

  /// Account Session for Stripe's EMBEDDED Connect components (web-only
  /// rendering: the clientSecret feeds Connect.js in a browser/webview;
  /// native callers hand it to their embedded web surface).
  graphql::Json createAccountSession(const graphql::JVal& vars) const {
    return management_.run("MarketplaceCreateAccountSession", vars);
  }
  void createAccountSessionAsync(const graphql::JVal& vars,
                                 graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceCreateAccountSession", vars, std::move(cb));
  }

  /// Embedded-components Account Session for an ORG payout account
  /// (manage_billing in the org).
  graphql::Json createOrgAccountSession(const graphql::JVal& vars) const {
    return management_.run("MarketplaceCreateOrgAccountSession", vars);
  }
  void createOrgAccountSessionAsync(const graphql::JVal& vars,
                                    graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceCreateOrgAccountSession", vars,
                         std::move(cb));
  }

  /// Begin Stripe Connect Express onboarding (returns a BROWSER URL — native
  /// callers open it externally; the hosted KYC flow itself is web-only).
  graphql::Json beginSellerOnboarding(const graphql::JVal& vars) const {
    return management_.run("MarketplaceBeginSellerOnboarding", vars);
  }
  void beginSellerOnboardingAsync(const graphql::JVal& vars,
                                  graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceBeginSellerOnboarding", vars, std::move(cb));
  }

  /// Begin onboarding for an ORG payout account (manage_billing in the org).
  graphql::Json beginOrgSellerOnboarding(const graphql::JVal& vars) const {
    return management_.run("MarketplaceBeginOrgSellerOnboarding", vars);
  }
  void beginOrgSellerOnboardingAsync(const graphql::JVal& vars,
                                     graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceBeginOrgSellerOnboarding", vars,
                         std::move(cb));
  }

  /// The calling player's seller payout balance.
  graphql::Json mySellerBalance() const {
    return management_.run("MarketplaceMySellerBalance", graphql::JVal());
  }
  void mySellerBalanceAsync(graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceMySellerBalance",
                         graphql::JVal(), std::move(cb));
  }

  /// Pay out the payable balance to the caller's Connect account.
  graphql::Json requestPayout() const {
    return management_.run("MarketplaceRequestPayout", graphql::JVal());
  }
  void requestPayoutAsync(graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceRequestPayout", graphql::JVal(),
                         std::move(cb));
  }

  /// Earn-to-mod: convert payable balance into player wallet credit.
  graphql::Json spendPayoutToWallet(const graphql::JVal& vars) const {
    return management_.run("MarketplaceSpendPayoutToWallet", vars);
  }
  void spendPayoutToWalletAsync(const graphql::JVal& vars,
                                graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceSpendPayoutToWallet", vars, std::move(cb));
  }

  /// The app's open T11 commerce risk queue (manage_compute).
  graphql::Json commerceRiskQueue(const graphql::JVal& vars) const {
    return management_.run("MarketplaceCommerceRiskQueue", vars);
  }
  void commerceRiskQueueAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceCommerceRiskQueue", vars, std::move(cb));
  }

  /// Release or confirm a T11 risk flag (manage_compute).
  graphql::Json decideRiskFlag(const graphql::JVal& vars) const {
    return management_.run("MarketplaceDecideRiskFlag", vars);
  }
  void decideRiskFlagAsync(const graphql::JVal& vars,
                           graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceDecideRiskFlag", vars, std::move(cb));
  }

  /// Studio: create a grid listing (blueprint or concrete; manage_apps).
  graphql::Json createGridListing(const graphql::JVal& vars) const {
    return management_.run("MarketplaceCreateGridListing", vars);
  }
  void createGridListingAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    management_.runAsync("MarketplaceCreateGridListing", vars, std::move(cb));
  }

 private:
  Endpoint game_;
  Endpoint management_;
};

}  // namespace crowdy::domains
