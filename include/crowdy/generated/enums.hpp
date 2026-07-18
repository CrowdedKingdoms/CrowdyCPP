// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDLs at https://docs.crowdedkingdoms.com/schema/).

#pragma once

#include <optional>
#include <string_view>

/// GraphQL enums from the published schema. Values keep their wire spelling;
/// toString/fromString convert between the enum and the GraphQL string.
namespace crowdy::gen {

enum class AppDeploymentTarget {
  NONE,
  SHARED,
  DEDICATED,
};

inline constexpr std::string_view toString(AppDeploymentTarget v) {
  switch (v) {
    case AppDeploymentTarget::NONE: return "NONE";
    case AppDeploymentTarget::SHARED: return "SHARED";
    case AppDeploymentTarget::DEDICATED: return "DEDICATED";
  }
  return "";
}

inline std::optional<AppDeploymentTarget> appDeploymentTargetFromString(std::string_view s) {
  if (s == "NONE") return AppDeploymentTarget::NONE;
  if (s == "SHARED") return AppDeploymentTarget::SHARED;
  if (s == "DEDICATED") return AppDeploymentTarget::DEDICATED;
  return std::nullopt;
}

enum class AppRuntimeStatus {
  ACTIVE,
  GRACE,
  DENIED,
  SUSPENDED,
};

inline constexpr std::string_view toString(AppRuntimeStatus v) {
  switch (v) {
    case AppRuntimeStatus::ACTIVE: return "ACTIVE";
    case AppRuntimeStatus::GRACE: return "GRACE";
    case AppRuntimeStatus::DENIED: return "DENIED";
    case AppRuntimeStatus::SUSPENDED: return "SUSPENDED";
  }
  return "";
}

inline std::optional<AppRuntimeStatus> appRuntimeStatusFromString(std::string_view s) {
  if (s == "ACTIVE") return AppRuntimeStatus::ACTIVE;
  if (s == "GRACE") return AppRuntimeStatus::GRACE;
  if (s == "DENIED") return AppRuntimeStatus::DENIED;
  if (s == "SUSPENDED") return AppRuntimeStatus::SUSPENDED;
  return std::nullopt;
}

enum class AppStatus {
  DRAFT,
  LIVE,
  ARCHIVED,
};

inline constexpr std::string_view toString(AppStatus v) {
  switch (v) {
    case AppStatus::DRAFT: return "DRAFT";
    case AppStatus::LIVE: return "LIVE";
    case AppStatus::ARCHIVED: return "ARCHIVED";
  }
  return "";
}

inline std::optional<AppStatus> appStatusFromString(std::string_view s) {
  if (s == "DRAFT") return AppStatus::DRAFT;
  if (s == "LIVE") return AppStatus::LIVE;
  if (s == "ARCHIVED") return AppStatus::ARCHIVED;
  return std::nullopt;
}

enum class AppVisibility {
  PUBLIC,
  UNLISTED,
  PRIVATE,
};

inline constexpr std::string_view toString(AppVisibility v) {
  switch (v) {
    case AppVisibility::PUBLIC: return "PUBLIC";
    case AppVisibility::UNLISTED: return "UNLISTED";
    case AppVisibility::PRIVATE: return "PRIVATE";
  }
  return "";
}

inline std::optional<AppVisibility> appVisibilityFromString(std::string_view s) {
  if (s == "PUBLIC") return AppVisibility::PUBLIC;
  if (s == "UNLISTED") return AppVisibility::UNLISTED;
  if (s == "PRIVATE") return AppVisibility::PRIVATE;
  return std::nullopt;
}

enum class CheckoutPurpose {
  ORG_WALLET_TOPUP,
  APP_ACCESS_PURCHASE,
};

inline constexpr std::string_view toString(CheckoutPurpose v) {
  switch (v) {
    case CheckoutPurpose::ORG_WALLET_TOPUP: return "ORG_WALLET_TOPUP";
    case CheckoutPurpose::APP_ACCESS_PURCHASE: return "APP_ACCESS_PURCHASE";
  }
  return "";
}

inline std::optional<CheckoutPurpose> checkoutPurposeFromString(std::string_view s) {
  if (s == "ORG_WALLET_TOPUP") return CheckoutPurpose::ORG_WALLET_TOPUP;
  if (s == "APP_ACCESS_PURCHASE") return CheckoutPurpose::APP_ACCESS_PURCHASE;
  return std::nullopt;
}

enum class CheckoutStatus {
  PENDING,
  COMPLETED,
  FAILED,
  EXPIRED,
  CANCELED,
};

inline constexpr std::string_view toString(CheckoutStatus v) {
  switch (v) {
    case CheckoutStatus::PENDING: return "PENDING";
    case CheckoutStatus::COMPLETED: return "COMPLETED";
    case CheckoutStatus::FAILED: return "FAILED";
    case CheckoutStatus::EXPIRED: return "EXPIRED";
    case CheckoutStatus::CANCELED: return "CANCELED";
  }
  return "";
}

inline std::optional<CheckoutStatus> checkoutStatusFromString(std::string_view s) {
  if (s == "PENDING") return CheckoutStatus::PENDING;
  if (s == "COMPLETED") return CheckoutStatus::COMPLETED;
  if (s == "FAILED") return CheckoutStatus::FAILED;
  if (s == "EXPIRED") return CheckoutStatus::EXPIRED;
  if (s == "CANCELED") return CheckoutStatus::CANCELED;
  return std::nullopt;
}

enum class PaymentProvider {
  STRIPE,
  PAYPAL,
};

inline constexpr std::string_view toString(PaymentProvider v) {
  switch (v) {
    case PaymentProvider::STRIPE: return "STRIPE";
    case PaymentProvider::PAYPAL: return "PAYPAL";
  }
  return "";
}

inline std::optional<PaymentProvider> paymentProviderFromString(std::string_view s) {
  if (s == "STRIPE") return PaymentProvider::STRIPE;
  if (s == "PAYPAL") return PaymentProvider::PAYPAL;
  return std::nullopt;
}

enum class RedeployDeployMode {
  FULL,
  SERVICES,
};

inline constexpr std::string_view toString(RedeployDeployMode v) {
  switch (v) {
    case RedeployDeployMode::FULL: return "FULL";
    case RedeployDeployMode::SERVICES: return "SERVICES";
  }
  return "";
}

inline std::optional<RedeployDeployMode> redeployDeployModeFromString(std::string_view s) {
  if (s == "FULL") return RedeployDeployMode::FULL;
  if (s == "SERVICES") return RedeployDeployMode::SERVICES;
  return std::nullopt;
}

enum class ServerState {
  Starting,
  ReadyForClients,
  Stopping,
  Offline,
  NearCapacity,
  Full,
};

inline constexpr std::string_view toString(ServerState v) {
  switch (v) {
    case ServerState::Starting: return "Starting";
    case ServerState::ReadyForClients: return "ReadyForClients";
    case ServerState::Stopping: return "Stopping";
    case ServerState::Offline: return "Offline";
    case ServerState::NearCapacity: return "NearCapacity";
    case ServerState::Full: return "Full";
  }
  return "";
}

inline std::optional<ServerState> serverStateFromString(std::string_view s) {
  if (s == "Starting") return ServerState::Starting;
  if (s == "ReadyForClients") return ServerState::ReadyForClients;
  if (s == "Stopping") return ServerState::Stopping;
  if (s == "Offline") return ServerState::Offline;
  if (s == "NearCapacity") return ServerState::NearCapacity;
  if (s == "Full") return ServerState::Full;
  return std::nullopt;
}

enum class UdpErrorCode {
  NO_ERROR,
  UNKNOWN_ERROR,
  EMAIL_NOT_FOUND,
  BAD_PASSWORD,
  EMAIL_ALREADY_EXISTS,
  INVALID_TOKEN,
  APP_NOT_FOUND,
  UNAUTHORIZED,
  APP_NOT_LOADED,
  EMAIL_TOO_SHORT,
  EMAIL_TOO_LONG,
  PASSWORD_TOO_SHORT,
  PASSWORD_TOO_LONG,
  GAME_TOKEN_WRONG_SIZE,
  NAME_TOO_LONG,
  INVALID_REQUEST,
  EMAIL_INVALID,
  INVALID_TOKEN_LENGTH,
  INVALID_APP_ID,
  CHUNK_NOT_FOUND,
  USER_NOT_AUTHENTICATED,
  INVALID_STATE_DATA,
  USER_NOT_APP_ADMIN,
  GRID_OUTSIDE_ASSIGNMENT,
  NO_MATCHING_GRID_ASSIGNMENT,
  INVALID_GRID_COORDINATES,
  GRID_ALREADY_EXISTS,
  GRID_OVERLAPS_EXISTING,
  GAMERTAG_ALREADY_EXISTS,
  GRID_NOT_FOUND,
  CANNOT_DELETE_DEFAULT_WORLD_GRID,
  GRID_HAS_NESTED_CHILDREN,
  TOKEN_EXPIRED,
};

inline constexpr std::string_view toString(UdpErrorCode v) {
  switch (v) {
    case UdpErrorCode::NO_ERROR: return "NO_ERROR";
    case UdpErrorCode::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
    case UdpErrorCode::EMAIL_NOT_FOUND: return "EMAIL_NOT_FOUND";
    case UdpErrorCode::BAD_PASSWORD: return "BAD_PASSWORD";
    case UdpErrorCode::EMAIL_ALREADY_EXISTS: return "EMAIL_ALREADY_EXISTS";
    case UdpErrorCode::INVALID_TOKEN: return "INVALID_TOKEN";
    case UdpErrorCode::APP_NOT_FOUND: return "APP_NOT_FOUND";
    case UdpErrorCode::UNAUTHORIZED: return "UNAUTHORIZED";
    case UdpErrorCode::APP_NOT_LOADED: return "APP_NOT_LOADED";
    case UdpErrorCode::EMAIL_TOO_SHORT: return "EMAIL_TOO_SHORT";
    case UdpErrorCode::EMAIL_TOO_LONG: return "EMAIL_TOO_LONG";
    case UdpErrorCode::PASSWORD_TOO_SHORT: return "PASSWORD_TOO_SHORT";
    case UdpErrorCode::PASSWORD_TOO_LONG: return "PASSWORD_TOO_LONG";
    case UdpErrorCode::GAME_TOKEN_WRONG_SIZE: return "GAME_TOKEN_WRONG_SIZE";
    case UdpErrorCode::NAME_TOO_LONG: return "NAME_TOO_LONG";
    case UdpErrorCode::INVALID_REQUEST: return "INVALID_REQUEST";
    case UdpErrorCode::EMAIL_INVALID: return "EMAIL_INVALID";
    case UdpErrorCode::INVALID_TOKEN_LENGTH: return "INVALID_TOKEN_LENGTH";
    case UdpErrorCode::INVALID_APP_ID: return "INVALID_APP_ID";
    case UdpErrorCode::CHUNK_NOT_FOUND: return "CHUNK_NOT_FOUND";
    case UdpErrorCode::USER_NOT_AUTHENTICATED: return "USER_NOT_AUTHENTICATED";
    case UdpErrorCode::INVALID_STATE_DATA: return "INVALID_STATE_DATA";
    case UdpErrorCode::USER_NOT_APP_ADMIN: return "USER_NOT_APP_ADMIN";
    case UdpErrorCode::GRID_OUTSIDE_ASSIGNMENT: return "GRID_OUTSIDE_ASSIGNMENT";
    case UdpErrorCode::NO_MATCHING_GRID_ASSIGNMENT: return "NO_MATCHING_GRID_ASSIGNMENT";
    case UdpErrorCode::INVALID_GRID_COORDINATES: return "INVALID_GRID_COORDINATES";
    case UdpErrorCode::GRID_ALREADY_EXISTS: return "GRID_ALREADY_EXISTS";
    case UdpErrorCode::GRID_OVERLAPS_EXISTING: return "GRID_OVERLAPS_EXISTING";
    case UdpErrorCode::GAMERTAG_ALREADY_EXISTS: return "GAMERTAG_ALREADY_EXISTS";
    case UdpErrorCode::GRID_NOT_FOUND: return "GRID_NOT_FOUND";
    case UdpErrorCode::CANNOT_DELETE_DEFAULT_WORLD_GRID: return "CANNOT_DELETE_DEFAULT_WORLD_GRID";
    case UdpErrorCode::GRID_HAS_NESTED_CHILDREN: return "GRID_HAS_NESTED_CHILDREN";
    case UdpErrorCode::TOKEN_EXPIRED: return "TOKEN_EXPIRED";
  }
  return "";
}

inline std::optional<UdpErrorCode> udpErrorCodeFromString(std::string_view s) {
  if (s == "NO_ERROR") return UdpErrorCode::NO_ERROR;
  if (s == "UNKNOWN_ERROR") return UdpErrorCode::UNKNOWN_ERROR;
  if (s == "EMAIL_NOT_FOUND") return UdpErrorCode::EMAIL_NOT_FOUND;
  if (s == "BAD_PASSWORD") return UdpErrorCode::BAD_PASSWORD;
  if (s == "EMAIL_ALREADY_EXISTS") return UdpErrorCode::EMAIL_ALREADY_EXISTS;
  if (s == "INVALID_TOKEN") return UdpErrorCode::INVALID_TOKEN;
  if (s == "APP_NOT_FOUND") return UdpErrorCode::APP_NOT_FOUND;
  if (s == "UNAUTHORIZED") return UdpErrorCode::UNAUTHORIZED;
  if (s == "APP_NOT_LOADED") return UdpErrorCode::APP_NOT_LOADED;
  if (s == "EMAIL_TOO_SHORT") return UdpErrorCode::EMAIL_TOO_SHORT;
  if (s == "EMAIL_TOO_LONG") return UdpErrorCode::EMAIL_TOO_LONG;
  if (s == "PASSWORD_TOO_SHORT") return UdpErrorCode::PASSWORD_TOO_SHORT;
  if (s == "PASSWORD_TOO_LONG") return UdpErrorCode::PASSWORD_TOO_LONG;
  if (s == "GAME_TOKEN_WRONG_SIZE") return UdpErrorCode::GAME_TOKEN_WRONG_SIZE;
  if (s == "NAME_TOO_LONG") return UdpErrorCode::NAME_TOO_LONG;
  if (s == "INVALID_REQUEST") return UdpErrorCode::INVALID_REQUEST;
  if (s == "EMAIL_INVALID") return UdpErrorCode::EMAIL_INVALID;
  if (s == "INVALID_TOKEN_LENGTH") return UdpErrorCode::INVALID_TOKEN_LENGTH;
  if (s == "INVALID_APP_ID") return UdpErrorCode::INVALID_APP_ID;
  if (s == "CHUNK_NOT_FOUND") return UdpErrorCode::CHUNK_NOT_FOUND;
  if (s == "USER_NOT_AUTHENTICATED") return UdpErrorCode::USER_NOT_AUTHENTICATED;
  if (s == "INVALID_STATE_DATA") return UdpErrorCode::INVALID_STATE_DATA;
  if (s == "USER_NOT_APP_ADMIN") return UdpErrorCode::USER_NOT_APP_ADMIN;
  if (s == "GRID_OUTSIDE_ASSIGNMENT") return UdpErrorCode::GRID_OUTSIDE_ASSIGNMENT;
  if (s == "NO_MATCHING_GRID_ASSIGNMENT") return UdpErrorCode::NO_MATCHING_GRID_ASSIGNMENT;
  if (s == "INVALID_GRID_COORDINATES") return UdpErrorCode::INVALID_GRID_COORDINATES;
  if (s == "GRID_ALREADY_EXISTS") return UdpErrorCode::GRID_ALREADY_EXISTS;
  if (s == "GRID_OVERLAPS_EXISTING") return UdpErrorCode::GRID_OVERLAPS_EXISTING;
  if (s == "GAMERTAG_ALREADY_EXISTS") return UdpErrorCode::GAMERTAG_ALREADY_EXISTS;
  if (s == "GRID_NOT_FOUND") return UdpErrorCode::GRID_NOT_FOUND;
  if (s == "CANNOT_DELETE_DEFAULT_WORLD_GRID") return UdpErrorCode::CANNOT_DELETE_DEFAULT_WORLD_GRID;
  if (s == "GRID_HAS_NESTED_CHILDREN") return UdpErrorCode::GRID_HAS_NESTED_CHILDREN;
  if (s == "TOKEN_EXPIRED") return UdpErrorCode::TOKEN_EXPIRED;
  return std::nullopt;
}

}  // namespace crowdy::gen
