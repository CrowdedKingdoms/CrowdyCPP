#include <exception>
#include <utility>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"

namespace crowdy::graphql {

namespace {

class InlineAsyncTransport final : public IAsyncHttpTransport {
 public:
  explicit InlineAsyncTransport(std::shared_ptr<IHttpTransport> sync) : sync_(std::move(sync)) {}

  void sendAsync(const HttpRequest& request, std::function<void(HttpOutcome)> cb) override {
    HttpOutcome out;
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      out.response = sync_->send(request);
      out.status = Errc::Ok;
    } catch (const CrowdyTimeoutError& e) {
      out.status = Errc::Timeout;
      out.errorMessage = e.what();
    } catch (const std::exception& e) {
      out.status = Errc::SocketError;
      out.errorMessage = e.what();
    }
#else
    out.response = sync_->send(request);
    out.status = Errc::Ok;
#endif
    cb(std::move(out));
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
