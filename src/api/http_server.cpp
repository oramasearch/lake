// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same stack Zetta serves with
// (zetta/src/core/api/http_server.cpp). This is the ONE translation unit that knows about
// beast2 / boost::http / Boost.JSON. It serves the hello page, the registry's REST rendering
// (/v1/<capability>), the OpenAPI document and MCP (POST /mcp); every capability call goes
// through Registry::call, so REST and MCP authorize and validate identically.

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
#include <vector>

#include <boost/beast2/http_server.hpp>
#include <boost/capy/io/push_to.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/http/field.hpp>
#include <boost/http/json/json_sink.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/cors.hpp>
#include <boost/http/server/router.hpp>
#include <boost/json.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/version.hpp"

namespace lake::api {
namespace {

// Beast2 computes the framing from the payload size we declare; anything a handler set by
// hand would race it. Same rule as Zetta.
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

// The bearer presented on a request, or empty ("Bearer <token>" only).
std::string bearer_of(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::authorization) == 0) return {};
    std::string_view header_value = route.req.at(boost::http::field::authorization);
    if (!header_value.starts_with("Bearer ")) return {};
    return std::string(header_value.substr(7));
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

nlohmann::json to_nlohmann(const boost::json::value& value) {
    return nlohmann::json::parse(boost::json::serialize(value), nullptr, false);
}

std::string error_body(std::string_view code, std::string_view message) {
    return nlohmann::json{{"ok", false}, {"error", {{"code", code}, {"message", message}, {"path", ""}}}}.dump();
}

}  // namespace

struct ApiServer::Impl {
    ServerOptions options;
    Registry registry;
    std::string openapi_body;
    boost::corosio::io_context io_context;
    std::unique_ptr<boost::beast2::http_server> server;
    std::thread io_thread;
    std::atomic<bool> is_running{false};
    unsigned short bound_port = 0;
    std::string failure_reason;
    std::mutex work_mutex;
    std::condition_variable work_cv;

    explicit Impl(ServerOptions opts) : options(std::move(opts)) {
        register_core_capabilities(registry, version_string());
        openapi_body = openapi_document(registry, version_string()).dump();
    }

    ApiContext context_for(const boost::http::route_params& route) const {
        ApiContext ctx;
        ctx.store = options.store.get();
        ctx.node_name = options.node_name;
        const std::string bearer = bearer_of(route);
        if (!options.admin_token.empty() && !bearer.empty() && constant_time_equal(bearer, options.admin_token)) {
            ctx.authorized = true;
            ctx.acting_identity = "admin";
        }
        return ctx;
    }

    boost::http::route_task serve_web_root(boost::http::route_params& route) {
        return send_text(route, boost::http::status::ok, web_ui_hello(options.node_name, version_string()),
                         "text/html; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_style(boost::http::route_params& route) {
        static const std::string css = web_ui_stylesheet();
        return send_text(route, boost::http::status::ok, css, "text/css; charset=utf-8", "public, max-age=86400");
    }

    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        return send_text(route, boost::http::status::ok, openapi_body, "application/json; charset=utf-8", "no-store");
    }

    // One REST rendering of a capability: GET reads (input in the query string, coerced by the
    // input schema), POST actions (input as the JSON body). Auth, validation, the call and the
    // envelope are Registry::call + rest_*, exactly the MCP path.
    boost::http::route_task serve_capability(boost::http::route_params& route,
                                             std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;
        nlohmann::json input = nlohmann::json::object();
        std::string body;
        http::status status = http::status::ok;

        if (route.req.method() == http::method::get) {
            std::vector<std::pair<std::string, std::string>> query;
            for (const auto& parameter : route.url.params()) query.emplace_back(parameter.key, parameter.value);
            input = rest_input_from_query(*capability, query);
        } else {
            http::json_sink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error) {
                status = http::status::bad_request;
                body = error_body("invalid_input", "could not read the request body as JSON");
            } else if (bytes_read > 0) {
                input = to_nlohmann(sink.release());
                if (input.is_discarded()) {
                    status = http::status::bad_request;
                    body = error_body("invalid_input", "the request body is not valid JSON");
                }
            }
        }
        if (body.empty()) {
            ApiContext ctx = context_for(route);
            const ApiResult result = registry.call(capability->name, input, ctx);
            status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
            body = rest_envelope(result).dump();
        }
        route.status(status);
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /mcp — JSON-RPC 2.0 over HTTP. Notifications get 202 and no body.
    boost::http::route_task serve_mcp(boost::http::route_params& route) {
        namespace http = boost::http;
        std::string body;
        http::status status = http::status::ok;
        http::json_sink sink;
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        nlohmann::json request;
        if (read_error || bytes_read == 0 || (request = to_nlohmann(sink.release())).is_discarded()) {
            status = http::status::bad_request;
            body = nlohmann::json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}.dump();
        } else {
            ApiContext ctx = context_for(route);
            const auto response = mcp_dispatch(registry, request, ctx, version_string());
            if (!response) {
                status = http::status::accepted;
            } else {
                body = *response;
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // Anything no route claimed. The router falls through (route_next) past the last handler and
    // nothing answers otherwise — a request must never hang, so this trailing middleware
    // terminates every miss with a 404.
    boost::http::route_task serve_not_found(boost::http::route_params& route) {
        return send_text(route, boost::http::status::not_found, error_body("not_found", "no such route"),
                         "application/json; charset=utf-8", "no-store");
    }
};

ApiServer::ApiServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

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
    router.add(http::method::get, "/", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_root(route);
    });
    router.add(http::method::get, "/style.css", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_style(route);
    });
    router.add(http::method::get, "/v1/openapi.json", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_openapi(route);
    });
    for (const auto& capability : impl.registry.list()) {
        const auto method = rest_method(*capability) == "GET" ? http::method::get : http::method::post;
        router.add(method, rest_path(*capability),
                   [&impl, capability](http::route_params& route) -> http::route_task {
                       return impl.serve_capability(route, capability);
                   });
    }
    router.add(http::method::post, "/mcp", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_mcp(route);
    });
    router.use([&impl](http::route_params& route) -> http::route_task { return impl.serve_not_found(route); });

    impl.server = std::make_unique<beast2::http_server>(
        impl.io_context, static_cast<std::size_t>(impl.options.worker_coroutines), std::move(router),
        http::make_parser_config(http::parser_config(true)), http::make_serializer_config(http::serializer_config()));

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
