// The balancer's affinity cookie (cks_ga) and the jar the HTTP transport keeps it in.
//
// A game balancer pins a client to one ck-api with cks_ga. When that instance drains,
// it answers with the cookie EXPIRED (Max-Age=0), so the client's next request carries
// none and the balancer places it on a live instance. CrowdyJS clears its store on
// SERVER_DRAINING as well (18.24.0); here the move rides libcurl's cookie engine, which
// the transport enables on a cookie-sharing handle -- so what must hold is that the
// engine drops an expired cookie instead of sending it on. This pins that, on the same
// kind of handle, without a network.
#include <curl/curl.h>

#include <ctime>
#include <string>
#include <vector>

#include "test_util.hpp"

namespace {

std::vector<std::string> cookiesIn(CURL* curl) {
  std::vector<std::string> out;
  struct curl_slist* list = nullptr;
  if (curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &list) != CURLE_OK) return out;
  for (struct curl_slist* it = list; it; it = it->next) out.emplace_back(it->data);
  curl_slist_free_all(list);
  return out;
}

// A cks_ga the engine would SEND: libcurl keeps an expired cookie listed (empty value,
// expiry 1) and purges it before building a request's Cookie header, so "held" means a
// value that is a session cookie or not yet expired.
bool holdsAffinity(CURL* curl) {
  for (const auto& line : cookiesIn(curl)) {
    // Netscape format: domain, tailmatch, path, secure, expiry, name, value.
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (std::size_t tab = line.find('\t'); tab != std::string::npos;
         tab = line.find('\t', start)) {
      fields.push_back(line.substr(start, tab - start));
      start = tab + 1;
    }
    fields.push_back(line.substr(start));
    if (fields.size() < 7 || fields[5] != "cks_ga" || fields[6].empty()) continue;
    const long long expires = std::stoll(fields[4]);
    if (expires == 0 || expires > static_cast<long long>(std::time(nullptr))) return true;
  }
  return false;
}

void testAnExpiredAffinityCookieLeavesTheJar() {
  CURLSH* share = curl_share_init();
  curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
  CURL* curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(curl, CURLOPT_SHARE, share);

  curl_easy_setopt(
      curl, CURLOPT_COOKIELIST,
      "Set-Cookie: cks_ga=0123456789abcdef; Domain=ck-or.dev.crowdedkingdoms.com; "
      "Path=/; HttpOnly; Secure; SameSite=None");
  CHECK(holdsAffinity(curl));

  // What a draining ck-api sends (cks-game-api lb-affinity.ts).
  curl_easy_setopt(
      curl, CURLOPT_COOKIELIST,
      "Set-Cookie: cks_ga=; Domain=ck-or.dev.crowdedkingdoms.com; Path=/; Max-Age=0; "
      "Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly; Secure; SameSite=None");
  CHECK(!holdsAffinity(curl));

  curl_easy_cleanup(curl);
  curl_share_cleanup(share);
}

void testTheJarIsSharedAcrossHandles() {
  // The transport makes a handle per request; the share is what carries the cookie
  // from the response that set it to the next request, and its expiry likewise.
  CURLSH* share = curl_share_init();
  curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
  CURL* first = curl_easy_init();
  curl_easy_setopt(first, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(first, CURLOPT_SHARE, share);
  curl_easy_setopt(first, CURLOPT_COOKIELIST,
                   "Set-Cookie: cks_ga=fedcba9876543210; "
                   "Domain=ck-or.dev.crowdedkingdoms.com; Path=/");
  curl_easy_cleanup(first);

  CURL* second = curl_easy_init();
  curl_easy_setopt(second, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(second, CURLOPT_SHARE, share);
  CHECK(holdsAffinity(second));
  curl_easy_setopt(second, CURLOPT_COOKIELIST,
                   "Set-Cookie: cks_ga=; Domain=ck-or.dev.crowdedkingdoms.com; "
                   "Path=/; Max-Age=0");
  curl_easy_cleanup(second);

  CURL* third = curl_easy_init();
  curl_easy_setopt(third, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(third, CURLOPT_SHARE, share);
  CHECK(!holdsAffinity(third));
  curl_easy_cleanup(third);
  curl_share_cleanup(share);
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  testAnExpiredAffinityCookieLeavesTheJar();
  testTheJarIsSharedAcrossHandles();
  curl_global_cleanup();
  std::puts("lb_affinity_cookie_test OK");
  return 0;
}
