// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same
// stack Zetta serves with (zetta/src/core/api/http_server.cpp). This is the
// ONE translation unit that knows about beast2 / boost::http / Boost.JSON.
// Everything it serves is a pure renderer from web_ui.hpp or a value from
// version.hpp — the handlers only stamp headers and send bytes.

#include "lake/api/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/beast2/http_server.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/http/field.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/cors.hpp>
#include <boost/http/server/router.hpp>
#include <boost/json.hpp>

#include "lake/api/web_ui.hpp"
#include "lake/version.hpp"

namespace lake::api {
namespace {

// Beast2 computes the framing from the payload size we declare; anything a
// handler set by hand would race it. Same rule as Zetta.
void stamp_response_headers(boost::http::route_params& route, std::string_view body) {
    route.res.erase(boost::http::field::etag);
    route.res.erase(boost::http::field::content_length);
    route.res.set_payload_size(body.size());
}

boost::http::route_task send_text(boost::http::route_params& route, boost::http::status status,
                                  std::string body, std::string_view content_type,
                                  std::string_view cache_control) {
    namespace http = boost::http;
    route.status(status);
    route.res.set(http::field::content_type, content_type);
    route.res.set(http::field::cache_control, cache_control);
    stamp_response_headers(route, body);
    auto [send_error] = co_await route.send(body);
    if (send_error) co_return http::route_error(send_error);
    co_return http::route_done;
}

}  // namespace

struct ApiServer::Impl {
    ServerOptions options;
    boost::corosio::io_context io_context;
    std::unique_ptr<boost::beast2::http_server> server;
    std::thread io_thread;
    std::atomic<bool> is_running{false};
    unsigned short bound_port = 0;
    std::string failure_reason;

    // Owner-loop plumbing. Nothing is marshalled onto the owner thread yet
    // (hello world), so wait_for_work() is a plain timed park and drain() is
    // a no-op; the API is kept so the loop shape matches Zetta from day one.
    std::mutex work_mutex;
    std::condition_variable work_cv;

    explicit Impl(ServerOptions opts) : options(std::move(opts)) {}

    // GET / — the hello page, rendered fresh per request (it carries the
    // node name and version; no-store so a redeploy is visible on reload).
    boost::http::route_task serve_web_root(boost::http::route_params& route) {
        return send_text(route, boost::http::status::ok, web_ui_hello(options.node_name, version_string()),
                         "text/html; charset=utf-8", "no-store");
    }

    // GET /style.css — the stylesheet, built once; only changes with the binary.
    boost::http::route_task serve_style(boost::http::route_params& route) {
        static const std::string css = web_ui_stylesheet();
        return send_text(route, boost::http::status::ok, css, "text/css; charset=utf-8", "public, max-age=86400");
    }

    // GET /v1/server_info — the first REST capability: who am I.
    boost::http::route_task serve_server_info(boost::http::route_params& route) {
        boost::json::object info;
        info["name"] = "lake";
        info["node"] = options.node_name;
        info["version"] = std::string(version_string());
        info["api"] = "v1";
        return send_text(route, boost::http::status::ok, boost::json::serialize(info),
                         "application/json; charset=utf-8", "no-store");
    }

    // Anything no route claimed. The router falls through (route_next) past
    // the last handler and nothing answers otherwise — a request must never
    // hang, so this trailing middleware terminates every miss with a 404.
    boost::http::route_task serve_not_found(boost::http::route_params& route) {
        boost::json::object err;
        err["error"] = "not_found";
        return send_text(route, boost::http::status::not_found, boost::json::serialize(err),
                         "application/json; charset=utf-8", "no-store");
    }
};

ApiServer::ApiServer(ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start() {
    namespace http = boost::http;
    namespace corosio = boost::corosio;
    namespace beast2 = boost::beast2;
    Impl& impl = *impl_;
    if (impl.is_running.load()) return true;
    impl.failure_reason.clear();

    http::router<http::route_params> router;
    router.use(http::cors());
    router.add(http::method::get, "/",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_web_root(route);
               });
    router.add(http::method::get, "/style.css",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_style(route);
               });
    router.add(http::method::get, "/v1/server_info",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_server_info(route);
               });
    router.use([&impl](http::route_params& route) -> http::route_task {
        return impl.serve_not_found(route);
    });

    impl.server = std::make_unique<beast2::http_server>(
        impl.io_context, static_cast<std::size_t>(impl.options.worker_coroutines),
        std::move(router), http::make_parser_config(http::parser_config(true)),
        http::make_serializer_config(http::serializer_config()));

    corosio::ipv4_address address(impl.options.listen_address);
    auto bind_error = impl.server->bind(corosio::endpoint(address, impl.options.port));
    if (bind_error) {
        impl.failure_reason = "bind failed: " + bind_error.message();
        impl.server.reset();
        return false;
    }
    impl.bound_port = impl.server->local_endpoint().port();
    impl.server->start();
    impl.io_thread = std::thread([&impl] { impl.io_context.run(); });
    impl.is_running.store(true);
    return true;
}

void ApiServer::stop() {
    Impl& impl = *impl_;
    if (!impl.is_running.exchange(false)) return;
    impl.server->stop();
    impl.io_context.stop();
    if (impl.io_thread.joinable()) impl.io_thread.join();
    impl.work_cv.notify_all();
}

bool ApiServer::running() const { return impl_->is_running.load(); }
unsigned short ApiServer::port() const { return impl_->bound_port; }
const std::string& ApiServer::reason() const { return impl_->failure_reason; }

void ApiServer::drain() {}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
    Impl& impl = *impl_;
    std::unique_lock lock(impl.work_mutex);
    impl.work_cv.wait_for(lock, max_idle, [&impl] { return !impl.is_running.load(); });
}

}  // namespace lake::api
