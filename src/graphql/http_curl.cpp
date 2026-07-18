#include <curl/curl.h>

#include <mutex>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"

namespace crowdy::graphql {

namespace {

std::size_t writeCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(data, size * nmemb);
  return size * nmemb;
}

/// libcurl transport with a shared in-memory cookie jar (sticky
/// load-balancer cookies persist across requests within one transport).
class CurlTransport final : public IHttpTransport {
 public:
  CurlTransport() {
    static std::once_flag globalInit;
    std::call_once(globalInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    share_ = curl_share_init();
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, lockCb);
    curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, unlockCb);
    curl_share_setopt(share_, CURLSHOPT_USERDATA, this);
  }

  ~CurlTransport() override {
    if (share_) curl_share_cleanup(share_);
  }

  HttpResponse send(const HttpRequest& request) override {
    CURL* curl = curl_easy_init();
    if (!curl) throw CrowdyNetworkError("failed to initialize HTTP handle");

    std::string responseBody;
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : request.headers) {
      headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (request.method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
      if (!request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
      }
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
    curl_easy_setopt(curl, CURLOPT_SHARE, share_);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OPERATION_TIMEDOUT) {
      throw CrowdyTimeoutError("HTTP request timed out: " + request.url);
    }
    if (rc != CURLE_OK) {
      throw CrowdyNetworkError(std::string("HTTP request failed: ") + curl_easy_strerror(rc));
    }
    return HttpResponse{static_cast<int>(status), std::move(responseBody)};
  }

 private:
  static void lockCb(CURL*, curl_lock_data, curl_lock_access, void* userdata) {
    static_cast<CurlTransport*>(userdata)->shareMutex_.lock();
  }
  static void unlockCb(CURL*, curl_lock_data, void* userdata) {
    static_cast<CurlTransport*>(userdata)->shareMutex_.unlock();
  }

  CURLSH* share_ = nullptr;
  std::mutex shareMutex_;
};

}  // namespace

std::shared_ptr<IHttpTransport> makeCurlTransport() { return std::make_shared<CurlTransport>(); }

}  // namespace crowdy::graphql
