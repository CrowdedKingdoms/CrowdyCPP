// GENERATED — do not edit. `scripts/sync-client-origins.mjs --write --tier dev`
// in the cks-michael-root wrapper writes this file, and
// `scripts/check-sdk-default-origin.mjs` refuses it when it names the wrong tier
// or a host the tier table does not declare.
//
// THE DEFAULT IS LOAD-BEARING DURING A ROLLOUT: while the branches are
// mid-migration this is what an unconfigured consumer gets, so a WRONG default on
// one branch is worse than no default at all. The literal that used to live in
// tests/prodsmoke/main.cpp named a host retired on 2026-08-19 and nothing said so.
//
// Source: cp-tiers.json tiers.dev.clientOriginHost (mirror of CK_CLIENT_ORIGIN_HOST_BY_TIER in dns-tier.ts)
#ifndef CROWDY_DEFAULT_ORIGIN_HPP
#define CROWDY_DEFAULT_ORIGIN_HPP

namespace crowdy {

/// The tier this build of the SDK is released for.
inline constexpr const char* kDefaultTier = "dev";

/// The public CK API origin for that tier.
inline constexpr const char* kDefaultHttpOrigin = "https://ck.dev.crowdedkingdoms.com";

/// The same host over WebSocket. A scheme is composed; a hostname is looked up.
inline constexpr const char* kDefaultWsOrigin = "wss://ck.dev.crowdedkingdoms.com";

/// The bare hostname, for callers that need to compare rather than dial.
inline constexpr const char* kDefaultHost = "ck.dev.crowdedkingdoms.com";

}  // namespace crowdy

#endif  // CROWDY_DEFAULT_ORIGIN_HPP
