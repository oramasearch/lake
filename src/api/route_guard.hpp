#pragma once

// The last-resort answer for a route handler that throws (lake#440).
//
// Beast2 drops the connection when a handler's task ends in an exception, so the client sees a
// reset (a 502 behind nginx) and lake logs no response line. guard_route wraps a handler: an
// exception it throws, while being called or while its task runs, is logged as one JSON line
// with the route and request id, and the request is answered 500 — an HTML error page when the
// client asked for HTML, the lake JSON error envelope otherwise.
//
// This header names Boost.HTTP types, so it stays out of include/: only the Beast2 adapter
// (http_server.cpp) and its tests include it.

#include <chrono>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include <boost/http/field.hpp>
#include <boost/http/server/router.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/web_ui.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

struct RouteFailure {
    std::string route;      // the registered route pattern
    std::string method;
    std::string path;       // for the handler's caller; never logged (decision 54 S9)
    std::string request_id; // the caller's X-Request-Id, or a new one
    std::string message;    // what the exception said; logged, never sent to the client
};

// Called after the 500 has been sent, so the server can count and log the response.
using RouteFailureObserver =
    std::function<void(boost::http::route_params&, const RouteFailure&)>;

inline bool route_failure_wants_html(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::accept) == 0) {
        return false;
    }
    return std::string_view(route.req.at(boost::http::field::accept)).find("text/html") !=
           std::string_view::npos;
}

inline RouteFailure route_failure(const boost::http::route_params& route,
                                  std::string_view pattern,
                                  std::string message) {
    RouteFailure failure;
    failure.route = std::string(pattern);
    failure.method = std::string(route.req.method_text());
    failure.path = std::string(route.url.path());
    failure.request_id = route.req.count("x-request-id") == 0
                             ? control::new_uuid_v4()
                             : std::string(route.req.at("x-request-id"));
    failure.message = std::move(message);
    return failure;
}

// One JSON line, beside the request log lines, so the failure is found by route or request id.
inline std::string route_failure_log_line(const RouteFailure& failure) {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return nlohmann::json{{"ts", std::format("{:%FT%T}Z", now)},
                          {"level", "error"},
                          {"event", "handler_exception"},
                          {"method", failure.method},
                          {"route", failure.route},
                          {"request_id", failure.request_id},
                          {"error", failure.message}}
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline void log_route_failure(const RouteFailure& failure) {
    std::println("{}", route_failure_log_line(failure));
}

inline boost::http::route_task answer_route_failure(boost::http::route_params& route,
                                                    RouteFailure failure) {
    namespace http = boost::http;
    const std::string message =
        "Lake could not answer this request. Request id: " + failure.request_id;
    const bool html = route_failure_wants_html(route);
    const std::string body =
        html ? web_ui_error("500", message)
             : nlohmann::json{{"ok", false},
                              {"error",
                               {{"code", "internal_error"}, {"message", message}, {"path", ""}}}}
                   .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    // A handler may have set fields before it threw: start the response again.
    route.res.clear();
    route.res.set_start_line(http::status::internal_server_error, route.req.version());
    route.res.set_keep_alive(route.req.keep_alive());
    route.res.set(http::field::content_type,
                  html ? "text/html; charset=utf-8" : "application/json; charset=utf-8");
    route.res.set(http::field::cache_control, "no-store");
    route.res.set("X-Request-Id", failure.request_id);
    route.res.set_payload_size(body.size());
    auto [error] = co_await route.send(body);
    if (error) {
        co_return http::route_error(error);
    }
    co_return http::route_done;
}

namespace detail {

template <class Handler>
boost::http::route_task run_guarded_route(std::shared_ptr<Handler> handler,
                                          std::shared_ptr<const std::string> pattern,
                                          std::shared_ptr<const RouteFailureObserver> observer,
                                          boost::http::route_params& route) {
    std::string message;
    try {
        co_return co_await (*handler)(route);
    } catch (const std::exception& error) {
        message = error.what();
    } catch (...) {
        message = "unknown exception";
    }
    RouteFailure failure = route_failure(route, *pattern, std::move(message));
    log_route_failure(failure);
    auto result = co_await answer_route_failure(route, failure);
    if (*observer) {
        (*observer)(route, failure);
    }
    co_return result;
}

} // namespace detail

// Wraps `handler` so an exception from it is answered 500 instead of dropping the connection.
template <class Handler>
auto guard_route(std::string pattern, Handler handler, RouteFailureObserver observer = {}) {
    auto shared_handler = std::make_shared<Handler>(std::move(handler));
    auto shared_pattern = std::make_shared<const std::string>(std::move(pattern));
    auto shared_observer = std::make_shared<const RouteFailureObserver>(std::move(observer));
    return [shared_handler, shared_pattern, shared_observer](
               boost::http::route_params& route) -> boost::http::route_task {
        return detail::run_guarded_route(shared_handler, shared_pattern, shared_observer, route);
    };
}

} // namespace lake::api
