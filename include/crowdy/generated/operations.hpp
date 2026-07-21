// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDLs at https://docs.crowdedkingdoms.com/schema/).

#pragma once

#include <string_view>

/// GraphQL operation documents, one namespace per domain. Each constant is
/// the full document text of its source file (which may contain fragments or
/// several operations); the matching *OperationName constant names the
/// operation to execute.
namespace crowdy::gen {

namespace actors {

/// actors/Actor.graphql
inline constexpr std::string_view kActorDocument = R"gql(query Actor($uuid: String!) {
  actor(uuid: $uuid) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kActorOperationName = "Actor";

/// actors/Actors.graphql
inline constexpr std::string_view kActorsDocument = R"gql(query Actors($filter: ActorFilterInput) {
  actors(filter: $filter) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
}

query ActorsConnection($first: Int, $after: String, $filter: ActorFilterInput) {
  actorsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        uuid
        appId
        userId
        avatarId
        chunk {
          x
          y
          z
        }
        privateState
        publicState
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kActorsOperationName = "Actors";
inline constexpr std::string_view kActorsConnectionOperationName = "ActorsConnection";

/// actors/BatchLookupActors.graphql
inline constexpr std::string_view kBatchLookupActorsDocument = R"gql(query BatchLookupActors($input: BatchActorLookupInput!) {
  batchLookupActors(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kBatchLookupActorsOperationName = "BatchLookupActors";

/// actors/CreateActor.graphql
inline constexpr std::string_view kCreateActorDocument = R"gql(mutation CreateActor($input: CreateActorInput!) {
  createActor(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateActorOperationName = "CreateActor";

/// actors/DeleteActor.graphql
inline constexpr std::string_view kDeleteActorDocument = R"gql(mutation DeleteActor($uuid: String!, $idempotencyKey: String) {
  deleteActor(uuid: $uuid, idempotencyKey: $idempotencyKey) {
    uuid
    appId
    userId
  }
})gql";
inline constexpr std::string_view kDeleteActorOperationName = "DeleteActor";

/// actors/UpdateActor.graphql
inline constexpr std::string_view kUpdateActorDocument = R"gql(mutation UpdateActor($uuid: String!, $input: UpdateActorInput!) {
  updateActor(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateActorOperationName = "UpdateActor";

/// actors/UpdateActorState.graphql
inline constexpr std::string_view kUpdateActorStateDocument = R"gql(mutation UpdateActorState($uuid: String!, $input: UpdateActorStateInput!) {
  updateActorState(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    privateState
    publicState
  }
})gql";
inline constexpr std::string_view kUpdateActorStateOperationName = "UpdateActorState";

}  // namespace actors

namespace appAccess {

/// appAccess/AppAccessTiers.graphql
inline constexpr std::string_view kAppAccessTiersDocument = R"gql(query AppAccessTiers($appId: BigInt!) {
  appAccessTiers(appId: $appId) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppAccessTiersOperationName = "AppAccessTiers";

/// appAccess/AppGrantMemberCandidates.graphql
inline constexpr std::string_view kAppGrantMemberCandidatesDocument = R"gql(query AppGrantMemberCandidates($appId: BigInt!) {
  appGrantMemberCandidates(appId: $appId) {
    userId
    email
    gamertag
  }
})gql";
inline constexpr std::string_view kAppGrantMemberCandidatesOperationName = "AppGrantMemberCandidates";

/// appAccess/AppUserAccessByApp.graphql
inline constexpr std::string_view kAppUserAccessByAppDocument = R"gql(query AppUserAccessByApp(
  $appId: BigInt!
  $status: String
  $limit: Int
  $offset: Int
) {
  appUserAccessByApp(
    appId: $appId
    status: $status
    limit: $limit
    offset: $offset
  ) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
}

query AppUserAccessConnection(
  $appId: BigInt!
  $first: Int
  $after: String
  $status: String
) {
  appUserAccessConnection(
    appId: $appId
    first: $first
    after: $after
    status: $status
  ) {
    edges {
      cursor
      node {
        appUserAccessId
        appId
        userId
        tierId
        status
        grantedBy
        subscriptionId
        expiresAt
        createdAt
        updatedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kAppUserAccessByAppOperationName = "AppUserAccessByApp";
inline constexpr std::string_view kAppUserAccessConnectionOperationName = "AppUserAccessConnection";

/// appAccess/ArchiveAccessTier.graphql
inline constexpr std::string_view kArchiveAccessTierDocument = R"gql(mutation ArchiveAccessTier($tierId: BigInt!) {
  archiveAccessTier(tierId: $tierId) {
    tierId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAccessTierOperationName = "ArchiveAccessTier";

/// appAccess/ClaimFreeAppAccess.graphql
inline constexpr std::string_view kClaimFreeAppAccessDocument = R"gql(mutation ClaimFreeAppAccess($appId: BigInt!) {
  claimFreeAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kClaimFreeAppAccessOperationName = "ClaimFreeAppAccess";

/// appAccess/CreateAccessTier.graphql
inline constexpr std::string_view kCreateAccessTierDocument = R"gql(mutation CreateAccessTier($input: CreateAccessTierInput!) {
  createAccessTier(input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateAccessTierOperationName = "CreateAccessTier";

/// appAccess/GrantAppAccess.graphql
inline constexpr std::string_view kGrantAppAccessDocument = R"gql(mutation GrantAppAccess($input: GrantAppAccessInput!) {
  grantAppAccess(input: $input) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantAppAccessOperationName = "GrantAppAccess";

/// appAccess/GrantMyAppAccess.graphql
inline constexpr std::string_view kGrantMyAppAccessDocument = R"gql(mutation GrantMyAppAccess($appId: BigInt!) {
  grantMyAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantMyAppAccessOperationName = "GrantMyAppAccess";

/// appAccess/MyAppAccess.graphql
inline constexpr std::string_view kMyAppAccessDocument = R"gql(query MyAppAccess($appId: BigInt!) {
  myAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kMyAppAccessOperationName = "MyAppAccess";

/// appAccess/RevokeAppAccess.graphql
inline constexpr std::string_view kRevokeAppAccessDocument = R"gql(mutation RevokeAppAccess($appId: BigInt!, $userId: BigInt!) {
  revokeAppAccess(appId: $appId, userId: $userId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kRevokeAppAccessOperationName = "RevokeAppAccess";

/// appAccess/RuntimePermissions.graphql
inline constexpr std::string_view kRuntimePermissionsDocument = R"gql(query RuntimePermissions {
  runtimePermissions
})gql";
inline constexpr std::string_view kRuntimePermissionsOperationName = "RuntimePermissions";

/// appAccess/UpdateAccessTier.graphql
inline constexpr std::string_view kUpdateAccessTierDocument = R"gql(mutation UpdateAccessTier($tierId: BigInt!, $input: UpdateAccessTierInput!) {
  updateAccessTier(tierId: $tierId, input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAccessTierOperationName = "UpdateAccessTier";

}  // namespace appAccess

namespace apps {

/// apps/App.graphql
inline constexpr std::string_view kAppDocument = R"gql(query App($appId: BigInt!) {
  app(appId: $appId) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    deploymentTarget
    runtimeStatus
    runtimeDenialReason
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppOperationName = "App";

/// apps/AppBySlug.graphql
inline constexpr std::string_view kAppBySlugDocument = R"gql(query AppBySlug($orgSlug: String!, $appSlug: String!) {
  appBySlug(orgSlug: $orgSlug, appSlug: $appSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppBySlugOperationName = "AppBySlug";

/// apps/AppsForOrg.graphql
inline constexpr std::string_view kAppsForOrgDocument = R"gql(query AppsForOrg($orgSlug: String!) {
  appsForOrg(orgSlug: $orgSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppsForOrgOperationName = "AppsForOrg";

/// apps/ArchiveApp.graphql
inline constexpr std::string_view kArchiveAppDocument = R"gql(mutation ArchiveApp($appId: BigInt!) {
  archiveApp(appId: $appId) {
    appId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAppOperationName = "ArchiveApp";

/// apps/CodeAdmissions.graphql
inline constexpr std::string_view kCodeAdmissionsDocument = R"gql(fragment AppCodeAdmissionFields on AppCodeAdmission {
  admissionId
  appId
  subjectKind
  subjectRef
  versionRange
  admittedBy
  admittedAt
  revokedAt
}

query AppCodeAdmissionMode($appId: BigInt!) {
  appCodeAdmissionMode(appId: $appId)
}

query AppCodeAdmissions($appId: BigInt!, $includeRevoked: Boolean) {
  appCodeAdmissions(appId: $appId, includeRevoked: $includeRevoked) {
    ...AppCodeAdmissionFields
  }
}

mutation SetAppCodeAdmissionMode(
  $appId: BigInt!
  $mode: CodeAdmissionMode!
) {
  setAppCodeAdmissionMode(appId: $appId, mode: $mode)
}

mutation AdmitAppCode($input: AdmitAppCodeInput!) {
  admitAppCode(input: $input) {
    ...AppCodeAdmissionFields
  }
}

mutation RevokeAppCodeAdmission(
  $appId: BigInt!
  $admissionId: String!
) {
  revokeAppCodeAdmission(appId: $appId, admissionId: $admissionId) {
    ...AppCodeAdmissionFields
  }
})gql";
inline constexpr std::string_view kAppCodeAdmissionModeOperationName = "AppCodeAdmissionMode";
inline constexpr std::string_view kAppCodeAdmissionsOperationName = "AppCodeAdmissions";
inline constexpr std::string_view kSetAppCodeAdmissionModeOperationName = "SetAppCodeAdmissionMode";
inline constexpr std::string_view kAdmitAppCodeOperationName = "AdmitAppCode";
inline constexpr std::string_view kRevokeAppCodeAdmissionOperationName = "RevokeAppCodeAdmission";

/// apps/CreateApp.graphql
inline constexpr std::string_view kCreateAppDocument = R"gql(mutation CreateApp($input: CreateAppInput!) {
  createApp(input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateAppOperationName = "CreateApp";

/// apps/MarketplaceApps.graphql
inline constexpr std::string_view kMarketplaceAppsDocument = R"gql(query MarketplaceApps(
  $filter: AppMarketplaceFilterInput
  $limit: Int
  $offset: Int
) {
  apps(filter: $filter, limit: $limit, offset: $offset) {
    items {
      appId
      orgId
      name
      slug
      description
      visibility
      status
      metadata
      splitMode
      gameApiUrl
      createdAt
      updatedAt
      org {
        orgId
        slug
        name
      }
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query AppsConnection(
  $first: Int
  $after: String
  $filter: AppMarketplaceFilterInput
) {
  appsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        appId
        orgId
        name
        slug
        description
        visibility
        status
        metadata
        splitMode
        gameApiUrl
        createdAt
        updatedAt
        org {
          orgId
          slug
          name
        }
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kMarketplaceAppsOperationName = "MarketplaceApps";
inline constexpr std::string_view kAppsConnectionOperationName = "AppsConnection";

/// apps/MyApps.graphql
inline constexpr std::string_view kMyAppsDocument = R"gql(query MyApps {
  myApps {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kMyAppsOperationName = "MyApps";

/// apps/SetAppVisibility.graphql
inline constexpr std::string_view kSetAppVisibilityDocument = R"gql(mutation SetAppVisibility($appId: BigInt!, $visibility: AppVisibility!) {
  setAppVisibility(appId: $appId, visibility: $visibility) {
    appId
    visibility
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppVisibilityOperationName = "SetAppVisibility";

/// apps/UpdateApp.graphql
inline constexpr std::string_view kUpdateAppDocument = R"gql(mutation UpdateApp($appId: BigInt!, $input: UpdateAppInput!) {
  updateApp(appId: $appId, input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAppOperationName = "UpdateApp";

}  // namespace apps

namespace auth {

/// auth/Logout.graphql
inline constexpr std::string_view kLogoutDocument = R"gql(mutation Logout {
  logout
})gql";
inline constexpr std::string_view kLogoutOperationName = "Logout";

/// auth/LogoutAllDevices.graphql
inline constexpr std::string_view kLogoutAllDevicesDocument = R"gql(mutation LogoutAllDevices {
  logoutAllDevices
})gql";
inline constexpr std::string_view kLogoutAllDevicesOperationName = "LogoutAllDevices";

}  // namespace auth

namespace avatars {

/// avatars/Avatars.graphql
inline constexpr std::string_view kAvatarsDocument = R"gql(query UserAvatars($userId: BigInt!) {
  userAvatars(userId: $userId) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query AvatarById($id: BigInt!) {
  avatar(id: $id) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query MyAvatars {
  myAvatars {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query AvatarAppState($appId: BigInt!, $avatarId: BigInt!) {
  avatarAppState(appId: $appId, avatarId: $avatarId) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
}

query AvatarAppStates($appId: BigInt!, $avatarIds: [BigInt!]!) {
  avatarAppStates(appId: $appId, avatarIds: $avatarIds) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
}

mutation CreateAvatar($input: CreateAvatarInput!) {
  createAvatar(input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation UpdateAvatar($id: BigInt!, $input: UpdateAvatarInput!) {
  updateAvatar(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation DeleteAvatar($id: BigInt!, $idempotencyKey: String) {
  deleteAvatar(id: $id, idempotencyKey: $idempotencyKey) {
    avatarId
    userId
    name
    createdAt
  }
}

mutation UpdateAvatarState($id: BigInt!, $input: UpdateAvatarStateInput!) {
  updateAvatarState(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation UpdateAvatarAppState($input: UpdateAvatarAppStateInput!) {
  updateAvatarAppState(input: $input) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAvatarsOperationName = "UserAvatars";
inline constexpr std::string_view kAvatarByIdOperationName = "AvatarById";
inline constexpr std::string_view kMyAvatarsOperationName = "MyAvatars";
inline constexpr std::string_view kAvatarAppStateOperationName = "AvatarAppState";
inline constexpr std::string_view kAvatarAppStatesOperationName = "AvatarAppStates";
inline constexpr std::string_view kCreateAvatarOperationName = "CreateAvatar";
inline constexpr std::string_view kUpdateAvatarOperationName = "UpdateAvatar";
inline constexpr std::string_view kDeleteAvatarOperationName = "DeleteAvatar";
inline constexpr std::string_view kUpdateAvatarStateOperationName = "UpdateAvatarState";
inline constexpr std::string_view kUpdateAvatarAppStateOperationName = "UpdateAvatarAppState";

}  // namespace avatars

namespace billing {

/// billing/AppBudget.graphql
inline constexpr std::string_view kAppBudgetDocument = R"gql(query AppBudget($orgId: BigInt!, $appId: BigInt!) {
  appBudget(orgId: $orgId, appId: $appId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetOperationName = "AppBudget";

/// billing/AppBudgets.graphql
inline constexpr std::string_view kAppBudgetsDocument = R"gql(query AppBudgets($orgId: BigInt!) {
  appBudgets(orgId: $orgId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetsOperationName = "AppBudgets";

/// billing/BillingTiers.graphql
inline constexpr std::string_view kBillingTiersDocument = R"gql(query BuddyBillingTiers {
  buddyBillingTiers {
    tierLevel
    messagesPerSecond
    bandwidthMbitPerSecond
    chargeCents
    currency
    label
    description
  }
}

query GraphqlBillingTiers {
  graphqlBillingTiers {
    tierLevel
    endpointCallsPerSecond
    bandwidthMbitPerSecond
    chargeCents
    currency
    label
    description
  }
}

query PostgresBillingTiers {
  postgresBillingTiers {
    tierLevel
    bandwidthMbitPerSecond
    chargeCents
    currency
    label
    description
  }
})gql";
inline constexpr std::string_view kBuddyBillingTiersOperationName = "BuddyBillingTiers";
inline constexpr std::string_view kGraphqlBillingTiersOperationName = "GraphqlBillingTiers";
inline constexpr std::string_view kPostgresBillingTiersOperationName = "PostgresBillingTiers";

/// billing/SetAppBudget.graphql
inline constexpr std::string_view kSetAppBudgetDocument = R"gql(mutation SetAppBudget($orgId: BigInt!, $appId: BigInt!, $monthlyLimitCents: BigInt!) {
  setAppBudget(orgId: $orgId, appId: $appId, monthlyLimitCents: $monthlyLimitCents) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppBudgetOperationName = "SetAppBudget";

/// billing/WalletBalance.graphql
inline constexpr std::string_view kWalletBalanceDocument = R"gql(query WalletBalance($orgId: BigInt!) {
  walletBalance(orgId: $orgId) {
    walletId
    orgId
    balanceCents
    currency
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kWalletBalanceOperationName = "WalletBalance";

/// billing/WalletTransactions.graphql
inline constexpr std::string_view kWalletTransactionsDocument = R"gql(query WalletTransactions($orgId: BigInt!, $limit: Int, $offset: Int) {
  walletTransactions(orgId: $orgId, limit: $limit, offset: $offset) {
    transactionId
    walletId
    orgId
    amountCents
    balanceAfter
    transactionType
    description
    referenceId
    appId
    createdAt
  }
}

query WalletTransactionsConnection(
  $orgId: BigInt!
  $first: Int
  $after: String
) {
  walletTransactionsConnection(orgId: $orgId, first: $first, after: $after) {
    edges {
      cursor
      node {
        transactionId
        walletId
        orgId
        amountCents
        balanceAfter
        transactionType
        description
        referenceId
        appId
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kWalletTransactionsOperationName = "WalletTransactions";
inline constexpr std::string_view kWalletTransactionsConnectionOperationName = "WalletTransactionsConnection";

}  // namespace billing

namespace channels {

/// channels/AddChannelMember.graphql
inline constexpr std::string_view kAddChannelMemberDocument = R"gql(mutation AddChannelMember($groupId: BigInt!, $userId: BigInt!) {
  addChannelMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddChannelMemberOperationName = "AddChannelMember";

/// channels/Channel.graphql
inline constexpr std::string_view kChannelDocument = R"gql(query Channel($groupId: BigInt!) {
  channel(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelOperationName = "Channel";

/// channels/ChannelMembers.graphql
inline constexpr std::string_view kChannelMembersDocument = R"gql(query ChannelMembers($groupId: BigInt!) {
  channelMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kChannelMembersOperationName = "ChannelMembers";

/// channels/ChannelPolicy.graphql
inline constexpr std::string_view kChannelPolicyDocument = R"gql(query ChannelPolicy($appId: BigInt!) {
  channelPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kChannelPolicyOperationName = "ChannelPolicy";

/// channels/ChannelRoles.graphql
inline constexpr std::string_view kChannelRolesDocument = R"gql(query ChannelRoles($groupId: BigInt!) {
  channelRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelRolesOperationName = "ChannelRoles";

/// channels/Channels.graphql
inline constexpr std::string_view kChannelsDocument = R"gql(query Channels($appId: BigInt!) {
  channels(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelsOperationName = "Channels";

/// channels/CreateChannel.graphql
inline constexpr std::string_view kCreateChannelDocument = R"gql(mutation CreateChannel($input: CreateChannelInput!) {
  createChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelOperationName = "CreateChannel";

/// channels/CreateChannelRole.graphql
inline constexpr std::string_view kCreateChannelRoleDocument = R"gql(mutation CreateChannelRole($input: CreateGroupRoleInput!) {
  createChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelRoleOperationName = "CreateChannelRole";

/// channels/DeleteChannel.graphql
inline constexpr std::string_view kDeleteChannelDocument = R"gql(mutation DeleteChannel($groupId: BigInt!) {
  deleteChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kDeleteChannelOperationName = "DeleteChannel";

/// channels/DeleteChannelRole.graphql
inline constexpr std::string_view kDeleteChannelRoleDocument = R"gql(mutation DeleteChannelRole($groupRoleId: BigInt!) {
  deleteChannelRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteChannelRoleOperationName = "DeleteChannelRole";

/// channels/JoinChannel.graphql
inline constexpr std::string_view kJoinChannelDocument = R"gql(mutation JoinChannel($groupId: BigInt!) {
  joinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinChannelOperationName = "JoinChannel";

/// channels/LeaveChannel.graphql
inline constexpr std::string_view kLeaveChannelDocument = R"gql(mutation LeaveChannel($groupId: BigInt!) {
  leaveChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kLeaveChannelOperationName = "LeaveChannel";

/// channels/MyChannels.graphql
inline constexpr std::string_view kMyChannelsDocument = R"gql(query MyChannels($appId: BigInt!) {
  myChannels(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyChannelsOperationName = "MyChannels";

/// channels/RemoveChannelMember.graphql
inline constexpr std::string_view kRemoveChannelMemberDocument = R"gql(mutation RemoveChannelMember($groupId: BigInt!, $userId: BigInt!) {
  removeChannelMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveChannelMemberOperationName = "RemoveChannelMember";

/// channels/RequestToJoinChannel.graphql
inline constexpr std::string_view kRequestToJoinChannelDocument = R"gql(mutation RequestToJoinChannel($groupId: BigInt!) {
  requestToJoinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinChannelOperationName = "RequestToJoinChannel";

/// channels/SetChannelMemberRoles.graphql
inline constexpr std::string_view kSetChannelMemberRolesDocument = R"gql(mutation SetChannelMemberRoles($input: SetMemberRolesInput!) {
  setChannelMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetChannelMemberRolesOperationName = "SetChannelMemberRoles";

/// channels/SetChannelPolicy.graphql
inline constexpr std::string_view kSetChannelPolicyDocument = R"gql(mutation SetChannelPolicy($input: SetChannelPolicyInput!) {
  setChannelPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetChannelPolicyOperationName = "SetChannelPolicy";

/// channels/UpdateChannel.graphql
inline constexpr std::string_view kUpdateChannelDocument = R"gql(mutation UpdateChannel($input: UpdateChannelInput!) {
  updateChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelOperationName = "UpdateChannel";

/// channels/UpdateChannelRole.graphql
inline constexpr std::string_view kUpdateChannelRoleDocument = R"gql(mutation UpdateChannelRole($input: UpdateGroupRoleInput!) {
  updateChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelRoleOperationName = "UpdateChannelRole";

}  // namespace channels

namespace chunks {

/// chunks/GetChunk.graphql
inline constexpr std::string_view kGetChunkDocument = R"gql(query GetChunk($input: GetChunkInput!) {
  getChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    voxelStates {
      voxelCoord {
        x
        y
        z
      }
      voxelType
      state
    }
    owner
    createdAt
    updatedAt
    chunkState
    cdnUploadedAt
    lods {
      level
      data
    }
  }
})gql";
inline constexpr std::string_view kGetChunkOperationName = "GetChunk";

/// chunks/GetChunkLods.graphql
inline constexpr std::string_view kGetChunkLodsDocument = R"gql(query GetChunkLods($input: GetChunkLodsInput!) {
  getChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kGetChunkLodsOperationName = "GetChunkLods";

/// chunks/GetChunksByDistance.graphql
inline constexpr std::string_view kGetChunksByDistanceDocument = R"gql(query GetChunksByDistance($input: GetChunksByDistanceInput!) {
  getChunksByDistance(input: $input) {
    limit
    skip
    chunks {
      chunkId
      appId
      coordinates {
        x
        y
        z
      }
      voxels
      owner
      createdAt
      updatedAt
      chunkState
      cdnUploadedAt
      lods {
        level
        data
      }
    }
  }
})gql";
inline constexpr std::string_view kGetChunksByDistanceOperationName = "GetChunksByDistance";

/// chunks/GetVoxelList.graphql
inline constexpr std::string_view kGetVoxelListDocument = R"gql(query GetVoxelList($input: GetVoxelListInput!) {
  getVoxelList(input: $input) {
    coordinates {
      x
      y
      z
    }
    voxels {
      voxelUpdateId
      appId
      coordinates {
        x
        y
        z
      }
      location {
        x
        y
        z
      }
      voxelType
      state
      createdBy
      createdAt
    }
  }
})gql";
inline constexpr std::string_view kGetVoxelListOperationName = "GetVoxelList";

/// chunks/UpdateChunk.graphql
inline constexpr std::string_view kUpdateChunkDocument = R"gql(mutation UpdateChunk($input: ChunkUpdateInput!) {
  updateChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkOperationName = "UpdateChunk";

/// chunks/UpdateChunkLods.graphql
inline constexpr std::string_view kUpdateChunkLodsDocument = R"gql(mutation UpdateChunkLods($input: UpdateChunkLodsInput!) {
  updateChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkLodsOperationName = "UpdateChunkLods";

/// chunks/UpdateChunkState.graphql
inline constexpr std::string_view kUpdateChunkStateDocument = R"gql(mutation UpdateChunkState($input: UpdateChunkStateInput!) {
  updateChunkState(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkStateOperationName = "UpdateChunkState";

}  // namespace chunks

namespace compute {

/// compute/ComputeModules.graphql
inline constexpr std::string_view kComputeModulesDocument = R"gql(fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  alwaysOn
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  lastError
  createdAt
  updatedAt
}

fragment ComputeVersionFields on WasmModuleVersion {
  versionId
  moduleId
  appId
  versionNo
  sourceHash
  sdkVersion
  abiVersion
  compileStatus
  compileLog
  compiledSizeBytes
  publishedAt
  createdAt
}

fragment ComputeTriggerFields on WasmModuleTrigger {
  triggerId
  appId
  moduleId
  triggerType
  tickHz
  onEvent
  functionName
  containerTypeName
  propertyKey
  eventName
  debounceMs
  exportName
  invokePolicyJson
  contractJson
  createdAt
}

fragment ComputePolicyFields on WasmModulePolicy {
  appId
  enabled
  maxModules
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  failureThreshold
  cooldownMs
}

fragment ComputeRunFields on WasmModuleRun {
  runId
  appId
  flowId
  moduleId
  moduleName
  triggerSource
  entry
  startedAt
  durationUs
  fuelUsed
  dbReads
  dbWrites
  egressMsgs
  egressBytes
  success
  errorMessage
  circuitAction
}

mutation ComputeUpsertModule($input: UpsertComputeModuleInput!) {
  computeUpsertModule(input: $input) {
    ...ComputeModuleFields
  }
}

mutation ComputeDeployVersion($input: DeployComputeVersionInput!) {
  computeDeployVersion(input: $input) {
    ...ComputeVersionFields
  }
}

mutation ComputeSetModuleEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  computeSetModuleEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...ComputeModuleFields
  }
}

mutation ComputeDeleteModule($appId: BigInt!, $name: String!) {
  computeDeleteModule(appId: $appId, name: $name)
}

mutation ComputeUpsertTrigger($input: UpsertComputeTriggerInput!) {
  computeUpsertTrigger(input: $input) {
    ...ComputeTriggerFields
  }
}

mutation ComputeDeleteTrigger($appId: BigInt!, $triggerId: String!) {
  computeDeleteTrigger(appId: $appId, triggerId: $triggerId)
}

mutation ComputeSetPolicy($input: SetComputePolicyInput!) {
  computeSetPolicy(input: $input) {
    ...ComputePolicyFields
  }
}

mutation ComputeInvoke(
  $appId: BigInt!
  $moduleName: String!
  $exportName: String!
  $paramsJson: String
) {
  computeInvoke(
    appId: $appId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
}

query ComputeModules($appId: BigInt!) {
  computeModules(appId: $appId) {
    ...ComputeModuleFields
  }
}

query ComputeModule($appId: BigInt!, $name: String!) {
  computeModule(appId: $appId, name: $name) {
    ...ComputeModuleFields
  }
}

query ComputeModuleVersions($appId: BigInt!, $moduleName: String!, $limit: Int) {
  computeModuleVersions(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ...ComputeVersionFields
  }
}

query ComputeModuleTriggers($appId: BigInt!, $moduleName: String) {
  computeModuleTriggers(appId: $appId, moduleName: $moduleName) {
    ...ComputeTriggerFields
  }
}

query ComputeModulePolicy($appId: BigInt!) {
  computeModulePolicy(appId: $appId) {
    ...ComputePolicyFields
  }
}

query ComputeModuleRuns(
  $appId: BigInt!
  $moduleName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  computeModuleRuns(
    appId: $appId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...ComputeRunFields
  }
}

query ComputeModuleStats($appId: BigInt!, $windowMinutes: Int) {
  computeModuleStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    totalFuelUsed
    totalEgressMsgs
    avgDurationUs
    byModule {
      moduleName
      runs
      failures
      fuelUsed
      avgDurationUs
      circuitState
    }
  }
}

query ComputeModuleLogs($appId: BigInt!, $moduleName: String, $limit: Int) {
  computeModuleLogs(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ts
    moduleName
    level
    message
    triggerSource
  }
}

query ComputeAppDiagnostics($appId: BigInt!) {
  computeAppDiagnostics(appId: $appId) {
    appId
    moduleCount
    enabledModuleCount
    versionCount
    triggerCount
    runs24h
    failedRuns24h
    fuelUsed24h
    topModules {
      moduleName
      runs
      failures
    }
    toolchainRustVersion
    toolchainWasmOptVersion
  }
}

query ComputeTemplates($appId: BigInt!) {
  computeTemplates(appId: $appId) {
    name
    description
    exports
  }
}

mutation ComputeDeployTemplate($appId: BigInt!, $templateName: String!, $moduleName: String) {
  computeDeployTemplate(appId: $appId, templateName: $templateName, moduleName: $moduleName) {
    ...ComputeModuleFields
  }
})gql";
inline constexpr std::string_view kComputeUpsertModuleOperationName = "ComputeUpsertModule";
inline constexpr std::string_view kComputeDeployVersionOperationName = "ComputeDeployVersion";
inline constexpr std::string_view kComputeSetModuleEnabledOperationName = "ComputeSetModuleEnabled";
inline constexpr std::string_view kComputeDeleteModuleOperationName = "ComputeDeleteModule";
inline constexpr std::string_view kComputeUpsertTriggerOperationName = "ComputeUpsertTrigger";
inline constexpr std::string_view kComputeDeleteTriggerOperationName = "ComputeDeleteTrigger";
inline constexpr std::string_view kComputeSetPolicyOperationName = "ComputeSetPolicy";
inline constexpr std::string_view kComputeInvokeOperationName = "ComputeInvoke";
inline constexpr std::string_view kComputeModulesOperationName = "ComputeModules";
inline constexpr std::string_view kComputeModuleOperationName = "ComputeModule";
inline constexpr std::string_view kComputeModuleVersionsOperationName = "ComputeModuleVersions";
inline constexpr std::string_view kComputeModuleTriggersOperationName = "ComputeModuleTriggers";
inline constexpr std::string_view kComputeModulePolicyOperationName = "ComputeModulePolicy";
inline constexpr std::string_view kComputeModuleRunsOperationName = "ComputeModuleRuns";
inline constexpr std::string_view kComputeModuleStatsOperationName = "ComputeModuleStats";
inline constexpr std::string_view kComputeModuleLogsOperationName = "ComputeModuleLogs";
inline constexpr std::string_view kComputeAppDiagnosticsOperationName = "ComputeAppDiagnostics";
inline constexpr std::string_view kComputeTemplatesOperationName = "ComputeTemplates";
inline constexpr std::string_view kComputeDeployTemplateOperationName = "ComputeDeployTemplate";

}  // namespace compute

namespace controlPlane {

/// controlPlane/ControlPlane.graphql
inline constexpr std::string_view kControlPlaneDocument = R"gql(query CpEnvironments($page: Int, $pageSize: Int) {
  cpEnvironments(page: $page, pageSize: $pageSize) {
    rows {
      id
      orgId
      slug
      displayName
      primaryCloud
      primaryRegion
      status
      deletionProtectionEnabled
      subdomainHandle
      createdAt
      updatedAt
    }
    total
    page
    pageSize
  }
}

query CpEnvironment($slug: String!) {
  cpEnvironment(slug: $slug) {
    id
    orgId
    slug
    displayName
    primaryCloud
    primaryRegion
    status
    deletionProtectionEnabled
    deletionProtectionSetAt
    deletionProtectionSetByEmail
    subdomainHandle
    createdAt
    updatedAt
  }
}

query CpChangeOrders($environmentId: String, $page: Int, $pageSize: Int) {
  cpChangeOrders(
    environmentId: $environmentId
    page: $page
    pageSize: $pageSize
  ) {
    rows {
      id
      environmentId
      kind
      status
      claimedBy
      claimedAt
      finishedAt
      error
      createdAt
      updatedAt
    }
    total
    page
    pageSize
  }
}

query CpChangeOrder($id: String!) {
  cpChangeOrder(id: $id) {
    order {
      id
      environmentId
      kind
      status
      error
      createdAt
      updatedAt
      finishedAt
    }
    tasks {
      id
      changeOrderId
      kind
      ordinal
      status
      error
      createdAt
      finishedAt
    }
    steps {
      id
      taskId
      ordinal
      kind
      status
      attempt
      error
      createdAt
      finishedAt
    }
  }
}

query CpAudit($environmentId: String, $limit: Int) {
  cpAudit(environmentId: $environmentId, limit: $limit) {
    id
    actorUserId
    actorKind
    action
    entityKind
    entityId
    environmentId
    payloadJson
    createdAt
  }
}

query CpSecrets($environmentId: String) {
  cpSecrets(environmentId: $environmentId) {
    id
    environmentId
    name
    kind
    createdAt
    rotatedAt
  }
}

query CpEnvSecrets($environmentId: String) {
  cpEnvSecrets(environmentId: $environmentId) {
    id
    environmentId
    name
    kind
    createdAt
    rotatedAt
  }
}

query CpOvhCatalogSummary($region: String) {
  cpOvhCatalogSummary(region: $region) {
    region
    flavorName
    vcpus
    ramMb
    diskGb
    ovhHourlyPriceCents
    customerHourlyPriceCents
    customerPricingMode
    quotaAvailable
  }
}

query CpUsageSummary($environmentSlug: String!, $since: DateTime!) {
  cpUsageSummary(environmentSlug: $environmentSlug, since: $since) {
    environmentSlug
    orgId
    replication {
      minute
      recvBytes
      sendBytes
      recvMsgs
      sendMsgs
    }
    graphql {
      minute
      recvBytes
      sendBytes
    }
    replicationRates {
      peakSendMsgsPerSec
      peakSendMbitPerSec
      avgSendMsgsPerSec
      avgSendMbitPerSec
      sampleMinutes
    }
    buddyLive {
      serverId
      clientSendMsgsPerSec
      clientRecvMsgsPerSec
      clients
      updatedAt
    }
  }
}

query CpUnreleasedGameApiTags {
  cpUnreleasedGameApiTags {
    tags {
      tag
      taggedAt
      proposedEnvironmentVersion
      schemaChanged
    }
    currentDeployTargetGameApiTag
    gitSourceAvailable
  }
}

query CpEnvironmentVersions {
  cpEnvironmentVersions {
    rows {
      version
      releasedAt
      status
      notes
      sourceCommit
      gameApiGitTag
      buddyVersion
      ingestedAt
      updatedAt
      inGit
      inDb
      isLatestAvailable
    }
    latestAvailableVersion
    gitSourceAvailable
  }
}

query OperatorUsers {
  operatorUsers {
    userId
    email
    gamertag
    isOperator
    isSuperAdmin
    createdAt
  }
}

mutation SetEnvironmentDeletionProtection(
  $environmentId: String!
  $enabled: Boolean!
) {
  setEnvironmentDeletionProtection(
    environmentId: $environmentId
    enabled: $enabled
  )
}

mutation PutCpSecret(
  $environmentId: String!
  $name: String!
  $plaintext: String!
  $kind: String
) {
  putCpSecret(
    environmentId: $environmentId
    name: $name
    plaintext: $plaintext
    kind: $kind
  ) {
    id
    environmentId
    name
    kind
    createdAt
    rotatedAt
  }
}

mutation DeleteCpSecret($environmentId: String!, $name: String!) {
  deleteCpSecret(environmentId: $environmentId, name: $name)
}

mutation PutCpEnvSecret(
  $environmentId: String!
  $name: String!
  $plaintext: String!
  $kind: String
) {
  putCpEnvSecret(
    environmentId: $environmentId
    name: $name
    plaintext: $plaintext
    kind: $kind
  ) {
    id
    environmentId
    name
    kind
    createdAt
    rotatedAt
  }
}

mutation IngestEnvironmentVersion($input: IngestEnvironmentVersionInput!) {
  ingestEnvironmentVersion(input: $input) {
    version
    releasedAt
    status
    notes
    gameApiGitTag
    inGit
    inDb
    isLatestAvailable
  }
}

mutation PublishEnvironmentReleaseFromGameApiTag(
  $input: PublishEnvironmentReleaseFromGameApiTagInput!
) {
  publishEnvironmentReleaseFromGameApiTag(input: $input) {
    version {
      version
      status
      gameApiGitTag
    }
    schemaChanged
    committedToGit
    gitCommitError
  }
}

mutation YankEnvironmentVersion($version: String!) {
  yankEnvironmentVersion(version: $version)
}

query CpComputePlatformCeilings {
  cpComputePlatformCeilings {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    updatedAt
    updatedByUserId
  }
}

mutation CpSetComputePlatformCeilings($input: CpSetComputePlatformCeilingsInput!) {
  cpSetComputePlatformCeilings(input: $input) {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    updatedAt
    updatedByUserId
  }
})gql";
inline constexpr std::string_view kCpEnvironmentsOperationName = "CpEnvironments";
inline constexpr std::string_view kCpEnvironmentOperationName = "CpEnvironment";
inline constexpr std::string_view kCpChangeOrdersOperationName = "CpChangeOrders";
inline constexpr std::string_view kCpChangeOrderOperationName = "CpChangeOrder";
inline constexpr std::string_view kCpAuditOperationName = "CpAudit";
inline constexpr std::string_view kCpSecretsOperationName = "CpSecrets";
inline constexpr std::string_view kCpEnvSecretsOperationName = "CpEnvSecrets";
inline constexpr std::string_view kCpOvhCatalogSummaryOperationName = "CpOvhCatalogSummary";
inline constexpr std::string_view kCpUsageSummaryOperationName = "CpUsageSummary";
inline constexpr std::string_view kCpUnreleasedGameApiTagsOperationName = "CpUnreleasedGameApiTags";
inline constexpr std::string_view kCpEnvironmentVersionsOperationName = "CpEnvironmentVersions";
inline constexpr std::string_view kOperatorUsersOperationName = "OperatorUsers";
inline constexpr std::string_view kSetEnvironmentDeletionProtectionOperationName = "SetEnvironmentDeletionProtection";
inline constexpr std::string_view kPutCpSecretOperationName = "PutCpSecret";
inline constexpr std::string_view kDeleteCpSecretOperationName = "DeleteCpSecret";
inline constexpr std::string_view kPutCpEnvSecretOperationName = "PutCpEnvSecret";
inline constexpr std::string_view kIngestEnvironmentVersionOperationName = "IngestEnvironmentVersion";
inline constexpr std::string_view kPublishEnvironmentReleaseFromGameApiTagOperationName = "PublishEnvironmentReleaseFromGameApiTag";
inline constexpr std::string_view kYankEnvironmentVersionOperationName = "YankEnvironmentVersion";
inline constexpr std::string_view kCpComputePlatformCeilingsOperationName = "CpComputePlatformCeilings";
inline constexpr std::string_view kCpSetComputePlatformCeilingsOperationName = "CpSetComputePlatformCeilings";

}  // namespace controlPlane

namespace environments {

/// environments/CreateEnvironment.graphql
inline constexpr std::string_view kCreateEnvironmentDocument = R"gql(mutation CreateEnvironment($input: CreateEnvironmentInput!) {
  createEnvironment(input: $input) {
    environment {
      id
      orgId
      slug
      displayName
      status
      billingStatus
      environmentClass
      primaryRegion
      desiredEnvironmentVersion
      observedEnvironmentVersion
      createdAt
      updatedAt
    }
    changeOrders {
      id
      kind
      status
      error
      createdAt
    }
  }
})gql";
inline constexpr std::string_view kCreateEnvironmentOperationName = "CreateEnvironment";

/// environments/DestroyEnvironment.graphql
inline constexpr std::string_view kDestroyEnvironmentDocument = R"gql(mutation DestroyEnvironment($input: DestroyEnvironmentInput!) {
  destroyEnvironment(input: $input) {
    id
    environmentId
    kind
    status
    requestedBy
    error
    createdAt
    updatedAt
    finishedAt
  }
})gql";
inline constexpr std::string_view kDestroyEnvironmentOperationName = "DestroyEnvironment";

/// environments/EnvironmentDatacenters.graphql
inline constexpr std::string_view kEnvironmentDatacentersDocument = R"gql(query EnvironmentDatacenters {
  environmentDatacenters {
    region
    name
    city
    continent
    status
    isAvailable
    selectableInstanceCount
    syncedAt
  }
})gql";
inline constexpr std::string_view kEnvironmentDatacentersOperationName = "EnvironmentDatacenters";

/// environments/EnvironmentFlavors.graphql
inline constexpr std::string_view kEnvironmentFlavorsDocument = R"gql(query EnvironmentFlavors($datacenter: String!) {
  environmentFlavors(datacenter: $datacenter) {
    flavorName
    flavorType
    vcpus
    ramMb
    diskGb
    quotaAvailable
    customerHourlyPriceCents
    customerMonthlyPriceCents
    currency
    availabilityStatus
    pricingMode
    syncedAt
  }
})gql";
inline constexpr std::string_view kEnvironmentFlavorsOperationName = "EnvironmentFlavors";

/// environments/EnvironmentForwardVersions.graphql
inline constexpr std::string_view kEnvironmentForwardVersionsDocument = R"gql(query EnvironmentForwardVersions($orgId: BigInt!, $slug: String!) {
  environmentForwardVersions(orgId: $orgId, slug: $slug) {
    version
    releasedAt
    status
    notes
    gameApiGitTag
  }
})gql";
inline constexpr std::string_view kEnvironmentForwardVersionsOperationName = "EnvironmentForwardVersions";

/// environments/EnvironmentQuote.graphql
inline constexpr std::string_view kEnvironmentQuoteDocument = R"gql(query EnvironmentQuote($input: EnvironmentQuoteInput!) {
  environmentQuote(input: $input) {
    datacenter
    databaseFlavor
    gameApiFlavor
    udpBuddyFlavor
    caddyFlavor
    gameApiMinServers
    gameApiMaxServers
    udpBuddyMinServers
    udpBuddyMaxServers
    loadBalancerCount
    environmentClass
    singleBoxFlavor
    hourlyCostCents
    firstDayReserveCents
    walletBalanceCents
    availableBalanceCents
    currency
    canCreate
  }
})gql";
inline constexpr std::string_view kEnvironmentQuoteOperationName = "EnvironmentQuote";

/// environments/EnvironmentRedeployPlan.graphql
inline constexpr std::string_view kEnvironmentRedeployPlanDocument = R"gql(query EnvironmentRedeployPlan($input: RedeployEnvironmentInput!) {
  environmentRedeployPlan(input: $input) {
    environmentSlug
    currentVersion
    targetVersion
    deployMode
    changeOrderKind
    schemaWillApply
    schemaGitRef
    componentChanges {
      component
      fromVersion
      toVersion
      changed
    }
    tasks {
      kind
      dependsOn
      steps
    }
    blockers
    notes
  }
})gql";
inline constexpr std::string_view kEnvironmentRedeployPlanOperationName = "EnvironmentRedeployPlan";

/// environments/EnvironmentVersions.graphql
inline constexpr std::string_view kEnvironmentVersionsDocument = R"gql(query EnvironmentVersions {
  environmentVersions {
    version
    releasedAt
    status
    notes
    gameApiGitTag
  }
})gql";
inline constexpr std::string_view kEnvironmentVersionsOperationName = "EnvironmentVersions";

/// environments/LinkAppToEnvironment.graphql
inline constexpr std::string_view kLinkAppToEnvironmentDocument = R"gql(mutation LinkAppToEnvironment($input: LinkAppToEnvironmentInput!) {
  linkAppToEnvironment(input: $input) {
    appId
    orgId
    slug
    name
    splitMode
    deploymentTarget
    gameApiUrl
    status
  }
})gql";
inline constexpr std::string_view kLinkAppToEnvironmentOperationName = "LinkAppToEnvironment";

/// environments/OrgEnvironment.graphql
inline constexpr std::string_view kOrgEnvironmentDocument = R"gql(query OrgEnvironment($orgId: BigInt!, $slug: String!) {
  orgEnvironment(orgId: $orgId, slug: $slug) {
    environment {
      id
      orgId
      slug
      displayName
      status
      billingStatus
      environmentClass
      singleBoxFlavor
      primaryRegion
      desiredEnvironmentVersion
      observedEnvironmentVersion
      gameApiMinServers
      gameApiMaxServers
      udpBuddyMinServers
      udpBuddyMaxServers
      loadBalancerCount
      createdAt
      updatedAt
    }
    components {
      id
      kind
      status
      desiredVersion
      observedVersion
    }
    changeOrders {
      id
      kind
      status
      error
      createdAt
      finishedAt
    }
    outputs {
      name
      label
      value
      valueKind
    }
  }
})gql";
inline constexpr std::string_view kOrgEnvironmentOperationName = "OrgEnvironment";

/// environments/OrgEnvironments.graphql
inline constexpr std::string_view kOrgEnvironmentsDocument = R"gql(query OrgEnvironments($orgId: BigInt!) {
  orgEnvironments(orgId: $orgId) {
    id
    orgId
    slug
    displayName
    status
    billingStatus
    environmentClass
    singleBoxFlavor
    primaryCloud
    primaryRegion
    desiredEnvironmentVersion
    observedEnvironmentVersion
    gameApiMinServers
    gameApiMaxServers
    udpBuddyMinServers
    udpBuddyMaxServers
    loadBalancerCount
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgEnvironmentsOperationName = "OrgEnvironments";

/// environments/PurgeEnvironment.graphql
inline constexpr std::string_view kPurgeEnvironmentDocument = R"gql(mutation PurgeEnvironment($input: PurgeEnvironmentInput!) {
  purgeEnvironment(input: $input)
})gql";
inline constexpr std::string_view kPurgeEnvironmentOperationName = "PurgeEnvironment";

/// environments/RedeployEnvironment.graphql
inline constexpr std::string_view kRedeployEnvironmentDocument = R"gql(mutation RedeployEnvironment($input: RedeployEnvironmentInput!) {
  redeployEnvironment(input: $input) {
    id
    environmentId
    kind
    status
    requestedBy
    error
    createdAt
    updatedAt
    finishedAt
  }
})gql";
inline constexpr std::string_view kRedeployEnvironmentOperationName = "RedeployEnvironment";

/// environments/RestartEnvironmentServices.graphql
inline constexpr std::string_view kRestartEnvironmentServicesDocument = R"gql(mutation RestartEnvironmentServices($input: RestartEnvironmentServicesInput!) {
  restartEnvironmentServices(input: $input) {
    id
    environmentId
    kind
    status
    requestedBy
    error
    createdAt
    updatedAt
    finishedAt
  }
})gql";
inline constexpr std::string_view kRestartEnvironmentServicesOperationName = "RestartEnvironmentServices";

/// environments/ResumeEnvironment.graphql
inline constexpr std::string_view kResumeEnvironmentDocument = R"gql(mutation ResumeEnvironment($input: ResumeEnvironmentInput!) {
  resumeEnvironment(input: $input) {
    id
    environmentId
    kind
    status
    requestedBy
    error
    createdAt
    updatedAt
    finishedAt
  }
})gql";
inline constexpr std::string_view kResumeEnvironmentOperationName = "ResumeEnvironment";

/// environments/UpdateEnvironmentBillingTiers.graphql
inline constexpr std::string_view kUpdateEnvironmentBillingTiersDocument = R"gql(mutation UpdateEnvironmentBillingTiers(
  $input: UpdateEnvironmentBillingTiersInput!
) {
  updateEnvironmentBillingTiers(input: $input) {
    id
    orgId
    slug
    displayName
    status
    billingStatus
    environmentClass
    singleBoxFlavor
    primaryCloud
    primaryRegion
    desiredEnvironmentVersion
    observedEnvironmentVersion
    gameApiMinServers
    gameApiMaxServers
    udpBuddyMinServers
    udpBuddyMaxServers
    loadBalancerCount
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateEnvironmentBillingTiersOperationName = "UpdateEnvironmentBillingTiers";

/// environments/UpdateEnvironmentScaling.graphql
inline constexpr std::string_view kUpdateEnvironmentScalingDocument = R"gql(mutation UpdateEnvironmentScaling($input: UpdateEnvironmentScalingInput!) {
  updateEnvironmentScaling(input: $input) {
    id
    environmentId
    kind
    status
    requestedBy
    error
    createdAt
    updatedAt
    finishedAt
  }
})gql";
inline constexpr std::string_view kUpdateEnvironmentScalingOperationName = "UpdateEnvironmentScaling";

}  // namespace environments

namespace gameApps {

/// gameApps/GameApps.graphql
inline constexpr std::string_view kGameAppsDocument = R"gql(fragment GridOwnershipFields on GridOwnership {
  gridOwnershipId
  gridId
  appId
  ownerKind
  ownerRef
  tenure
  acquiredVia
  acquiredAt
  expiresAt
}

query GridOwnership($appId: BigInt!, $gridId: BigInt!) {
  gridOwnership(appId: $appId, gridId: $gridId) {
    ...GridOwnershipFields
  }
}

mutation AssignGridOwnership($input: AssignGridOwnershipInput!) {
  assignGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

mutation TransferGridOwnership($input: TransferGridOwnershipInput!) {
  transferGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

query GridUserPermissions($appId: BigInt!, $gridId: BigInt!, $userId: BigInt!) {
  gridUserPermissions(appId: $appId, gridId: $gridId, userId: $userId) {
    appId
    gridId
    userId
    permissionKeys
  }
}

query NearbyGridPermissions($input: NearbyGridPermissionsInput!) {
  nearbyGridPermissions(input: $input) {
    appId
    gridId
    userId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    permissionKeys
  }
}

query GridPermissionLimits($appId: BigInt!, $gridId: BigInt!) {
  gridPermissionLimits(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
}

query GridGroupGrants($appId: BigInt!, $gridId: BigInt!, $groupId: BigInt!) {
  gridGroupGrants(appId: $appId, gridId: $gridId, groupId: $groupId) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
}

mutation CreateGrid($input: CreateGridInput!) {
  createGrid(input: $input) {
    grid {
      grid_id
      app_id
      low_chunk {
        x
        y
        z
      }
      high_chunk {
        x
        y
        z
      }
    }
    error
  }
}

mutation DeleteGrid($input: DeleteGridInput!) {
  deleteGrid(input: $input) {
    gridId
    error
  }
}

mutation GrantGridPermissions($input: GrantGridPermissionsInput!) {
  grantGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
}

mutation RevokeGridPermissions($input: RevokeGridPermissionsInput!) {
  revokeGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
}

mutation SetGridPermissionLimits($input: SetGridPermissionLimitsInput!) {
  setGridPermissionLimits(input: $input) {
    appId
    gridId
    permissionKeys
  }
}

mutation AssignGroupToGrid($input: AssignGroupToGridInput!) {
  assignGroupToGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
}

mutation RevokeGroupFromGrid($input: RevokeGroupFromGridInput!) {
  revokeGroupFromGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
})gql";
inline constexpr std::string_view kGridOwnershipOperationName = "GridOwnership";
inline constexpr std::string_view kAssignGridOwnershipOperationName = "AssignGridOwnership";
inline constexpr std::string_view kTransferGridOwnershipOperationName = "TransferGridOwnership";
inline constexpr std::string_view kGridUserPermissionsOperationName = "GridUserPermissions";
inline constexpr std::string_view kNearbyGridPermissionsOperationName = "NearbyGridPermissions";
inline constexpr std::string_view kGridPermissionLimitsOperationName = "GridPermissionLimits";
inline constexpr std::string_view kGridGroupGrantsOperationName = "GridGroupGrants";
inline constexpr std::string_view kCreateGridOperationName = "CreateGrid";
inline constexpr std::string_view kDeleteGridOperationName = "DeleteGrid";
inline constexpr std::string_view kGrantGridPermissionsOperationName = "GrantGridPermissions";
inline constexpr std::string_view kRevokeGridPermissionsOperationName = "RevokeGridPermissions";
inline constexpr std::string_view kSetGridPermissionLimitsOperationName = "SetGridPermissionLimits";
inline constexpr std::string_view kAssignGroupToGridOperationName = "AssignGroupToGrid";
inline constexpr std::string_view kRevokeGroupFromGridOperationName = "RevokeGroupFromGrid";

}  // namespace gameApps

namespace gameModel {

/// gameModel/GameModelAutomations.graphql
inline constexpr std::string_view kGameModelAutomationsDocument = R"gql(fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
}

fragment GmAutomationTriggerFields on GmAutomationTrigger {
  triggerId
  appId
  automationId
  onEvent
  functionName
  containerTypeName
  propertyKey
  debounceMs
}

fragment GmAutomationPolicyFields on GmAutomationPolicy {
  appId
  enabled
  maxAutomations
  minIntervalMs
  maxFanout
  maxCascadeDepth
  globalRunsPerMinute
}

fragment GmAutomationRunFields on GmAutomationRun {
  runId
  appId
  flowId
  automationId
  automationName
  triggerSource
  parentRunId
  cascadeDepth
  startedAt
  finishedAt
  durationUs
  targets
  invocations
  mutations
  fnCalls
  gasUsed
  success
  errorMessage
  circuitAction
  computeUnits
}

mutation GameModelUpsertAutomation($input: UpsertAutomationInput!) {
  gameModelUpsertAutomation(input: $input) {
    ...GmAutomationFields
  }
}

mutation GameModelDeleteAutomation($appId: BigInt!, $name: String!) {
  gameModelDeleteAutomation(appId: $appId, name: $name)
}

mutation GameModelSetAutomationEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  gameModelSetAutomationEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...GmAutomationFields
  }
}

mutation GameModelUpsertAutomationTrigger($input: UpsertAutomationTriggerInput!) {
  gameModelUpsertAutomationTrigger(input: $input) {
    ...GmAutomationTriggerFields
  }
}

mutation GameModelDeleteAutomationTrigger($appId: BigInt!, $triggerId: String!) {
  gameModelDeleteAutomationTrigger(appId: $appId, triggerId: $triggerId)
}

mutation GameModelSetAutomationPolicy($input: SetAutomationPolicyInput!) {
  gameModelSetAutomationPolicy(input: $input) {
    ...GmAutomationPolicyFields
  }
}

mutation GameModelRunAutomation($appId: BigInt!, $name: String!) {
  gameModelRunAutomation(appId: $appId, name: $name) {
    ...GmAutomationRunFields
  }
}

query GameModelAutomations($appId: BigInt!) {
  gameModelAutomations(appId: $appId) {
    ...GmAutomationFields
  }
}

query GameModelAutomation($appId: BigInt!, $name: String!) {
  gameModelAutomation(appId: $appId, name: $name) {
    ...GmAutomationFields
  }
}

query GameModelAutomationTriggers($appId: BigInt!, $automationName: String) {
  gameModelAutomationTriggers(appId: $appId, automationName: $automationName) {
    ...GmAutomationTriggerFields
  }
}

query GameModelAutomationPolicy($appId: BigInt!) {
  gameModelAutomationPolicy(appId: $appId) {
    ...GmAutomationPolicyFields
  }
}

query GameModelAutomationRuns(
  $appId: BigInt!
  $automationName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  gameModelAutomationRuns(
    appId: $appId
    automationName: $automationName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...GmAutomationRunFields
  }
}

query GameModelAutomationStats($appId: BigInt!, $windowMinutes: Int) {
  gameModelAutomationStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    runsPerMinute
    totalInvocations
    totalMutations
    totalComputeUnits
    avgDurationUs
    byAutomation {
      automationName
      runs
      failures
      invocations
      computeUnits
      avgDurationUs
      circuitState
    }
  }
}

query GameModelAppDiagnostics($appId: BigInt!) {
  gameModelAppDiagnostics(appId: $appId) {
    appId
    containerCount
    propertyCount
    edgeCount
    sessionCount
    functionCount
    automationCount
    eventCount
    events24h
    failedEvents24h
    automationEvents24h
    topFunctions {
      functionName
      invocations
      failures
    }
  }
})gql";
inline constexpr std::string_view kGameModelUpsertAutomationOperationName = "GameModelUpsertAutomation";
inline constexpr std::string_view kGameModelDeleteAutomationOperationName = "GameModelDeleteAutomation";
inline constexpr std::string_view kGameModelSetAutomationEnabledOperationName = "GameModelSetAutomationEnabled";
inline constexpr std::string_view kGameModelUpsertAutomationTriggerOperationName = "GameModelUpsertAutomationTrigger";
inline constexpr std::string_view kGameModelDeleteAutomationTriggerOperationName = "GameModelDeleteAutomationTrigger";
inline constexpr std::string_view kGameModelSetAutomationPolicyOperationName = "GameModelSetAutomationPolicy";
inline constexpr std::string_view kGameModelRunAutomationOperationName = "GameModelRunAutomation";
inline constexpr std::string_view kGameModelAutomationsOperationName = "GameModelAutomations";
inline constexpr std::string_view kGameModelAutomationOperationName = "GameModelAutomation";
inline constexpr std::string_view kGameModelAutomationTriggersOperationName = "GameModelAutomationTriggers";
inline constexpr std::string_view kGameModelAutomationPolicyOperationName = "GameModelAutomationPolicy";
inline constexpr std::string_view kGameModelAutomationRunsOperationName = "GameModelAutomationRuns";
inline constexpr std::string_view kGameModelAutomationStatsOperationName = "GameModelAutomationStats";
inline constexpr std::string_view kGameModelAppDiagnosticsOperationName = "GameModelAppDiagnostics";

/// gameModel/GameModelRuntime.graphql
inline constexpr std::string_view kGameModelRuntimeDocument = R"gql(fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
}

fragment GmInvokeResultFields on GmInvokeResult {
  eventId
  functionName
  success
  returnValueJson
  errorMessage
  mutationsApplied {
    containerId
    key
    valueType
    oldValueJson
    newValueJson
  }
}

mutation GameModelCreateSession($input: CreateSessionInput!) {
  gameModelCreateSession(input: $input) {
    ...GmSessionFields
  }
}

mutation GameModelJoinSession($input: JoinSessionInput!) {
  gameModelJoinSession(input: $input) {
    sessionId
    userId
    role
  }
}

mutation GameModelSetSessionTurn($input: SetSessionTurnInput!) {
  gameModelSetSessionTurn(input: $input) {
    ...GmSessionFields
  }
}

mutation GameModelCreateContainer($input: CreateContainerInput!) {
  gameModelCreateContainer(input: $input) {
    ...GmContainerFields
  }
}

mutation GameModelDeleteContainer($appId: BigInt!, $containerId: String!) {
  gameModelDeleteContainer(appId: $appId, containerId: $containerId)
}

mutation GameModelSetProperty($input: SetContainerPropertyInput!) {
  gameModelSetProperty(input: $input) {
    ...GmContainerFields
  }
}

mutation GameModelAddEdge($input: AddEdgeInput!) {
  gameModelAddEdge(input: $input) {
    edgeId
    fromContainerId
    toContainerId
    relationshipType
    weight
  }
}

mutation GameModelDeleteEdge($appId: BigInt!, $edgeId: String!) {
  gameModelDeleteEdge(appId: $appId, edgeId: $edgeId)
}

mutation GameModelInvoke($input: InvokeFunctionInput!) {
  gameModelInvoke(input: $input) {
    ...GmInvokeResultFields
  }
}

query GameModelContainer($appId: BigInt!, $containerId: String!) {
  gameModelContainer(appId: $appId, containerId: $containerId) {
    ...GmContainerFields
  }
}

query GameModelContainers(
  $appId: BigInt!
  $typeName: String
  $sessionId: String
  $where: [GmPropertyPredicateInput!]
  $limit: Int
  $offset: Int
) {
  gameModelContainers(
    appId: $appId
    typeName: $typeName
    sessionId: $sessionId
    where: $where
    limit: $limit
    offset: $offset
  ) {
    ...GmContainerFields
  }
}

query GameModelContainerState($appId: BigInt!, $containerId: String!) {
  gameModelContainerState(appId: $appId, containerId: $containerId) {
    containerId
    appId
    sessionId
    typeName
    displayName
    ownerUserId
    propertiesJson
  }
}

query GameModelTraverse(
  $appId: BigInt!
  $rootId: String!
  $relationshipType: String!
  $depth: Int
) {
  gameModelTraverse(
    appId: $appId
    rootId: $rootId
    relationshipType: $relationshipType
    depth: $depth
  ) {
    rootId
    nodes {
      ...GmContainerFields
    }
    edges {
      edgeId
      fromContainerId
      toContainerId
      relationshipType
      weight
    }
  }
}

query GameModelSession($appId: BigInt!, $sessionId: String!) {
  gameModelSession(appId: $appId, sessionId: $sessionId) {
    ...GmSessionFields
  }
}

query GameModelSessions($appId: BigInt!, $status: String) {
  gameModelSessions(appId: $appId, status: $status) {
    ...GmSessionFields
  }
}

query GameModelEvents(
  $appId: BigInt!
  $sessionId: String
  $selfContainerId: String
  $functionName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  gameModelEvents(
    appId: $appId
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    eventId
    flowId
    sessionId
    functionName
    selfContainerId
    callerUserId
    callerKind
    automationId
    paramsJson
    mutationsAppliedJson
    permissionEffectsAppliedJson
    returnValueJson
    success
    errorMessage
    executedAt
  }
}

query GameModelEventsConnection(
  $appId: BigInt!
  $first: Int
  $after: String
  $sessionId: String
  $selfContainerId: String
  $functionName: String
  $success: Boolean
) {
  gameModelEventsConnection(
    appId: $appId
    first: $first
    after: $after
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
  ) {
    edges {
      cursor
      node {
        eventId
        flowId
        sessionId
        functionName
        selfContainerId
        callerUserId
        callerKind
        automationId
        paramsJson
        mutationsAppliedJson
        permissionEffectsAppliedJson
        returnValueJson
        success
        errorMessage
        executedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
}

# Cross-engine flow timeline (diagnostics; requires manage_apps and a
# game-api with gameModelFlow, 2026-07-19+). Selections are inlined because
# each operations file is shipped as one self-contained document.
query GameModelFlow($appId: BigInt!, $flowId: String!) {
  gameModelFlow(appId: $appId, flowId: $flowId) {
    flowId
    events {
      eventId
      flowId
      sessionId
      functionName
      selfContainerId
      callerUserId
      callerKind
      automationId
      paramsJson
      mutationsAppliedJson
      permissionEffectsAppliedJson
      returnValueJson
      success
      errorMessage
      executedAt
    }
    automationRuns {
      runId
      appId
      flowId
      automationId
      automationName
      triggerSource
      parentRunId
      cascadeDepth
      startedAt
      finishedAt
      durationUs
      targets
      invocations
      mutations
      fnCalls
      gasUsed
      success
      errorMessage
      circuitAction
      computeUnits
    }
    moduleRuns {
      runId
      appId
      flowId
      moduleId
      moduleName
      triggerSource
      entry
      startedAt
      durationUs
      fuelUsed
      dbReads
      dbWrites
      egressMsgs
      egressBytes
      success
      errorMessage
      circuitAction
    }
  }
})gql";
inline constexpr std::string_view kGameModelCreateSessionOperationName = "GameModelCreateSession";
inline constexpr std::string_view kGameModelJoinSessionOperationName = "GameModelJoinSession";
inline constexpr std::string_view kGameModelSetSessionTurnOperationName = "GameModelSetSessionTurn";
inline constexpr std::string_view kGameModelCreateContainerOperationName = "GameModelCreateContainer";
inline constexpr std::string_view kGameModelDeleteContainerOperationName = "GameModelDeleteContainer";
inline constexpr std::string_view kGameModelSetPropertyOperationName = "GameModelSetProperty";
inline constexpr std::string_view kGameModelAddEdgeOperationName = "GameModelAddEdge";
inline constexpr std::string_view kGameModelDeleteEdgeOperationName = "GameModelDeleteEdge";
inline constexpr std::string_view kGameModelInvokeOperationName = "GameModelInvoke";
inline constexpr std::string_view kGameModelContainerOperationName = "GameModelContainer";
inline constexpr std::string_view kGameModelContainersOperationName = "GameModelContainers";
inline constexpr std::string_view kGameModelContainerStateOperationName = "GameModelContainerState";
inline constexpr std::string_view kGameModelTraverseOperationName = "GameModelTraverse";
inline constexpr std::string_view kGameModelSessionOperationName = "GameModelSession";
inline constexpr std::string_view kGameModelSessionsOperationName = "GameModelSessions";
inline constexpr std::string_view kGameModelEventsOperationName = "GameModelEvents";
inline constexpr std::string_view kGameModelEventsConnectionOperationName = "GameModelEventsConnection";
inline constexpr std::string_view kGameModelFlowOperationName = "GameModelFlow";

/// gameModel/GameModelStudio.graphql
inline constexpr std::string_view kGameModelStudioDocument = R"gql(fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
}

fragment GmPropertyDefFields on GmPropertyDef {
  appId
  containerTypeName
  key
  valueType
  defaultValueJson
  visibility
  writable
  description
}

mutation GameModelSeed($input: SeedGameModelInput!) {
  gameModelSeed(input: $input) {
    containerTypesCreated
    propertyDefinitionsCreated
    functionsCreated
    containersCreated
    edgesCreated
    warnings
    idMapJson
  }
}

mutation GameModelUpsertContainerType($input: UpsertContainerTypeInput!) {
  gameModelUpsertContainerType(input: $input) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
}

mutation GameModelUpsertPropertyDef($input: UpsertPropertyDefInput!) {
  gameModelUpsertPropertyDef(input: $input) {
    ...GmPropertyDefFields
  }
}

mutation GameModelDeletePropertyDef(
  $appId: BigInt!
  $containerTypeName: String!
  $key: String!
) {
  gameModelDeletePropertyDef(
    appId: $appId
    containerTypeName: $containerTypeName
    key: $key
  )
}

mutation GameModelDeleteContainerType($appId: BigInt!, $typeName: String!) {
  gameModelDeleteContainerType(appId: $appId, typeName: $typeName)
}

mutation GameModelUpsertFunction($input: UpsertFunctionInput!) {
  gameModelUpsertFunction(input: $input) {
    ...GmFunctionFields
  }
}

mutation GameModelDeleteFunction($appId: BigInt!, $name: String!) {
  gameModelDeleteFunction(appId: $appId, name: $name)
}

mutation GameModelDefineFeature($input: DefineAppFeatureInput!) {
  gameModelDefineFeature(input: $input) {
    appId
    featureKey
    description
  }
}

mutation GameModelGrantTierFeature($input: GrantTierFeatureInput!) {
  gameModelGrantTierFeature(input: $input) {
    appId
    tierId
    featureKey
  }
}

mutation GameModelSetPolicy($input: SetGameModelPolicyInput!) {
  gameModelSetPolicy(input: $input) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
}

query GameModelTypeSchema($appId: BigInt!, $typeName: String!) {
  gameModelTypeSchema(appId: $appId, typeName: $typeName) {
    typeName
    propertyDefinitions {
      ...GmPropertyDefFields
    }
    functions {
      ...GmFunctionFields
    }
  }
}

query GameModelContainerTypes($appId: BigInt!) {
  gameModelContainerTypes(appId: $appId) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
}

query GameModelPropertyDefs($appId: BigInt!, $typeName: String!) {
  gameModelPropertyDefs(appId: $appId, typeName: $typeName) {
    ...GmPropertyDefFields
  }
}

query GameModelFunction($appId: BigInt!, $name: String!) {
  gameModelFunction(appId: $appId, name: $name) {
    ...GmFunctionFields
  }
}

query GameModelFunctions($appId: BigInt!, $containerTypeName: String) {
  gameModelFunctions(appId: $appId, containerTypeName: $containerTypeName) {
    ...GmFunctionFields
  }
}

query GameModelFeatures($appId: BigInt!) {
  gameModelFeatures(appId: $appId) {
    appId
    featureKey
    description
  }
}

query GameModelTierFeatures($appId: BigInt!, $tierId: BigInt) {
  gameModelTierFeatures(appId: $appId, tierId: $tierId) {
    appId
    tierId
    featureKey
  }
}

query GameModelPolicy($appId: BigInt!) {
  gameModelPolicy(appId: $appId) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
}

mutation GameModelRevokeTierFeature($input: GrantTierFeatureInput!) {
  gameModelRevokeTierFeature(input: $input)
})gql";
inline constexpr std::string_view kGameModelSeedOperationName = "GameModelSeed";
inline constexpr std::string_view kGameModelUpsertContainerTypeOperationName = "GameModelUpsertContainerType";
inline constexpr std::string_view kGameModelUpsertPropertyDefOperationName = "GameModelUpsertPropertyDef";
inline constexpr std::string_view kGameModelDeletePropertyDefOperationName = "GameModelDeletePropertyDef";
inline constexpr std::string_view kGameModelDeleteContainerTypeOperationName = "GameModelDeleteContainerType";
inline constexpr std::string_view kGameModelUpsertFunctionOperationName = "GameModelUpsertFunction";
inline constexpr std::string_view kGameModelDeleteFunctionOperationName = "GameModelDeleteFunction";
inline constexpr std::string_view kGameModelDefineFeatureOperationName = "GameModelDefineFeature";
inline constexpr std::string_view kGameModelGrantTierFeatureOperationName = "GameModelGrantTierFeature";
inline constexpr std::string_view kGameModelSetPolicyOperationName = "GameModelSetPolicy";
inline constexpr std::string_view kGameModelTypeSchemaOperationName = "GameModelTypeSchema";
inline constexpr std::string_view kGameModelContainerTypesOperationName = "GameModelContainerTypes";
inline constexpr std::string_view kGameModelPropertyDefsOperationName = "GameModelPropertyDefs";
inline constexpr std::string_view kGameModelFunctionOperationName = "GameModelFunction";
inline constexpr std::string_view kGameModelFunctionsOperationName = "GameModelFunctions";
inline constexpr std::string_view kGameModelFeaturesOperationName = "GameModelFeatures";
inline constexpr std::string_view kGameModelTierFeaturesOperationName = "GameModelTierFeatures";
inline constexpr std::string_view kGameModelPolicyOperationName = "GameModelPolicy";
inline constexpr std::string_view kGameModelRevokeTierFeatureOperationName = "GameModelRevokeTierFeature";

}  // namespace gameModel

namespace host {

/// host/Host.graphql
inline constexpr std::string_view kHostDocument = R"gql(query GameHost($appId: BigInt!) {
  gameHost(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
}

query AmIGameHost($appId: BigInt!) {
  amIGameHost(appId: $appId)
}

mutation ActorHeartbeat($appId: BigInt!) {
  actorHeartbeat(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
})gql";
inline constexpr std::string_view kGameHostOperationName = "GameHost";
inline constexpr std::string_view kAmIGameHostOperationName = "AmIGameHost";
inline constexpr std::string_view kActorHeartbeatOperationName = "ActorHeartbeat";

}  // namespace host

namespace marketplace {

/// marketplace/Marketplace.graphql
inline constexpr std::string_view kMarketplaceDocument = R"gql(fragment PlayerCodeListingFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  priceCents
  rentIntervalDays
  windowDays
  unitBudget
  status
  createdAt
}

fragment PlayerCodeListingVersionFields on PlayerCodeListingVersion {
  versionId
  listingId
  versionNo
  serverArtifactHashes
  clientArtifactHashes
  capabilitySummaryJson
  capabilityHash
  openSource
  licenseText
  createdAt
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
}

fragment PlayerCodeInstallFields on PlayerCodeInstall {
  installId
  acquisitionId
  listingId
  appId
  pinnedVersionId
  targetGridId
  consentedCapabilityHash
  status
  createdAt
}

fragment GridClaimRequestFields on GridClaimRequest {
  requestId
  appId
  gridId
  requesterUserId
  status
  createdAt
}

# ---- Game API: browse / publish / acquire / install / consent -----------------

query MarketplaceListings($appId: BigInt!) {
  playerCodeListings(appId: $appId) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

query MarketplaceListingVersions($appId: BigInt!, $listingId: String!) {
  playerCodeListingVersions(appId: $appId, listingId: $listingId) {
    ...PlayerCodeListingVersionFields
  }
}

query MarketplaceMyAcquisitions($appId: BigInt!) {
  myPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionFields
  }
}

query MarketplaceMyInstalls($appId: BigInt!) {
  myPlayerCodeInstalls(appId: $appId) {
    ...PlayerCodeInstallFields
  }
}

query MarketplaceGridClientMods($appId: BigInt!, $gridId: BigInt!) {
  gridClientMods(appId: $appId, gridId: $gridId) {
    attachmentId
    listingId
    listingName
    versionId
    gridId
    capabilitySummaryJson
    capabilityHash
    callerConsented
  }
}

query MarketplaceClientArtifact(
  $appId: BigInt!
  $listingId: String!
  $versionId: String
) {
  playerCodeClientArtifact(
    appId: $appId
    listingId: $listingId
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
}

mutation MarketplacePublishListing($input: PublishPlayerCodeInput!) {
  publishPlayerCode(input: $input) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

mutation MarketplacePublishVersion($input: PublishPlayerCodeVersionInput!) {
  publishPlayerCodeVersion(input: $input) {
    ...PlayerCodeListingVersionFields
  }
}

mutation MarketplaceAcquire($appId: BigInt!, $listingId: String!) {
  acquirePlayerCode(appId: $appId, listingId: $listingId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceInstall(
  $appId: BigInt!
  $acquisitionId: String!
  $consentCapabilityHash: String!
  $gridId: BigInt
  $versionId: String
) {
  installPlayerCode(
    appId: $appId
    acquisitionId: $acquisitionId
    consentCapabilityHash: $consentCapabilityHash
    gridId: $gridId
    versionId: $versionId
  ) {
    ...PlayerCodeInstallFields
  }
}

mutation MarketplaceUninstall($appId: BigInt!, $installId: String!) {
  uninstallPlayerCode(appId: $appId, installId: $installId)
}

mutation MarketplaceConsentGridClientMod(
  $appId: BigInt!
  $attachmentId: String!
  $consentCapabilityHash: String!
) {
  consentGridClientMod(
    appId: $appId
    attachmentId: $attachmentId
    consentCapabilityHash: $consentCapabilityHash
  )
}

# ---- Game API: D4 grid claim flows --------------------------------------------

query MarketplaceGridClaimPolicy($appId: BigInt!) {
  gridClaimPolicy(appId: $appId)
}

query MarketplaceGridClaimRequests($appId: BigInt!) {
  gridClaimRequests(appId: $appId) {
    ...GridClaimRequestFields
  }
}

mutation MarketplaceClaimGridOwnership($appId: BigInt!, $gridId: BigInt!) {
  claimGridOwnership(appId: $appId, gridId: $gridId) {
    policy
    ownershipAssigned
    claimRequestId
  }
}

mutation MarketplaceDecideGridClaim(
  $appId: BigInt!
  $requestId: String!
  $approve: Boolean!
) {
  decideGridClaim(appId: $appId, requestId: $requestId, approve: $approve) {
    ...GridClaimRequestFields
  }
}

mutation MarketplaceIssueGridClaimInvite(
  $appId: BigInt!
  $gridId: BigInt!
  $inviteeUserId: BigInt!
) {
  issueGridClaimInvite(
    appId: $appId
    gridId: $gridId
    inviteeUserId: $inviteeUserId
  )
}

# ---- Management API: studio moderation / catalog administration ---------------

query MarketplaceAdmissionQueue($appId: BigInt!) {
  appCodeAdmissionQueue(appId: $appId) {
    listing {
      ...PlayerCodeListingFields
    }
    admissionState
    admissionId
    matchedSubjectKind
  }
}

query MarketplaceAppListings($appId: BigInt!, $includeDelisted: Boolean) {
  appPlayerCodeListings(appId: $appId, includeDelisted: $includeDelisted) {
    ...PlayerCodeListingFields
    updatedAt
  }
}

query MarketplaceAppAcquisitions($appId: BigInt!) {
  appPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionFields
    acquirerUserId
    revokedAt
  }
}

mutation MarketplaceTransferListing($input: TransferPlayerCodeListingInput!) {
  transferPlayerCodeListing(input: $input) {
    ...PlayerCodeListingFields
    updatedAt
  }
}

mutation MarketplaceSetListingStatus(
  $appId: BigInt!
  $listingId: String!
  $status: PlayerCodeListingStatus!
) {
  setPlayerCodeListingStatus(
    appId: $appId
    listingId: $listingId
    status: $status
  ) {
    ...PlayerCodeListingFields
    updatedAt
  }
}

mutation MarketplaceSetGridClaimPolicy(
  $appId: BigInt!
  $policy: GridClaimPolicy!
  $approverUserIds: [BigInt!]
) {
  setAppGridClaimPolicy(
    appId: $appId
    policy: $policy
    approverUserIds: $approverUserIds
  )
}

# ---- P4b: paid modes, grid commerce, seller payouts ---------------------------

mutation MarketplaceRenewAcquisition($appId: BigInt!, $acquisitionId: String!) {
  renewPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceTopUpAcquisition($appId: BigInt!, $acquisitionId: String!) {
  topUpPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceRefundAcquisition($appId: BigInt!, $acquisitionId: String!) {
  refundPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId)
}

query MarketplaceGridListings($appId: BigInt!) {
  gridListings(appId: $appId) {
    gridListingId
    appId
    kind
    name
    description
    priceCents
    conferredPermissionKeys
    resalePolicy
  }
}

mutation MarketplacePurchaseGrid(
  $appId: BigInt!
  $gridListingId: String!
  $chunkX: Int
  $chunkY: Int
  $chunkZ: Int
) {
  purchaseGrid(
    appId: $appId
    gridListingId: $gridListingId
    chunkX: $chunkX
    chunkY: $chunkY
    chunkZ: $chunkZ
  ) {
    gridId
    ownershipAssigned
  }
}

# ---- P4b management: pricing, seller onboarding/payouts, grid listing CRUD -----

mutation MarketplaceSetListingPricing($input: SetListingPricingInput!) {
  setListingPricing(input: $input)
}

mutation MarketplaceSetOrgShare($appId: BigInt!, $bps: Int!) {
  setAppMarketplaceOrgShare(appId: $appId, bps: $bps)
}

mutation MarketplaceBeginSellerOnboarding($country: String!) {
  beginSellerOnboarding(country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
}

mutation MarketplaceCreateAccountSession($country: String!) {
  createSellerAccountSession(country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
}

mutation MarketplaceCreateOrgAccountSession($orgId: BigInt!, $country: String!) {
  createOrgSellerAccountSession(orgId: $orgId, country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
}

mutation MarketplaceBeginOrgSellerOnboarding($orgId: BigInt!, $country: String!) {
  beginOrgSellerOnboarding(orgId: $orgId, country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
}

query MarketplaceMySellerBalance {
  mySellerPayoutBalance {
    partyKind
    partyRef
    pendingCents
    payableCents
    reservedCents
    onboardingStatus
    payoutsFrozen
  }
}

mutation MarketplaceRequestPayout {
  requestSellerPayout
}

mutation MarketplaceSpendPayoutToWallet($amountCents: Int!) {
  spendPayoutBalanceToWallet(amountCents: $amountCents)
}

query MarketplaceCommerceRiskQueue($appId: BigInt!) {
  commerceRiskQueue(appId: $appId) {
    flagId
    appId
    kind
    orderId
    subjectKind
    subjectRef
    detail
    status
    createdAt
  }
}

mutation MarketplaceDecideRiskFlag(
  $appId: BigInt!
  $flagId: String!
  $release: Boolean!
) {
  decideCommerceRiskFlag(appId: $appId, flagId: $flagId, release: $release)
}

mutation MarketplaceCreateGridListing($input: CreateGridListingInput!) {
  createGridListing(input: $input) {
    gridListingId
    appId
    kind
    name
    priceCents
    resalePolicy
    status
  }
})gql";
inline constexpr std::string_view kMarketplaceListingsOperationName = "MarketplaceListings";
inline constexpr std::string_view kMarketplaceListingVersionsOperationName = "MarketplaceListingVersions";
inline constexpr std::string_view kMarketplaceMyAcquisitionsOperationName = "MarketplaceMyAcquisitions";
inline constexpr std::string_view kMarketplaceMyInstallsOperationName = "MarketplaceMyInstalls";
inline constexpr std::string_view kMarketplaceGridClientModsOperationName = "MarketplaceGridClientMods";
inline constexpr std::string_view kMarketplaceClientArtifactOperationName = "MarketplaceClientArtifact";
inline constexpr std::string_view kMarketplacePublishListingOperationName = "MarketplacePublishListing";
inline constexpr std::string_view kMarketplacePublishVersionOperationName = "MarketplacePublishVersion";
inline constexpr std::string_view kMarketplaceAcquireOperationName = "MarketplaceAcquire";
inline constexpr std::string_view kMarketplaceInstallOperationName = "MarketplaceInstall";
inline constexpr std::string_view kMarketplaceUninstallOperationName = "MarketplaceUninstall";
inline constexpr std::string_view kMarketplaceConsentGridClientModOperationName = "MarketplaceConsentGridClientMod";
inline constexpr std::string_view kMarketplaceGridClaimPolicyOperationName = "MarketplaceGridClaimPolicy";
inline constexpr std::string_view kMarketplaceGridClaimRequestsOperationName = "MarketplaceGridClaimRequests";
inline constexpr std::string_view kMarketplaceClaimGridOwnershipOperationName = "MarketplaceClaimGridOwnership";
inline constexpr std::string_view kMarketplaceDecideGridClaimOperationName = "MarketplaceDecideGridClaim";
inline constexpr std::string_view kMarketplaceIssueGridClaimInviteOperationName = "MarketplaceIssueGridClaimInvite";
inline constexpr std::string_view kMarketplaceAdmissionQueueOperationName = "MarketplaceAdmissionQueue";
inline constexpr std::string_view kMarketplaceAppListingsOperationName = "MarketplaceAppListings";
inline constexpr std::string_view kMarketplaceAppAcquisitionsOperationName = "MarketplaceAppAcquisitions";
inline constexpr std::string_view kMarketplaceTransferListingOperationName = "MarketplaceTransferListing";
inline constexpr std::string_view kMarketplaceSetListingStatusOperationName = "MarketplaceSetListingStatus";
inline constexpr std::string_view kMarketplaceSetGridClaimPolicyOperationName = "MarketplaceSetGridClaimPolicy";
inline constexpr std::string_view kMarketplaceRenewAcquisitionOperationName = "MarketplaceRenewAcquisition";
inline constexpr std::string_view kMarketplaceTopUpAcquisitionOperationName = "MarketplaceTopUpAcquisition";
inline constexpr std::string_view kMarketplaceRefundAcquisitionOperationName = "MarketplaceRefundAcquisition";
inline constexpr std::string_view kMarketplaceGridListingsOperationName = "MarketplaceGridListings";
inline constexpr std::string_view kMarketplacePurchaseGridOperationName = "MarketplacePurchaseGrid";
inline constexpr std::string_view kMarketplaceSetListingPricingOperationName = "MarketplaceSetListingPricing";
inline constexpr std::string_view kMarketplaceSetOrgShareOperationName = "MarketplaceSetOrgShare";
inline constexpr std::string_view kMarketplaceBeginSellerOnboardingOperationName = "MarketplaceBeginSellerOnboarding";
inline constexpr std::string_view kMarketplaceCreateAccountSessionOperationName = "MarketplaceCreateAccountSession";
inline constexpr std::string_view kMarketplaceCreateOrgAccountSessionOperationName = "MarketplaceCreateOrgAccountSession";
inline constexpr std::string_view kMarketplaceBeginOrgSellerOnboardingOperationName = "MarketplaceBeginOrgSellerOnboarding";
inline constexpr std::string_view kMarketplaceMySellerBalanceOperationName = "MarketplaceMySellerBalance";
inline constexpr std::string_view kMarketplaceRequestPayoutOperationName = "MarketplaceRequestPayout";
inline constexpr std::string_view kMarketplaceSpendPayoutToWalletOperationName = "MarketplaceSpendPayoutToWallet";
inline constexpr std::string_view kMarketplaceCommerceRiskQueueOperationName = "MarketplaceCommerceRiskQueue";
inline constexpr std::string_view kMarketplaceDecideRiskFlagOperationName = "MarketplaceDecideRiskFlag";
inline constexpr std::string_view kMarketplaceCreateGridListingOperationName = "MarketplaceCreateGridListing";

}  // namespace marketplace

namespace organizations {

/// organizations/CreateOrgRole.graphql
inline constexpr std::string_view kCreateOrgRoleDocument = R"gql(mutation CreateOrgRole($input: CreateOrgRoleInput!) {
  createOrgRole(input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kCreateOrgRoleOperationName = "CreateOrgRole";

/// organizations/CreateOrgToken.graphql
inline constexpr std::string_view kCreateOrgTokenDocument = R"gql(mutation CreateOrgToken($input: CreateOrgTokenInput!) {
  createOrgToken(input: $input) {
    orgTokenId
    orgId
    token
    label
    isActive
    expiresAt
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateOrgTokenOperationName = "CreateOrgToken";

/// organizations/CreateOrganization.graphql
inline constexpr std::string_view kCreateOrganizationDocument = R"gql(mutation CreateOrganization($input: CreateOrganizationInput!) {
  createOrganization(input: $input) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateOrganizationOperationName = "CreateOrganization";

/// organizations/DeleteOrgRole.graphql
inline constexpr std::string_view kDeleteOrgRoleDocument = R"gql(mutation DeleteOrgRole($orgRoleId: BigInt!) {
  deleteOrgRole(orgRoleId: $orgRoleId)
})gql";
inline constexpr std::string_view kDeleteOrgRoleOperationName = "DeleteOrgRole";

/// organizations/InviteOrgMember.graphql
inline constexpr std::string_view kInviteOrgMemberDocument = R"gql(mutation InviteOrgMember($input: InviteOrgMemberInput!) {
  inviteOrgMember(input: $input) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kInviteOrgMemberOperationName = "InviteOrgMember";

/// organizations/MemberRoles.graphql
inline constexpr std::string_view kMemberRolesDocument = R"gql(query MemberRoles($orgMemberId: BigInt!) {
  memberRoles(orgMemberId: $orgMemberId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kMemberRolesOperationName = "MemberRoles";

/// organizations/MyOrganizations.graphql
inline constexpr std::string_view kMyOrganizationsDocument = R"gql(query MyOrganizations {
  myOrganizations {
    org {
      orgId
      slug
      name
      ownerUserId
      status
      createdAt
      updatedAt
    }
    permissions
    roles {
      orgRoleId
      orgId
      roleName
      isSystem
      permissions
    }
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyOrganizationsOperationName = "MyOrganizations";

/// organizations/OrgMembers.graphql
inline constexpr std::string_view kOrgMembersDocument = R"gql(query OrgMembers($orgId: BigInt!) {
  orgMembers(orgId: $orgId) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgMembersOperationName = "OrgMembers";

/// organizations/OrgPermissions.graphql
inline constexpr std::string_view kOrgPermissionsDocument = R"gql(query OrgPermissions {
  orgPermissions {
    permissionKey
    description
    category
  }
})gql";
inline constexpr std::string_view kOrgPermissionsOperationName = "OrgPermissions";

/// organizations/OrgRoles.graphql
inline constexpr std::string_view kOrgRolesDocument = R"gql(query OrgRoles($orgId: BigInt!) {
  orgRoles(orgId: $orgId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kOrgRolesOperationName = "OrgRoles";

/// organizations/OrgTokens.graphql
inline constexpr std::string_view kOrgTokensDocument = R"gql(query OrgTokens($orgId: BigInt!) {
  orgTokens(orgId: $orgId) {
    orgTokenId
    orgId
    label
    isActive
    lastUsedAt
    revokedAt
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgTokensOperationName = "OrgTokens";

/// organizations/Organization.graphql
inline constexpr std::string_view kOrganizationDocument = R"gql(query Organization($id: BigInt!) {
  organization(id: $id) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationOperationName = "Organization";

/// organizations/OrganizationBySlug.graphql
inline constexpr std::string_view kOrganizationBySlugDocument = R"gql(query OrganizationBySlug($slug: String!) {
  organizationBySlug(slug: $slug) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationBySlugOperationName = "OrganizationBySlug";

/// organizations/RemoveOrgMember.graphql
inline constexpr std::string_view kRemoveOrgMemberDocument = R"gql(mutation RemoveOrgMember($orgId: BigInt!, $userId: BigInt!) {
  removeOrgMember(orgId: $orgId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveOrgMemberOperationName = "RemoveOrgMember";

/// organizations/RevokeOrgToken.graphql
inline constexpr std::string_view kRevokeOrgTokenDocument = R"gql(mutation RevokeOrgToken($orgTokenId: BigInt!) {
  revokeOrgToken(orgTokenId: $orgTokenId)
})gql";
inline constexpr std::string_view kRevokeOrgTokenOperationName = "RevokeOrgToken";

/// organizations/SetOrgStatus.graphql
inline constexpr std::string_view kSetOrgStatusDocument = R"gql(mutation SetOrgStatus($orgId: BigInt!, $status: String!) {
  setOrgStatus(orgId: $orgId, status: $status) {
    orgId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetOrgStatusOperationName = "SetOrgStatus";

/// organizations/UpdateOrgMemberRoles.graphql
inline constexpr std::string_view kUpdateOrgMemberRolesDocument = R"gql(mutation UpdateOrgMemberRoles(
  $orgId: BigInt!
  $userId: BigInt!
  $roleIds: [BigInt!]!
) {
  updateOrgMemberRoles(orgId: $orgId, userId: $userId, roleIds: $roleIds) {
    orgMemberId
    orgId
    userId
    status
  }
})gql";
inline constexpr std::string_view kUpdateOrgMemberRolesOperationName = "UpdateOrgMemberRoles";

/// organizations/UpdateOrgRole.graphql
inline constexpr std::string_view kUpdateOrgRoleDocument = R"gql(mutation UpdateOrgRole($orgRoleId: BigInt!, $input: UpdateOrgRoleInput!) {
  updateOrgRole(orgRoleId: $orgRoleId, input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kUpdateOrgRoleOperationName = "UpdateOrgRole";

/// organizations/UpdateOrgToken.graphql
inline constexpr std::string_view kUpdateOrgTokenDocument = R"gql(mutation UpdateOrgToken($orgTokenId: BigInt!, $input: UpdateOrgTokenInput!) {
  updateOrgToken(orgTokenId: $orgTokenId, input: $input) {
    orgTokenId
    label
    isActive
    expiresAt
    revokedAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateOrgTokenOperationName = "UpdateOrgToken";

}  // namespace organizations

namespace payments {

/// payments/CapturePaypalCheckout.graphql
inline constexpr std::string_view kCapturePaypalCheckoutDocument = R"gql(mutation CapturePaypalCheckout($orderId: String!, $idempotencyKey: String) {
  capturePaypalCheckout(orderId: $orderId, idempotencyKey: $idempotencyKey) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCapturePaypalCheckoutOperationName = "CapturePaypalCheckout";

/// payments/Checkouts.graphql
inline constexpr std::string_view kCheckoutsDocument = R"gql(query Checkouts($filter: CheckoutFilterInput, $limit: Int, $offset: Int) {
  checkouts(filter: $filter, limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query CheckoutsConnection(
  $first: Int
  $after: String
  $filter: CheckoutFilterInput
) {
  checkoutsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kCheckoutsOperationName = "Checkouts";
inline constexpr std::string_view kCheckoutsConnectionOperationName = "CheckoutsConnection";

/// payments/CreateCheckout.graphql
inline constexpr std::string_view kCreateCheckoutDocument = R"gql(mutation CreateCheckout($input: CreateCheckoutInput!) {
  createCheckout(input: $input) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCreateCheckoutOperationName = "CreateCheckout";

/// payments/MyCheckouts.graphql
inline constexpr std::string_view kMyCheckoutsDocument = R"gql(query MyCheckouts($limit: Int, $offset: Int) {
  myCheckouts(limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      error
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query MyCheckoutsConnection($first: Int, $after: String) {
  myCheckoutsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kMyCheckoutsOperationName = "MyCheckouts";
inline constexpr std::string_view kMyCheckoutsConnectionOperationName = "MyCheckoutsConnection";

/// payments/PaymentEvents.graphql
inline constexpr std::string_view kPaymentEventsDocument = R"gql(query PaymentEvents($limit: Int, $offset: Int) {
  paymentEvents(limit: $limit, offset: $offset) {
    items {
      eventId
      provider
      externalEventId
      eventType
      checkoutId
      processedAt
      error
      createdAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query PaymentEventsConnection($first: Int, $after: String) {
  paymentEventsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        eventId
        provider
        externalEventId
        eventType
        checkoutId
        processedAt
        error
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kPaymentEventsOperationName = "PaymentEvents";
inline constexpr std::string_view kPaymentEventsConnectionOperationName = "PaymentEventsConnection";

}  // namespace payments

namespace platform {

/// platform/PlatformConfig.graphql
inline constexpr std::string_view kPlatformConfigDocument = R"gql(query PlatformConfig {
  platformConfig {
    sharedGameApiUrl
    sharedGameApiWsUrl
    freeAppsPerOrg
  }
})gql";
inline constexpr std::string_view kPlatformConfigOperationName = "PlatformConfig";

}  // namespace platform

namespace playerCompute {

/// playerCompute/PlayerCompute.graphql
inline constexpr std::string_view kPlayerComputeDocument = R"gql(fragment PlayerWasmModuleFields on PlayerWasmModule {
  moduleId
  appId
  gridId
  name
  description
  authorUserId
  authorOrgId
  enabled
  draft
  currentVersionId
  circuitState
  lastError
  createdAt
  updatedAt
}

fragment PlayerWasmModuleVersionFields on PlayerWasmModuleVersion {
  versionId
  moduleId
  versionNo
  target
  sourceFilesJson
  openSource
  compileStatus
  compileLog
  compiledSizeBytes
  createdAt
}

mutation PlayerComputeDeploy($input: DeployPlayerComputeInput!) {
  playerComputeDeploy(input: $input) {
    ...PlayerWasmModuleVersionFields
  }
}

mutation PlayerComputeSetEnabled(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $enabled: Boolean!
) {
  playerComputeSetEnabled(
    appId: $appId
    gridId: $gridId
    name: $name
    enabled: $enabled
  ) {
    ...PlayerWasmModuleFields
  }
}

query PlayerComputeMyModules($appId: BigInt!) {
  playerComputeMyModules(appId: $appId) {
    ...PlayerWasmModuleFields
  }
}

query PlayerComputeVersions(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
) {
  playerComputeVersions(appId: $appId, gridId: $gridId, name: $name) {
    ...PlayerWasmModuleVersionFields
  }
}

mutation PlayerComputeDelete(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
) {
  playerComputeDelete(appId: $appId, gridId: $gridId, name: $name)
}

mutation PlayerComputeInvoke(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String!
  $exportName: String!
  $paramsJson: String
) {
  playerComputeInvoke(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
}

fragment PlayerWasmModuleRunFields on PlayerWasmModuleRun {
  runId
  appId
  gridId
  moduleId
  moduleName
  executedAsUserId
  flowId
  triggerSource
  startedAt
  durationUs
  fuelUsed
  success
  errorMessage
}

query PlayerComputeUsage($appId: BigInt!) {
  playerComputeUsage(appId: $appId) {
    appId
    hourUnitsUsed
    dayUnitsUsed
    unitsPerHour
    unitsPerDay
    compilesThisHour
    maxCompilesPerHour
    gateStatus
    gateReason
  }
}

query PlayerComputeRuns(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  playerComputeRuns(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...PlayerWasmModuleRunFields
  }
}

query PlayerComputeLogs(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String
  $limit: Int
) {
  playerComputeLogs(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    limit: $limit
  ) {
    ...PlayerWasmModuleRunFields
  }
}

mutation PlayerComputeSetSwitch(
  $appId: BigInt!
  $scope: String!
  $disabled: Boolean!
  $scopeRef: BigInt
  $reason: String
) {
  playerComputeSetSwitch(
    appId: $appId
    scope: $scope
    disabled: $disabled
    scopeRef: $scopeRef
    reason: $reason
  )
}

query PlayerComputeSwitches($appId: BigInt!) {
  playerComputeSwitches(appId: $appId) {
    switchId
    appId
    scope
    scopeRef
    reason
    disabledAt
  }
}

query PlayerComputeArtifact(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $versionId: String
) {
  playerComputeArtifact(
    appId: $appId
    gridId: $gridId
    name: $name
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
})gql";
inline constexpr std::string_view kPlayerComputeDeployOperationName = "PlayerComputeDeploy";
inline constexpr std::string_view kPlayerComputeSetEnabledOperationName = "PlayerComputeSetEnabled";
inline constexpr std::string_view kPlayerComputeMyModulesOperationName = "PlayerComputeMyModules";
inline constexpr std::string_view kPlayerComputeVersionsOperationName = "PlayerComputeVersions";
inline constexpr std::string_view kPlayerComputeDeleteOperationName = "PlayerComputeDelete";
inline constexpr std::string_view kPlayerComputeInvokeOperationName = "PlayerComputeInvoke";
inline constexpr std::string_view kPlayerComputeUsageOperationName = "PlayerComputeUsage";
inline constexpr std::string_view kPlayerComputeRunsOperationName = "PlayerComputeRuns";
inline constexpr std::string_view kPlayerComputeLogsOperationName = "PlayerComputeLogs";
inline constexpr std::string_view kPlayerComputeSetSwitchOperationName = "PlayerComputeSetSwitch";
inline constexpr std::string_view kPlayerComputeSwitchesOperationName = "PlayerComputeSwitches";
inline constexpr std::string_view kPlayerComputeArtifactOperationName = "PlayerComputeArtifact";

}  // namespace playerCompute

namespace playerModel {

/// playerModel/PlayerModel.graphql
inline constexpr std::string_view kPlayerModelDocument = R"gql(query PlayerModelContainers($appId: BigInt!, $gridId: BigInt!) {
  playerModelContainers(appId: $appId, gridId: $gridId) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

query PlayerModelContainer($input: PlayerModelContainerRefInput!) {
  playerModelContainer(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelCreateContainer($input: CreatePlayerModelContainerInput!) {
  playerModelCreateContainer(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelSetProperty($input: SetPlayerModelPropertyInput!) {
  playerModelSetProperty(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelDeleteContainer($input: PlayerModelContainerRefInput!) {
  playerModelDeleteContainer(input: $input)
}

query PlayerAutomations($appId: BigInt!, $gridId: BigInt!) {
  playerAutomations(appId: $appId, gridId: $gridId) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationCreate($input: CreatePlayerAutomationInput!) {
  playerAutomationCreate(input: $input) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationSetEnabled($input: SetPlayerAutomationEnabledInput!) {
  playerAutomationSetEnabled(input: $input) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationDelete($input: PlayerAutomationRefInput!) {
  playerAutomationDelete(input: $input)
})gql";
inline constexpr std::string_view kPlayerModelContainersOperationName = "PlayerModelContainers";
inline constexpr std::string_view kPlayerModelContainerOperationName = "PlayerModelContainer";
inline constexpr std::string_view kPlayerModelCreateContainerOperationName = "PlayerModelCreateContainer";
inline constexpr std::string_view kPlayerModelSetPropertyOperationName = "PlayerModelSetProperty";
inline constexpr std::string_view kPlayerModelDeleteContainerOperationName = "PlayerModelDeleteContainer";
inline constexpr std::string_view kPlayerAutomationsOperationName = "PlayerAutomations";
inline constexpr std::string_view kPlayerAutomationCreateOperationName = "PlayerAutomationCreate";
inline constexpr std::string_view kPlayerAutomationSetEnabledOperationName = "PlayerAutomationSetEnabled";
inline constexpr std::string_view kPlayerAutomationDeleteOperationName = "PlayerAutomationDelete";

}  // namespace playerModel

namespace playerWallet {

/// playerWallet/PlayerWallet.graphql
inline constexpr std::string_view kPlayerWalletDocument = R"gql(fragment PlayerWalletFields on PlayerWallet {
  walletId
  userId
  balanceCents
  currency
  createdAt
}

fragment PlayerWalletTransactionFields on PlayerWalletTransaction {
  transactionId
  walletId
  userId
  amountCents
  balanceAfter
  transactionType
  description
  referenceId
  appId
  createdAt
}

fragment PlayerSpendCapFields on PlayerSpendCap {
  userId
  scope
  scopeRef
  dailyLimitCents
  monthlyLimitCents
  currentDayUsageCents
  currentMonthUsageCents
}

fragment PlayerAutoBillingFields on PlayerAutoBilling {
  userId
  enabled
  limitCents
  autoBilledThisPeriodCents
  rechargeAmountCents
  lowWaterThresholdCents
  hasPaymentMethod
  lastError
}

fragment PlayerUsageChargeFields on PlayerUsageCharge {
  chargeId
  userId
  appId
  periodStart
  periodEnd
  amountCents
  platformCents
  markupCents
  currency
  usageSnapshotJson
  createdAt
}

fragment PlayerWasmPolicyFields on PlayerWasmPolicy {
  policyId
  appId
  scope
  scopeRef
  enabled
  maxModulesPerGrid
  maxModulesTotal
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  unitsPerHour
  unitsPerDay
  maxCompilesPerHour
  maxContainerCreatesDay
  clientFuelPerDispatch
}

query PlayerWalletBalance {
  playerWalletBalance {
    ...PlayerWalletFields
  }
}

query PlayerWalletTransactions($limit: Int, $offset: Int) {
  playerWalletTransactions(limit: $limit, offset: $offset) {
    ...PlayerWalletTransactionFields
  }
}

query PlayerUsageCharges($appId: BigInt, $limit: Int) {
  playerUsageCharges(appId: $appId, limit: $limit) {
    ...PlayerUsageChargeFields
  }
}

query PlayerSpendCaps {
  playerSpendCaps {
    ...PlayerSpendCapFields
  }
}

mutation SetPlayerSpendCap(
  $scope: String!
  $appId: BigInt
  $dailyLimitCents: BigInt
  $monthlyLimitCents: BigInt
) {
  setPlayerSpendCap(
    scope: $scope
    appId: $appId
    dailyLimitCents: $dailyLimitCents
    monthlyLimitCents: $monthlyLimitCents
  ) {
    ...PlayerSpendCapFields
  }
}

query PlayerAutoBilling {
  playerAutoBilling {
    ...PlayerAutoBillingFields
  }
}

mutation BeginPlayerCardSetup {
  beginPlayerCardSetup {
    clientSecret
    publishableKey
    externalCustomerId
  }
}

mutation SetPlayerAutoBilling(
  $enabled: Boolean!
  $limitCents: BigInt
  $rechargeAmountCents: BigInt
  $lowWaterThresholdCents: BigInt
) {
  setPlayerAutoBilling(
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
  ) {
    ...PlayerAutoBillingFields
  }
}

query PlayerRuntimeStates {
  playerRuntimeStates {
    userId
    appId
    status
    reason
    updatedAt
  }
}

query PlayerWasmPolicies($appId: BigInt!) {
  playerWasmPolicies(appId: $appId) {
    ...PlayerWasmPolicyFields
  }
}

mutation SetPlayerWasmPolicy($input: SetPlayerWasmPolicyInput!) {
  setPlayerWasmPolicy(input: $input) {
    ...PlayerWasmPolicyFields
  }
}

mutation DeletePlayerWasmPolicy(
  $appId: BigInt!
  $scope: String!
  $scopeRef: BigInt
) {
  deletePlayerWasmPolicy(appId: $appId, scope: $scope, scopeRef: $scopeRef)
}

query PlayerRateMarkup($appId: BigInt!) {
  playerRateMarkup(appId: $appId)
}

mutation SetPlayerRateMarkup($appId: BigInt!, $markupBps: Int!) {
  setPlayerRateMarkup(appId: $appId, markupBps: $markupBps)
}

query AppPlayerUsage($appId: BigInt!, $hours: Int) {
  appPlayerUsage(appId: $appId, hours: $hours) {
    userId
    computeUnits
    automationUnits
    compileCount
    chargedCents
  }
}

query AppPlayerMarkupAccrued($appId: BigInt!) {
  appPlayerMarkupAccrued(appId: $appId)
})gql";
inline constexpr std::string_view kPlayerWalletBalanceOperationName = "PlayerWalletBalance";
inline constexpr std::string_view kPlayerWalletTransactionsOperationName = "PlayerWalletTransactions";
inline constexpr std::string_view kPlayerUsageChargesOperationName = "PlayerUsageCharges";
inline constexpr std::string_view kPlayerSpendCapsOperationName = "PlayerSpendCaps";
inline constexpr std::string_view kSetPlayerSpendCapOperationName = "SetPlayerSpendCap";
inline constexpr std::string_view kPlayerAutoBillingOperationName = "PlayerAutoBilling";
inline constexpr std::string_view kBeginPlayerCardSetupOperationName = "BeginPlayerCardSetup";
inline constexpr std::string_view kSetPlayerAutoBillingOperationName = "SetPlayerAutoBilling";
inline constexpr std::string_view kPlayerRuntimeStatesOperationName = "PlayerRuntimeStates";
inline constexpr std::string_view kPlayerWasmPoliciesOperationName = "PlayerWasmPolicies";
inline constexpr std::string_view kSetPlayerWasmPolicyOperationName = "SetPlayerWasmPolicy";
inline constexpr std::string_view kDeletePlayerWasmPolicyOperationName = "DeletePlayerWasmPolicy";
inline constexpr std::string_view kPlayerRateMarkupOperationName = "PlayerRateMarkup";
inline constexpr std::string_view kSetPlayerRateMarkupOperationName = "SetPlayerRateMarkup";
inline constexpr std::string_view kAppPlayerUsageOperationName = "AppPlayerUsage";
inline constexpr std::string_view kAppPlayerMarkupAccruedOperationName = "AppPlayerMarkupAccrued";

}  // namespace playerWallet

namespace quotas {

/// quotas/DeleteQuota.graphql
inline constexpr std::string_view kDeleteQuotaDocument = R"gql(mutation DeleteQuota($quotaId: BigInt!) {
  deleteQuota(quotaId: $quotaId)
})gql";
inline constexpr std::string_view kDeleteQuotaOperationName = "DeleteQuota";

/// quotas/EffectiveQuota.graphql
inline constexpr std::string_view kEffectiveQuotaDocument = R"gql(query EffectiveQuota(
  $metric: String!
  $orgId: BigInt
  $appId: BigInt
  $tierId: BigInt
) {
  effectiveQuota(
    metric: $metric
    orgId: $orgId
    appId: $appId
    tierId: $tierId
  ) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
  }
})gql";
inline constexpr std::string_view kEffectiveQuotaOperationName = "EffectiveQuota";

/// quotas/QuotasForApp.graphql
inline constexpr std::string_view kQuotasForAppDocument = R"gql(query QuotasForApp($appId: BigInt!) {
  quotasForApp(appId: $appId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForAppOperationName = "QuotasForApp";

/// quotas/QuotasForOrg.graphql
inline constexpr std::string_view kQuotasForOrgDocument = R"gql(query QuotasForOrg($orgId: BigInt!) {
  quotasForOrg(orgId: $orgId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForOrgOperationName = "QuotasForOrg";

/// quotas/SetQuota.graphql
inline constexpr std::string_view kSetQuotaDocument = R"gql(mutation SetQuota($input: SetQuotaInput!) {
  setQuota(input: $input) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetQuotaOperationName = "SetQuota";

}  // namespace quotas

namespace serverStatus {

/// serverStatus/ActiveGraphQLServers.graphql
inline constexpr std::string_view kActiveGraphQLServersDocument = R"gql(query ActiveGraphQLServers {
  activeGraphQLServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kActiveGraphQLServersOperationName = "ActiveGraphQLServers";

/// serverStatus/GameClientBootstrap.graphql
inline constexpr std::string_view kGameClientBootstrapDocument = R"gql(query GameClientBootstrap($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) {
    appId
    realtimeProtocol
    subscriptionName
    maxReplicationDistance
    maxDecayRate
    sequenceNumberModulo
    udpProxyConnectionStatus {
      connected
      serverIp6
      serverClientPort
      lastMessageTime
    }
    versionInfo {
      serverVersion {
        major
        minor
        patch
        build
      }
      minimumClientVersion {
        major
        minor
        patch
        build
      }
    }
    me {
      userId
      email
      gamertag
      disambiguation
      state
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
  }
})gql";
inline constexpr std::string_view kGameClientBootstrapOperationName = "GameClientBootstrap";

/// serverStatus/GraphqlServers.graphql
inline constexpr std::string_view kGraphqlServersDocument = R"gql(query GraphqlServers {
  graphqlServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGraphqlServersOperationName = "GraphqlServers";

/// serverStatus/ServerWithLeastClients.graphql
inline constexpr std::string_view kServerWithLeastClientsDocument = R"gql(query ServerWithLeastClients {
  serverWithLeastClients {
    serverId
    ip4
    ip6
    clientPort
    status
    peers
    clients
    cpuPeakPct
    updatedAt
    createdAt
  }
})gql";
inline constexpr std::string_view kServerWithLeastClientsOperationName = "ServerWithLeastClients";

/// serverStatus/VersionInfo.graphql
inline constexpr std::string_view kVersionInfoDocument = R"gql(query VersionInfo {
  versionInfo {
    serverVersion {
      major
      minor
      patch
      build
    }
    minimumClientVersion {
      major
      minor
      patch
      build
    }
  }
})gql";
inline constexpr std::string_view kVersionInfoOperationName = "VersionInfo";

}  // namespace serverStatus

namespace sharedEnvironment {

/// sharedEnvironment/SharedEnvironment.graphql
inline constexpr std::string_view kSharedEnvironmentDocument = R"gql(query SharedEnvPlans {
  sharedEnvPlans {
    planId
    code
    name
    description
    priceCents
    currency
    billingInterval
    status
  }
}

query OrgFreeAppQuota($orgId: BigInt!) {
  orgFreeAppQuota(orgId: $orgId) {
    orgId
    quota
    usedFree
    paidApps
    remainingFree
  }
}

query AppSharedSubscription($appId: BigInt!) {
  appSharedSubscription(appId: $appId) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
}

query AppRuntimeState($appId: BigInt!) {
  appRuntimeState(appId: $appId) {
    appId
    deploymentTarget
    runtimeStatus
    runtimeDenialReason
    walletBalanceCents
    currentHourUsageCents
    currentDayUsageCents
    hourlyLimitCents
    dailyLimitCents
  }
}

query OrgAutoBilling($orgId: BigInt!) {
  orgAutoBilling(orgId: $orgId) {
    orgId
    enabled
    limitCents
    period
    autoBilledThisPeriodCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
    lastError
  }
}

query OrgPaymentMethods($orgId: BigInt!) {
  orgPaymentMethods(orgId: $orgId) {
    paymentMethodId
    provider
    brand
    last4
    isDefault
    status
  }
}

mutation PublishAppToShared(
  $appId: BigInt!
  $planId: BigInt
  $provider: PaymentProvider
  $successUrl: String
  $cancelUrl: String
  $idempotencyKey: String
) {
  publishAppToShared(
    appId: $appId
    planId: $planId
    provider: $provider
    successUrl: $successUrl
    cancelUrl: $cancelUrl
    idempotencyKey: $idempotencyKey
  ) {
    appId
    free
    checkout {
      checkoutId
      provider
      status
      amountCents
      currency
      externalUrl
    }
  }
}

mutation CancelSharedSubscription($appId: BigInt!, $idempotencyKey: String) {
  cancelSharedSubscription(appId: $appId, idempotencyKey: $idempotencyKey) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
}

mutation SetAppSpendCaps(
  $appId: BigInt!
  $hourlyLimitCents: BigInt
  $dailyLimitCents: BigInt
) {
  setAppSpendCaps(
    appId: $appId
    hourlyLimitCents: $hourlyLimitCents
    dailyLimitCents: $dailyLimitCents
  ) {
    appId
    runtimeStatus
    runtimeDenialReason
    hourlyLimitCents
    dailyLimitCents
  }
}

mutation SetAutoBilling(
  $orgId: BigInt!
  $enabled: Boolean!
  $limitCents: BigInt
  $rechargeAmountCents: BigInt
  $lowWaterThresholdCents: BigInt
  $idempotencyKey: String
) {
  setAutoBilling(
    orgId: $orgId
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
    idempotencyKey: $idempotencyKey
  ) {
    orgId
    enabled
    limitCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
  }
}

mutation SetupSharedPaymentMethod($orgId: BigInt!, $idempotencyKey: String) {
  setupSharedPaymentMethod(orgId: $orgId, idempotencyKey: $idempotencyKey) {
    externalCustomerId
    clientSecret
    publishableKey
  }
}

mutation RemoveSharedPaymentMethod(
  $orgId: BigInt!
  $paymentMethodId: BigInt!
  $idempotencyKey: String
) {
  removeSharedPaymentMethod(
    orgId: $orgId
    paymentMethodId: $paymentMethodId
    idempotencyKey: $idempotencyKey
  )
})gql";
inline constexpr std::string_view kSharedEnvPlansOperationName = "SharedEnvPlans";
inline constexpr std::string_view kOrgFreeAppQuotaOperationName = "OrgFreeAppQuota";
inline constexpr std::string_view kAppSharedSubscriptionOperationName = "AppSharedSubscription";
inline constexpr std::string_view kAppRuntimeStateOperationName = "AppRuntimeState";
inline constexpr std::string_view kOrgAutoBillingOperationName = "OrgAutoBilling";
inline constexpr std::string_view kOrgPaymentMethodsOperationName = "OrgPaymentMethods";
inline constexpr std::string_view kPublishAppToSharedOperationName = "PublishAppToShared";
inline constexpr std::string_view kCancelSharedSubscriptionOperationName = "CancelSharedSubscription";
inline constexpr std::string_view kSetAppSpendCapsOperationName = "SetAppSpendCaps";
inline constexpr std::string_view kSetAutoBillingOperationName = "SetAutoBilling";
inline constexpr std::string_view kSetupSharedPaymentMethodOperationName = "SetupSharedPaymentMethod";
inline constexpr std::string_view kRemoveSharedPaymentMethodOperationName = "RemoveSharedPaymentMethod";

}  // namespace sharedEnvironment

namespace state {

/// state/DeleteUserAppState.graphql
inline constexpr std::string_view kDeleteUserAppStateDocument = R"gql(mutation DeleteUserAppState($appId: BigInt!) {
  deleteUserAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kDeleteUserAppStateOperationName = "DeleteUserAppState";

/// state/UpdateUserAppState.graphql
inline constexpr std::string_view kUpdateUserAppStateDocument = R"gql(mutation UpdateUserAppState($input: CreateUserAppStateInput!) {
  updateUserAppState(input: $input) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateUserAppStateOperationName = "UpdateUserAppState";

/// state/UserAppState.graphql
inline constexpr std::string_view kUserAppStateDocument = R"gql(query UserAppState($appId: BigInt!) {
  userAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStateOperationName = "UserAppState";

/// state/UserAppStates.graphql
inline constexpr std::string_view kUserAppStatesDocument = R"gql(query UserAppStates {
  userAppStates {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStatesOperationName = "UserAppStates";

}  // namespace state

namespace teams {

/// teams/AddTeamMember.graphql
inline constexpr std::string_view kAddTeamMemberDocument = R"gql(mutation AddTeamMember($groupId: BigInt!, $userId: BigInt!) {
  addTeamMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddTeamMemberOperationName = "AddTeamMember";

/// teams/CreateTeam.graphql
inline constexpr std::string_view kCreateTeamDocument = R"gql(mutation CreateTeam($input: CreateTeamInput!) {
  createTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamOperationName = "CreateTeam";

/// teams/CreateTeamRole.graphql
inline constexpr std::string_view kCreateTeamRoleDocument = R"gql(mutation CreateTeamRole($input: CreateGroupRoleInput!) {
  createTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamRoleOperationName = "CreateTeamRole";

/// teams/DeleteTeam.graphql
inline constexpr std::string_view kDeleteTeamDocument = R"gql(mutation DeleteTeam($groupId: BigInt!, $idempotencyKey: String) {
  deleteTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kDeleteTeamOperationName = "DeleteTeam";

/// teams/DeleteTeamRole.graphql
inline constexpr std::string_view kDeleteTeamRoleDocument = R"gql(mutation DeleteTeamRole($groupRoleId: BigInt!) {
  deleteTeamRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteTeamRoleOperationName = "DeleteTeamRole";

/// teams/JoinTeam.graphql
inline constexpr std::string_view kJoinTeamDocument = R"gql(mutation JoinTeam($groupId: BigInt!) {
  joinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinTeamOperationName = "JoinTeam";

/// teams/LeaveTeam.graphql
inline constexpr std::string_view kLeaveTeamDocument = R"gql(mutation LeaveTeam($groupId: BigInt!, $idempotencyKey: String) {
  leaveTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kLeaveTeamOperationName = "LeaveTeam";

/// teams/MyTeams.graphql
inline constexpr std::string_view kMyTeamsDocument = R"gql(query MyTeams($appId: BigInt!) {
  myTeams(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyTeamsOperationName = "MyTeams";

/// teams/RemoveTeamMember.graphql
inline constexpr std::string_view kRemoveTeamMemberDocument = R"gql(mutation RemoveTeamMember($groupId: BigInt!, $userId: BigInt!) {
  removeTeamMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveTeamMemberOperationName = "RemoveTeamMember";

/// teams/RequestToJoinTeam.graphql
inline constexpr std::string_view kRequestToJoinTeamDocument = R"gql(mutation RequestToJoinTeam($groupId: BigInt!) {
  requestToJoinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinTeamOperationName = "RequestToJoinTeam";

/// teams/SetTeamMemberRoles.graphql
inline constexpr std::string_view kSetTeamMemberRolesDocument = R"gql(mutation SetTeamMemberRoles($input: SetMemberRolesInput!) {
  setTeamMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetTeamMemberRolesOperationName = "SetTeamMemberRoles";

/// teams/SetTeamPolicy.graphql
inline constexpr std::string_view kSetTeamPolicyDocument = R"gql(mutation SetTeamPolicy($input: SetTeamPolicyInput!) {
  setTeamPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetTeamPolicyOperationName = "SetTeamPolicy";

/// teams/Team.graphql
inline constexpr std::string_view kTeamDocument = R"gql(query Team($groupId: BigInt!) {
  team(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamOperationName = "Team";

/// teams/TeamMembers.graphql
inline constexpr std::string_view kTeamMembersDocument = R"gql(query TeamMembers($groupId: BigInt!) {
  teamMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kTeamMembersOperationName = "TeamMembers";

/// teams/TeamPolicy.graphql
inline constexpr std::string_view kTeamPolicyDocument = R"gql(query TeamPolicy($appId: BigInt!) {
  teamPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kTeamPolicyOperationName = "TeamPolicy";

/// teams/TeamRoles.graphql
inline constexpr std::string_view kTeamRolesDocument = R"gql(query TeamRoles($groupId: BigInt!) {
  teamRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamRolesOperationName = "TeamRoles";

/// teams/Teams.graphql
inline constexpr std::string_view kTeamsDocument = R"gql(query Teams($appId: BigInt!) {
  teams(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamsOperationName = "Teams";

/// teams/UpdateTeam.graphql
inline constexpr std::string_view kUpdateTeamDocument = R"gql(mutation UpdateTeam($input: UpdateTeamInput!) {
  updateTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamOperationName = "UpdateTeam";

/// teams/UpdateTeamRole.graphql
inline constexpr std::string_view kUpdateTeamRoleDocument = R"gql(mutation UpdateTeamRole($input: UpdateGroupRoleInput!) {
  updateTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamRoleOperationName = "UpdateTeamRole";

}  // namespace teams

namespace teleport {

/// teleport/TeleportRequest.graphql
inline constexpr std::string_view kTeleportRequestDocument = R"gql(mutation TeleportRequest($input: TeleportRequestInput!) {
  teleportRequest(input: $input) {
    success
    errorCode
  }
})gql";
inline constexpr std::string_view kTeleportRequestOperationName = "TeleportRequest";

}  // namespace teleport

namespace usage {

/// usage/AppGraphqlOperations.graphql
inline constexpr std::string_view kAppGraphqlOperationsDocument = R"gql(query AppGraphqlOperations(
  $orgId: BigInt!
  $appId: BigInt!
  $since: DateTime!
  $limit: Int
) {
  appGraphqlOperations(
    orgId: $orgId
    appId: $appId
    since: $since
    limit: $limit
  ) {
    operationName
    totalOps
    sendBytes
    recvBytes
  }
})gql";
inline constexpr std::string_view kAppGraphqlOperationsOperationName = "AppGraphqlOperations";

/// usage/AppUsageSummary.graphql
inline constexpr std::string_view kAppUsageSummaryDocument = R"gql(query AppUsageSummary(
  $orgId: BigInt!
  $appId: BigInt!
  $since: DateTime!
  $operationLimit: Int
) {
  appUsageSummary(
    orgId: $orgId
    appId: $appId
    since: $since
    operationLimit: $operationLimit
  ) {
    appId
    replicationSendBytes
    replicationRecvBytes
    graphqlSendBytes
    graphqlRecvBytes
    automationRuns
    automationInvocations
    automationComputeUnits
    topGraphqlOperations {
      operationName
      totalOps
      sendBytes
      recvBytes
    }
  }
})gql";
inline constexpr std::string_view kAppUsageSummaryOperationName = "AppUsageSummary";

/// usage/EnvironmentUsageByApp.graphql
inline constexpr std::string_view kEnvironmentUsageByAppDocument = R"gql(query EnvironmentUsageByApp(
  $orgId: BigInt!
  $environmentSlug: String!
  $since: DateTime!
) {
  environmentUsageByApp(
    orgId: $orgId
    environmentSlug: $environmentSlug
    since: $since
  ) {
    appId
    appSlug
    appName
    replicationSendBytes
    replicationRecvBytes
    graphqlSendBytes
    graphqlRecvBytes
  }
})gql";
inline constexpr std::string_view kEnvironmentUsageByAppOperationName = "EnvironmentUsageByApp";

/// usage/EnvironmentUsageSummary.graphql
inline constexpr std::string_view kEnvironmentUsageSummaryDocument = R"gql(query EnvironmentUsageSummary(
  $orgId: BigInt!
  $environmentSlug: String!
  $since: DateTime!
) {
  environmentUsageSummary(
    orgId: $orgId
    environmentSlug: $environmentSlug
    since: $since
  ) {
    environmentSlug
    environmentId
    orgId
    replication {
      minute
      recvBytes
      sendBytes
      recvMsgs
      sendMsgs
    }
    graphql {
      minute
      recvBytes
      sendBytes
    }
    replicationRates {
      peakSendMsgsPerSec
      peakSendMbitPerSec
      avgSendMsgsPerSec
      avgSendMbitPerSec
      sampleMinutes
    }
    buddyLive {
      serverId
      clientSendMsgsPerSec
      clientSendMbitPerSec
      clientRecvMsgsPerSec
      clientRecvMbitPerSec
      clients
      updatedAt
    }
  }
})gql";
inline constexpr std::string_view kEnvironmentUsageSummaryOperationName = "EnvironmentUsageSummary";

/// usage/OrgUsageByEnvironment.graphql
inline constexpr std::string_view kOrgUsageByEnvironmentDocument = R"gql(query OrgUsageByEnvironment($orgId: BigInt!, $since: DateTime!) {
  orgUsageByEnvironment(orgId: $orgId, since: $since) {
    environmentId
    environmentSlug
    displayName
    replicationSendBytes
    replicationRecvBytes
    graphqlSendBytes
    graphqlRecvBytes
  }
})gql";
inline constexpr std::string_view kOrgUsageByEnvironmentOperationName = "OrgUsageByEnvironment";

/// usage/PlayerPulse.graphql
inline constexpr std::string_view kPlayerPulseDocument = R"gql(query PlayerPulse($orgId: BigInt!) {
  playerPulse(orgId: $orgId) {
    orgLivePlayers
    orgAllTimePeak
    orgAllTimePeakAt
    globalLivePlayers
    percentile
    poolSize
  }
})gql";
inline constexpr std::string_view kPlayerPulseOperationName = "PlayerPulse";

}  // namespace usage

namespace users {

/// users/DeleteMyAccount.graphql
inline constexpr std::string_view kDeleteMyAccountDocument = R"gql(mutation DeleteMyAccount {
  deleteMyAccount
})gql";
inline constexpr std::string_view kDeleteMyAccountOperationName = "DeleteMyAccount";

/// users/ForceLogoutUser.graphql
inline constexpr std::string_view kForceLogoutUserDocument = R"gql(mutation ForceLogoutUser($userId: BigInt!) {
  forceLogoutUser(userId: $userId)
})gql";
inline constexpr std::string_view kForceLogoutUserOperationName = "ForceLogoutUser";

/// users/FreePlayWindow.graphql
inline constexpr std::string_view kFreePlayWindowDocument = R"gql(query FreePlayWindow {
  freePlayWindowInfo {
    isCurrentlyActive
    description
    nextWindowStart
  }
})gql";
inline constexpr std::string_view kFreePlayWindowOperationName = "FreePlayWindow";

/// users/Me.graphql
inline constexpr std::string_view kMeDocument = R"gql(query Me {
  me {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kMeOperationName = "Me";

/// users/SetEarlyAccessOverride.graphql
inline constexpr std::string_view kSetEarlyAccessOverrideDocument = R"gql(mutation SetEarlyAccessOverride($userId: BigInt!, $value: Boolean!) {
  setEarlyAccessOverride(userId: $userId, value: $value) {
    userId
    grantEarlyAccessOverride
  }
})gql";
inline constexpr std::string_view kSetEarlyAccessOverrideOperationName = "SetEarlyAccessOverride";

/// users/SetOperator.graphql
inline constexpr std::string_view kSetOperatorDocument = R"gql(mutation SetOperator($userId: BigInt!, $value: Boolean!) {
  setOperator(userId: $userId, value: $value) {
    userId
    isOperator
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetOperatorOperationName = "SetOperator";

/// users/SetSuperAdmin.graphql
inline constexpr std::string_view kSetSuperAdminDocument = R"gql(mutation SetSuperAdmin($userId: BigInt!, $value: Boolean!) {
  setSuperAdmin(userId: $userId, value: $value) {
    userId
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetSuperAdminOperationName = "SetSuperAdmin";

/// users/UpdateGamertag.graphql
inline constexpr std::string_view kUpdateGamertagDocument = R"gql(mutation UpdateGamertag($input: UpdateGamertagInput!) {
  updateGamertag(input: $input) {
    userId
    gamertag
    disambiguation
    userType
  }
})gql";
inline constexpr std::string_view kUpdateGamertagOperationName = "UpdateGamertag";

/// users/UpdateUserState.graphql
inline constexpr std::string_view kUpdateUserStateDocument = R"gql(mutation UpdateUserState($input: UpdateUserStateInput!) {
  updateUserState(input: $input) {
    userId
    state
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserStateOperationName = "UpdateUserState";

/// users/UpdateUserType.graphql
inline constexpr std::string_view kUpdateUserTypeDocument = R"gql(mutation UpdateUserType($userId: BigInt!, $value: String!) {
  updateUserType(userId: $userId, value: $value) {
    userId
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserTypeOperationName = "UpdateUserType";

/// users/User.graphql
inline constexpr std::string_view kUserDocument = R"gql(query User($id: BigInt!) {
  user(id: $id) {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kUserOperationName = "User";

/// users/UsersPaginated.graphql
inline constexpr std::string_view kUsersPaginatedDocument = R"gql(query UsersPaginated($query: String, $limit: Int, $offset: Int) {
  usersPaginated(query: $query, limit: $limit, offset: $offset) {
    items {
      userId
      email
      gamertag
      disambiguation
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query UsersConnection($first: Int, $after: String, $query: String) {
  usersConnection(first: $first, after: $after, query: $query) {
    edges {
      cursor
      node {
        userId
        email
        gamertag
        disambiguation
        isConfirmed
        createdAt
        grantEarlyAccess
        grantEarlyAccessOverride
        orgId
        externalId
        userType
        isSuperAdmin
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kUsersPaginatedOperationName = "UsersPaginated";
inline constexpr std::string_view kUsersConnectionOperationName = "UsersConnection";

}  // namespace users

namespace voxels {

/// voxels/ListVoxelUpdatesByDistance.graphql
inline constexpr std::string_view kListVoxelUpdatesByDistanceDocument = R"gql(query ListVoxelUpdatesByDistance($input: ListVoxelUpdatesByDistanceInput!) {
  listVoxelUpdatesByDistance(input: $input) {
    centerCoordinate {
      x
      y
      z
    }
    limit
    skip
    chunks {
      coordinates {
        x
        y
        z
      }
      voxels {
        voxelUpdateId
        appId
        location {
          x
          y
          z
        }
        voxelType
        state
        createdBy
        createdAt
      }
    }
  }
})gql";
inline constexpr std::string_view kListVoxelUpdatesByDistanceOperationName = "ListVoxelUpdatesByDistance";

/// voxels/ListVoxels.graphql
inline constexpr std::string_view kListVoxelsDocument = R"gql(query ListVoxels($input: ListVoxelsInput!) {
  listVoxels(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kListVoxelsOperationName = "ListVoxels";

/// voxels/RollbackVoxelUpdates.graphql
inline constexpr std::string_view kRollbackVoxelUpdatesDocument = R"gql(mutation RollbackVoxelUpdates($input: RollbackVoxelUpdatesInput!) {
  rollbackVoxelUpdates(input: $input) {
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    fromVoxelType
    toVoxelType
    plannedAction
    applied
    reason
  }
})gql";
inline constexpr std::string_view kRollbackVoxelUpdatesOperationName = "RollbackVoxelUpdates";

/// voxels/UpdateVoxel.graphql
inline constexpr std::string_view kUpdateVoxelDocument = R"gql(mutation UpdateVoxel($input: UpdateVoxelInput!) {
  updateVoxel(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateVoxelOperationName = "UpdateVoxel";

/// voxels/VoxelUpdateHistory.graphql
inline constexpr std::string_view kVoxelUpdateHistoryDocument = R"gql(query VoxelUpdateHistory(
  $appId: BigInt!
  $userId: BigInt
  $from: DateTime
  $to: DateTime
  $limit: Int
  $offset: Int
) {
  voxelUpdateHistory(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    limit: $limit
    offset: $offset
  ) {
    id
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    oldVoxelType
    newVoxelType
    changedBy
    changedAt
  }
}

query VoxelUpdateHistoryConnection(
  $appId: BigInt!
  $userId: BigInt
  $from: DateTime
  $to: DateTime
  $first: Int
  $after: String
) {
  voxelUpdateHistoryConnection(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    first: $first
    after: $after
  ) {
    edges {
      cursor
      node {
        id
        appId
        coordinates {
          x
          y
          z
        }
        location {
          x
          y
          z
        }
        oldVoxelType
        newVoxelType
        changedBy
        changedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kVoxelUpdateHistoryOperationName = "VoxelUpdateHistory";
inline constexpr std::string_view kVoxelUpdateHistoryConnectionOperationName = "VoxelUpdateHistoryConnection";

}  // namespace voxels

}  // namespace crowdy::gen
