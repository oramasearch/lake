// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same stack
// Zetta serves with (zetta/src/core/api/http_server.cpp). This is the ONE
// translation unit that knows about beast2 / boost::http / Boost.JSON. It
// serves the hello page, the registry's REST rendering
// (/v1/<capability>), the OpenAPI document and MCP (POST /mcp); every
// capability call goes through Registry::call, so REST and MCP authorize and
// validate identically.

#include "lake/api/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <print>
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
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/version.hpp"

namespace lake::api {
namespace {

// Beast2 computes the framing from the payload size we declare; anything a
// handler set by hand would race it. Same rule as Zetta.
void stamp_response_headers(boost::http::route_params &route,
                            std::string_view body) {
  route.res.erase(boost::http::field::etag);
  route.res.erase(boost::http::field::content_length);
  route.res.set_payload_size(body.size());
}

boost::http::route_task send_text(boost::http::route_params &route,
                                  boost::http::status status, std::string body,
                                  std::string_view content_type,
                                  std::string_view cache_control) {
  namespace http = boost::http;
  route.status(status);
  route.res.set(http::field::content_type, content_type);
  route.res.set(http::field::cache_control, cache_control);
  stamp_response_headers(route, body);
  auto [send_error] = co_await route.send(body);
  if (send_error)
    co_return http::route_error(send_error);
  co_return http::route_done;
}

// The bearer presented on a request, or empty ("Bearer <token>" only).
std::string bearer_of(const boost::http::route_params &route) {
  if (route.req.count(boost::http::field::authorization) == 0)
    return {};
  std::string_view header_value =
      route.req.at(boost::http::field::authorization);
  if (!header_value.starts_with("Bearer "))
    return {};
  return std::string(header_value.substr(7));
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

// A capy WriteSink that collects a request body into a std::string — for the
// form-urlencoded bodies the server-rendered sign-in posts (json_sink parses
// JSON, which form bodies are not).
struct StringBodySink {
  std::string data;
  template <boost::capy::ConstBufferSequence CB>
  boost::capy::immediate<boost::capy::io_result<std::size_t>>
  write_some(const CB &buffers) {
    std::size_t total = 0;
    for (auto it = boost::capy::begin(buffers); it != boost::capy::end(buffers);
         ++it) {
      const boost::capy::const_buffer buffer(*it);
      data.append(static_cast<const char *>(buffer.data()), buffer.size());
      total += buffer.size();
    }
    return {{{}, total}};
  }
  template <boost::capy::ConstBufferSequence CB>
  boost::capy::immediate<boost::capy::io_result<std::size_t>>
  write(const CB &buffers) {
    return write_some(buffers);
  }
  boost::capy::immediate<boost::capy::io_result<>> write_eof() { return {{}}; }
};

std::string cookie_value(const boost::http::route_params &route,
                         std::string_view name) {
  if (route.req.count(boost::http::field::cookie) == 0)
    return {};
  std::string_view header = route.req.at(boost::http::field::cookie);
  std::size_t pos = 0;
  while (pos < header.size()) {
    const std::size_t semi = header.find(';', pos);
    std::string_view pair = header.substr(pos, semi == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : semi - pos);
    while (!pair.empty() && pair.front() == ' ')
      pair.remove_prefix(1);
    const std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == name)
      return std::string(pair.substr(eq + 1));
    if (semi == std::string_view::npos)
      break;
    pos = semi + 1;
  }
  return {};
}

// Percent-decode an application/x-www-form-urlencoded token ('+' → space, %XX →
// byte).
std::string form_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 &&
               hex(in[i + 2]) >= 0) {
      out += static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2]));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

std::string form_field(std::string_view body, std::string_view name) {
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t amp = body.find('&', pos);
    std::string_view pair =
        body.substr(pos, amp == std::string_view::npos ? std::string_view::npos
                                                       : amp - pos);
    const std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos && form_decode(pair.substr(0, eq)) == name)
      return form_decode(pair.substr(eq + 1));
    if (amp == std::string_view::npos)
      break;
    pos = amp + 1;
  }
  return {};
}

bool request_is_https(const boost::http::route_params &route) {
  return route.req.count("x-forwarded-proto") != 0 &&
         std::string_view(route.req.at("x-forwarded-proto")) == "https";
}

std::string query_value(const boost::http::route_params &route,
                        std::string_view name) {
  for (const auto &parameter : route.url.params()) {
    if (parameter.key == name)
      return std::string(parameter.value);
  }
  return {};
}

nlohmann::json to_nlohmann(const boost::json::value &value) {
  return nlohmann::json::parse(boost::json::serialize(value), nullptr, false);
}

std::string error_body(std::string_view code, std::string_view message) {
  return nlohmann::json{
      {"ok", false},
      {"error", {{"code", code}, {"message", message}, {"path", ""}}}}
      .dump();
}

std::string log_timestamp() {
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  return std::format("{:%FT%T}Z", now);
}

} // namespace

struct ApiServer::Impl {
  ServerOptions options;
  telemetry::Metrics metrics;
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
    metrics.set_gauge("lake_build_info",
                      {{"version", std::string(version_string())},
                       {"node", options.node_name}},
                      1.0);
    metrics.set_gauge("lake_up", {}, 0.0);
    options.store =
        telemetry::instrument_store(std::move(options.store), metrics);
  }

  void record_request(const boost::http::route_params &route,
                      std::string_view route_pattern,
                      std::chrono::steady_clock::time_point start) {
    const double duration_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    const std::string method(route.req.method_text());
    const std::string status = std::to_string(route.res.status_int());
    const telemetry::Labels labels{{"method", method},
                                   {"route", std::string(route_pattern)},
                                   {"status", status}};
    metrics.increment_counter("lake_http_requests_total", labels);
    metrics.observe_histogram("lake_http_request_duration_seconds", labels,
                              duration_seconds);

    std::println("{}",
                 nlohmann::json{{"ts", log_timestamp()},
                                {"method", method},
                                {"path", std::string(route.url.path())},
                                {"route", route_pattern},
                                {"status", route.res.status_int()},
                                {"duration_ms", duration_seconds * 1000.0},
                                {"node", options.node_name}}
                     .dump());
  }

  boost::http::route_task observe_request(boost::http::route_params &route,
                                          std::string route_pattern,
                                          boost::http::route_task task) {
    const auto start = std::chrono::steady_clock::now();
    auto result = co_await std::move(task);
    record_request(route, route_pattern, start);
    co_return result;
  }

  std::string route_pattern_for(const boost::http::route_params &route) const {
    static constexpr std::string_view fixed_routes[] = {
        "/",
        "/style.css",
        "/healthz",
        "/readyz",
        "/metrics",
        "/login",
        "/session",
        "/logout",
        "/customers",
        "/repositories",
        "/repository",
        "/search",
        "/v1/openapi.json",
        "/mcp",
    };
    const std::string path(route.url.path());
    for (const std::string_view pattern : fixed_routes) {
      if (path == pattern)
        return std::string(pattern);
    }
    for (const auto &capability : registry.list()) {
      const std::string pattern = rest_path(*capability);
      if (path == pattern)
        return pattern;
    }
    return "unmatched";
  }

  ApiContext context_for(const boost::http::route_params &route) const {
    ApiContext ctx;
    ctx.store = options.store.get();
    ctx.search = options.search.get();
    ctx.node_name = options.node_name;
    // Authorization: the bearer wins; else the HttpOnly session cookie (its
    // value IS the admin token, validated identically), so the server-rendered
    // pages authenticate with the cookie set by POST /session and the token
    // never touches the page.
    std::string bearer = bearer_of(route);
    if (bearer.empty())
      bearer = cookie_value(route, "lake_session");
    if (!options.admin_token.empty() && !bearer.empty() &&
        constant_time_equal(bearer, options.admin_token)) {
      ctx.authorized = true;
      ctx.acting_identity = "admin";
    }
    return ctx;
  }

  boost::http::route_task serve_web_root(boost::http::route_params &route) {
    return send_text(route, boost::http::status::ok,
                     web_ui_hello(options.node_name, version_string(),
                                  context_for(route).authorized),
                     "text/html; charset=utf-8", "no-store");
  }

  boost::http::route_task redirect(boost::http::route_params &route,
                                   std::string_view location,
                                   std::string_view set_cookie = {}) {
    namespace http = boost::http;
    route.status(http::status::found);
    route.res.set(http::field::location, location);
    if (!set_cookie.empty())
      route.res.set(http::field::set_cookie, set_cookie);
    route.res.set(http::field::cache_control, "no-store");
    stamp_response_headers(route, "");
    auto [send_error] = co_await route.send("");
    if (send_error)
      co_return http::route_error(send_error);
    co_return http::route_done;
  }

  boost::http::route_task serve_login(boost::http::route_params &route) {
    if (context_for(route).authorized)
      return redirect(route, "/repositories");
    return send_text(route, boost::http::status::ok, web_ui_login(""),
                     "text/html; charset=utf-8", "no-store");
  }

  // POST /session — the sign-in form. A matching token becomes the HttpOnly
  // lake_session cookie.
  boost::http::route_task serve_session(boost::http::route_params &route) {
    namespace http = boost::http;
    StringBodySink sink;
    auto [read_error, bytes_read] =
        co_await boost::capy::push_to(route.req_body, sink);
    if (read_error)
      co_return http::route_error(read_error);
    const std::string token = form_field(sink.data, "token");
    const bool ok = !options.admin_token.empty() && !token.empty() &&
                    constant_time_equal(token, options.admin_token);
    if (!ok) {
      const std::string body = web_ui_login("That token did not match.");
      route.status(http::status::unauthorized);
      route.res.set(http::field::content_type, "text/html; charset=utf-8");
      route.res.set(http::field::cache_control, "no-store");
      stamp_response_headers(route, body);
      auto [send_error] = co_await route.send(body);
      if (send_error)
        co_return http::route_error(send_error);
      co_return http::route_done;
    }
    std::string cookie =
        "lake_session=" + token + "; HttpOnly; SameSite=Strict; Path=/";
    if (request_is_https(route))
      cookie += "; Secure";
    co_return co_await redirect(route, "/repositories", cookie);
  }

  boost::http::route_task serve_logout(boost::http::route_params &route) {
    return redirect(
        route, "/",
        "lake_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
  }

  boost::http::route_task
  serve_customers_page(boost::http::route_params &route) {
    const ApiContext ctx = context_for(route);
    if (!ctx.authorized)
      return redirect(route, "/login");
    std::vector<control::Customer> customers;
    if (ctx.store) {
      if (auto rows = ctx.store->list_customers(500))
        customers = std::move(*rows);
    }
    return send_text(route, boost::http::status::ok,
                     web_ui_customers(customers), "text/html; charset=utf-8",
                     "no-store");
  }

  boost::http::route_task
  serve_repositories_page(boost::http::route_params &route) {
    const ApiContext ctx = context_for(route);
    if (!ctx.authorized)
      return redirect(route, "/login");
    std::vector<control::Repository> repositories;
    std::map<std::string, control::Customer> owners;
    if (ctx.store) {
      if (auto rows = ctx.store->list_repositories(
              query_value(route, "customer_id"), 500))
        repositories = std::move(*rows);
      if (auto customers = ctx.store->list_customers(1000)) {
        for (auto &c : *customers)
          owners.emplace(c.id, std::move(c));
      }
    }
    return send_text(route, boost::http::status::ok,
                     web_ui_repositories(repositories, owners),
                     "text/html; charset=utf-8", "no-store");
  }

  boost::http::route_task
  serve_repository_page(boost::http::route_params &route) {
    const ApiContext ctx = context_for(route);
    if (!ctx.authorized)
      return redirect(route, "/login");
    const std::string id = query_value(route, "id");
    if (!ctx.store || id.empty())
      return redirect(route, "/repositories");
    auto repository = ctx.store->get_repository(id);
    if (!repository) {
      return send_text(route, boost::http::status::not_found,
                       error_body("not_found", repository.error().message),
                       "application/json; charset=utf-8", "no-store");
    }
    auto owner = ctx.store->get_customer(repository->customer_id);
    return send_text(route, boost::http::status::ok,
                     web_ui_repository(*repository, owner ? &*owner : nullptr),
                     "text/html; charset=utf-8", "no-store");
  }

  boost::http::route_task serve_search_page(boost::http::route_params &route) {
    const ApiContext ctx = context_for(route);
    if (!ctx.authorized) {
      return redirect(route, "/login");
    }
    const std::string query = query_value(route, "query");
    std::vector<search::Hit> hits;
    if (!query.empty() && ctx.search) {
      const auto results = ctx.search->query(
          search::Query{query, "", "", "", 100}, ctx.acting_identity);
      if (results) {
        hits = *results;
      }
    }
    return send_text(route, boost::http::status::ok, web_ui_search(query, hits),
                     "text/html; charset=utf-8", "no-store");
  }

  boost::http::route_task serve_style(boost::http::route_params &route) {
    static const std::string css = web_ui_stylesheet();
    return send_text(route, boost::http::status::ok, css,
                     "text/css; charset=utf-8", "public, max-age=86400");
  }

  boost::http::route_task serve_openapi(boost::http::route_params &route) {
    return send_text(route, boost::http::status::ok, openapi_body,
                     "application/json; charset=utf-8", "no-store");
  }

  boost::http::route_task serve_healthz(boost::http::route_params &route) {
    return send_text(route, boost::http::status::ok, R"({"ok":true})",
                     "application/json; charset=utf-8", "no-store");
  }

  boost::http::route_task serve_readyz(boost::http::route_params &route) {
    if (!options.store) {
      return send_text(route, boost::http::status::service_unavailable,
                       error_body("store_unavailable",
                                  "no control-plane store is configured"),
                       "application/json; charset=utf-8", "no-store");
    }
    if (auto result = options.store->ping(); !result) {
      return send_text(route, boost::http::status::service_unavailable,
                       error_body("store_unavailable", result.error().message),
                       "application/json; charset=utf-8", "no-store");
    }
    return send_text(
        route, boost::http::status::ok,
        nlohmann::json{{"ok", true}, {"store", options.store->kind()}}.dump(),
        "application/json; charset=utf-8", "no-store");
  }

  boost::http::route_task serve_metrics(boost::http::route_params &route) {
    if (!context_for(route).authorized) {
      return send_text(
          route, boost::http::status::forbidden,
          error_body("unauthorized", "a valid admin bearer token is required"),
          "application/json; charset=utf-8", "no-store");
    }
    return send_text(route, boost::http::status::ok, metrics.prometheus_text(),
                     "text/plain; version=0.0.4", "no-store");
  }

  // One REST rendering of a capability: GET reads (input in the query string,
  // coerced by the input schema), POST actions (input as the JSON body). Auth,
  // validation, the call and the envelope are Registry::call + rest_*, exactly
  // the MCP path.
  boost::http::route_task
  serve_capability(boost::http::route_params &route,
                   std::shared_ptr<const Capability> capability) {
    namespace http = boost::http;
    nlohmann::json input = nlohmann::json::object();
    std::string body;
    http::status status = http::status::ok;

    if (route.req.method() == http::method::get) {
      std::vector<std::pair<std::string, std::string>> query;
      for (const auto &parameter : route.url.params())
        query.emplace_back(parameter.key, parameter.value);
      input = rest_input_from_query(*capability, query);
    } else {
      http::json_sink sink;
      auto [read_error, bytes_read] =
          co_await boost::capy::push_to(route.req_body, sink);
      if (read_error) {
        status = http::status::bad_request;
        body = error_body("invalid_input",
                          "could not read the request body as JSON");
      } else if (bytes_read > 0) {
        input = to_nlohmann(sink.release());
        if (input.is_discarded()) {
          status = http::status::bad_request;
          body =
              error_body("invalid_input", "the request body is not valid JSON");
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
    if (send_error)
      co_return http::route_error(send_error);
    co_return http::route_done;
  }

  // POST /mcp — JSON-RPC 2.0 over HTTP. Notifications get 202 and no body.
  boost::http::route_task serve_mcp(boost::http::route_params &route) {
    namespace http = boost::http;
    std::string body;
    http::status status = http::status::ok;
    http::json_sink sink;
    auto [read_error, bytes_read] =
        co_await boost::capy::push_to(route.req_body, sink);
    nlohmann::json request;
    if (read_error || bytes_read == 0 ||
        (request = to_nlohmann(sink.release())).is_discarded()) {
      status = http::status::bad_request;
      body = nlohmann::json{{"jsonrpc", "2.0"},
                            {"id", nullptr},
                            {"error",
                             {{"code", -32700}, {"message", "parse error"}}}}
                 .dump();
    } else {
      ApiContext ctx = context_for(route);
      const auto response =
          mcp_dispatch(registry, request, ctx, version_string());
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
    if (send_error)
      co_return http::route_error(send_error);
    co_return http::route_done;
  }

  // Anything no route claimed. The router falls through (route_next) past the
  // last handler and nothing answers otherwise — a request must never hang, so
  // this trailing middleware terminates every miss with a 404.
  boost::http::route_task serve_not_found(boost::http::route_params &route) {
    return send_text(route, boost::http::status::not_found,
                     error_body("not_found", "no such route"),
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
  Impl &impl = *impl_;
  if (impl.is_running.load())
    return true;
  impl.failure_reason.clear();

  http::router<http::route_params> router;
  router.use([&impl, cors = http::cors()](
                 http::route_params &route) -> http::route_task {
    const auto start = std::chrono::steady_clock::now();
    auto result = co_await cors(route);
    if (result.what() != http::route_what::next) {
      impl.record_request(route, impl.route_pattern_for(route), start);
    }
    co_return result;
  });
  router.add(http::method::get, "/",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/",
                                           impl.serve_web_root(route));
             });
  router.add(http::method::get, "/style.css",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/style.css",
                                           impl.serve_style(route));
             });
  router.add(http::method::get, "/healthz",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/healthz",
                                           impl.serve_healthz(route));
             });
  router.add(http::method::get, "/readyz",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/readyz",
                                           impl.serve_readyz(route));
             });
  router.add(http::method::get, "/metrics",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/metrics",
                                           impl.serve_metrics(route));
             });
  router.add(http::method::get, "/login",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/login",
                                           impl.serve_login(route));
             });
  router.add(http::method::post, "/session",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/session",
                                           impl.serve_session(route));
             });
  router.add(http::method::post, "/logout",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/logout",
                                           impl.serve_logout(route));
             });
  router.add(http::method::get, "/customers",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/customers",
                                           impl.serve_customers_page(route));
             });
  router.add(http::method::get, "/repositories",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/repositories",
                                           impl.serve_repositories_page(route));
             });
  router.add(http::method::get, "/repository",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/repository",
                                           impl.serve_repository_page(route));
             });
  router.add(http::method::get, "/search",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/search",
                                           impl.serve_search_page(route));
             });
  router.add(http::method::get, "/v1/openapi.json",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/v1/openapi.json",
                                           impl.serve_openapi(route));
             });
  for (const auto &capability : impl.registry.list()) {
    const auto method = rest_method(*capability) == "GET" ? http::method::get
                                                          : http::method::post;
    const std::string path = rest_path(*capability);
    router.add(method, path,
               [&impl, capability,
                path](http::route_params &route) -> http::route_task {
                 return impl.observe_request(
                     route, path, impl.serve_capability(route, capability));
               });
  }
  router.add(http::method::post, "/mcp",
             [&impl](http::route_params &route) -> http::route_task {
               return impl.observe_request(route, "/mcp",
                                           impl.serve_mcp(route));
             });
  router.use([&impl](http::route_params &route) -> http::route_task {
    return impl.observe_request(route, "unmatched",
                                impl.serve_not_found(route));
  });

  impl.server = std::make_unique<beast2::http_server>(
      impl.io_context, static_cast<std::size_t>(impl.options.worker_coroutines),
      std::move(router), http::make_parser_config(http::parser_config(true)),
      http::make_serializer_config(http::serializer_config()));

  corosio::ipv4_address address(impl.options.listen_address);
  auto bind_error =
      impl.server->bind(corosio::endpoint(address, impl.options.port));
  if (bind_error) {
    impl.failure_reason = "bind failed: " + bind_error.message();
    impl.server.reset();
    return false;
  }
  impl.bound_port = impl.server->local_endpoint().port();
  impl.server->start();
  impl.io_thread = std::thread([&impl] { impl.io_context.run(); });
  impl.is_running.store(true);
  impl.metrics.set_gauge("lake_up", {}, 1.0);
  return true;
}

void ApiServer::stop() {
  Impl &impl = *impl_;
  if (!impl.is_running.exchange(false))
    return;
  impl.metrics.set_gauge("lake_up", {}, 0.0);
  impl.server->stop();
  impl.io_context.stop();
  if (impl.io_thread.joinable())
    impl.io_thread.join();
  impl.work_cv.notify_all();
}

bool ApiServer::running() const { return impl_->is_running.load(); }
unsigned short ApiServer::port() const { return impl_->bound_port; }
const std::string &ApiServer::reason() const { return impl_->failure_reason; }

void ApiServer::drain() {}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
  Impl &impl = *impl_;
  std::unique_lock lock(impl.work_mutex);
  impl.work_cv.wait_for(lock, max_idle,
                        [&impl] { return !impl.is_running.load(); });
}

} // namespace lake::api
