#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.gameModel() — the abstract game model: containers, properties,
/// functions with invoke policies, sessions, edges, events, and automations
/// (server-side NPCs / world ticks). Schema authoring (seed/upsert*) is a
/// studio-admin step run before players connect; runtime ops execute with a
/// player's app token. JSON payloads cross the wire as *Json strings.
/// See https://docs.crowdedkingdoms.com/game-api/game-models and
/// https://docs.crowdedkingdoms.com/game-api/autonomous-processes.
namespace crowdy::domains {

class GameModelAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  // ----- Studio authoring (requires manage_apps) ----------------------------

  /// Idempotent bulk seed: container types, property defs, and functions in
  /// one call. The reference pattern for "load the rules" scripts.
  graphql::Json seed(const graphql::JVal& input) const {
    return studio("GameModelSeed", input);
  }
  graphql::Json upsertContainerType(const graphql::JVal& input) const {
    return studio("GameModelUpsertContainerType", input);
  }
  graphql::Json upsertPropertyDef(const graphql::JVal& input) const {
    return studio("GameModelUpsertPropertyDef", input);
  }
  graphql::Json deletePropertyDef(std::string_view appId, const graphql::JVal& args) const {
    graphql::JVal vars = args;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeletePropertyDef");
  }
  graphql::Json deleteContainerType(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars,
                      "GameModelDeleteContainerType");
  }
  graphql::Json upsertFunction(const graphql::JVal& input) const {
    return studio("GameModelUpsertFunction", input);
  }
  graphql::Json deleteFunction(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeleteFunction");
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return studio("GameModelSetPolicy", input);
  }
  graphql::Json defineFeature(const graphql::JVal& input) const {
    return studio("GameModelDefineFeature", input);
  }
  graphql::Json grantTierFeature(const graphql::JVal& input) const {
    return studio("GameModelGrantTierFeature", input);
  }
  graphql::Json revokeTierFeature(const graphql::JVal& input) const {
    return studio("GameModelRevokeTierFeature", input);
  }

  // ----- Studio reads --------------------------------------------------------

  graphql::Json typeSchema(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTypeSchema");
  }
  graphql::Json containerTypes(std::string_view appId) const {
    return studioByApp("GameModelContainerTypes", appId);
  }
  graphql::Json propertyDefs(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelPropertyDefs");
  }
  graphql::Json function(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunction");
  }
  graphql::Json functions(std::string_view appId, std::string_view containerTypeName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!containerTypeName.empty()) vars["containerTypeName"] = containerTypeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunctions");
  }
  graphql::Json features(std::string_view appId) const {
    return studioByApp("GameModelFeatures", appId);
  }
  graphql::Json tierFeatures(std::string_view appId, std::string_view tierId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!tierId.empty()) vars["tierId"] = tierId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTierFeatures");
  }
  graphql::Json policy(std::string_view appId) const {
    return studioByApp("GameModelPolicy", appId);
  }

  // ----- Runtime (player app token) ------------------------------------------

  graphql::Json createContainer(const graphql::JVal& input) const {
    return runtime("GameModelCreateContainer", input);
  }
  graphql::Json deleteContainer(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteContainer");
  }
  graphql::Json setProperty(const graphql::JVal& input) const {
    return runtime("GameModelSetProperty", input);
  }
  graphql::Json addEdge(const graphql::JVal& input) const {
    return runtime("GameModelAddEdge", input);
  }
  graphql::Json deleteEdge(std::string_view appId, std::string_view edgeId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["edgeId"] = edgeId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteEdge");
  }

  /// Transactional server-side mutation: invoke a seeded function. Invoke
  /// policies (owner_of_self, condition, is_host, is_current_turn, allow,
  /// and/or) are the authority model.
  graphql::Json invoke(const graphql::JVal& input) const {
    return runtime("GameModelInvoke", input);
  }

  graphql::Json container(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainer");
  }
  graphql::Json containers(std::string_view appId, std::string_view typeName = {},
                           std::string_view sessionId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainers");
  }
  /// Filtered/paged container list (2026-07+ servers): `where` is an array of
  /// up to 8 AND-combined `{key, op, valueJson}` predicates (ops ==, !=, <,
  /// >, <=, >=; requires typeName; missing properties fall back to the type
  /// default — the same shape automation selectors use); limit/offset page
  /// after filtering over the stable created-at ordering (pass -1 to omit).
  graphql::Json containersWhere(std::string_view appId, std::string_view typeName,
                                const graphql::JVal& where, int limit = -1, int offset = -1,
                                std::string_view sessionId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!where.isNull()) vars["where"] = where;
    if (limit >= 0) vars["limit"] = limit;
    if (offset >= 0) vars["offset"] = offset;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainers");
  }
  graphql::Json containerState(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainerState");
  }
  graphql::Json traverse(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelTraverse");
  }

  graphql::Json createSession(const graphql::JVal& input) const {
    return runtime("GameModelCreateSession", input);
  }
  graphql::Json joinSession(const graphql::JVal& input) const {
    return runtime("GameModelJoinSession", input);
  }
  graphql::Json setSessionTurn(const graphql::JVal& input) const {
    return runtime("GameModelSetSessionTurn", input);
  }
  graphql::Json session(std::string_view appId, std::string_view sessionId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["sessionId"] = sessionId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSession");
  }
  graphql::Json sessions(std::string_view appId, std::string_view status = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!status.empty()) vars["status"] = status;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSessions");
  }
  graphql::Json events(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEvents");
  }
  graphql::Json eventsConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEventsConnection");
  }

  // ----- Automations (admin-authored; diagnostics readable at runtime) -------

  graphql::Json upsertAutomation(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomation", input);
  }
  graphql::Json deleteAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelDeleteAutomation");
  }
  graphql::Json setAutomationEnabled(std::string_view appId, std::string_view name,
                                     bool enabled) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelSetAutomationEnabled");
  }
  graphql::Json upsertAutomationTrigger(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomationTrigger", input);
  }
  graphql::Json deleteAutomationTrigger(std::string_view appId, std::string_view triggerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelDeleteAutomationTrigger");
  }
  graphql::Json setAutomationPolicy(const graphql::JVal& input) const {
    return automations("GameModelSetAutomationPolicy", input);
  }
  graphql::Json runAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelRunAutomation");
  }
  graphql::Json automationsList(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomations");
  }
  graphql::Json automation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomation");
  }
  graphql::Json automationTriggers(std::string_view appId,
                                   std::string_view automationName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!automationName.empty()) vars["automationName"] = automationName;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationTriggers");
  }
  graphql::Json automationPolicy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationPolicy");
  }
  graphql::Json automationRuns(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationRuns");
  }
  graphql::Json automationStats(std::string_view appId, int windowMinutes = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationStats");
  }
  graphql::Json appDiagnostics(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAppDiagnostics");
  }

 private:
  graphql::Json studio(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, op);
  }
  graphql::Json studioByApp(std::string_view op, std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, op);
  }
  graphql::Json runtime(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, op);
  }
  graphql::Json automations(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, op);
  }
};

}  // namespace crowdy::domains
