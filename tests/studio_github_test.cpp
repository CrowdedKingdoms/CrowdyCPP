#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/studio/github_layout.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::studio;

namespace {

const std::string SHA_A(40, 'a');
const std::string SHA_B(40, 'b');
const std::string SHA_C(40, 'c');

class NamedTransport final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  std::function<graphql::HttpResponse(const graphql::HttpRequest&)> handler;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (!handler) throw std::runtime_error("NamedTransport has no handler");
    return handler(request);
  }
};

std::string operationName(std::string_view body) {
  const std::string key = "\"operationName\":\"";
  const auto pos = body.find(key);
  if (pos == std::string_view::npos) return {};
  const auto start = pos + key.size();
  const auto end = body.find('"', start);
  return std::string(body.substr(start, end - start));
}

std::string jsonField(std::string_view body, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const auto pos = body.find(needle);
  if (pos == std::string_view::npos) return {};
  const auto start = pos + needle.size();
  const auto end = body.find('"', start);
  return std::string(body.substr(start, end - start));
}

bool hasJsonKey(std::string_view body, std::string_view key) {
  return body.find("\"" + std::string(key) + "\":") != std::string_view::npos;
}

graphql::HttpResponse dataResponse(std::string_view root,
                                   const std::string& value) {
  return {200, R"({"data":{")" + std::string(root) + "\":" + value + "}}"};
}

std::string fileJson(std::string_view target, std::string_view path,
                     std::string_view content) {
  return std::string(R"({"target":")") + std::string(target) +
         R"(","path":")" + std::string(path) + R"(","content":")" +
         std::string(content) +
         R"(","revision":"1","provenance":"AUTHORED","provenanceLibraryFileId":null,"provenanceLibraryRevision":null,"provenanceCommonVersionId":null,"createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:00Z"})";
}

std::string boundProjectJson(std::string_view revision, std::string_view sha,
                             std::string_view name = "Tools",
                             std::string_view serverContent = "fn server() {}",
                             bool includeClient = true) {
  std::string files = fileJson("SERVER", "src/lib.rs", serverContent);
  if (includeClient) {
    files += "," + fileJson("CLIENT", "src/lib.rs", "fn client() {}");
  }
  return std::string(R"({"projectId":"11111111-1111-4111-8111-111111111111","appId":"1","ownerUserId":"7","gridId":"2","name":")") +
         std::string(name) +
         R"(","description":null,"serverModuleName":"tools-server","clientModuleName":"tools-client","pairingPreference":"PAIRED","sdkVersion":"0.1.5","abiVersion":0,"revision":")" +
         std::string(revision) +
         R"(","archived":false,"archivedAt":null,"fileCount":)" +
         (includeClient ? "2" : "1") +
         R"(,"totalBytes":"28","source":"GITHUB","githubOwner":"modder","githubRepo":"my-mod","githubBranch":"main","githubSha":")" +
         std::string(sha) +
         R"(","createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:00Z","files":[)" +
         files + "]}";
}

const std::string kStatusJson =
    R"({"configured":true,"connected":true,"accountLogin":"modder","accountType":"User","owner":"modder","repo":"my-mod","branch":"main","githubSha":")" +
    SHA_A +
    R"(","installUrl":"https://github.com/settings/installations/1"})";

void testLayoutHelpers() {
  const CrowdyStudioGitHubLayoutRoots layout{"server", "client"};
  CHECK(studioFileToRepoPath(layout, CrowdyStudioTarget::Server,
                             "src/lib.rs") ==
        std::optional<std::string>{"server/src/lib.rs"});
  CHECK(studioFileToRepoPath(layout, CrowdyStudioTarget::Client,
                             "Cargo.toml") ==
        std::optional<std::string>{"client/Cargo.toml"});
  CHECK(studioFileToRepoPath({".", std::nullopt}, CrowdyStudioTarget::Server,
                             "src/lib.rs") ==
        std::optional<std::string>{"src/lib.rs"});
  CHECK(!studioFileToRepoPath({".", std::nullopt}, CrowdyStudioTarget::Client,
                              "src/lib.rs"));

  const auto clientFile =
      repoPathToStudioFile(layout, "client/src/lib.rs");
  CHECK(clientFile.has_value());
  CHECK(clientFile->first == CrowdyStudioTarget::Client);
  CHECK(clientFile->second == "src/lib.rs");
  const auto serverToml = repoPathToStudioFile(layout, "server/Cargo.toml");
  CHECK(serverToml.has_value());
  CHECK(serverToml->first == CrowdyStudioTarget::Server);
  CHECK(serverToml->second == "Cargo.toml");
  for (const char* path :
       {"README.md", "assets/mesh.glb", "server/Cargo.lock", "server/build.rs",
        "crowdy.json"}) {
    CHECK(!repoPathToStudioFile(layout, path));
  }
  const CrowdyStudioGitHubLayoutRoots nested{".", "client"};
  const auto nestedClient = repoPathToStudioFile(nested, "client/src/lib.rs");
  CHECK(nestedClient.has_value());
  CHECK(nestedClient->first == CrowdyStudioTarget::Client);
  const auto nestedServer = repoPathToStudioFile(nested, "src/lib.rs");
  CHECK(nestedServer.has_value());
  CHECK(nestedServer->first == CrowdyStudioTarget::Server);

  CHECK(!isRustAuthoringPath("src/../x.rs"));
  CHECK(isRustAuthoringPath("src/a/b.rs"));
  CHECK(joinRepo("/server/", "/src/lib.rs") == "server/src/lib.rs");
  CHECK(underRoot("server/src/lib.rs",
                  std::optional<std::string>{"server/"}) ==
        std::optional<std::string>{"src/lib.rs"});
  CHECK(!underRoot("client/src/lib.rs", std::optional<std::string>{"server"}));
}

void testGitHubTransportOperations() {
  auto transport = std::make_shared<NamedTransport>();
  transport->handler = [&](const graphql::HttpRequest& request) {
    const auto name = operationName(request.body);
    if (name == "CrowdyStudioGitHubStatus") {
      return dataResponse("crowdyStudioGitHubStatus", kStatusJson);
    }
    if (name == "CrowdyStudioGitHubConnectUrl") {
      return dataResponse(
          "crowdyStudioGitHubConnectUrl",
          R"({"connectUrl":"https://github.com/apps/crowdy-studio-dev/installations/new?state=x"})");
    }
    if (name == "CrowdyStudioGitHubRepos") {
      return dataResponse(
          "crowdyStudioGitHubRepos",
          R"([{"owner":"modder","name":"my-mod","fullName":"modder/my-mod","private":true,"defaultBranch":"main"}])");
    }
    if (name == "CrowdyStudioGitHubBind") {
      CHECK(jsonField(request.body, "initial") == "PUSH_PROJECT");
      CHECK(!hasJsonKey(request.body.substr(request.body.find("\"input\"")),
                        "owner") ||
            jsonField(request.body, "owner") == "modder");
      return dataResponse("crowdyStudioGitHubBind", kStatusJson);
    }
    if (name == "CrowdyStudioGitHubUnbind") {
      return dataResponse(
          "crowdyStudioGitHubUnbind",
          R"({"configured":true,"connected":true,"accountLogin":"modder","accountType":"User","owner":null,"repo":null,"branch":null,"githubSha":null,"installUrl":null})");
    }
    if (name == "CrowdyStudioGitHubRefresh") {
      auto refreshed = kStatusJson;
      refreshed.replace(refreshed.find(SHA_A), SHA_A.size(), SHA_B);
      return dataResponse("crowdyStudioGitHubRefresh", refreshed);
    }
    if (name == "CrowdyStudioGitHubLayout") {
      return dataResponse(
          "crowdyStudioGitHubLayout",
          R"({"commitSha":")" + SHA_A +
              R"(","server":"server","client":"client","assets":"assets","fromFile":true})");
    }
    if (name == "CrowdyStudioGitHubTree") {
      return dataResponse(
          "crowdyStudioGitHubTree",
          R"({"commitSha":")" + SHA_A +
              R"(","entries":[{"path":"server","type":"tree","sha":null,"size":null}]})");
    }
    if (name == "CrowdyStudioGitHubFile") {
      return dataResponse(
          "crowdyStudioGitHubFile",
          R"({"path":"server/src/lib.rs","content":"fn a(){}","sha":"abc","commitSha":")" +
              SHA_A + R"("})");
    }
    if (name == "CrowdyStudioGitHubPutFile") {
      CHECK(jsonField(request.body, "expectedCommitSha") == SHA_A);
      CHECK(!hasJsonKey(request.body, "sha") ||
            request.body.find("\"sha\":\"") == std::string::npos);
      return dataResponse(
          "crowdyStudioGitHubPutFile",
          R"({"path":"server/src/lib.rs","content":"fn b(){}","sha":"def","commitSha":")" +
              SHA_B + R"("})");
    }
    if (name == "CrowdyStudioGitHubDeleteFile") {
      auto status = kStatusJson;
      status.replace(status.find(SHA_A), SHA_A.size(), SHA_B);
      return dataResponse("crowdyStudioGitHubDeleteFile", status);
    }
    throw std::runtime_error("unexpected " + name);
  };

  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));
  auto& github = client.crowdyStudioGitHub();
  const CrowdyStudioGitHubProjectScope scope{"89", "p1"};

  CHECK(github.status("89", "p1").githubSha == SHA_A);
  CHECK(github.connectUrl().find("installations/new") != std::string::npos);
  CHECK(github.repos()[0].fullName == "modder/my-mod");
  CHECK(github.bind({scope.appId, scope.projectId, "modder", "my-mod",
                     std::nullopt,
                     crowdy::gen::CrowdyStudioGitHubBindInitial::PUSH_PROJECT})
            .branch == "main");
  CHECK(!github.unbind(scope).owner);
  CHECK(github.refresh(scope).githubSha == SHA_B);
  CHECK(github.layout(scope, SHA_A).server == "server");
  CHECK(github.tree(scope).entries[0].type == "tree");
  CHECK(github.getFile(scope, "server/src/lib.rs").commitSha == SHA_A);
  CHECK(github
            .putFile({scope.appId, scope.projectId, "server/src/lib.rs",
                      "fn b(){}", "m", SHA_A, std::nullopt})
            .commitSha == SHA_B);
  CHECK(github
            .deleteFile({scope.appId, scope.projectId, "server/src/old.rs",
                         "rm", SHA_B, std::nullopt})
            .githubSha == SHA_B);
}

struct BoundHarness {
  std::shared_ptr<NamedTransport> transport;
  std::unique_ptr<CrowdyClient> client;
  std::string remoteJson;
  std::string sha = SHA_A;
  bool staleOnPut = false;
  std::string stalePath;

  BoundHarness() {
    transport = std::make_shared<NamedTransport>();
    remoteJson = boundProjectJson("1", SHA_A);
    transport->handler = [this](const graphql::HttpRequest& request) {
      const auto name = operationName(request.body);
      if (name == "CrowdyStudioProject") {
        return dataResponse("crowdyStudioProject", remoteJson);
      }
      if (name == "CrowdyStudioProjectSave") {
        CHECK(request.body.find("\"upserts\":[]") != std::string::npos);
        CHECK(request.body.find("\"deletes\":[]") != std::string::npos);
        const auto nextName = jsonField(request.body, "name");
        remoteJson = boundProjectJson("2", sha, nextName.empty() ? "Tools"
                                                                : nextName);
        return dataResponse("crowdyStudioProjectSave", remoteJson);
      }
      if (name == "CrowdyStudioGitHubLayout") {
        return dataResponse(
            "crowdyStudioGitHubLayout",
            R"({"commitSha":")" + jsonField(request.body, "commitSha") +
                R"(","server":"server","client":"client","assets":"assets","fromFile":true})");
      }
      if (name == "CrowdyStudioGitHubPutFile") {
        const auto path = jsonField(request.body, "path");
        if (staleOnPut && path == stalePath) {
          return graphql::HttpResponse{
              200,
              R"({"errors":[{"message":"The project moved on since it was read.","extensions":{"code":"GITHUB_STALE_SHA"}}]})"};
        }
        CHECK_EQ(jsonField(request.body, "expectedCommitSha"), sha);
        CHECK(request.body.find("\"sha\":\"") == std::string::npos);
        sha = sha == SHA_A ? SHA_B : SHA_C;
        remoteJson = boundProjectJson("2", sha, "Tools renamed",
                                      "fn server_v2() {}");
        return dataResponse(
            "crowdyStudioGitHubPutFile",
            R"({"path":")" + path +
                R"(","content":"x","sha":"blob","commitSha":")" + sha +
                R"("})");
      }
      if (name == "CrowdyStudioGitHubDeleteFile") {
        CHECK_EQ(jsonField(request.body, "expectedCommitSha"), sha);
        sha = sha == SHA_A ? SHA_B : SHA_C;
        remoteJson = boundProjectJson("2", sha, "Tools renamed",
                                      "fn server_v2() {}", false);
        return dataResponse(
            "crowdyStudioGitHubDeleteFile",
            R"({"configured":true,"connected":true,"accountLogin":"modder","accountType":"User","owner":"modder","repo":"my-mod","branch":"main","githubSha":")" +
                sha + R"(","installUrl":null})");
      }
      throw std::runtime_error("unexpected " + name);
    };
    ClientConfig config;
    config.httpUrl = "https://game.invalid";
    config.transport = transport;
    client = std::make_unique<CrowdyClient>(std::move(config));
  }

  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const auto& request : transport->requests) {
      out.push_back(operationName(request.body));
    }
    return out;
  }
};

void testBoundSaveCommitsEachFile() {
  BoundHarness harness;
  const CrowdyStudioProjectScope scope{"1", "2"};
  const auto project = harness.client->crowdyStudio().getProject(
      scope, "11111111-1111-4111-8111-111111111111");
  CHECK(project.source == CrowdyStudioProjectSource::GitHub);
  CHECK(project.github && project.github->sha == SHA_A);

  CrowdyStudioProjectMetadata metadata = project.metadata;
  metadata.name = "Tools renamed";
  const auto saved = harness.client->crowdyStudio().saveProject({
      project.appId,
      "2",
      project.projectId,
      project.revision.id,
      metadata,
      {
          [] {
            CrowdyStudioProjectFile file;
            file.target = CrowdyStudioTarget::Server;
            file.path = "src/lib.rs";
            file.content = "fn server_v2() {}";
            return file;
          }(),
          [] {
            CrowdyStudioProjectFile file;
            file.target = CrowdyStudioTarget::Server;
            file.path = "src/extra.rs";
            file.content = "fn extra() {}";
            return file;
          }(),
      },
      project.sdkVersion,
      project.abiVersion,
      std::nullopt,
  });
  const auto names = harness.names();
  CHECK(names.size() == 7);
  CHECK(names[0] == "CrowdyStudioProject");
  CHECK(names[1] == "CrowdyStudioProjectSave");
  CHECK(names[2] == "CrowdyStudioGitHubLayout");
  CHECK(names[3] == "CrowdyStudioGitHubPutFile");
  CHECK(names[4] == "CrowdyStudioGitHubPutFile");
  CHECK(names[5] == "CrowdyStudioGitHubDeleteFile");
  CHECK(names[6] == "CrowdyStudioProject");
  CHECK(harness.transport->requests[1].body.find("\"name\":\"Tools renamed\"") !=
        std::string::npos);
  std::vector<std::string> putPaths;
  std::vector<std::string> putShas;
  for (const auto& request : harness.transport->requests) {
    if (operationName(request.body) == "CrowdyStudioGitHubPutFile") {
      putPaths.push_back(jsonField(request.body, "path"));
      putShas.push_back(jsonField(request.body, "expectedCommitSha"));
    }
  }
  std::sort(putPaths.begin(), putPaths.end());
  CHECK(putPaths.size() == 2);
  CHECK(putPaths[0] == "server/src/extra.rs");
  CHECK(putPaths[1] == "server/src/lib.rs");
  CHECK(putShas[0] == SHA_A);
  CHECK(putShas[1] == SHA_B);
  std::string deletePath;
  std::string deleteSha;
  for (const auto& request : harness.transport->requests) {
    if (operationName(request.body) == "CrowdyStudioGitHubDeleteFile") {
      deletePath = jsonField(request.body, "path");
      deleteSha = jsonField(request.body, "expectedCommitSha");
    }
  }
  CHECK(deletePath == "client/src/lib.rs");
  CHECK(deleteSha == SHA_C);
  CHECK(saved.github && saved.github->sha == harness.sha);
}

void testBoundSaveNoChangesOnlyRereads() {
  BoundHarness harness;
  const CrowdyStudioProjectScope scope{"1", "2"};
  const auto project = harness.client->crowdyStudio().getProject(
      scope, "11111111-1111-4111-8111-111111111111");
  (void)harness.client->crowdyStudio().saveProject({
      project.appId,
      "2",
      project.projectId,
      project.revision.id,
      project.metadata,
      project.files,
      project.sdkVersion,
      project.abiVersion,
      std::nullopt,
  });
  const auto names = harness.names();
  CHECK(names.size() == 2);
  CHECK(names[0] == "CrowdyStudioProject");
  CHECK(names[1] == "CrowdyStudioProject");
}

void testBoundSaveStaleRevisionRefusesBeforeCommit() {
  BoundHarness harness;
  const CrowdyStudioProjectScope scope{"1", "2"};
  const auto project = harness.client->crowdyStudio().getProject(
      scope, "11111111-1111-4111-8111-111111111111");
  CrowdyStudioProjectFile newer;
  newer.target = CrowdyStudioTarget::Server;
  newer.path = "src/lib.rs";
  newer.content = "fn newer() {}";
  (void)harness.client->crowdyStudio().saveProject({
      project.appId,
      "2",
      project.projectId,
      project.revision.id,
      project.metadata,
      {newer, project.files[1]},
      project.sdkVersion,
      project.abiVersion,
      std::nullopt,
  });
  const auto before = harness.transport->requests.size();
  CrowdyStudioProjectFile stale;
  stale.target = CrowdyStudioTarget::Server;
  stale.path = "src/lib.rs";
  stale.content = "fn stale() {}";
  bool conflict = false;
  try {
    (void)harness.client->crowdyStudio().saveProject({
        project.appId,
        "2",
        project.projectId,
        project.revision.id,
        project.metadata,
        {stale, project.files[1]},
        project.sdkVersion,
        project.abiVersion,
        std::nullopt,
    });
  } catch (const CrowdyStudioRevisionConflictError& error) {
    conflict = error.remoteProject() &&
               error.remoteProject()->source ==
                   CrowdyStudioProjectSource::GitHub;
  }
  CHECK(conflict);
  std::size_t puts = 0;
  for (std::size_t i = before; i < harness.transport->requests.size(); ++i) {
    if (operationName(harness.transport->requests[i].body) ==
        "CrowdyStudioGitHubPutFile") {
      ++puts;
    }
  }
  CHECK(puts == 0);
}

void testBoundSaveStaleShaBecomesRevisionConflict() {
  BoundHarness harness;
  harness.staleOnPut = true;
  harness.stalePath = "server/src/lib.rs";
  const CrowdyStudioProjectScope scope{"1", "2"};
  const auto project = harness.client->crowdyStudio().getProject(
      scope, "11111111-1111-4111-8111-111111111111");
  CrowdyStudioProjectFile changed;
  changed.target = CrowdyStudioTarget::Server;
  changed.path = "src/lib.rs";
  changed.content = "fn stale() {}";
  bool conflict = false;
  try {
    (void)harness.client->crowdyStudio().saveProject({
        project.appId,
        "2",
        project.projectId,
        project.revision.id,
        project.metadata,
        {changed, project.files[1]},
        project.sdkVersion,
        project.abiVersion,
        std::nullopt,
    });
  } catch (const CrowdyStudioRevisionConflictError& error) {
    conflict = error.remoteProject() &&
               error.remoteProject()->source ==
                   CrowdyStudioProjectSource::GitHub;
  }
  CHECK(conflict);
}

}  // namespace

int main() {
  testLayoutHelpers();
  testGitHubTransportOperations();
  testBoundSaveCommitsEachFile();
  testBoundSaveNoChangesOnlyRereads();
  testBoundSaveStaleRevisionRefusesBeforeCommit();
  testBoundSaveStaleShaBecomesRevisionConflict();
  std::printf("studio_github_test passed\n");
  return 0;
}
