#pragma once
// lake#534: lake's HTTP connections, decoupled from the requests being handled.
//
// Beast2's http_server runs a fixed pool of workers, each owning one connection for its whole
// life, so a client holding idle keep-alive connections held every worker and no one else could
// connect. This follows Zetta's server (zetta/src/core/api/http_server.cpp): lake accepts every
// connection up to `max_connections` and serves each with its own session coroutine, answering
// `503` with `Connection: close` above the cap. On top of that:
//
// - a request takes one of `workers` slots only while its route is dispatched, so an idle
//   connection, or one still sending its header, holds no worker; a request that finds every slot
//   taken waits `worker_wait` for one and is then answered `503` and its connection closed;
// - a connection waiting for its next request is closed after `idle_timeout`;
// - a request header must arrive whole within `header_timeout` of its first byte (of the accept,
//   for a connection's first request), so a client trickling a header (slowloris) is cut off;
// - a response with status 503 or 429 closes its connection, so a refused client gives the
//   connection back while it waits to retry.
//
// Every member of the shared state belongs to the IO executor (one thread runs the io_context):
// the accept loop, the sessions and the worker queue never run concurrently. stop() and
// begin_drain() hand their work to that executor. Only http_server.cpp and the tests include this
// header; no beast2 or boost::http type reaches lake's public headers.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <boost/beast2/http_worker.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/delay.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/any_buffer_sink.hpp>
#include <boost/capy/io/any_buffer_source.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/timeout.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/http/error.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/router.hpp>
#include <boost/url/parse.hpp>

#include "lake/telemetry/metrics.hpp"

namespace lake::api {

struct HttpConnectionLimits {
    // Requests dispatched at once (LAKE_HTTP_WORKERS). A streaming response holds its worker until
    // it ends.
    std::size_t workers = 32;
    // Open connections, idle ones included (LAKE_HTTP_MAX_CONNECTIONS). Above it a new connection
    // is answered 503 and closed at once.
    std::size_t max_connections = 4096;
    // How long a keep-alive connection waits for its next request (LAKE_HTTP_IDLE_TIMEOUT_SECONDS).
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(30)};
    // How long a request header may take, from its first byte, or from the accept for a
    // connection's first request (LAKE_HTTP_HEADER_TIMEOUT_SECONDS).
    std::chrono::milliseconds header_timeout{std::chrono::seconds(10)};
    // How long a request waits for a free worker before it is answered 503
    // (LAKE_HTTP_WORKER_WAIT_SECONDS).
    std::chrono::milliseconds worker_wait{std::chrono::seconds(10)};
    // Pending kernel handshakes, not admitted connections; the OS may clamp it.
    int listen_backlog = 1024;
    // Paths answered without taking a worker: the probes, so a saturated lake is not restarted.
    std::function<bool(std::string_view path)> needs_no_worker;
};

struct HttpConnectionStats {
    std::size_t connections = 0;
    std::size_t active_requests = 0;
    std::size_t waiting_requests = 0;
    std::uint64_t refused_connections = 0;
    std::uint64_t idle_timeouts = 0;
    std::uint64_t header_timeouts = 0;
    std::uint64_t busy_refusals = 0;
};

namespace detail {

// The serializer's sink, closing the connection of a 503 or 429 response before its header is
// written. Every response body leaves through it, so no send helper has to remember.
class RefusalClosingSink {
public:
    using Inner = boost::http::serializer::sink<boost::corosio::tcp_socket>;

    RefusalClosingSink(Inner inner, boost::http::response& response)
        : inner_(inner), response_(&response) {}

    void begin_response() {
        checked_ = false;
    }

    std::span<boost::capy::mutable_buffer> prepare(std::span<boost::capy::mutable_buffer> dest) {
        check();
        return inner_.prepare(dest);
    }
    auto commit(std::size_t n) {
        check();
        return inner_.commit(n);
    }
    auto commit_eof(std::size_t n) {
        check();
        return inner_.commit_eof(n);
    }
    template <class ConstBufferSequence> auto write(ConstBufferSequence const& buffers) {
        check();
        return inner_.write(buffers);
    }
    template <class ConstBufferSequence> auto write_eof(ConstBufferSequence const& buffers) {
        check();
        return inner_.write_eof(buffers);
    }
    auto write_eof() {
        check();
        return inner_.write_eof();
    }

private:
    // Only before the serializer starts: the header must not change once it is being written.
    void check() {
        if (checked_) {
            return;
        }
        checked_ = true;
        const auto status = response_->status();
        if ((status == boost::http::status::service_unavailable ||
             status == boost::http::status::too_many_requests) &&
            response_->keep_alive()) {
            response_->set_keep_alive(false);
        }
    }

    Inner inner_;
    boost::http::response* response_;
    bool checked_ = false;
};

} // namespace detail

class HttpConnections {
public:
    HttpConnections(boost::corosio::io_context& io_context,
                    boost::http::router<boost::http::route_params> router,
                    boost::http::shared_parser_config parser_config,
                    boost::http::shared_serializer_config serializer_config,
                    HttpConnectionLimits limits,
                    telemetry::Metrics* metrics = nullptr)
        : state_(std::make_shared<State>(io_context,
                                         std::move(router),
                                         std::move(parser_config),
                                         std::move(serializer_config),
                                         std::move(limits),
                                         metrics)) {}

    HttpConnections(const HttpConnections&) = delete;
    HttpConnections& operator=(const HttpConnections&) = delete;

    // Opens, binds and listens. An empty string on success, otherwise why not.
    std::string bind(const boost::corosio::endpoint& endpoint) {
        State& state = *state_;
        if (state.limits.workers == 0 || state.limits.max_connections == 0 ||
            state.limits.listen_backlog <= 0 || state.limits.idle_timeout.count() <= 0 ||
            state.limits.header_timeout.count() <= 0 || state.limits.worker_wait.count() <= 0) {
            return "HTTP workers, max_connections, listen_backlog and the idle, header and worker "
                   "wait timeouts must be positive";
        }
        state.acceptor.open();
        state.acceptor.set_option(boost::corosio::socket_option::reuse_address(true));
        auto error = state.acceptor.bind(endpoint);
        if (!error) {
            error = state.acceptor.listen(state.limits.listen_backlog);
        }
        if (error) {
            state.acceptor.close();
            return "bind failed: " + error.message();
        }
        state.port = state.acceptor.local_endpoint().port();
        return {};
    }

    [[nodiscard]] unsigned short port() const {
        return state_->port;
    }

    // Starts the accept loop on the IO executor; the io_context's thread runs it.
    void start() {
        state_->running.store(true);
        state_->live.fetch_add(1);
        boost::capy::run_async(state_->io_context.get_executor())(accept_connections(state_));
    }

    // At the drain (lake#536): connections waiting for their next request are closed; the
    // requests in progress finish, and their responses close their connections.
    void begin_drain() {
        if (state_->draining.exchange(true)) {
            return;
        }
        boost::capy::run_async(state_->io_context.get_executor())(close_idle(state_));
    }

    // Stops accepting, closes every connection and refuses every waiting request, then waits up
    // to `wait` for the sessions to unwind on the still-running io_context. The caller stops the
    // io_context afterwards.
    void stop(std::chrono::milliseconds wait) {
        if (!state_->running.exchange(false)) {
            return;
        }
        boost::capy::run_async(state_->io_context.get_executor())(close_all(state_));
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (state_->live.load() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    [[nodiscard]] HttpConnectionStats stats() const {
        const State& state = *state_;
        return {state.connection_count.load(),
                state.active_requests.load(),
                state.waiting_requests.load(),
                state.refused_connections.load(),
                state.idle_timeouts.load(),
                state.header_timeouts.load(),
                state.busy_refusals.load()};
    }

private:
    struct Connection : boost::beast2::http_worker {
        boost::corosio::tcp_socket socket;
        detail::RefusalClosingSink sink;
        // Waiting for the first byte of its next request: the drain closes it.
        bool idle = false;

        Connection(boost::corosio::tcp_socket accepted,
                   boost::http::router<boost::http::route_params> router,
                   boost::http::shared_parser_config parser_config,
                   boost::http::shared_serializer_config serializer_config)
            : http_worker(std::move(router), std::move(parser_config), std::move(serializer_config)),
              socket(std::move(accepted)),
              sink(serializer.sink_for(socket), rp.res) {
            rp.req_body = boost::capy::any_buffer_source(parser.source_for(socket));
            rp.res_body = boost::capy::any_buffer_sink(&sink);
            stream = boost::capy::any_read_stream(&socket);
        }
    };

    // A request waiting for a worker. Granted by release_worker(), or refused by its own timer.
    struct WorkerWaiter {
        boost::corosio::timer* timer = nullptr;
        bool granted = false;
    };

    struct State {
        State(boost::corosio::io_context& context,
              boost::http::router<boost::http::route_params> routes,
              boost::http::shared_parser_config parser,
              boost::http::shared_serializer_config serializer,
              HttpConnectionLimits connection_limits,
              telemetry::Metrics* metric_sink)
            : io_context(context),
              acceptor(context),
              router(std::move(routes)),
              parser_config(std::move(parser)),
              serializer_config(std::move(serializer)),
              limits(std::move(connection_limits)),
              metrics(metric_sink),
              free_workers(limits.workers) {}

        boost::corosio::io_context& io_context;
        boost::corosio::tcp_acceptor acceptor;
        boost::http::router<boost::http::route_params> router;
        boost::http::shared_parser_config parser_config;
        boost::http::shared_serializer_config serializer_config;
        HttpConnectionLimits limits;
        telemetry::Metrics* metrics;
        unsigned short port = 0;

        // IO executor only.
        std::map<std::uint64_t, std::shared_ptr<Connection>> connections;
        std::uint64_t next_connection = 0;
        std::size_t free_workers;
        std::deque<WorkerWaiter*> waiters;

        std::atomic<bool> running{false};
        std::atomic<bool> draining{false};
        // The accept loop and every session not yet unwound; stop() waits for zero.
        std::atomic<std::size_t> live{0};
        std::atomic<std::size_t> connection_count{0};
        std::atomic<std::size_t> active_requests{0};
        std::atomic<std::size_t> waiting_requests{0};
        std::atomic<std::uint64_t> refused_connections{0};
        std::atomic<std::uint64_t> idle_timeouts{0};
        std::atomic<std::uint64_t> header_timeouts{0};
        std::atomic<std::uint64_t> busy_refusals{0};

        void gauges() {
            if (metrics == nullptr) {
                return;
            }
            metrics->set_gauge("lake_http_connections", {}, static_cast<double>(connection_count.load()));
            metrics->set_gauge("lake_http_requests_active", {}, static_cast<double>(active_requests.load()));
            metrics->set_gauge("lake_http_requests_waiting", {}, static_cast<double>(waiting_requests.load()));
        }

        void count(const char* name, const char* reason) {
            if (metrics != nullptr) {
                metrics->increment_counter(name, {{"reason", reason}});
            }
        }

        // A worker for one request: at once when one is free, otherwise the first released within
        // limits.worker_wait. False when the wait ran out or the server is stopping.
        boost::capy::task<bool> acquire_worker() {
            if (free_workers > 0 && waiters.empty()) {
                --free_workers;
                co_return true;
            }
            boost::corosio::timer timer(io_context);
            WorkerWaiter waiter{&timer, false};
            timer.expires_after(limits.worker_wait);
            waiters.push_back(&waiter);
            waiting_requests.fetch_add(1);
            gauges();
            struct Leave {
                State& state;
                WorkerWaiter& waiter;
                ~Leave() {
                    std::erase(state.waiters, &waiter);
                    state.waiting_requests.fetch_sub(1);
                }
            } leave{*this, waiter};
            static_cast<void>(co_await timer.wait());
            // Granted wins over a timer that fired just before release_worker() reached it.
            co_return waiter.granted;
        }

        void release_worker() {
            if (!waiters.empty() && running.load()) {
                WorkerWaiter* next = waiters.front();
                waiters.pop_front();
                next->granted = true;
                next->timer->cancel();
                return;
            }
            ++free_workers;
        }
    };

    struct WorkerSlot {
        State* state = nullptr;
        ~WorkerSlot() {
            if (state != nullptr) {
                state->active_requests.fetch_sub(1);
                state->release_worker();
                state->gauges();
            }
        }
    };

    enum class HeaderRead { ok, closed, idle_timeout, header_timeout, failed };

    // The parser's read_header with deadlines: `idle_timeout` for the first byte of a later
    // request on a kept-alive connection, then `header_timeout` for the rest of the header; a
    // connection's first request has `header_timeout` from the accept for all of it.
    static boost::capy::task<HeaderRead> read_header(State& state, Connection& connection, bool first) {
        using clock = std::chrono::steady_clock;
        bool received = false;
        auto deadline = clock::now() + state.limits.header_timeout;
        for (;;) {
            boost::system::error_code parse_error;
            connection.parser.parse(parse_error);
            if (connection.parser.got_header()) {
                co_return HeaderRead::ok;
            }
            if (parse_error != boost::http::condition::need_more_input) {
                co_return parse_error == boost::http::error::end_of_stream ? HeaderRead::closed
                                                                           : HeaderRead::failed;
            }
            const bool waiting_for_request = !first && !received;
            if (waiting_for_request && state.draining.load()) {
                co_return HeaderRead::closed;
            }
            const auto wait =
                waiting_for_request
                    ? state.limits.idle_timeout
                    : std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
            if (wait <= std::chrono::milliseconds::zero()) {
                co_return HeaderRead::header_timeout;
            }
            connection.idle = waiting_for_request;
            auto buffers = connection.parser.prepare();
            auto [read_error, read] =
                co_await boost::capy::timeout(connection.socket.read_some(buffers), wait);
            connection.idle = false;
            if (read_error == boost::capy::cond::eof) {
                connection.parser.commit_eof();
                continue;
            }
            if (read_error == boost::capy::cond::timeout) {
                co_return waiting_for_request ? HeaderRead::idle_timeout : HeaderRead::header_timeout;
            }
            if (read_error) {
                co_return HeaderRead::failed;
            }
            connection.parser.commit(read);
            if (!received && read > 0) {
                received = true;
                if (!first) {
                    deadline = clock::now() + state.limits.header_timeout;
                }
            }
        }
    }

    // A transport-level refusal, bounded even for a peer that does not read.
    static boost::capy::task<void> refuse(boost::corosio::tcp_socket& socket,
                                          std::string_view message,
                                          std::chrono::milliseconds bound) {
        const std::string body =
            std::string(R"({"ok":false,"error":{"code":"busy","message":")") + std::string(message) +
            "\"}}";
        const std::string response =
            "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
            "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"
            "Retry-After: 1\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        [[maybe_unused]] auto sent = co_await boost::capy::timeout(
            boost::capy::write(socket, boost::capy::const_buffer(response.data(), response.size())),
            bound);
    }

    // beast2's http_worker::do_http_session with deadlines on the header and a worker only for
    // the dispatch.
    static boost::capy::task<void> session(State& state, Connection& connection) {
        auto& rp = connection.rp;
        for (bool first = true;; first = false) {
            connection.parser.reset();
            connection.parser.start();
            rp.session_data.clear();
            const HeaderRead header = co_await read_header(state, connection, first);
            if (header == HeaderRead::idle_timeout) {
                state.idle_timeouts.fetch_add(1);
                state.count("lake_http_connections_closed_total", "idle_timeout");
            } else if (header == HeaderRead::header_timeout) {
                state.header_timeouts.fetch_add(1);
                state.count("lake_http_connections_closed_total", "header_timeout");
            }
            if (header != HeaderRead::ok) {
                co_return;
            }
            rp.req = connection.parser.get();
            rp.route_data.clear();
            rp.res.set_start_line(boost::http::status::ok, rp.req.version());
            rp.res.set_keep_alive(rp.req.keep_alive());
            connection.serializer.reset();
            connection.sink.begin_response();
            auto url = boost::urls::parse_uri_reference(rp.req.target());
            if (url.has_error()) {
                co_return;
            }
            rp.url = url.value();
            WorkerSlot slot;
            const bool exempt =
                state.limits.needs_no_worker && state.limits.needs_no_worker(rp.url.path());
            if (!exempt) {
                if (!co_await state.acquire_worker()) {
                    if (state.running.load()) {
                        state.busy_refusals.fetch_add(1);
                        state.count("lake_http_refusals_total", "workers_busy");
                        co_await refuse(connection.socket,
                                        "every request worker stayed busy; retry",
                                        std::chrono::seconds(1));
                    }
                    co_return;
                }
                slot.state = &state;
                state.active_requests.fetch_add(1);
                state.gauges();
            }
            auto routed = co_await connection.fr.dispatch(rp.req.method(), rp.url, rp);
            if (routed.failed() || !rp.res.keep_alive()) {
                co_return;
            }
        }
    }

    static boost::capy::task<void> serve(std::shared_ptr<State> state,
                                         std::uint64_t id,
                                         std::shared_ptr<Connection> connection) {
        try {
            co_await session(*state, *connection);
        } catch (const std::exception& error) {
            if (state->running.load()) {
                std::fprintf(stderr, "lake: HTTP session failed: %s\n", error.what());
            }
        }
        connection->socket.close();
        state->connections.erase(id);
        state->connection_count.store(state->connections.size());
        state->gauges();
        state->live.fetch_sub(1);
    }

    static boost::capy::task<void> accept_connections(std::shared_ptr<State> state) {
        bool reported_error = false;
        bool reported_capacity = false;
        while (state->running.load()) {
            boost::corosio::tcp_socket accepted(state->io_context);
            auto [error] = co_await state->acceptor.accept(accepted);
            if (!state->running.load()) {
                break;
            }
            if (error) {
                if (!reported_error) {
                    std::fprintf(stderr, "lake: HTTP accept failed: %s\n", error.message().c_str());
                    reported_error = true;
                }
                // Descriptor exhaustion must not become a loop that keeps sessions from closing.
                [[maybe_unused]] auto paused = co_await boost::capy::delay(std::chrono::milliseconds(10));
                continue;
            }
            reported_error = false;
            if (state->connections.size() >= state->limits.max_connections) {
                state->refused_connections.fetch_add(1);
                state->count("lake_http_refusals_total", "connection_limit");
                if (!reported_capacity) {
                    std::fprintf(stderr,
                                 "lake: HTTP connection limit %zu reached; answering 503\n",
                                 state->limits.max_connections);
                    reported_capacity = true;
                }
                co_await refuse(accepted, "the HTTP connection limit is reached; retry",
                                std::chrono::milliseconds(50));
                accepted.close();
                continue;
            }
            reported_capacity = false;
            auto connection = std::make_shared<Connection>(
                std::move(accepted), state->router, state->parser_config, state->serializer_config);
            const auto id = state->next_connection++;
            state->connections.emplace(id, connection);
            state->connection_count.store(state->connections.size());
            state->gauges();
            state->live.fetch_add(1);
            boost::capy::run_async(state->io_context.get_executor())(
                serve(state, id, std::move(connection)));
        }
        state->live.fetch_sub(1);
    }

    static boost::capy::task<void> close_idle(std::shared_ptr<State> state) {
        for (const auto& [id, connection] : state->connections) {
            if (connection->idle) {
                connection->socket.cancel();
            }
        }
        co_return;
    }

    static boost::capy::task<void> close_all(std::shared_ptr<State> state) {
        state->acceptor.close();
        for (const auto& [id, connection] : state->connections) {
            connection->socket.close();
        }
        // Refused, not granted: release_worker() hands out nothing once running is false.
        for (WorkerWaiter* waiter : state->waiters) {
            waiter->timer->cancel();
        }
        co_return;
    }

    std::shared_ptr<State> state_;
};

} // namespace lake::api
