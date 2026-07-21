#pragma once

#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerModel() — flexible player-owned model data and grid-confined
/// player automations. All identities/scopes are forced by game-api.
namespace crowdy::domains {

class PlayerModelAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json containers(std::string_view appId,
                           std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerModelContainers", vars);
  }

  graphql::Json container(const graphql::JVal& input) const {
    return byInput("PlayerModelContainer", input);
  }

  graphql::Json createContainer(const graphql::JVal& input) const {
    return byInput("PlayerModelCreateContainer", input);
  }

  graphql::Json setProperty(const graphql::JVal& input) const {
    return byInput("PlayerModelSetProperty", input);
  }

  graphql::Json deleteContainer(const graphql::JVal& input) const {
    return byInput("PlayerModelDeleteContainer", input);
  }

  graphql::Json automations(std::string_view appId,
                            std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerAutomations", vars);
  }

  graphql::Json createAutomation(const graphql::JVal& input) const {
    return byInput("PlayerAutomationCreate", input);
  }

  graphql::Json setAutomationEnabled(const graphql::JVal& input) const {
    return byInput("PlayerAutomationSetEnabled", input);
  }

  graphql::Json deleteAutomation(const graphql::JVal& input) const {
    return byInput("PlayerAutomationDelete", input);
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerModel::kPlayerModelDocument, vars, op);
  }

  graphql::Json byInput(std::string_view op,
                        const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run(op, vars);
  }
};

}  // namespace crowdy::domains
