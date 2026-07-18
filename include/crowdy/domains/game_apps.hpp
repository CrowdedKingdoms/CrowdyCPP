#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.gameApps() — app grids + grid runtime-permission administration
/// (land claims, zoning, enforced voxel/voice permissions). Targets the Game
/// API. See https://docs.crowdedkingdoms.com/game-api/grids-and-permissions.
namespace crowdy::domains {

class GameAppsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json userPermissions(std::string_view appId, std::string_view gridId,
                                std::string_view userId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["userId"] = userId;
    return run("GridUserPermissions", vars);
  }

  graphql::Json nearbyPermissions(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("NearbyGridPermissions", vars);
  }

  graphql::Json permissionLimits(std::string_view appId, std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("GridPermissionLimits", vars);
  }

  graphql::Json groupGrants(std::string_view appId, std::string_view gridId,
                            std::string_view groupId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["groupId"] = groupId;
    return run("GridGroupGrants", vars);
  }

  graphql::Json createGrid(const graphql::JVal& input) const { return byInput("CreateGrid", input); }

  /// Destructive; input accepts an idempotencyKey.
  graphql::Json deleteGrid(const graphql::JVal& input) const { return byInput("DeleteGrid", input); }

  graphql::Json grantPermissions(const graphql::JVal& input) const {
    return byInput("GrantGridPermissions", input);
  }
  graphql::Json revokePermissions(const graphql::JVal& input) const {
    return byInput("RevokeGridPermissions", input);
  }
  graphql::Json setPermissionLimits(const graphql::JVal& input) const {
    return byInput("SetGridPermissionLimits", input);
  }
  graphql::Json assignGroup(const graphql::JVal& input) const {
    return byInput("AssignGroupToGrid", input);
  }
  graphql::Json revokeGroup(const graphql::JVal& input) const {
    return byInput("RevokeGroupFromGrid", input);
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::gameApps::kGameAppsDocument, vars, op);
  }
  graphql::Json byInput(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run(op, vars);
  }
};

/// client.platform() — public platform configuration.
class PlatformAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;
  graphql::Json config() const { return execUnwrap(gen::platform::kPlatformConfigDocument); }
};

}  // namespace crowdy::domains
