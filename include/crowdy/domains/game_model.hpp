#pragma once

#include <functional>
#include <utility>

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
  void seedAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelSeed", input, std::move(cb));
  }
  graphql::Json upsertContainerType(const graphql::JVal& input) const {
    return studio("GameModelUpsertContainerType", input);
  }
  void upsertContainerTypeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertContainerType", input, std::move(cb));
  }
  graphql::Json upsertPropertyDef(const graphql::JVal& input) const {
    return studio("GameModelUpsertPropertyDef", input);
  }
  void upsertPropertyDefAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertPropertyDef", input, std::move(cb));
  }
  graphql::Json deletePropertyDef(std::string_view appId, const graphql::JVal& args) const {
    graphql::JVal vars = args;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeletePropertyDef");
  }
  void deletePropertyDefAsync(std::string_view appId, const graphql::JVal& args,
                              graphql::GraphQLCallback cb) const {
    graphql::JVal vars = args;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeletePropertyDef",
                    std::move(cb));
  }
  graphql::Json deleteContainerType(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars,
                      "GameModelDeleteContainerType");
  }
  void deleteContainerTypeAsync(std::string_view appId, std::string_view typeName,
                                graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeleteContainerType",
                    std::move(cb));
  }
  graphql::Json upsertFunction(const graphql::JVal& input) const {
    return studio("GameModelUpsertFunction", input);
  }
  void upsertFunctionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertFunction", input, std::move(cb));
  }
  graphql::Json deleteFunction(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeleteFunction");
  }
  void deleteFunctionAsync(std::string_view appId, std::string_view name,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelDeleteFunction",
                    std::move(cb));
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return studio("GameModelSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelSetPolicy", input, std::move(cb));
  }
  graphql::Json defineFeature(const graphql::JVal& input) const {
    return studio("GameModelDefineFeature", input);
  }
  void defineFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelDefineFeature", input, std::move(cb));
  }
  graphql::Json grantTierFeature(const graphql::JVal& input) const {
    return studio("GameModelGrantTierFeature", input);
  }
  void grantTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelGrantTierFeature", input, std::move(cb));
  }
  graphql::Json revokeTierFeature(const graphql::JVal& input) const {
    return studio("GameModelRevokeTierFeature", input);
  }
  void revokeTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelRevokeTierFeature", input, std::move(cb));
  }

  // ----- Studio reads --------------------------------------------------------

  graphql::Json typeSchema(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTypeSchema");
  }
  void typeSchemaAsync(std::string_view appId, std::string_view typeName,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTypeSchema",
                    std::move(cb));
  }
  graphql::Json containerTypes(std::string_view appId) const {
    return studioByApp("GameModelContainerTypes", appId);
  }
  void containerTypesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelContainerTypes", appId, std::move(cb));
  }
  graphql::Json propertyDefs(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelPropertyDefs");
  }
  void propertyDefsAsync(std::string_view appId, std::string_view typeName,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelPropertyDefs",
                    std::move(cb));
  }
  graphql::Json function(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunction");
  }
  void functionAsync(std::string_view appId, std::string_view name,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunction",
                    std::move(cb));
  }
  graphql::Json functions(std::string_view appId, std::string_view containerTypeName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!containerTypeName.empty()) vars["containerTypeName"] = containerTypeName;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunctions");
  }
  void functionsAsync(std::string_view appId, std::string_view containerTypeName,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!containerTypeName.empty()) vars["containerTypeName"] = containerTypeName;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelFunctions",
                    std::move(cb));
  }
  graphql::Json features(std::string_view appId) const {
    return studioByApp("GameModelFeatures", appId);
  }
  void featuresAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelFeatures", appId, std::move(cb));
  }
  graphql::Json tierFeatures(std::string_view appId, std::string_view tierId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!tierId.empty()) vars["tierId"] = tierId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTierFeatures");
  }
  void tierFeaturesAsync(std::string_view appId, std::string_view tierId,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!tierId.empty()) vars["tierId"] = tierId;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, "GameModelTierFeatures",
                    std::move(cb));
  }
  graphql::Json policy(std::string_view appId) const {
    return studioByApp("GameModelPolicy", appId);
  }
  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelPolicy", appId, std::move(cb));
  }

  // ----- Runtime (player app token) ------------------------------------------

  graphql::Json createContainer(const graphql::JVal& input) const {
    return runtime("GameModelCreateContainer", input);
  }
  void createContainerAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelCreateContainer", input, std::move(cb));
  }
  graphql::Json deleteContainer(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteContainer");
  }
  void deleteContainerAsync(std::string_view appId, std::string_view containerId,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteContainer",
                    std::move(cb));
  }
  graphql::Json setProperty(const graphql::JVal& input) const {
    return runtime("GameModelSetProperty", input);
  }
  void setPropertyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelSetProperty", input, std::move(cb));
  }
  graphql::Json addEdge(const graphql::JVal& input) const {
    return runtime("GameModelAddEdge", input);
  }
  void addEdgeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelAddEdge", input, std::move(cb));
  }
  graphql::Json deleteEdge(std::string_view appId, std::string_view edgeId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["edgeId"] = edgeId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteEdge");
  }
  void deleteEdgeAsync(std::string_view appId, std::string_view edgeId,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["edgeId"] = edgeId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelDeleteEdge",
                    std::move(cb));
  }

  /// Transactional server-side mutation: invoke a seeded function. Invoke
  /// policies (owner_of_self, condition, is_host, is_current_turn, allow,
  /// and/or) are the authority model.
  graphql::Json invoke(const graphql::JVal& input) const {
    return runtime("GameModelInvoke", input);
  }
  void invokeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelInvoke", input, std::move(cb));
  }

  graphql::Json container(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainer");
  }
  void containerAsync(std::string_view appId, std::string_view containerId,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainer",
                    std::move(cb));
  }
  graphql::Json containers(std::string_view appId, std::string_view typeName = {},
                           std::string_view sessionId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainers");
  }
  void containersAsync(std::string_view appId, std::string_view typeName,
                       std::string_view sessionId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainers",
                    std::move(cb));
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
  void containersWhereAsync(std::string_view appId, std::string_view typeName,
                            const graphql::JVal& where, int limit, int offset,
                            std::string_view sessionId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!where.isNull()) vars["where"] = where;
    if (limit >= 0) vars["limit"] = limit;
    if (offset >= 0) vars["offset"] = offset;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainers",
                    std::move(cb));
  }
  graphql::Json containerState(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainerState");
  }
  void containerStateAsync(std::string_view appId, std::string_view containerId,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelContainerState",
                    std::move(cb));
  }
  graphql::Json traverse(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelTraverse");
  }
  void traverseAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelTraverse",
                    std::move(cb));
  }

  graphql::Json createSession(const graphql::JVal& input) const {
    return runtime("GameModelCreateSession", input);
  }
  void createSessionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelCreateSession", input, std::move(cb));
  }
  graphql::Json joinSession(const graphql::JVal& input) const {
    return runtime("GameModelJoinSession", input);
  }
  void joinSessionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelJoinSession", input, std::move(cb));
  }
  graphql::Json setSessionTurn(const graphql::JVal& input) const {
    return runtime("GameModelSetSessionTurn", input);
  }
  void setSessionTurnAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelSetSessionTurn", input, std::move(cb));
  }
  graphql::Json session(std::string_view appId, std::string_view sessionId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["sessionId"] = sessionId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSession");
  }
  void sessionAsync(std::string_view appId, std::string_view sessionId,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["sessionId"] = sessionId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSession",
                    std::move(cb));
  }
  graphql::Json sessions(std::string_view appId, std::string_view status = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!status.empty()) vars["status"] = status;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSessions");
  }
  void sessionsAsync(std::string_view appId, std::string_view status,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!status.empty()) vars["status"] = status;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelSessions",
                    std::move(cb));
  }
  graphql::Json events(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEvents");
  }
  void eventsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEvents",
                    std::move(cb));
  }
  graphql::Json eventsConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEventsConnection");
  }
  void eventsConnectionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelEventsConnection",
                    std::move(cb));
  }
  /// Diagnostics: stitch one flow correlation id (a UUID from the flowId
  /// field of a GmEvent, GmAutomationRun, or WasmModuleRun) into a single
  /// cross-engine timeline — the model events, automation runs, and compute
  /// module runs sharing the flowId minted at the entry edge, each array
  /// ordered by time ascending. Requires the app-admin manage_apps
  /// permission and a game-api with gameModelFlow (2026-07-19+; older
  /// servers reject the operation with a validation error). An unknown
  /// flowId returns three empty arrays.
  graphql::Json flow(std::string_view appId, std::string_view flowId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["flowId"] = flowId;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelFlow");
  }
  void flowAsync(std::string_view appId, std::string_view flowId,
                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["flowId"] = flowId;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, "GameModelFlow",
                    std::move(cb));
  }

  // ----- Automations (admin-authored; diagnostics readable at runtime) -------

  graphql::Json upsertAutomation(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomation", input);
  }
  void upsertAutomationAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelUpsertAutomation", input, std::move(cb));
  }
  graphql::Json deleteAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelDeleteAutomation");
  }
  void deleteAutomationAsync(std::string_view appId, std::string_view name,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelDeleteAutomation",
                    std::move(cb));
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
  void setAutomationEnabledAsync(std::string_view appId, std::string_view name, bool enabled,
                                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars,
                    "GameModelSetAutomationEnabled", std::move(cb));
  }
  graphql::Json upsertAutomationTrigger(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomationTrigger", input);
  }
  void upsertAutomationTriggerAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelUpsertAutomationTrigger", input, std::move(cb));
  }
  graphql::Json deleteAutomationTrigger(std::string_view appId, std::string_view triggerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelDeleteAutomationTrigger");
  }
  void deleteAutomationTriggerAsync(std::string_view appId, std::string_view triggerId,
                                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars,
                    "GameModelDeleteAutomationTrigger", std::move(cb));
  }
  graphql::Json setAutomationPolicy(const graphql::JVal& input) const {
    return automations("GameModelSetAutomationPolicy", input);
  }
  void setAutomationPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelSetAutomationPolicy", input, std::move(cb));
  }
  graphql::Json runAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelRunAutomation");
  }
  void runAutomationAsync(std::string_view appId, std::string_view name,
                          graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelRunAutomation",
                    std::move(cb));
  }
  graphql::Json automationsList(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomations");
  }
  void automationsListAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomations",
                    std::move(cb));
  }
  graphql::Json automation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomation");
  }
  void automationAsync(std::string_view appId, std::string_view name,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomation",
                    std::move(cb));
  }
  graphql::Json automationTriggers(std::string_view appId,
                                   std::string_view automationName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!automationName.empty()) vars["automationName"] = automationName;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationTriggers");
  }
  void automationTriggersAsync(std::string_view appId, std::string_view automationName,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!automationName.empty()) vars["automationName"] = automationName;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars,
                    "GameModelAutomationTriggers", std::move(cb));
  }
  graphql::Json automationPolicy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationPolicy");
  }
  void automationPolicyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomationPolicy",
                    std::move(cb));
  }
  graphql::Json automationRuns(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationRuns");
  }
  void automationRunsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomationRuns",
                    std::move(cb));
  }
  graphql::Json automationStats(std::string_view appId, int windowMinutes = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAutomationStats");
  }
  void automationStatsAsync(std::string_view appId, int windowMinutes,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAutomationStats",
                    std::move(cb));
  }
  graphql::Json appDiagnostics(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars,
                      "GameModelAppDiagnostics");
  }
  void appDiagnosticsAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, "GameModelAppDiagnostics",
                    std::move(cb));
  }

 private:
  graphql::Json studio(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, op);
  }
  void studioAsync(std::string_view op, const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, op, std::move(cb));
  }
  graphql::Json studioByApp(std::string_view op, std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::kGameModelStudioDocument, vars, op);
  }
  void studioByAppAsync(std::string_view op, std::string_view appId,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::kGameModelStudioDocument, vars, op, std::move(cb));
  }
  graphql::Json runtime(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelRuntimeDocument, vars, op);
  }
  void runtimeAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::kGameModelRuntimeDocument, vars, op, std::move(cb));
  }
  graphql::Json automations(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::kGameModelAutomationsDocument, vars, op);
  }
  void automationsAsync(std::string_view op, const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::kGameModelAutomationsDocument, vars, op, std::move(cb));
  }
};

}  // namespace crowdy::domains
