#include <exception>
#include <utility>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"

namespace crowdy::graphql {

HttpOutcome IHttpTransport::sendOutcome(const HttpRequest& request) noexcept {
  HttpOutcome out;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    out.response = send(request);
    out.status = Errc::Ok;
  } catch (const CrowdyTimeoutError& error) {
    out.status = Errc::Timeout;
    out.errorMessage = error.what();
  } catch (const std::exception& error) {
    out.status = Errc::SocketError;
    out.errorMessage = error.what();
  } catch (...) {
    out.status = Errc::SocketError;
    out.errorMessage = "HTTP transport failed with an unknown exception";
  }
#else
  out.response = send(request);
  out.status = Errc::Ok;
#endif
  return out;
}

namespace {

class InlineAsyncTransport final : public IAsyncHttpTransport {
 public:
  explicit InlineAsyncTransport(std::shared_ptr<IHttpTransport> sync) : sync_(std::move(sync)) {}

  void sendAsync(const HttpRequest& request, std::function<void(HttpOutcome)> cb) override {
    cb(sync_ ? sync_->sendOutcome(request)
             : HttpOutcome{Errc::NotConnected, {},
                           "No HTTP transport is available"});
  }

 private:
  std::shared_ptr<IHttpTransport> sync_;
};

}  // namespace

std::shared_ptr<IAsyncHttpTransport> makeInlineAsyncTransport(
    std::shared_ptr<IHttpTransport> sync) {
  return std::make_shared<InlineAsyncTransport>(std::move(sync));
}

}  // namespace crowdy::graphql
