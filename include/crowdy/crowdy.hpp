#pragma once

/// Umbrella header for the CrowdyCPP SDK.
///
/// Layers:
///   crowdy::CrowdyClient        — GraphQL surface (auth, portal, world data,
///                                 game model, teams/channels, admin, operator)
///   crowdy::replication         — native UDP replication client
///   crowdy::session             — world session layer (actors, chunks, inboxes)
///   crowdy::kit                 — Game Kit (blueprints + runtime helpers)

#include "crowdy/client.hpp"
#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/operator.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/kit/kit.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/world_session.hpp"
