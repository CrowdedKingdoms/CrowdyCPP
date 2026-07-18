#pragma once

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// client.serverStatus() — server discovery + capability bootstrap. Targets
/// the Game API with the APP-SCOPED token.
namespace crowdy::domains {

class ServerStatusAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Assign a replication server. NOT a passive lookup: as a side effect the
  /// Game API registers this client's UDP session with the chosen server. If
  /// this query errors, no session exists and every datagram is silently
  /// dropped. Wait ~1.5 s after this call before the first UDP send. See
  /// https://docs.crowdedkingdoms.com/replication-api/authenticate-and-assign.
  ServerAssignment serverWithLeastClients() const {
    return ServerAssignment::fromJson(
        execUnwrap(gen::serverStatus::kServerWithLeastClientsDocument));
  }

  graphql::Json listAll() const {
    return execUnwrap(gen::serverStatus::kGraphqlServersDocument);
  }

  graphql::Json listActiveGraphqlServers() const {
    return execUnwrap(gen::serverStatus::kActiveGraphQLServersDocument);
  }

  graphql::Json versionInfo() const {
    return execUnwrap(gen::serverStatus::kVersionInfoDocument);
  }

  /// Per-app bootstrap: version info, spatial limits (max distance/decay,
  /// sequence modulo), and profile — call once when entering an app.
  graphql::Json gameClientBootstrap(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::serverStatus::kGameClientBootstrapDocument, vars);
  }
};

}  // namespace crowdy::domains
