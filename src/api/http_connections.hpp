#pragma once
// lake#534: lake's HTTP connections, decoupled from the requests being handled.
//
// Beast2's http_server runs a fixed pool of workers, each owning one connection for its whole
// life, so a client holding idle keep-alive connections held every worker and no one else could
// connect. This follows Zetta's server (zetta/src/core/api/http_server.cpp): lake accepts every
// connection up to `max_connections` and serves each with its own session coroutine, answering
// `503` with `Connection: close` above the cap. On top of that:
//
// - a request takes one of `workers` slots only while its route runs, so an idle connection, or
//   one still sending its header, holds no worker; a request that finds every slot taken waits
//   `worker_wait` for one and is then answered `503` and its connection closed;
// - a request that must wait before any route can answer it (the store-root hand-off, `admitted`)
//   waits before it takes a worker;
// - a request that parks in a long poll or streams a long response leaves its worker
//   (leave_worker()) and finishes outside the short-request budget;
// - a connection waiting for its next request is closed after `idle_timeout`;
// - a request header must arrive whole within `header_timeout` of its first byte (of the accept,
//   for a connection's first request), so a client trickling a header (slowloris) is cut off;
// - every read of a request body and every write of a response must progress within
//   `io_timeout`, so a trickled body or a client that never reads is cut off;
// - a response with status 503 or 429 closes its connection, as does every response during the
//   drain and every request whose body its route did not read;
// - a connection is closed lingering: lake stops sending, then reads and discards what the client
//   still sends for a moment, so its last answer is not lost to a reset.
//
// Every member of the shared state belongs to the IO executor (one thread runs the io_context):
// the accept loop, the sessions and the worker queue never run concurrently. stop() and
// begin_drain() hand their work to that executor. Only http_server.cpp and the tests include this
// header; no beast2 or boost::http type reaches lake's public headers.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#if defined(__APPLE__)
#include <netinet/tcp_fsm.h>
#endif

#include <boost/beast2/http_worker.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/delay.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/any_buffer_sink.hpp>
#include <boost/capy/io/any_buffer_source.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/capy/io_task.hpp>
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
#include <boost/http/field.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/router.hpp>
#include <boost/url/parse.hpp>

#include "lake/telemetry/metrics.hpp"

namespace lake::api {

struct HttpConnectionLimits {
    // Requests whose routes run at once (LAKE_HTTP_WORKERS). A request that leaves its worker
    // (a long poll, a streamed pull) no longer counts.
    std::size_t workers = 32;
    // Open connections, idle ones included (LAKE_HTTP_MAX_CONNECTIONS). Above it a new connection
    // is answered 503 and closed at once.
    std::size_t max_connections = 4096;
    // How long a keep-alive connection waits for its next request (LAKE_HTTP_IDLE_TIMEOUT_SECONDS).
    // It must outlast the idle timeout of any proxy's pooled upstream connections (nginx: 60 s), or
    // the proxy reuses a connection lake is closing.
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(75)};
    // How long a request header may take, from its first byte, or from the accept for a
    // connection's first request (LAKE_HTTP_HEADER_TIMEOUT_SECONDS).
    std::chrono::milliseconds header_timeout{std::chrono::seconds(10)};
    // How long a request waits for a free worker before it is answered 503
    // (LAKE_HTTP_WORKER_WAIT_SECONDS).
    std::chrono::milliseconds worker_wait{std::chrono::seconds(10)};
    // How long one read of a request body or one write of a response may make no progress
    // (LAKE_HTTP_IO_TIMEOUT_SECONDS).
    std::chrono::milliseconds io_timeout{std::chrono::seconds(60)};
    // While a request holds a worker, the time it waits on its client may not exceed
    // `rate_grace` plus a second per `min_transfer_rate` bytes it moved
    // (LAKE_HTTP_MIN_TRANSFER_RATE, bytes per second; 0 turns it off).
    std::size_t min_transfer_rate = 1024;
    std::chrono::milliseconds rate_grace{std::chrono::seconds(10)};
    // Requests that left their worker (long polls, pulls): at most this many in all
    // (LAKE_HTTP_MAX_LONG_REQUESTS) and per caller (LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER; an
    // unauthenticated caller is its client address). Past either the request is answered 429.
    std::size_t max_long_requests = 512;
    std::size_t max_long_requests_per_caller = 64;
    // Counted while a request waits for the store root or a worker and while its route runs, so
    // the owner's drain (lake#479) answers it before it closes the connections.
    std::atomic<int>* in_flight = nullptr;
    // Pending kernel handshakes, not admitted connections; the OS may clamp it.
    int listen_backlog = 1024;
    // Paths answered without taking a worker: the probes, so a saturated lake is not restarted.
    std::function<bool(std::string_view path)> needs_no_worker;
    // False while a request for this path cannot be answered yet (lake waits for the store root):
    // it waits, polling, up to `admission_wait` before it takes a worker, then is dispatched.
    std::function<bool(std::string_view path)> admitted;
    std::chrono::milliseconds admission_wait{std::chrono::minutes(4)};
    // Told of every request answered before any route (400, 431, the busy 503): method, status
    // and reason. Lake logs and counts them.
    std::function<void(std::string_view method, unsigned status, std::string_view reason)> refused;
};

struct HttpConnectionStats {
    std::size_t connections = 0;
    std::size_t active_requests = 0;
    std::size_t waiting_requests = 0;
    std::size_t long_requests = 0;
    std::uint64_t refused_connections = 0;
    std::uint64_t idle_timeouts = 0;
    std::uint64_t header_timeouts = 0;
    std::uint64_t io_timeouts = 0;
    std::uint64_t busy_refusals = 0;
    std::uint64_t bad_requests = 0;
    std::uint64_t unread_bodies = 0;
    std::uint64_t gone_before_dispatch = 0;
    std::uint64_t slow_transfers = 0;
    std::uint64_t long_refusals = 0;
    std::uint64_t admission_timeouts = 0;
};

// The descriptors lake may hold for connections: the soft open-file limit less a reserve for
// PostgreSQL, store files and outbound connections (half the limit when that is smaller).
inline std::size_t connections_for_open_files(std::size_t wanted, std::uint64_t open_files,
                                              std::uint64_t reserve = 512) {
    const std::uint64_t kept = std::min(reserve, open_files / 2);
    const std::uint64_t room = open_files > kept ? open_files - kept : 1;
    return static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(wanted, room)));
}

namespace detail {

// The socket as the parser and serializer see it. Every read and every write must make progress
// within `bound`. While a request holds a worker the transfer is also metered, as a bucket of
// waiting time: it starts with `grace`, each byte moved adds 1/`min_rate` of a second (never past
// `grace`), and the time spent waiting on the client is taken out. An operation may wait only as
// long as the bucket holds, so a client that trickles a body or reads a response a few bytes at a
// time empties it and is cut off however steadily it keeps the connection alive, while a fast
// transfer keeps it full. Time the route spends on its own work is not counted. A pending
// `100 Continue` is sent at the route's first read of the body.
class TimedSocket {
public:
    using clock = std::chrono::steady_clock;

    struct Counters {
        std::atomic<std::uint64_t>* timeouts;
        std::atomic<std::uint64_t>* slow;
    };

    TimedSocket(boost::corosio::tcp_socket& socket, std::chrono::milliseconds bound,
                std::size_t min_rate, std::chrono::milliseconds grace, Counters counters)
        : socket_(&socket), bound_(bound), min_rate_(min_rate), grace_(grace),
          counters_(counters) {}

    // Metering while a worker is held; off for a request that left its worker.
    void meter(bool on) {
        metered_ = on && min_rate_ > 0;
        balance_ = grace_;
    }
    void expect_continue(bool pending) {
        continue_pending_ = pending;
    }
    [[nodiscard]] bool continue_pending() const {
        return continue_pending_;
    }

    template <class MutableBufferSequence>
    boost::capy::io_task<std::size_t> read_some(MutableBufferSequence buffers) {
        if (continue_pending_) {
            continue_pending_ = false;
            static constexpr std::string_view proceed = "HTTP/1.1 100 Continue\r\n\r\n";
            auto [sent, written] = co_await boost::capy::timeout(
                boost::capy::write(*socket_, boost::capy::const_buffer(proceed.data(), proceed.size())),
                bound_);
            if (sent) {
                co_return {sent, 0};
            }
        }
        const auto bound = allowance();
        if (bound <= std::chrono::milliseconds::zero()) {
            counters_.slow->fetch_add(1);
            co_return {std::error_code(boost::capy::error::timeout), 0};
        }
        const auto started = clock::now();
        auto [error, read] = co_await boost::capy::timeout(socket_->read_some(buffers), bound);
        account(started, read, error, bound);
        co_return {error, read};
    }

    template <class ConstBufferSequence>
    boost::capy::io_task<std::size_t> write_some(ConstBufferSequence buffers) {
        const auto bound = allowance();
        if (bound <= std::chrono::milliseconds::zero()) {
            counters_.slow->fetch_add(1);
            co_return {std::error_code(boost::capy::error::timeout), 0};
        }
        const auto started = clock::now();
        auto [error, written] = co_await boost::capy::timeout(socket_->write_some(buffers), bound);
        account(started, written, error, bound);
        co_return {error, written};
    }

private:
    // The bound for the next operation: the I/O bound, or less when the rate allows less.
    [[nodiscard]] std::chrono::milliseconds allowance() const {
        if (!metered_) {
            return bound_;
        }
        return std::min(bound_, std::chrono::ceil<std::chrono::milliseconds>(balance_));
    }

    void account(clock::time_point started, std::size_t moved, const std::error_code& error,
                 std::chrono::milliseconds bound) {
        if (metered_) {
            const auto earned = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(static_cast<double>(moved) /
                                              static_cast<double>(min_rate_)));
            balance_ = std::min<clock::duration>(grace_, balance_ - (clock::now() - started) + earned);
        }
        if (error == boost::capy::cond::timeout) {
            (bound < bound_ ? counters_.slow : counters_.timeouts)->fetch_add(1);
        }
    }

    boost::corosio::tcp_socket* socket_;
    std::chrono::milliseconds bound_;
    std::size_t min_rate_;
    std::chrono::milliseconds grace_;
    Counters counters_;
    bool metered_ = false;
    bool continue_pending_ = false;
    clock::duration balance_{};
};

// The serializer's sink, closing the connection of a 503 or 429 response, of any response during
// the drain, and of a response whose request body its route left unread and too large to discard
// (more than `discard_limit` bytes, or chunked), before its header is written. Every response body leaves through it, so no
// send helper has to remember.
class RefusalClosingSink {
public:
    using Inner = boost::http::serializer::sink<TimedSocket>;

    static constexpr std::uint64_t discard_limit = 64U * 1024U;

    RefusalClosingSink(Inner inner, boost::http::response& response,
                       const boost::http::request& request,
                       const boost::http::request_parser& parser, const TimedSocket& socket,
                       const std::atomic<bool>* draining)
        : inner_(inner), response_(&response), request_(&request), parser_(&parser),
          socket_(&socket), draining_(draining) {}

    // The body left unread is small enough for the session to read and discard after the route.
    // A client still waiting for `100 Continue` will not send it: the connection closes instead.
    [[nodiscard]] bool unread_body_discardable() const {
        return request_->payload() == boost::http::payload::size &&
               request_->payload_size() <= discard_limit && !socket_->continue_pending();
    }

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
        const bool refusal = status == boost::http::status::service_unavailable ||
                             status == boost::http::status::too_many_requests;
        const bool unread = !parser_->is_complete() && !unread_body_discardable();
        if ((refusal || draining_->load() || unread) && response_->keep_alive()) {
            response_->set_keep_alive(false);
        }
    }

    Inner inner_;
    boost::http::response* response_;
    const boost::http::request* request_;
    const boost::http::request_parser* parser_;
    const TimedSocket* socket_;
    const std::atomic<bool>* draining_;
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
        const auto& limits = state.limits;
        if (limits.workers == 0 || limits.max_connections == 0 || limits.listen_backlog <= 0 ||
            limits.idle_timeout.count() <= 0 || limits.header_timeout.count() <= 0 ||
            limits.worker_wait.count() <= 0 || limits.io_timeout.count() <= 0) {
            return "HTTP workers, max_connections, listen_backlog and the idle, header, worker "
                   "wait and I/O timeouts must be positive";
        }
        state.acceptor.open();
        state.acceptor.set_option(boost::corosio::socket_option::reuse_address(true));
        auto error = state.acceptor.bind(endpoint);
        if (!error) {
            error = state.acceptor.listen(limits.listen_backlog);
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
    // requests in progress finish, and every response closes its connection.
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

    // The request a route is handling, as a token leave_worker() takes later; 0 for none. IO
    // executor only: call it from the route.
    [[nodiscard]] std::uint64_t request_token(const boost::http::route_params& route) const {
        const auto found = state_->tokens.find(&route);
        return found == state_->tokens.end() ? 0 : found->second;
    }

    // The request parks (a long poll) or streams a long response: it gives its worker back and
    // finishes outside the worker budget, counted against the long-request caps for `principal`
    // (empty: the client's address). False when a cap is full: the request keeps its worker and
    // should be answered 429. A request that already left, or has ended, is ignored. IO executor
    // only.
    bool leave_worker(std::uint64_t token, std::string_view principal = {}) {
        const auto found = state_->slots.find(token);
        return found == state_->slots.end() || found->second->leave(principal);
    }
    bool leave_worker(const boost::http::route_params& route, std::string_view principal = {}) {
        return leave_worker(request_token(route), principal);
    }

    // Socket peer only: untrusted forwarding headers cannot buy more sign-in attempts.
    [[nodiscard]] std::string client_address(const boost::http::route_params& route) const {
        const auto token = request_token(route);
        const auto found = state_->slots.find(token);
        return found == state_->slots.end() ? "unknown" : found->second->connection->peer;
    }

    [[nodiscard]] HttpConnectionStats stats() const {
        const State& state = *state_;
        return {state.connection_count.load(),
                state.active_requests.load(),
                state.waiting_requests.load(),
                state.long_requests.load(),
                state.refused_connections.load(),
                state.idle_timeouts.load(),
                state.header_timeouts.load(),
                state.io_timeouts.load(),
                state.busy_refusals.load(),
                state.bad_requests.load(),
                state.unread_bodies.load(),
                state.gone_before_dispatch.load(),
                state.slow_transfers.load(),
                state.long_refusals.load(),
                state.admission_timeouts.load()};
    }

private:
    struct Connection : boost::beast2::http_worker {
        boost::corosio::tcp_socket socket;
        detail::TimedSocket timed;
        detail::RefusalClosingSink sink;
        // Waiting for the first byte of its next request: the drain closes it.
        bool idle = false;
        // The client's address: an unauthenticated caller's identity for the long-request cap.
        std::string peer;

        Connection(boost::corosio::tcp_socket accepted,
                   boost::http::router<boost::http::route_params> router,
                   boost::http::shared_parser_config parser_config,
                   boost::http::shared_serializer_config serializer_config,
                   const HttpConnectionLimits& limits,
                   detail::TimedSocket::Counters counters,
                   const std::atomic<bool>* draining)
            : http_worker(std::move(router), std::move(parser_config), std::move(serializer_config)),
              socket(std::move(accepted)),
              timed(socket, limits.io_timeout, limits.min_transfer_rate, limits.rate_grace, counters),
              sink(serializer.sink_for(timed), rp.res, rp.req, parser, timed, draining),
              peer(peer_address(socket)) {
            rp.req_body = boost::capy::any_buffer_source(parser.source_for(timed));
            rp.res_body = boost::capy::any_buffer_sink(&sink);
            stream = boost::capy::any_read_stream(&timed);
        }
    };

    static std::string peer_address(boost::corosio::tcp_socket& socket) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (::getpeername(socket.native_handle(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return "unknown";
        }
        char text[INET6_ADDRSTRLEN] = {};
        const void* raw = address.ss_family == AF_INET6
                              ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr)
                              : static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(&address)->sin_addr);
        return ::inet_ntop(address.ss_family, raw, text, sizeof(text)) != nullptr ? text : "unknown";
    }

    // A request waiting for a worker. Granted by release_worker(), or refused by its own timer.
    struct WorkerWaiter {
        boost::corosio::timer* timer = nullptr;
        bool granted = false;
    };

    struct State;

    // One request's place in the worker budget, from before its dispatch to its end.
    struct WorkerSlot {
        State* state = nullptr;
        Connection* connection = nullptr;
        std::uint64_t token = 0;
        const boost::http::route_params* route = nullptr;
        bool holds = false;
        bool left = false;
        std::string caller;

        void enter(State& owner, Connection& served) {
            state = &owner;
            connection = &served;
            token = ++owner.next_token;
            route = &served.rp;
            owner.slots.emplace(token, this);
            owner.tokens.insert_or_assign(route, token);
        }
        void hold() {
            holds = true;
            connection->timed.meter(true);
            state->active_requests.fetch_add(1);
            state->gauges();
        }
        // Gives the worker back for the rest of the request, counted against the long-request
        // caps for `principal` (empty: the client's address). False, keeping the worker, when a
        // cap is full.
        bool leave(std::string_view principal) {
            if (!holds) {
                return true;
            }
            std::string key = principal.empty() ? "address:" + connection->peer
                                                : "caller:" + std::string(principal);
            auto& by_caller = state->long_by_caller[key];
            if (state->long_requests.load() >= state->limits.max_long_requests ||
                by_caller >= state->limits.max_long_requests_per_caller) {
                if (by_caller == 0) {
                    state->long_by_caller.erase(key);
                }
                state->long_refusals.fetch_add(1);
                state->count("lake_http_refusals_total", "long_requests");
                return false;
            }
            ++by_caller;
            caller = std::move(key);
            release();
            left = true;
            state->long_requests.fetch_add(1);
            if (state->metrics != nullptr) {
                state->metrics->increment_counter("lake_http_long_requests_total", {});
            }
            state->gauges();
            return true;
        }
        // Gives the worker back early (before discarding an unread body); idempotent.
        void release() {
            if (!holds) {
                return;
            }
            holds = false;
            connection->timed.meter(false);
            state->active_requests.fetch_sub(1);
            state->release_worker();
            state->gauges();
        }
        ~WorkerSlot() {
            if (state == nullptr) {
                return;
            }
            state->slots.erase(token);
            const auto found = state->tokens.find(route);
            if (found != state->tokens.end() && found->second == token) {
                state->tokens.erase(found);
            }
            release();
            if (left) {
                state->long_requests.fetch_sub(1);
                const auto counted = state->long_by_caller.find(caller);
                if (counted != state->long_by_caller.end() && --counted->second == 0) {
                    state->long_by_caller.erase(counted);
                }
            }
            state->gauges();
        }
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
        std::map<std::uint64_t, WorkerSlot*> slots;
        std::map<const boost::http::route_params*, std::uint64_t> tokens;
        std::map<std::string, std::size_t> long_by_caller;
        std::uint64_t next_token = 0;
        // Refused connections being closed lingering (at most `lingering_bound` at once).
        std::set<boost::corosio::tcp_socket*> lingering;
        static constexpr std::size_t lingering_bound = 64;

        std::atomic<bool> running{false};
        std::atomic<bool> draining{false};
        // The accept loop and every session not yet unwound; stop() waits for zero.
        std::atomic<std::size_t> live{0};
        std::atomic<std::size_t> connection_count{0};
        std::atomic<std::size_t> active_requests{0};
        std::atomic<std::size_t> waiting_requests{0};
        std::atomic<std::size_t> long_requests{0};
        std::atomic<std::uint64_t> refused_connections{0};
        std::atomic<std::uint64_t> idle_timeouts{0};
        std::atomic<std::uint64_t> header_timeouts{0};
        std::atomic<std::uint64_t> io_timeouts{0};
        std::atomic<std::uint64_t> busy_refusals{0};
        std::atomic<std::uint64_t> bad_requests{0};
        std::atomic<std::uint64_t> unread_bodies{0};
        std::atomic<std::uint64_t> gone_before_dispatch{0};
        std::atomic<std::uint64_t> slow_transfers{0};
        std::atomic<std::uint64_t> long_refusals{0};
        std::atomic<std::uint64_t> admission_timeouts{0};

        void gauges() {
            if (metrics == nullptr) {
                return;
            }
            metrics->set_gauge("lake_http_connections", {}, static_cast<double>(connection_count.load()));
            metrics->set_gauge("lake_http_requests_active", {}, static_cast<double>(active_requests.load()));
            metrics->set_gauge("lake_http_requests_waiting", {}, static_cast<double>(waiting_requests.load()));
            metrics->set_gauge("lake_http_requests_long", {}, static_cast<double>(long_requests.load()));
        }

        void count(const char* name, const char* reason) {
            if (metrics != nullptr) {
                metrics->increment_counter(name, {{"reason", reason}});
            }
        }

        enum class Grant { at_once, after_wait, refused };

        // A worker for one request: at once when one is free, otherwise the first released within
        // limits.worker_wait.
        boost::capy::task<Grant> acquire_worker() {
            if (free_workers > 0 && waiters.empty()) {
                --free_workers;
                co_return Grant::at_once;
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
                    state.gauges();
                }
            } leave{*this, waiter};
            static_cast<void>(co_await timer.wait());
            // Granted wins over a timer that fired just before release_worker() reached it.
            co_return waiter.granted ? Grant::after_wait : Grant::refused;
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

    enum class HeaderRead { ok, closed, idle_timeout, header_timeout, too_large, bad, failed };

    static bool header_too_large(const boost::system::error_code& error) {
        return error == boost::http::error::headers_limit ||
               error == boost::http::error::field_size_limit ||
               error == boost::http::error::fields_limit ||
               error == boost::http::error::start_line_limit;
    }

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
                if (parse_error == boost::http::error::end_of_stream) {
                    co_return HeaderRead::closed;
                }
                co_return header_too_large(parse_error) ? HeaderRead::too_large : HeaderRead::bad;
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

    // A response written before any route, bounded even for a peer that does not read. A HEAD
    // request gets the header alone.
    static boost::capy::task<void> answer_raw(boost::corosio::tcp_socket& socket,
                                              std::string_view status_line,
                                              std::string_view code,
                                              std::string_view message,
                                              bool head,
                                              std::chrono::milliseconds bound) {
        const std::string body = std::string(R"({"ok":false,"error":{"code":")") +
                                 std::string(code) + R"(","message":")" + std::string(message) +
                                 "\"}}";
        std::string response = "HTTP/1.1 " + std::string(status_line) +
                               "\r\nConnection: close\r\n"
                               "Content-Type: application/json; charset=utf-8\r\n"
                               "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
        if (status_line.starts_with("503")) {
            response += "Retry-After: 1\r\n";
        }
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        if (!head) {
            response += body;
        }
        [[maybe_unused]] auto sent = co_await boost::capy::timeout(
            boost::capy::write(socket, boost::capy::const_buffer(response.data(), response.size())),
            bound);
    }

    // Close without losing the last answer: stop sending, then read and discard what the client
    // still sends (at most 64 KiB, at most a second), so the close is not a reset that discards a
    // response the client has not read yet.
    static boost::capy::task<void> linger_close(boost::corosio::tcp_socket& socket) {
        using clock = std::chrono::steady_clock;
        if (socket.is_open()) {
            try {
                socket.shutdown(boost::corosio::tcp_socket::shutdown_send);
            } catch (const std::exception&) {
            }
            std::array<char, 4096> scratch{};
            std::size_t discarded = 0;
            const auto deadline = clock::now() + std::chrono::seconds(1);
            while (discarded < 64U * 1024U && socket.is_open()) {
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
                if (left <= std::chrono::milliseconds::zero()) {
                    break;
                }
                auto [error, read] = co_await boost::capy::timeout(
                    socket.read_some(boost::capy::mutable_buffer(scratch.data(), scratch.size())), left);
                if (error) {
                    break;
                }
                discarded += read;
            }
        }
        socket.close();
    }

    // Reads and discards the rest of a request body of at most discard_limit bytes, within a
    // second; true when the request is then complete.
    static boost::capy::task<bool> discard_body(Connection& connection) {
        if (!connection.sink.unread_body_discardable()) {
            co_return false;
        }
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(1);
        std::array<char, 4096> scratch{};
        while (!connection.parser.is_complete()) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
            if (left <= std::chrono::milliseconds::zero()) {
                co_return false;
            }
            auto [error, read] = co_await boost::capy::timeout(
                connection.parser.read(connection.socket,
                                       boost::capy::mutable_buffer(scratch.data(), scratch.size())),
                left);
            if (error && error != boost::capy::cond::eof) {
                co_return false;
            }
            if (error == boost::capy::cond::eof && !connection.parser.is_complete()) {
                co_return false;
            }
        }
        co_return true;
    }

    // A client that reset its connection while its request waited, or closed it before sending
    // the whole request: nothing to answer. A client that half-closed after a complete request
    // (it sends nothing more but still reads) is answered.
    static bool client_gone(Connection& connection) {
        char byte = 0;
        const auto peeked =
            ::recv(connection.socket.native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peeked == 0) {
            return !connection.parser.is_complete() || reset_by_peer(connection.socket);
        }
        return peeked < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN);
    }

    // The reactor may already have taken a reset's error, leaving the socket reading as at end of
    // stream; the TCP state still tells a reset (closed) from a half-close (close-wait).
    static bool reset_by_peer(boost::corosio::tcp_socket& socket) {
#if defined(__linux__)
        tcp_info info{};
        socklen_t length = sizeof(info);
        return ::getsockopt(socket.native_handle(), IPPROTO_TCP, TCP_INFO, &info, &length) == 0 &&
               info.tcpi_state == TCP_CLOSE;
#elif defined(__APPLE__)
        tcp_connection_info info{};
        socklen_t length = sizeof(info);
        return ::getsockopt(socket.native_handle(), IPPROTO_TCP, TCP_CONNECTION_INFO, &info,
                            &length) == 0 &&
               info.tcpi_state == TCPS_CLOSED;
#else
        static_cast<void>(socket);
        return false;
#endif
    }

    struct InFlight {
        std::atomic<int>* counter;
        explicit InFlight(std::atomic<int>* in_flight) : counter(in_flight) {
            if (counter != nullptr) {
                counter->fetch_add(1);
            }
        }
        ~InFlight() {
            if (counter != nullptr) {
                counter->fetch_sub(1);
            }
        }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
    };

    static bool expects_continue(const boost::http::route_params& rp) {
        std::string expect(rp.req.value_or(boost::http::field::expect, ""));
        std::ranges::transform(expect, expect.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return expect == "100-continue";
    }

    static void report_refusal(State& state, const boost::http::route_params& rp, unsigned status,
                        std::string_view reason) {
        if (state.limits.refused) {
            state.limits.refused(rp.req.method_text(), status, reason);
        }
    }

    // beast2's http_worker::do_http_session with deadlines on the header, a worker only while a
    // route runs, and a close whenever the connection cannot carry another request.
    static boost::capy::task<void> session(State& state, Connection& connection) {
        auto& rp = connection.rp;
        for (bool first = true;; first = false) {
            connection.parser.reset();
            connection.parser.start();
            rp.session_data.clear();
            const HeaderRead header = co_await read_header(state, connection, first);
            switch (header) {
            case HeaderRead::ok:
                break;
            case HeaderRead::idle_timeout:
                state.idle_timeouts.fetch_add(1);
                state.count("lake_http_connections_closed_total", "idle_timeout");
                co_return;
            case HeaderRead::header_timeout:
                state.header_timeouts.fetch_add(1);
                state.count("lake_http_connections_closed_total", "header_timeout");
                co_return;
            case HeaderRead::too_large:
            case HeaderRead::bad: {
                const bool large = header == HeaderRead::too_large;
                state.bad_requests.fetch_add(1);
                state.count("lake_http_refusals_total", large ? "header_too_large" : "bad_request");
                if (state.limits.refused) {
                    state.limits.refused("", large ? 431U : 400U, large ? "header_too_large" : "bad_request");
                }
                co_await answer_raw(connection.socket,
                                    large ? "431 Request Header Fields Too Large" : "400 Bad Request",
                                    large ? "header_too_large" : "invalid_input",
                                    large ? "the request header exceeds 8 KiB"
                                          : "the request could not be parsed",
                                    false,
                                    std::chrono::seconds(1));
                co_return;
            }
            case HeaderRead::closed:
            case HeaderRead::failed:
                co_return;
            }
            rp.req = connection.parser.get();
            rp.route_data.clear();
            rp.res.set_start_line(boost::http::status::ok, rp.req.version());
            rp.res.set_keep_alive(rp.req.keep_alive() && !state.draining.load());
            connection.serializer.reset();
            connection.sink.begin_response();
            const bool head = rp.req.method() == boost::http::method::head;
            auto url = boost::urls::parse_uri_reference(rp.req.target());
            if (url.has_error()) {
                state.bad_requests.fetch_add(1);
                state.count("lake_http_refusals_total", "bad_request");
                report_refusal(state, rp, 400, "bad_url");
                co_await answer_raw(connection.socket, "400 Bad Request", "invalid_input",
                                    "the request target is not a valid URL", head,
                                    std::chrono::seconds(1));
                co_return;
            }
            rp.url = url.value();
            const std::string path(rp.url.path());
            // Counted from here to the end of the route, so the drain answers a request that is
            // still waiting for the store root or a worker before it closes the connections.
            InFlight counted(state.limits.in_flight);
            bool waited = false;
            // The store-root hand-off: wait for it before taking a worker, so a rollout's
            // requests do not use up the workers and are refused (lake#479, lake#534). A request
            // still not admitted when the wait runs out is answered here, never dispatched to wait
            // again while holding a worker.
            if (state.limits.admitted && !state.limits.admitted(path)) {
                waited = true;
                boost::corosio::timer timer(state.io_context);
                auto pause = std::chrono::milliseconds(5);
                const auto deadline = std::chrono::steady_clock::now() + state.limits.admission_wait;
                while (state.running.load() && !state.limits.admitted(path) &&
                       std::chrono::steady_clock::now() < deadline) {
                    timer.expires_after(pause);
                    static_cast<void>(co_await timer.wait());
                    pause = std::min(pause * 2, std::chrono::milliseconds(100));
                }
                if (!state.running.load()) {
                    co_return;
                }
                if (!state.limits.admitted(path)) {
                    state.admission_timeouts.fetch_add(1);
                    report_refusal(state, rp, 503, "store_root_wait");
                    co_await answer_raw(connection.socket, "503 Service Unavailable",
                                        "store_unavailable",
                                        "lake gave up waiting for the store root", head,
                                        std::chrono::seconds(1));
                    co_return;
                }
            }
            WorkerSlot slot;
            slot.enter(state, connection);
            const bool exempt = state.limits.needs_no_worker && state.limits.needs_no_worker(path);
            if (!exempt) {
                const auto grant = co_await state.acquire_worker();
                if (grant == State::Grant::refused) {
                    if (state.running.load()) {
                        state.busy_refusals.fetch_add(1);
                        state.count("lake_http_refusals_total", "workers_busy");
                        report_refusal(state, rp, 503, "workers_busy");
                        co_await answer_raw(connection.socket, "503 Service Unavailable", "busy",
                                            "every request worker stayed busy; retry", head,
                                            std::chrono::seconds(1));
                    }
                    co_return;
                }
                slot.hold();
                waited = waited || grant == State::Grant::after_wait;
            }
            if (waited && client_gone(connection)) {
                state.gone_before_dispatch.fetch_add(1);
                co_return;
            }
            // `100 Continue` goes out at the route's first read of the body, so a route that
            // refuses without reading never invites the body.
            connection.timed.expect_continue(expects_continue(rp) && !connection.parser.is_complete());
            auto routed = co_await connection.fr.dispatch(rp.req.method(), rp.url, rp);
            const bool invited = !connection.timed.continue_pending();
            connection.timed.expect_continue(false);
            if (routed.failed()) {
                co_return;
            }
            // A body the route did not read would be parsed as the next request: a small one is
            // read and discarded (with the worker given back first), anything else closes the
            // connection.
            if (!connection.parser.is_complete()) {
                slot.release();
                if (!rp.res.keep_alive() || !invited || !co_await discard_body(connection)) {
                    state.unread_bodies.fetch_add(1);
                    co_return;
                }
            }
            if (!rp.res.keep_alive()) {
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
        co_await linger_close(connection->socket);
        state->connections.erase(id);
        state->connection_count.store(state->connections.size());
        state->gauges();
        state->live.fetch_sub(1);
    }

    static boost::capy::task<void> refuse_connection(std::shared_ptr<State> state,
                                                     boost::corosio::tcp_socket socket) {
        state->lingering.insert(&socket);
        co_await answer_raw(socket, "503 Service Unavailable", "busy",
                            "the HTTP connection limit is reached; retry", false,
                            std::chrono::milliseconds(50));
        co_await linger_close(socket);
        state->lingering.erase(&socket);
        state->live.fetch_sub(1);
    }

#ifdef MSG_NOSIGNAL
    static constexpr int send_flags = MSG_NOSIGNAL;
#else
    static constexpr int send_flags = 0;
#endif

    static bool out_of_descriptors(const std::error_code& error) {
        return error == std::errc::too_many_files_open ||
               error == std::errc::too_many_files_open_in_system;
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
                if (out_of_descriptors(error)) {
                    [[maybe_unused]] auto paused =
                        co_await boost::capy::delay(std::chrono::milliseconds(10));
                }
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
                if (state->lingering.size() < State::lingering_bound) {
                    state->live.fetch_add(1);
                    boost::capy::run_async(state->io_context.get_executor())(
                        refuse_connection(state, std::move(accepted)));
                } else {
                    // Never waits: one non-blocking send, then close, so a flood of refusals
                    // cannot slow the accept loop.
                    static constexpr std::string_view refusal =
                        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
                        "Retry-After: 1\r\nContent-Length: 0\r\n\r\n";
                    static_cast<void>(::send(accepted.native_handle(), refusal.data(),
                                             refusal.size(), MSG_DONTWAIT | send_flags));
                    accepted.close();
                }
                continue;
            }
            reported_capacity = false;
            auto connection = std::make_shared<Connection>(
                std::move(accepted), state->router, state->parser_config, state->serializer_config,
                state->limits,
                detail::TimedSocket::Counters{&state->io_timeouts, &state->slow_transfers},
                &state->draining);
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
        for (boost::corosio::tcp_socket* socket : state->lingering) {
            socket->close();
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
