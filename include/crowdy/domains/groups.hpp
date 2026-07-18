#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.teams() and client.channels() — app-scoped groups (membership,
/// roles, join policies). Teams are player groups (guilds/parties); channels
/// are messaging groups whose realtime delivery happens over the replication
/// client. Both target the Game API.
namespace crowdy::domains {

/// client.teams()
class TeamsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kMyTeamsDocument, vars);
  }

  graphql::Json list(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kTeamsDocument, vars);
  }

  graphql::Json get(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamDocument, vars);
  }

  graphql::Json members(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamMembersDocument, vars);
  }

  graphql::Json roles(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamRolesDocument, vars);
  }

  graphql::Json policy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kTeamPolicyDocument, vars);
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kCreateTeamDocument, vars);
  }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kUpdateTeamDocument, vars);
  }

  /// Destructive; pass an idempotencyKey for safe retries.
  graphql::Json remove(std::string_view groupId, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::teams::kDeleteTeamDocument, vars);
  }

  graphql::Json setPolicy(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kSetTeamPolicyDocument, vars);
  }

  graphql::Json join(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kJoinTeamDocument, vars);
  }

  graphql::Json requestToJoin(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kRequestToJoinTeamDocument, vars);
  }

  graphql::Json leave(std::string_view groupId, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::teams::kLeaveTeamDocument, vars);
  }

  graphql::Json addMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::teams::kAddTeamMemberDocument, vars);
  }

  graphql::Json removeMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::teams::kRemoveTeamMemberDocument, vars);
  }

  graphql::Json setMemberRoles(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kSetTeamMemberRolesDocument, vars);
  }

  graphql::Json createRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kCreateTeamRoleDocument, vars);
  }

  graphql::Json updateRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kUpdateTeamRoleDocument, vars);
  }

  graphql::Json deleteRole(std::string_view groupRoleId) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    return execUnwrap(gen::teams::kDeleteTeamRoleDocument, vars);
  }
};

/// client.channels()
class ChannelsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kMyChannelsDocument, vars);
  }

  graphql::Json list(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kChannelsDocument, vars);
  }

  graphql::Json get(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelDocument, vars);
  }

  graphql::Json members(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelMembersDocument, vars);
  }

  graphql::Json roles(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelRolesDocument, vars);
  }

  graphql::Json policy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kChannelPolicyDocument, vars);
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kCreateChannelDocument, vars);
  }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kUpdateChannelDocument, vars);
  }

  graphql::Json remove(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kDeleteChannelDocument, vars);
  }

  graphql::Json setPolicy(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kSetChannelPolicyDocument, vars);
  }

  graphql::Json join(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kJoinChannelDocument, vars);
  }

  graphql::Json requestToJoin(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kRequestToJoinChannelDocument, vars);
  }

  graphql::Json leave(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kLeaveChannelDocument, vars);
  }

  graphql::Json addMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::channels::kAddChannelMemberDocument, vars);
  }

  graphql::Json removeMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::channels::kRemoveChannelMemberDocument, vars);
  }

  graphql::Json setMemberRoles(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kSetChannelMemberRolesDocument, vars);
  }

  graphql::Json createRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kCreateChannelRoleDocument, vars);
  }

  graphql::Json updateRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kUpdateChannelRoleDocument, vars);
  }

  graphql::Json deleteRole(std::string_view groupRoleId) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    return execUnwrap(gen::channels::kDeleteChannelRoleDocument, vars);
  }
};

}  // namespace crowdy::domains
