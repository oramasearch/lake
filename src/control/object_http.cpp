// The workspace store's HTTP client (lake#544): keep-alive connections to each workspace Zetta,
// one name resolution per origin while it stays fresh, TCP_NODELAY and keepalive probes on every
// connection, and a bounded dial.
//
// Before it, every object request went through Zetta's connector transport, which resolves the
// host, dials, sends `Connection: close` and drops the socket. On dev each resolution of a
// workspace Zetta's Service name took about 18 ms (the name has fewer dots than the cluster's
// `ndots:5`, so the resolver tries every search domain first), while the request itself took a
// fraction of a millisecond. See docs/architecture/workspace-store.md ("Per-request cost").
//
// Everything below runs on one io_context thread: the idle connections and the resolutions are
// touched only there, so they need no lock. A caller hands an exchange over and waits on an event.

#include "lake/control/workspace_store.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/request.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>
#include <zetta/connect/http_transport.hpp>
#include <zetta/runtime.hpp>

namespace lake::control {
namespace {

namespace net = boost::corosio;
namespace http = boost::http;
using clock = std::chrono::steady_clock;

// The largest answer read: a window of ciphertext is 4 MiB, a listing page far less, and a whole
// object read (get_all) is bounded by what lake wrote.
constexpr std::uint64_t response_body_limit = 512ULL * 1024ULL * 1024ULL;

struct Target {
    std::string host;
    std::string port;
    std::string target; // path and query
};

// Splits a plain http:// URL; https and anything else return false.
bool parse_plain_url(std::string_view url, Target& parsed) {
    if (!url.starts_with("http://")) {
        return false;
    }
    url.remove_prefix(7);
    const std::size_t path_start = url.find_first_of("/?");
    const std::string_view authority = path_start == std::string_view::npos ? url : url.substr(0, path_start);
    parsed.target = path_start == std::string_view::npos ? std::string("/") : std::string(url.substr(path_start));
    if (parsed.target.front() != '/') {
        parsed.target.insert(parsed.target.begin(), '/');
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    parsed.port = "80";
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        parsed.host = std::string(authority.substr(1, close - 1));
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            parsed.port = std::string(rest.substr(1));
        }
    } else if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
        parsed.host = std::string(authority.substr(0, colon));
        parsed.port = std::string(authority.substr(colon + 1));
    } else {
        parsed.host = std::string(authority);
    }
    return !parsed.host.empty() && !parsed.port.empty();
}

std::string host_header(const Target& target) {
    const std::string host = target.host.find(':') != std::string::npos ? "[" + target.host + "]" : target.host;
    return target.port == "80" ? host : host + ":" + target.port;
}

bool is_host_header(std::string_view name) {
    return name.size() == 4 && std::equal(name.begin(), name.end(), "host", [](char left, char right) {
               return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) == right;
           });
}

// A kept connection that the peer closed (or wrote to unasked) since its last answer is not
// reused. A peek that would block means it is open and silent, as an idle HTTP connection is.
bool still_open(const net::tcp_socket& socket) {
    if (!socket.is_open()) {
        return false;
    }
    char byte = 0;
    const auto got = ::recv(static_cast<int>(socket.native_handle()), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

// TCP_NODELAY, and TCP keepalive probes so that a kept connection to a peer that vanished without
// a word is found dead (and then closed by the peek before reuse) rather than kept for good.
void set_socket_options(const net::tcp_socket& socket) {
    const int handle = static_cast<int>(socket.native_handle());
    int yes = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    ::setsockopt(handle, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 10;
    int interval = 5;
    int count = 3;
#if defined(TCP_KEEPIDLE)
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif
#if defined(TCP_KEEPINTVL)
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#if defined(TCP_KEEPCNT)
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

struct Exchange {
    ObjectRequest request;
    ObjectResponse response;
    zetta::runtime::async_event finished;
};

struct Resolution {
    std::error_code error;
    std::vector<net::endpoint> endpoints;
    zetta::runtime::async_event done;
};

struct Connection {
    explicit Connection(net::io_context& context) : socket(context) {}
    net::tcp_socket socket;
    clock::time_point idle_since{};
};

// Sends `request` and reads the whole answer. Returns the failure, empty on success; `reusable`
// is set when the answer was read to its end and Zetta keeps the connection open.
boost::capy::io_task<std::string> exchange_over(net::tcp_socket& socket, const ObjectRequest& request,
                                                const Target& target, ObjectResponse& response,
                                                const http::shared_parser_config& parser_config,
                                                const http::shared_serializer_config& serializer_config,
                                                bool& reusable) {
    const http::method method = http::string_to_method(request.method);
    if (method == http::method::unknown) {
        co_return {{}, "the method " + request.method + " is not one the client sends"};
    }
    http::request message(method, target.target, http::version::http_1_1);
    boost::system::error_code refused;
    message.set(http::field::host, host_header(target), refused);
    if (refused) {
        co_return {{}, "could not set the Host header: " + refused.message()};
    }
    for (const auto& [name, value] : request.headers) {
        if (is_host_header(name)) {
            continue;
        }
        // Never a header dropped and the request sent without it. The value is not named: it
        // may be the bearer.
        message.set(name, value, refused);
        if (refused) {
            co_return {{}, "could not set the header " + name + ": " + refused.message()};
        }
    }
    message.set_content_length(request.body.size());

    http::serializer serializer(serializer_config);
    serializer.set_message(message);
    auto sink = serializer.sink_for(socket);
    if (auto [write_error, written] = co_await sink.write_eof(boost::capy::make_buffer(std::string_view(request.body)));
        write_error) {
        co_return {{}, "could not send the request: " + write_error.message()};
    }

    http::response_parser parser(parser_config);
    parser.reset();
    parser.start();
    if (auto [header_error] = co_await parser.read_header(socket); header_error) {
        co_return {{}, "could not read the answer: " + header_error.message()};
    }
    parser.set_body_limit(response_body_limit);
    response.status = static_cast<int>(parser.get().status_int());
    if (response.status < 200) {
        // An interim answer (lake never sends Expect): what follows on the connection is not
        // known to be framed, so it is not kept, and the request has no final answer.
        response.status = 0;
        co_return {{}, "the workspace Zetta sent an interim answer"};
    }
    const bool keep_alive = parser.get().keep_alive();
    auto body = parser.source_for(socket);
    boost::capy::const_buffer chunks[16];
    for (;;) {
        auto [pull_error, buffers] = co_await body.pull(chunks);
        if (pull_error == boost::capy::error::eof) {
            break;
        }
        if (pull_error) {
            co_return {{}, "could not read the answer's body: " + pull_error.message()};
        }
        std::size_t consumed = 0;
        for (const boost::capy::const_buffer& buffer : buffers) {
            response.body.append(static_cast<const char*>(buffer.data()), buffer.size());
            consumed += buffer.size();
        }
        body.consume(consumed);
    }
    reusable = keep_alive;
    co_return {{}, std::string()};
}

class PooledTransport final : public PooledObjectTransport {
public:
    explicit PooledTransport(PooledObjectTransportOptions options) : options_(options) {
        http::parser_config limits(/*server=*/false);
        limits.body_limit = response_body_limit;
        parser_config_ = http::make_parser_config(limits);
        serializer_config_ = http::make_serializer_config(http::serializer_config{});
        boost::capy::run_async(io_context_.get_executor())(keepalive());
        worker_ = std::thread([this] { io_context_.run(); });
    }

    ~PooledTransport() override {
        shutdown();
    }

    void shutdown() override {
        stopped_.store(true);
        io_context_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
        // The worker is gone: nothing else touches what it held. Answer every caller still waiting.
        std::set<std::shared_ptr<Exchange>> waiting;
        {
            std::lock_guard lock(pending_mutex_);
            waiting.swap(pending_);
        }
        for (const auto& exchange : waiting) {
            exchange->response = ObjectResponse{0, {}, "the workspace store's HTTP client stopped"};
            exchange->finished.set();
        }
    }

    PooledTransport(const PooledTransport&) = delete;
    PooledTransport& operator=(const PooledTransport&) = delete;

    liquid::runtime::task<ObjectResponse> exchange(ObjectRequest request) override {
        if (!request.url.starts_with("http://")) {
            co_return co_await unpooled(std::move(request));
        }
        auto exchange = std::make_shared<Exchange>();
        exchange->request = std::move(request);
        {
            std::lock_guard lock(pending_mutex_);
            if (stopped_.load()) {
                co_return ObjectResponse{0, {}, "the workspace store's HTTP client stopped"};
            }
            pending_.insert(exchange);
        }
        boost::capy::run_async(io_context_.get_executor())(perform(exchange));
        [[maybe_unused]] const auto waited = co_await exchange->finished.wait();
        co_return std::move(exchange->response);
    }

    ObjectConnectionStats stats() const override {
        return ObjectConnectionStats{resolutions_.load(), connections_.load(), reused_.load(), closed_.load(),
                                     overflow_.load()};
    }

private:
    // https (no workspace Zetta is reached that way today) goes through Zetta's client as before.
    static liquid::runtime::task<ObjectResponse> unpooled(ObjectRequest input) {
        const zetta::connect::AsyncHttpTransport& transport = zetta::connect::default_async_http_transport();
        if (!transport) {
            co_return ObjectResponse{0, {}, "Zetta HTTP transport is not installed"};
        }
        zetta::connect::HttpRequest request;
        request.method = std::move(input.method);
        request.url = std::move(input.url);
        request.headers = std::move(input.headers);
        request.body = std::move(input.body);
        request.timeout_seconds = input.timeout_seconds;
        request.secret_headers = {"Authorization"};
        auto [transport_error, response] = co_await transport(request);
        if (transport_error) {
            co_return ObjectResponse{0, {}, transport_error.message()};
        }
        co_return ObjectResponse{response.status, std::move(response.body), std::move(response.error)};
    }

    // Keeps run() from returning, and sweeps every sweep_interval.
    boost::capy::task<> keepalive() {
        net::timer timer(io_context_);
        for (;;) {
            timer.expires_after(options_.sweep_interval);
            [[maybe_unused]] const auto waited = co_await timer.wait();
            sweep();
        }
    }

    // Closes, for every origin, the kept connections idle past idle_limit or found closed, and
    // forgets resolutions past resolve_ttl. Each origin's connections are kept oldest first.
    void sweep() {
        const auto now = clock::now();
        for (auto origin = idle_.begin(); origin != idle_.end();) {
            auto& kept = origin->second;
            std::erase_if(kept, [&](std::unique_ptr<Connection>& connection) {
                if (now - connection->idle_since < options_.idle_limit && still_open(connection->socket)) {
                    return false;
                }
                closed_.fetch_add(1);
                connection->socket.close();
                return true;
            });
            origin = kept.empty() ? idle_.erase(origin) : std::next(origin);
        }
        std::erase_if(resolved_, [&](const auto& entry) { return now - entry.second.first >= options_.resolve_ttl; });
    }

    // The most recently used open connection to `origin`, or none.
    std::unique_ptr<Connection> take_idle(const std::string& origin) {
        const auto found = idle_.find(origin);
        if (found == idle_.end()) {
            return nullptr;
        }
        auto& kept = found->second;
        while (!kept.empty()) {
            std::unique_ptr<Connection> connection = std::move(kept.back());
            kept.pop_back();
            if (clock::now() - connection->idle_since < options_.idle_limit && still_open(connection->socket)) {
                return connection;
            }
            closed_.fetch_add(1);
            connection->socket.close();
        }
        return nullptr;
    }

    void drop_idle(const std::string& origin) {
        const auto found = idle_.find(origin);
        if (found == idle_.end()) {
            return;
        }
        for (auto& connection : found->second) {
            closed_.fetch_add(1);
            connection->socket.close();
        }
        idle_.erase(found);
    }

    void keep(const std::string& origin, std::unique_ptr<Connection> connection) {
        auto& kept = idle_[origin];
        if (kept.size() >= options_.idle_per_origin) {
            overflow_.fetch_add(1);
            connection->socket.close();
            return;
        }
        connection->idle_since = clock::now();
        kept.push_back(std::move(connection));
    }

    // The addresses of `target`, from a resolution younger than resolve_ttl or a new one. Dials
    // that need the same origin's addresses at once share one resolution.
    boost::capy::io_task<std::vector<net::endpoint>> addresses(const Target& target, bool fresh) {
        const std::string origin = target.host + ":" + target.port;
        // A resolution shared with the request that started it ends when that request is
        // cancelled; its waiters then resolve again rather than fail with its cancellation.
        for (;;) {
            if (!fresh) {
                if (const auto found = resolved_.find(origin);
                    found != resolved_.end() && clock::now() - found->second.first < options_.resolve_ttl) {
                    co_return {{}, found->second.second};
                }
            }
            const auto pending = resolving_.find(origin);
            if (pending == resolving_.end()) {
                break;
            }
            const std::shared_ptr<Resolution> shared = pending->second;
            auto [waited] = co_await shared->done.wait();
            if (waited) {
                co_return {waited, {}}; // this request itself was cancelled
            }
            if (shared->error == boost::capy::cond::canceled) {
                continue;
            }
            co_return {shared->error, shared->endpoints};
        }
        auto resolution = std::make_shared<Resolution>();
        resolving_[origin] = resolution;
        resolutions_.fetch_add(1);
        std::error_code error;
        net::resolver resolver(io_context_);
        if (options_.resolve_delay.count() > 0) {
            auto [waited] = co_await boost::capy::delay(options_.resolve_delay);
            error = waited;
        }
        if (!error) {
            auto [resolve_error, results] = co_await resolver.resolve(target.host, target.port);
            error = resolve_error;
            if (!error) {
                for (const auto& entry : results) {
                    resolution->endpoints.push_back(entry.get_endpoint());
                }
            }
        }
        resolution->error = error;
        if (error) {
            resolved_.erase(origin);
        } else {
            resolved_[origin] = {clock::now(), resolution->endpoints};
        }
        resolving_.erase(origin);
        resolution->done.set();
        co_return {error, resolution->endpoints};
    }

    // A new connection to `target`. A cached address that refuses is resolved again once.
    boost::capy::io_task<std::string> dial(const Target& target, Connection& connection) {
        for (const bool fresh : {false, true}) {
            auto [resolve_error, endpoints] = co_await addresses(target, fresh);
            if (resolve_error) {
                co_return {{}, "could not resolve " + target.host + ": " + resolve_error.message()};
            }
            std::string reason = "no address resolved for " + target.host;
            for (const net::endpoint& endpoint : endpoints) {
                if (connection.socket.is_open()) {
                    connection.socket.close();
                }
                // connect() opens the socket for the endpoint's family (IPv4 or IPv6), and throws
                // when it cannot; nothing may unwind on the worker.
                std::error_code connect_error;
                try {
                    auto [connected] =
                        co_await boost::capy::timeout(connection.socket.connect(endpoint), options_.connect_timeout);
                    connect_error = connected;
                } catch (const std::system_error& error) {
                    reason = "could not open a socket for " + target.host + ": " + error.code().message();
                    continue;
                }
                if (!connect_error) {
                    set_socket_options(connection.socket);
                    connections_.fetch_add(1);
                    co_return {{}, std::string()};
                }
                if (connect_error == boost::capy::cond::canceled) {
                    co_return {{}, "the request was cancelled while it connected"};
                }
                if (connect_error == boost::capy::cond::timeout) {
                    // The address may be gone with its peer: resolve again next time, and do not
                    // try it again here.
                    resolved_.erase(target.host + ":" + target.port);
                    co_return {{}, "could not connect to " + target.host + ":" + target.port +
                                       ": no answer within " +
                                       std::to_string(options_.connect_timeout.count()) + " ms"};
                }
                reason = "could not connect to " + target.host + ":" + target.port + ": " + connect_error.message();
            }
            if (fresh) {
                co_return {{}, reason};
            }
            resolved_.erase(target.host + ":" + target.port);
        }
        co_return {{}, std::string("could not connect to ") + target.host};
    }

    // One attempt: a kept connection or a new one, then the exchange. `connection` is left set to
    // a connection fit to keep, or null.
    boost::capy::io_task<std::string> attempt(const Target& target, Exchange& exchange,
                                              std::unique_ptr<Connection>& connection, bool& reused) {
        const std::string origin = target.host + ":" + target.port;
        connection = exchange.request.fresh_connection ? nullptr : take_idle(origin);
        reused = connection != nullptr;
        if (connection) {
            reused_.fetch_add(1);
        } else {
            connection = std::make_unique<Connection>(io_context_);
            auto [ignored, reason] = co_await dial(target, *connection);
            if (!reason.empty()) {
                connection.reset();
                co_return {{}, reason};
            }
        }
        bool reusable = false;
        auto [ignored, reason] = co_await exchange_over(connection->socket, exchange.request, target,
                                                        exchange.response, parser_config_, serializer_config_,
                                                        reusable);
        if (!reason.empty() || !reusable) {
            connection->socket.close();
            connection.reset();
        }
        co_return {{}, reason};
    }

    void finish(const std::shared_ptr<Exchange>& exchange) {
        {
            std::lock_guard lock(pending_mutex_);
            pending_.erase(exchange);
        }
        exchange->finished.set();
    }

    boost::capy::task<> perform(std::shared_ptr<Exchange> exchange) {
        Target target;
        if (!parse_plain_url(exchange->request.url, target)) {
            exchange->response.error = "not an absolute http URL";
            finish(exchange);
            co_return;
        }
        const std::chrono::seconds deadline(exchange->request.timeout_seconds > 0 ? exchange->request.timeout_seconds
                                                                                  : 60);
        std::unique_ptr<Connection> connection;
        bool reused = false;
        auto [timeout_error, reason] =
            co_await boost::capy::timeout(attempt(target, *exchange, connection, reused), deadline);
        if (timeout_error) {
            reason = "the request timed out: " + timeout_error.message();
            if (connection) {
                connection->socket.close();
                connection.reset();
            }
        }
        if (!reason.empty() && reused) {
            // A kept connection failed or went silent: the peer may be gone, and with it every
            // connection kept to it. The next request dials afresh.
            drop_idle(target.host + ":" + target.port);
        }
        if (!reason.empty()) {
            // Only a transport failure: what was read of an answer is not one.
            exchange->response = ObjectResponse{0, {}, std::move(reason)};
        } else if (connection) {
            keep(target.host + ":" + target.port, std::move(connection));
        }
        finish(exchange);
    }

    PooledObjectTransportOptions options_;
    net::io_context io_context_;
    std::thread worker_;
    http::shared_parser_config parser_config_;
    http::shared_serializer_config serializer_config_;
    // The io_context thread's alone.
    std::map<std::string, std::vector<std::unique_ptr<Connection>>> idle_;
    std::map<std::string, std::pair<clock::time_point, std::vector<net::endpoint>>> resolved_;
    std::map<std::string, std::shared_ptr<Resolution>> resolving_;
    std::atomic<std::uint64_t> resolutions_{0};
    std::atomic<std::uint64_t> connections_{0};
    std::atomic<std::uint64_t> reused_{0};
    std::atomic<std::uint64_t> closed_{0};
    std::atomic<std::uint64_t> overflow_{0};
    // The requests handed to the worker and not yet answered, answered by shutdown().
    std::mutex pending_mutex_;
    std::set<std::shared_ptr<Exchange>> pending_;
    std::atomic<bool> stopped_{false};
};

} // namespace

std::shared_ptr<PooledObjectTransport> make_pooled_object_transport(PooledObjectTransportOptions options) {
    return std::make_shared<PooledTransport>(options);
}

} // namespace lake::control
