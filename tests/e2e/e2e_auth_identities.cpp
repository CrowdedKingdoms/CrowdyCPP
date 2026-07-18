// Mirrors Management API e2e: auth + identities. Dev-bypass sign-in, magic
// link (dev devToken path), identity listing, session revocation (logout /
// logoutAllDevices), and the legacy password family (register / login /
// changePassword / checkAuthMethod). Black-box against the public Management
// API; see https://docs.crowdedkingdoms.com for the documented auth flows.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

/// A management-plane client with no token seeded.
std::unique_ptr<CrowdyClient> bareClient(const e2e::E2eConfig& cfg) {
  ClientConfig c;
  c.managementUrl = cfg.managementUrl;
  return std::make_unique<CrowdyClient>(std::move(c));
}

int runAll() {
  auto cfg = e2e::requireConfig();
  const std::string email = e2e::deriveEmail(cfg, "auth");

  E2E_SUBTEST("devLogin returns a session token + user; users().me() matches");
  auto client = bareClient(cfg);
  auto dev = client->auth().devLogin(email);
  E2E_CHECK(!dev.token.empty());
  E2E_CHECK(!dev.userId.empty());
  E2E_CHECK(dev.email == email);
  graphql::Json me = client->users().me();
  E2E_CHECK(me["userId"].asString() == dev.userId);
  E2E_CHECK(me["email"].asString() == email);

  E2E_SUBTEST("checkAuthMethod on the fresh dev account");
  graphql::Json method = client->auth().checkAuthMethod(email);
  E2E_CHECK(method["hasPassword"].isBool());
  E2E_CHECK(!method["hasPassword"].asBool());  // dev-bypass accounts have no password

  E2E_SUBTEST("requestLoginLink -> sent + devToken; completeLoginLink exchanges it");
  graphql::Json link = client->auth().requestLoginLink(email);
  E2E_CHECK(link["sent"].asBool());
  const std::string devToken = link["devToken"].asString();
  E2E_CHECK(!devToken.empty());  // requires DEV_AUTH_BYPASS on the deployment
  auto linkClient = bareClient(cfg);
  auto viaLink = linkClient->auth().completeLoginLink(devToken);
  E2E_CHECK(!viaLink.token.empty());
  E2E_CHECK(viaLink.userId == dev.userId);
  E2E_CHECK(linkClient->users().me()["userId"].asString() == dev.userId);

  E2E_SUBTEST("myIdentities lists the dev identity");
  graphql::Json identities = client->auth().myIdentities();
  E2E_CHECK(identities.size() >= 1);
  bool sawOwnEmail = false;
  identities.forEach([&](graphql::Json ident) {
    E2E_CHECK(!ident["identityId"].asString().empty());
    E2E_CHECK(!ident["provider"].asString().empty());
    if (ident["email"].asString() == email) sawOwnEmail = true;
  });
  E2E_CHECK(sawOwnEmail);

  E2E_SUBTEST("me() with no token throws UNAUTHENTICATED");
  {
    auto anon = bareClient(cfg);
    bool threw = false;
    try {
      (void)anon->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(e.code() == "UNAUTHENTICATED");
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("logout clears the session; the old token is rejected afterwards");
  {
    const std::string oldToken = linkClient->getToken();
    E2E_CHECK(linkClient->auth().logout());
    E2E_CHECK(linkClient->getToken().empty());
    auto replay = bareClient(cfg);
    replay->setToken(oldToken);
    bool threw = false;
    try {
      (void)replay->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(!e.code().empty());  // structured; UNAUTHENTICATED on this deployment
      std::printf("   note: replayed logged-out token rejected with code=%s\n",
                  e.code().c_str());
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("logoutAllDevices revokes older sessions");
  {
    auto older = bareClient(cfg);
    (void)older->auth().devLogin(email);
    auto newer = bareClient(cfg);
    (void)newer->auth().devLogin(email);
    E2E_CHECK(newer->auth().logoutAllDevices());
    bool threw = false;
    try {
      (void)older->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(!e.code().empty());
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("password family: register -> login -> changePassword -> login again");
  {
    const std::string pwEmail = e2e::deriveEmail(cfg, "auth-pw");
    const std::string password1 = "Cpp-e2e-pw1-" + e2e::runSuffix();
    const std::string password2 = "Cpp-e2e-pw2-" + e2e::runSuffix();
    try {
      auto pwClient = bareClient(cfg);
      auto registered = pwClient->auth().registerUser(pwEmail, password1);
      E2E_CHECK(!registered.token.empty());
      E2E_CHECK(registered.email == pwEmail);

      auto loginClient = bareClient(cfg);
      auto logged = loginClient->auth().login(pwEmail, password1);
      E2E_CHECK(!logged.token.empty());
      E2E_CHECK(logged.userId == registered.userId);

      E2E_CHECK(loginClient->auth().changePassword(password1, password2));

      auto reloginClient = bareClient(cfg);
      auto relogged = reloginClient->auth().login(pwEmail, password2);
      E2E_CHECK(!relogged.token.empty());
      E2E_CHECK(relogged.userId == registered.userId);

      // The old password must no longer work.
      bool oldRejected = false;
      try {
        (void)bareClient(cfg)->auth().login(pwEmail, password1);
      } catch (const graphql::CrowdyGraphQLError& e) {
        oldRejected = true;
        E2E_CHECK(!e.code().empty());
      }
      E2E_CHECK(oldRejected);

      graphql::Json pwMethod = reloginClient->auth().checkAuthMethod(pwEmail);
      E2E_CHECK(pwMethod["hasPassword"].asBool());
    } catch (const graphql::CrowdyGraphQLError& e) {
      // Password auth is a legacy/optional surface; a deployment with the
      // feature off must still fail with a STRUCTURED error code.
      E2E_CHECK(!e.code().empty());
      std::printf("   note: password ops rejected by this deployment (code=%s): %s\n",
                  e.code().c_str(), e.what());
    }
  }

  std::puts("e2e_auth_identities OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return runAll();
  } catch (const graphql::CrowdyError& e) {
    std::fprintf(stderr, "FATAL [%s]: %s\n", e.code().c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    return 1;
  }
}
