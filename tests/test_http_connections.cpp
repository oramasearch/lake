// lake#534: connections apart from request workers. An idle or still-arriving connection holds no
// worker, idle keep-alive connections and slow headers are closed after their timeouts, the
// connection cap answers 503, refusals close their connection, and streaming still works.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/http/server/router.hpp>

#include "api/http_connections.hpp"
#include "lake/api/server.hpp"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Two hundred client sockets and their two hundred accepted peers live in this one process; the
// macOS default soft limit is 256.
void raise_descriptor_limit(rlim_t wanted) {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= wanted) {
        return;
    }
    limit.rlim_cur = std::min(wanted, limit.rlim_max);
    static_cast<void>(::setrlimit(RLIMIT_NOFILE, &limit));
}

class Client {
public:
    explicit Client(unsigned short port, std::chrono::milliseconds receive_timeout = 5s) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        set_receive_timeout(receive_timeout);
#ifdef SO_NOSIGPIPE
        // A send to a connection lake closed must fail, not raise SIGPIPE (macOS).
        int one = 1;
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Client() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] bool connected() const {
        return fd_ >= 0;
    }

    void set_receive_timeout(std::chrono::milliseconds timeout) {
        timeval value{};
        value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
        value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)));
    }

    bool send(std::string_view bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t count = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL_OR_ZERO);
            if (count <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    // One response: its header and Content-Length bytes of body. Empty when the connection closed
    // or the receive timed out first.
    std::string response() {
        for (;;) {
            const auto header_end = buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::size_t length = content_length(buffer_.substr(0, header_end));
                const std::size_t total = header_end + 4 + length;
                if (buffer_.size() >= total) {
                    std::string whole = buffer_.substr(0, total);
                    buffer_.erase(0, total);
                    return whole;
                }
            }
            if (!receive()) {
                return {};
            }
        }
    }

    // True when the peer closed (EOF or reset) before the receive timeout.
    bool closed_by_peer() {
        char byte = 0;
        const ssize_t count = ::recv(fd_, &byte, 1, 0);
        if (count == 0) {
            return true;
        }
        return count < 0 && (errno == ECONNRESET || errno == EPIPE);
    }

private:
#ifdef MSG_NOSIGNAL
    static constexpr int MSG_NOSIGNAL_OR_ZERO = MSG_NOSIGNAL;
#else
    static constexpr int MSG_NOSIGNAL_OR_ZERO = 0;
#endif

    static std::size_t content_length(const std::string& header) {
        std::string lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto at = lower.find("\r\ncontent-length:");
        if (at == std::string::npos) {
            return 0;
        }
        return static_cast<std::size_t>(std::stoull(lower.substr(at + 17)));
    }

    bool receive() {
        char chunk[16384];
        const ssize_t count = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (count <= 0) {
            return false;
        }
        buffer_.append(chunk, static_cast<std::size_t>(count));
        return true;
    }

    int fd_ = -1;
    std::string buffer_;
};

std::string get(std::string_view path, bool keep_alive = true) {
    return "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: " +
           (keep_alive ? "keep-alive" : "close") + "\r\n\r\n";
}

std::string header_value(const std::string& response, std::string_view name) {
    std::string lower = response.substr(0, response.find("\r\n\r\n"));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto at = lower.find("\r\n" + std::string(name) + ":");
    if (at == std::string::npos) {
        return {};
    }
    const auto start = lower.find_first_not_of(' ', at + 3 + name.size());
    return lower.substr(start, lower.find("\r\n", start) - start);
}

constexpr std::size_t stream_size = 4U * 1024U * 1024U;

// HttpConnections over a small router: /fine answers, /hold keeps its worker until released,
// /busy and /limited refuse, /stream streams 4 MiB, /healthz needs no worker.
class Served {
public:
    explicit Served(lake::api::HttpConnectionLimits limits) {
        namespace http = boost::http;
        http::router<http::route_params> router;
        const auto answer = [](http::route_params& route, http::status status,
                               std::string body) -> http::route_task {
            route.status(status);
            route.res.set(http::field::content_type, "text/plain");
            route.res.set_payload_size(body.size());
            auto [error] = co_await route.send(body);
            if (error) {
                co_return http::route_error(error);
            }
            co_return http::route_done;
        };
        router.add(http::method::get, "/fine", [answer](http::route_params& route) {
            return answer(route, http::status::ok, "ok");
        });
        router.add(http::method::get, "/healthz", [answer](http::route_params& route) {
            return answer(route, http::status::ok, "healthy");
        });
        router.add(http::method::get, "/busy", [answer](http::route_params& route) {
            return answer(route, http::status::service_unavailable, "busy");
        });
        router.add(http::method::get, "/limited", [answer](http::route_params& route) {
            return answer(route, http::status::too_many_requests, "limited");
        });
        router.add(http::method::get,
                   "/hold",
                   [this, answer](http::route_params& route) -> http::route_task {
                       holding_.fetch_add(1);
                       boost::corosio::timer timer(io_context_);
                       while (!released_.load()) {
                           timer.expires_after(5ms);
                           auto [error] = co_await timer.wait();
                           if (error) {
                               co_return http::route_error(error);
                           }
                       }
                       co_return co_await answer(route, http::status::ok, "held");
                   });
        router.add(http::method::get,
                   "/stream",
                   [this](http::route_params& route) -> http::route_task {
                       route.status(http::status::ok);
                       route.res.set(http::field::content_type, "application/octet-stream");
                       route.res.set_payload_size(stream_size);
                       const std::string chunk(64U * 1024U, 's');
                       boost::corosio::timer timer(io_context_);
                       std::size_t sent = 0;
                       while (sent < stream_size) {
                           std::array<boost::capy::mutable_buffer, 16> buffers{};
                           const auto output = route.res_body.prepare(buffers);
                           const auto copied = boost::capy::buffer_copy(
                               output,
                               boost::capy::const_buffer(chunk.data(),
                                                         std::min(chunk.size(), stream_size - sent)));
                           sent += copied;
                           if (sent == stream_size) {
                               auto [error] = co_await route.res_body.commit_eof(copied);
                               if (error) {
                                   co_return http::route_error(error);
                               }
                               co_return http::route_done;
                           }
                           auto [error] = co_await route.res_body.commit(copied);
                           if (error) {
                               co_return http::route_error(error);
                           }
                           // Yields between chunks, as a pull reading its store does.
                           timer.expires_after(1ms);
                           static_cast<void>(co_await timer.wait());
                       }
                       co_return http::route_done;
                   });
        limits.needs_no_worker = [](std::string_view path) { return path == "/healthz"; };
        connections_ = std::make_unique<lake::api::HttpConnections>(
            io_context_,
            std::move(router),
            http::make_parser_config(http::parser_config{true}),
            http::make_serializer_config(http::serializer_config()),
            std::move(limits));
        const std::string error = connections_->bind(
            boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), 0));
        if (!error.empty()) {
            ADD_FAILURE() << error;
            return;
        }
        connections_->start();
        thread_ = std::thread([this] { io_context_.run(); });
    }

    ~Served() {
        released_.store(true);
        if (connections_) {
            connections_->stop(2s);
        }
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] unsigned short port() const {
        return connections_->port();
    }
    [[nodiscard]] lake::api::HttpConnectionStats stats() const {
        return connections_->stats();
    }
    void release() {
        released_.store(true);
    }
    [[nodiscard]] int holding() const {
        return holding_.load();
    }
    void begin_drain() {
        connections_->begin_drain();
    }

    // Polls a stats predicate until it holds or two seconds pass.
    template <class Predicate> bool eventually(Predicate predicate) const {
        const auto deadline = Clock::now() + 2s;
        while (Clock::now() < deadline) {
            if (predicate(stats())) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return predicate(stats());
    }

private:
    boost::corosio::io_context io_context_;
    std::unique_ptr<lake::api::HttpConnections> connections_;
    std::thread thread_;
    std::atomic<bool> released_{false};
    std::atomic<int> holding_{0};
};

lake::api::HttpConnectionLimits limits_with(std::size_t workers) {
    lake::api::HttpConnectionLimits limits;
    limits.workers = workers;
    return limits;
}

} // namespace

// The issue: 32 idle connections used to take every worker. Two hundred connections, half idle
// after a request and half silent, leave both workers free, and the next request answers at once.
TEST(HttpConnections, TwoHundredIdleConnectionsLeaveTheWorkersFree) {
    raise_descriptor_limit(2048);
    Served served(limits_with(2));
    std::vector<std::unique_ptr<Client>> idle;
    for (int i = 0; i < 200; ++i) {
        auto client = std::make_unique<Client>(served.port());
        ASSERT_TRUE(client->connected()) << "connection " << i;
        if (i % 2 == 0) {
            ASSERT_TRUE(client->send(get("/fine")));
            ASSERT_TRUE(client->response().starts_with("HTTP/1.1 200")) << "connection " << i;
        }
        idle.push_back(std::move(client));
    }
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.connections == 200; }));
    EXPECT_EQ(served.stats().active_requests, 0U);

    Client active(served.port());
    const auto started = Clock::now();
    ASSERT_TRUE(active.send(get("/fine")));
    const std::string response = active.response();
    const auto elapsed = Clock::now() - started;
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    EXPECT_LT(elapsed, 500ms);
    EXPECT_EQ(served.stats().refused_connections, 0U);
}

TEST(HttpConnections, AnIdleKeepAliveConnectionIsClosedAfterTheIdleTimeout) {
    auto limits = limits_with(2);
    limits.idle_timeout = 300ms;
    Served served(std::move(limits));
    Client client(served.port());
    ASSERT_TRUE(client.send(get("/fine")));
    ASSERT_TRUE(client.response().starts_with("HTTP/1.1 200"));
    // Still open within the timeout: a second request on it is served.
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(client.send(get("/fine")));
    ASSERT_TRUE(client.response().starts_with("HTTP/1.1 200"));
    const auto idle_since = Clock::now();
    EXPECT_TRUE(client.closed_by_peer());
    const auto idle_for = Clock::now() - idle_since;
    EXPECT_GE(idle_for, 250ms);
    EXPECT_LT(idle_for, 3s);
    EXPECT_TRUE(served.eventually([](const auto& stats) {
        return stats.idle_timeouts == 1 && stats.connections == 0;
    }));
}

// Slowloris: a header trickled a byte at a time is cut off at the header timeout, however steadily
// the bytes keep coming; a connection that never sends is closed at the same timeout.
TEST(HttpConnections, ASlowHeaderIsCutOff) {
    auto limits = limits_with(2);
    limits.header_timeout = 400ms;
    limits.idle_timeout = 10s;
    Served served(std::move(limits));

    Client slow(served.port());
    const std::string request = get("/fine");
    const auto started = Clock::now();
    bool sent_whole = true;
    for (std::size_t i = 0; i + 4 < request.size(); ++i) {
        if (!slow.send(request.substr(i, 1))) {
            sent_whole = false;
            break;
        }
        std::this_thread::sleep_for(50ms);
        if (Clock::now() - started > 2s) {
            break;
        }
    }
    static_cast<void>(sent_whole);
    EXPECT_TRUE(slow.closed_by_peer());
    EXPECT_LT(Clock::now() - started, 4s);

    Client silent(served.port());
    const auto silent_since = Clock::now();
    EXPECT_TRUE(silent.closed_by_peer());
    EXPECT_GE(Clock::now() - silent_since, 350ms);

    // A kept-alive connection's next header has the same bound from its first byte.
    Client kept(served.port());
    ASSERT_TRUE(kept.send(get("/fine")));
    ASSERT_TRUE(kept.response().starts_with("HTTP/1.1 200"));
    ASSERT_TRUE(kept.send("GET /fine HTTP/1.1\r\n"));
    const auto partial_since = Clock::now();
    EXPECT_TRUE(kept.closed_by_peer());
    EXPECT_LT(Clock::now() - partial_since, 3s);

    EXPECT_TRUE(served.eventually([](const auto& stats) { return stats.header_timeouts == 3; }))
        << served.stats().header_timeouts;
    EXPECT_EQ(served.stats().idle_timeouts, 0U);
}

TEST(HttpConnections, TheConnectionCapAnswers503AndCloses) {
    auto limits = limits_with(2);
    limits.max_connections = 3;
    Served served(std::move(limits));
    std::vector<std::unique_ptr<Client>> admitted;
    for (int i = 0; i < 3; ++i) {
        admitted.push_back(std::make_unique<Client>(served.port()));
        ASSERT_TRUE(admitted.back()->send(get("/fine")));
        ASSERT_TRUE(admitted.back()->response().starts_with("HTTP/1.1 200"));
    }
    Client over(served.port());
    ASSERT_TRUE(over.connected());
    const std::string refused = over.response();
    ASSERT_TRUE(refused.starts_with("HTTP/1.1 503")) << refused;
    EXPECT_EQ(header_value(refused, "connection"), "close");
    EXPECT_EQ(header_value(refused, "retry-after"), "1");
    EXPECT_NE(refused.find("\"code\":\"busy\""), std::string::npos);
    EXPECT_TRUE(over.closed_by_peer());
    EXPECT_EQ(served.stats().refused_connections, 1U);

    // A closed connection makes room.
    admitted.front().reset();
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.connections == 2; }));
    Client next(served.port());
    ASSERT_TRUE(next.send(get("/fine")));
    EXPECT_TRUE(next.response().starts_with("HTTP/1.1 200"));
}

// Refusals give the connection back (as #528 did for the registry): 503 and 429 close it, a 200
// keeps it.
TEST(HttpConnections, RefusalsCloseTheirConnection) {
    Served served(limits_with(2));
    for (const char* path : {"/busy", "/limited"}) {
        Client client(served.port());
        ASSERT_TRUE(client.send(get("/fine")));
        const std::string fine = client.response();
        ASSERT_TRUE(fine.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(header_value(fine, "connection").empty()) << fine;
        ASSERT_TRUE(client.send(get(path)));
        const std::string refused = client.response();
        EXPECT_TRUE(refused.starts_with(path == std::string_view("/busy") ? "HTTP/1.1 503"
                                                                         : "HTTP/1.1 429"))
            << refused;
        EXPECT_EQ(header_value(refused, "connection"), "close") << refused;
        EXPECT_TRUE(client.closed_by_peer()) << path;
    }
}

// A request that finds every worker taken waits for one; past worker_wait it is answered 503 and
// its connection closed. The probes need no worker.
TEST(HttpConnections, ARequestWaitsForAWorkerThenIsRefused) {
    auto limits = limits_with(1);
    limits.worker_wait = 300ms;
    Served served(std::move(limits));
    Client holder(served.port());
    ASSERT_TRUE(holder.send(get("/hold")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.holding() == 1; }));

    Client probe(served.port());
    ASSERT_TRUE(probe.send(get("/healthz")));
    EXPECT_TRUE(probe.response().starts_with("HTTP/1.1 200"));

    Client refused(served.port());
    const auto started = Clock::now();
    ASSERT_TRUE(refused.send(get("/fine")));
    const std::string busy = refused.response();
    EXPECT_GE(Clock::now() - started, 250ms);
    ASSERT_TRUE(busy.starts_with("HTTP/1.1 503")) << busy;
    EXPECT_EQ(header_value(busy, "connection"), "close");
    EXPECT_TRUE(refused.closed_by_peer());
    EXPECT_EQ(served.stats().busy_refusals, 1U);

    // A request waiting when the worker is released gets it.
    Client waiting(served.port());
    ASSERT_TRUE(waiting.send(get("/fine")));
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.waiting_requests == 1; }));
    served.release();
    EXPECT_TRUE(holder.response().starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(waiting.response().starts_with("HTTP/1.1 200"));
    EXPECT_EQ(served.stats().busy_refusals, 1U);
}

TEST(HttpConnections, AStreamingResponseCompletesBesideIdleConnections) {
    raise_descriptor_limit(1024);
    Served served(limits_with(2));
    std::vector<std::unique_ptr<Client>> idle;
    for (int i = 0; i < 100; ++i) {
        idle.push_back(std::make_unique<Client>(served.port()));
        ASSERT_TRUE(idle.back()->send(get("/fine")));
        ASSERT_TRUE(idle.back()->response().starts_with("HTTP/1.1 200"));
    }
    Client streaming(served.port());
    ASSERT_TRUE(streaming.send(get("/stream")));
    const std::string response = streaming.response();
    ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << response.substr(0, 200);
    const auto body = response.size() - (response.find("\r\n\r\n") + 4);
    EXPECT_EQ(body, stream_size);
    // The connection stays usable after the stream.
    ASSERT_TRUE(streaming.send(get("/fine")));
    EXPECT_TRUE(streaming.response().starts_with("HTTP/1.1 200"));
}

TEST(HttpConnections, TheDrainClosesIdleConnections) {
    Served served(limits_with(2));
    Client idle(served.port());
    ASSERT_TRUE(idle.send(get("/fine")));
    ASSERT_TRUE(idle.response().starts_with("HTTP/1.1 200"));
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.connections == 1; }));
    const auto started = Clock::now();
    served.begin_drain();
    EXPECT_TRUE(idle.closed_by_peer());
    EXPECT_LT(Clock::now() - started, 2s);
}

// The same through ApiServer and its options: two hundred idle connections, then a request that
// needs a worker answers within its budget, and an idle connection is closed at its timeout.
TEST(HttpConnections, ApiServerServesBesideTwoHundredIdleConnections) {
    raise_descriptor_limit(2048);
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 2;
    options.idle_timeout = 1500ms;
    options.node_name = "connections-test";
    options.instance_kek = "connections-test-kek";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    std::vector<std::unique_ptr<Client>> idle;
    for (int i = 0; i < 200; ++i) {
        idle.push_back(std::make_unique<Client>(server.port()));
        ASSERT_TRUE(idle.back()->connected()) << "connection " << i;
        ASSERT_TRUE(idle.back()->send(get("/v1/server_info")));
        ASSERT_TRUE(idle.back()->response().starts_with("HTTP/1.1 200")) << "connection " << i;
    }
    Client active(server.port());
    const auto started = Clock::now();
    ASSERT_TRUE(active.send(get("/v1/server_info")));
    const std::string response = active.response();
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response.substr(0, 200);
    EXPECT_LT(Clock::now() - started, 1s);

    const auto waited_from = Clock::now();
    EXPECT_TRUE(idle.front()->closed_by_peer());
    EXPECT_LT(Clock::now() - waited_from, 4s);
    server.stop();
}
