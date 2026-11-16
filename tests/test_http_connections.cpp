// SPDX-License-Identifier: MIT

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
#include <fstream>
#include <limits>
#include <iostream>
#include <mutex>
#include <tuple>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/read.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/http/server/router.hpp>

#include "api/http_connections.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
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
    [[nodiscard]] int descriptor() const {
        return fd_;
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

    // The header of one response, without reading a body (a HEAD answer).
    std::string header_only() {
        for (;;) {
            const auto header_end = buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string header = buffer_.substr(0, header_end + 4);
                buffer_.erase(0, header_end + 4);
                return header;
            }
            if (!receive()) {
                return {};
            }
        }
    }

    // Whatever arrives before the receive timeout; empty when nothing did.
    std::string anything() {
        if (!buffer_.empty()) {
            return std::exchange(buffer_, {});
        }
        receive();
        return std::exchange(buffer_, {});
    }

    // Closes with a reset, as a client that gave up does.
    void reset() {
        linger abort{1, 0};
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &abort, sizeof(abort)));
        ::close(fd_);
        fd_ = -1;
    }

    void shutdown_send() {
        static_cast<void>(::shutdown(fd_, SHUT_WR));
    }

    void set_send_timeout(std::chrono::milliseconds timeout) {
        timeval value{};
        value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
        value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)));
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
    explicit Served(lake::api::HttpConnectionLimits limits, std::size_t max_header_bytes = 0) {
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
        router.add(http::method::head,
                   "/fine",
                   [](http::route_params& route) -> http::route_task {
                       route.status(http::status::ok);
                       route.res.set(http::field::content_type, "text/plain");
                       route.res.set_payload_size(2);
                       auto [error] = co_await route.res_body.commit_eof(0);
                       if (error) {
                           co_return http::route_error(error);
                       }
                       co_return http::route_done;
                   });
        // Reads the whole body, Content-Length or chunked, and answers its size.
        router.add(http::method::post,
                   "/echo",
                   [answer](http::route_params& route) -> http::route_task {
                       std::vector<unsigned char> body;
                       auto [error, size] = co_await boost::capy::read(
                           route.req_body, boost::capy::dynamic_buffer(body, 64U * 1024U * 1024U));
                       static_cast<void>(size);
                       if (error) {
                           co_return http::route_error(error);
                       }
                       co_return co_await answer(route, http::status::ok,
                                                 "size=" + std::to_string(body.size()));
                   });
        // Answer without reading the body.
        router.add(http::method::post, "/ignore", [answer](http::route_params& route) {
            return answer(route, http::status::ok, "ignored");
        });
        router.add(http::method::post, "/busy", [answer](http::route_params& route) {
            return answer(route, http::status::service_unavailable, "busy");
        });
        // A long poll: gives its worker back, then waits until released.
        router.add(http::method::get,
                   "/park",
                   [this, answer](http::route_params& route) -> http::route_task {
                       std::string who;
                       if (auto found = route.url.params().find("who");
                           found != route.url.params().end()) {
                           who = (*found).value;
                       }
                       if (!connections_->leave_worker(route, who)) {
                           route.res.set(http::field::retry_after, "1");
                           co_return co_await answer(route, http::status::too_many_requests,
                                                     "too many long requests");
                       }
                       parked_.fetch_add(1);
                       boost::corosio::timer timer(io_context_);
                       while (!released_.load()) {
                           timer.expires_after(5ms);
                           auto [error] = co_await timer.wait();
                           if (error) {
                               co_return http::route_error(error);
                           }
                       }
                       co_return co_await answer(route, http::status::ok, "parked");
                   });
        // 64 MiB, for a client that never reads.
        router.add(http::method::get,
                   "/big",
                   [](http::route_params& route) -> http::route_task {
                       constexpr std::size_t total = 64U * 1024U * 1024U;
                       route.status(http::status::ok);
                       route.res.set(http::field::content_type, "application/octet-stream");
                       route.res.set_payload_size(total);
                       const std::string chunk(256U * 1024U, 'b');
                       std::size_t sent = 0;
                       while (sent < total) {
                           std::array<boost::capy::mutable_buffer, 16> buffers{};
                           const auto output = route.res_body.prepare(buffers);
                           const auto copied = boost::capy::buffer_copy(
                               output, boost::capy::const_buffer(chunk.data(),
                                                                 std::min(chunk.size(), total - sent)));
                           sent += copied;
                           auto [error] = sent == total ? co_await route.res_body.commit_eof(copied)
                                                        : co_await route.res_body.commit(copied);
                           if (error) {
                               co_return http::route_error(error);
                           }
                       }
                       co_return http::route_done;
                   });
        if (!limits.refused) {
            limits.refused = [this](std::string_view method, unsigned status, std::string_view reason) {
                std::lock_guard lock(mutex_);
                refusals_.emplace_back(std::string(method), status, std::string(reason));
            };
        }
        limits.needs_no_worker = [](std::string_view path) { return path == "/healthz"; };
        connections_ = std::make_unique<lake::api::HttpConnections>(
            io_context_,
            std::move(router),
            http::make_parser_config([&] {
                http::parser_config config{true};
                if (max_header_bytes > 0) {
                    lake::api::apply_header_limits(config, max_header_bytes);
                }
                return config;
            }()),
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
    [[nodiscard]] int parked() const {
        return parked_.load();
    }
    [[nodiscard]] std::vector<std::tuple<std::string, unsigned, std::string>> refusals() const {
        std::lock_guard lock(mutex_);
        return refusals_;
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
    std::atomic<int> parked_{0};
    mutable std::mutex mutex_;
    std::vector<std::tuple<std::string, unsigned, std::string>> refusals_;
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
    // A browser can read the refusal, and a HEAD refusal has no body.
    EXPECT_EQ(header_value(busy, "access-control-allow-origin"), "*");
    Client head(served.port());
    ASSERT_TRUE(head.send("HEAD /fine HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const std::string head_busy = head.header_only();
    ASSERT_TRUE(head_busy.starts_with("HTTP/1.1 503")) << head_busy;
    EXPECT_TRUE(head.closed_by_peer()) << "a HEAD refusal carries no body";
    EXPECT_EQ(served.stats().busy_refusals, 2U);
    // Both are reported, so lake logs and counts them.
    const auto reported = served.refusals();
    ASSERT_EQ(reported.size(), 2U);
    EXPECT_EQ(reported[0], std::make_tuple(std::string("GET"), 503U, std::string("workers_busy")));
    EXPECT_EQ(reported[1], std::make_tuple(std::string("HEAD"), 503U, std::string("workers_busy")));

    // A request waiting when the worker is released gets it.
    Client waiting(served.port());
    ASSERT_TRUE(waiting.send(get("/fine")));
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.waiting_requests == 1; }));
    served.release();
    EXPECT_TRUE(holder.response().starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(waiting.response().starts_with("HTTP/1.1 200"));
    EXPECT_EQ(served.stats().busy_refusals, 2U);
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

namespace {

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0, resident = 0;
    statm >> pages >> resident;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

std::string post(std::string_view path, std::string_view body, std::string_view extra = {}) {
    return "POST " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n" + std::string(extra) + "\r\n" + std::string(body);
}

} // namespace

// Review item 2: long polls (and streamed pulls) leave their worker, so forty of them never fill
// the two short-request workers.
TEST(HttpConnections, FortyLongPollsLeaveTheShortRequestWorkersFree) {
    Served served(limits_with(2));
    std::vector<std::unique_ptr<Client>> polls;
    for (int i = 0; i < 40; ++i) {
        polls.push_back(std::make_unique<Client>(served.port()));
        ASSERT_TRUE(polls.back()->send(get("/park")));
    }
    ASSERT_TRUE(served.eventually([&served](const auto& stats) {
        return served.parked() == 40 && stats.long_requests == 40;
    })) << served.parked();
    EXPECT_EQ(served.stats().active_requests, 0U);
    Client short_request(served.port());
    const auto started = Clock::now();
    ASSERT_TRUE(short_request.send(get("/fine")));
    EXPECT_TRUE(short_request.response().starts_with("HTTP/1.1 200"));
    EXPECT_LT(Clock::now() - started, 500ms);
    served.release();
    for (auto& poll : polls) {
        EXPECT_TRUE(poll->response().starts_with("HTTP/1.1 200"));
    }
    EXPECT_TRUE(served.eventually([](const auto& stats) { return stats.long_requests == 0; }));
    EXPECT_EQ(served.stats().busy_refusals, 0U);
}

// The registry gives the worker back before a capability parks on `wait_ms`, and only then.
TEST(HttpConnections, ALongPollCapabilityGivesItsWorkerBack) {
    lake::api::Registry registry;
    int left_at_handler = -1;
    int left = 0;
    ASSERT_TRUE(registry.add(lake::api::Capability{
        .name = "poll",
        .title = "Poll",
        .description = "Long-polls.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"wait_ms", {{"type", "integer"}, {"minimum", 0}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = lake::api::CapabilityKind::Read,
        .long_poll = true,
        .requires_auth = false,
        .async_handler = [&](const nlohmann::json&, lake::api::ApiContext&)
            -> liquid::runtime::task<lake::api::ApiResult> {
            left_at_handler = left;
            co_return lake::api::ApiResult::success(nlohmann::json::object());
        }}));
    lake::api::ApiContext context;
    context.leave_worker = [&left](const std::string&) {
        ++left;
        return true;
    };
    ASSERT_TRUE(liquid::runtime::block_on(registry.call_async("poll", {{"wait_ms", 25000}}, context))->ok);
    EXPECT_EQ(left_at_handler, 1);
    ASSERT_TRUE(liquid::runtime::block_on(registry.call_async("poll", {{"wait_ms", 0}}, context))->ok);
    ASSERT_TRUE(liquid::runtime::block_on(registry.call_async("poll", nlohmann::json::object(), context))->ok);
    EXPECT_EQ(left, 1);
    // A long poll with a default wait leaves its worker when the caller sends none.
    ASSERT_TRUE(registry.add(lake::api::Capability{
        .name = "poll_by_default",
        .title = "Poll by default",
        .description = "Long-polls 10 s unless told otherwise.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"wait_ms", {{"type", "integer"}, {"minimum", 0}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = lake::api::CapabilityKind::Read,
        .long_poll = true,
        .default_wait_ms = 10000,
        .requires_auth = false,
        .async_handler = [](const nlohmann::json&, lake::api::ApiContext&)
            -> liquid::runtime::task<lake::api::ApiResult> {
            co_return lake::api::ApiResult::success(nlohmann::json::object());
        }}));
    ASSERT_TRUE(liquid::runtime::block_on(
        registry.call_async("poll_by_default", nlohmann::json::object(), context))->ok);
    EXPECT_EQ(left, 2);
    ASSERT_TRUE(liquid::runtime::block_on(
        registry.call_async("poll_by_default", {{"wait_ms", 0}}, context))->ok);
    EXPECT_EQ(left, 2);
    // A full cap refuses the long poll with 429 before it parks.
    context.leave_worker = [](const std::string&) { return false; };
    const auto refused = liquid::runtime::block_on(registry.call_async("poll", {{"wait_ms", 1}}, context));
    ASSERT_TRUE(refused);
    EXPECT_FALSE(refused->ok);
    EXPECT_EQ(refused->error.code, "rate_limited");
}

// Review item 3: lake's idle timeout outlasts nginx's 60 s upstream keep-alive.
TEST(HttpConnections, TheIdleTimeoutOutlastsTheProxys) {
    EXPECT_EQ(lake::api::HttpConnectionLimits{}.idle_timeout, 75s);
    EXPECT_EQ(lake::api::ServerOptions{}.idle_timeout, 75s);
    EXPECT_GT(lake::api::ServerOptions{}.idle_timeout, 60s);
    EXPECT_EQ(lake::api::ServerOptions{}.io_timeout, 60s);
}

// Review item 4: a body trickled slower than the I/O bound, and a client that never reads, are
// cut off, and lake keeps serving.
TEST(HttpConnections, ATrickledBodyAndAClientThatNeverReadsAreCutOff) {
    auto limits = limits_with(2);
    limits.io_timeout = 300ms;
    Served served(std::move(limits));

    Client trickle(served.port());
    ASSERT_TRUE(trickle.send("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n"
                             "0123456789"));
    const auto started = Clock::now();
    EXPECT_TRUE(trickle.closed_by_peer());
    EXPECT_GE(Clock::now() - started, 250ms);
    EXPECT_LT(Clock::now() - started, 4s);
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.io_timeouts >= 1; }));

    Client deaf(served.port());
    ASSERT_TRUE(deaf.send(get("/big")));
    const auto before = served.stats().io_timeouts;
    const auto deadline = Clock::now() + 10s;
    while (served.stats().io_timeouts == before && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GT(served.stats().io_timeouts, before) << "a response nobody reads is never cut off";
    EXPECT_TRUE(served.eventually([](const auto& stats) { return stats.active_requests == 0; }));

    Client next(served.port());
    ASSERT_TRUE(next.send(get("/fine")));
    EXPECT_TRUE(next.response().starts_with("HTTP/1.1 200"));
}

// Review item 5: the answer to a request whose body is still arriving reaches the client; the
// close lingers instead of resetting.
TEST(HttpConnections, ARefusalReachesAClientStillSendingItsBody) {
    Served served(limits_with(2));
    for (int attempt = 0; attempt < 5; ++attempt) {
        Client client(served.port());
        client.set_send_timeout(3s);
        constexpr std::size_t declared = 8U * 1024U * 1024U;
        ASSERT_TRUE(client.send("POST /busy HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                                std::to_string(declared) + "\r\n\r\n"));
        std::thread sender([&client] {
            const std::string piece(64U * 1024U, 'x');
            for (std::size_t sent = 0; sent < declared; sent += piece.size()) {
                if (!client.send(piece)) {
                    return;
                }
            }
        });
        const std::string answer = client.response();
        sender.join();
        EXPECT_TRUE(answer.starts_with("HTTP/1.1 503")) << "attempt " << attempt << ": " << answer;
        EXPECT_EQ(header_value(answer, "connection"), "close");
    }
}

// Review item 6: a body the route did not read is never parsed as the next request. A small one
// is discarded and the connection kept; a large one closes the connection.
TEST(HttpConnections, AnUnreadBodyIsNeverParsedAsARequest) {
    Served served(limits_with(2));
    Client client(served.port(), 300ms);
    const std::string smuggled = get("/hold");
    ASSERT_TRUE(client.send(post("/ignore", smuggled)));
    const std::string ignored = client.response();
    ASSERT_TRUE(ignored.starts_with("HTTP/1.1 200")) << ignored;
    EXPECT_TRUE(header_value(ignored, "connection").empty()) << ignored;
    // Nothing else is answered: the body was not a request.
    EXPECT_TRUE(client.anything().empty());
    EXPECT_EQ(served.holding(), 0);
    client.set_receive_timeout(5s);
    ASSERT_TRUE(client.send(get("/fine")));
    EXPECT_TRUE(client.response().starts_with("HTTP/1.1 200"));

    Client large(served.port());
    std::string body(128U * 1024U, 'y');
    body.replace(0, smuggled.size(), smuggled);
    ASSERT_TRUE(large.send(post("/ignore", body)));
    const std::string answer = large.response();
    ASSERT_TRUE(answer.starts_with("HTTP/1.1 200")) << answer;
    EXPECT_EQ(header_value(answer, "connection"), "close");
    EXPECT_TRUE(large.closed_by_peer());
    EXPECT_EQ(served.holding(), 0);
    EXPECT_EQ(served.stats().unread_bodies, 1U);
}

// Review item 7: the connection cap fits in the open-file limit, with room kept for the rest.
TEST(HttpConnections, TheConnectionCapFitsTheOpenFileLimit) {
    EXPECT_EQ(lake::api::connections_for_open_files(4096, 1048576), 4096U);
    EXPECT_EQ(lake::api::connections_for_open_files(4096, 4096), 3584U);
    EXPECT_EQ(lake::api::connections_for_open_files(4096, 1024), 512U);
    EXPECT_EQ(lake::api::connections_for_open_files(4096, 256), 128U);
    EXPECT_EQ(lake::api::connections_for_open_files(4096, 1), 1U);
    rlimit before{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &before), 0);
    const auto budget = lake::api::fit_connections_to_open_files(4096);
    rlimit after{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &after), 0);
    EXPECT_GE(after.rlim_cur, before.rlim_cur);
    EXPECT_EQ(budget.open_files, after.rlim_cur == RLIM_INFINITY
                                     ? std::uint64_t{std::numeric_limits<std::uint32_t>::max()}
                                     : static_cast<std::uint64_t>(after.rlim_cur));
    EXPECT_EQ(budget.max_connections,
              lake::api::connections_for_open_files(4096, budget.open_files));
}

// lake#1108: with lake's limits a browser's Clerk cookies (a 12 KiB Cookie field, 20 KiB of headers)
// are answered, and past the total it is still 431, one field or many.
TEST(HttpConnections, ABrowsersClerkCookiesFitLakesHeaderLimits) {
    Served served(limits_with(1), lake::api::default_max_header_bytes);
    Client signed_in(served.port());
    ASSERT_TRUE(signed_in.send("GET /fine HTTP/1.1\r\nHost: localhost\r\nCookie: __session=" +
                               std::string(12 * 1024, 'c') + "\r\nX-Other: " +
                               std::string(8 * 1024, 'o') + "\r\n\r\n"));
    const std::string answer = signed_in.response();
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 200")) << answer.substr(0, 200);

    Client one_field(served.port());
    ASSERT_TRUE(one_field.send("GET /fine HTTP/1.1\r\nHost: localhost\r\nCookie: a=" +
                               std::string(1025 * 1024, 'c') + "\r\n\r\n"));
    EXPECT_TRUE(one_field.response().starts_with("HTTP/1.1 431"));

    Client total(served.port());
    std::string headers;
    for (int i = 0; i < 9; ++i) {
        headers += "X-Part-" + std::to_string(i) + ": " + std::string(120 * 1024, 'p') + "\r\n";
    }
    ASSERT_TRUE(total.send("GET /fine HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n"));
    EXPECT_TRUE(total.response().starts_with("HTTP/1.1 431"));
}

// Nits: a malformed target is 400, a header over 8 KiB is 431, `Expect: 100-continue` is answered,
// and a client that left while its request waited is not dispatched.
TEST(HttpConnections, MalformedRequestsAndExpectContinue) {
    Served served(limits_with(1));
    Client bad(served.port());
    ASSERT_TRUE(bad.send("GET /%zz HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const std::string bad_answer = bad.response();
    EXPECT_TRUE(bad_answer.starts_with("HTTP/1.1 400")) << bad_answer;
    EXPECT_TRUE(bad.closed_by_peer());

    Client large(served.port());
    ASSERT_TRUE(large.send("GET /fine HTTP/1.1\r\nHost: localhost\r\nX-Large: " +
                           std::string(9000, 'h') + "\r\n\r\n"));
    const std::string large_answer = large.response();
    EXPECT_TRUE(large_answer.starts_with("HTTP/1.1 431")) << large_answer;

    Client expecting(served.port());
    ASSERT_TRUE(expecting.send("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n"
                               "Expect: 100-continue\r\n\r\n"));
    const std::string proceed = expecting.header_only();
    EXPECT_EQ(proceed, "HTTP/1.1 100 Continue\r\n\r\n");
    ASSERT_TRUE(expecting.send("hello"));
    const std::string echoed = expecting.response();
    ASSERT_TRUE(echoed.starts_with("HTTP/1.1 200")) << echoed;
    EXPECT_TRUE(echoed.ends_with("size=5"));

    Client holder(served.port());
    ASSERT_TRUE(holder.send(get("/hold")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.holding() == 1; }));
    {
        Client gone(served.port());
        ASSERT_TRUE(gone.send(get("/fine")));
        ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.waiting_requests == 1; }));
        gone.reset();
        std::this_thread::sleep_for(100ms);
    }
    served.release();
    EXPECT_TRUE(holder.response().starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(served.eventually([](const auto& stats) { return stats.gone_before_dispatch == 1; }));
}

// The review's protocol cases: Content-Length and chunked uploads, HEAD, and a client's
// `Connection: close`, each over a kept-alive connection.
TEST(HttpConnections, UploadsHeadAndAClientsClose) {
    Served served(limits_with(2));
    Client client(served.port());
    ASSERT_TRUE(client.send(post("/echo", std::string(60000, 'a'))));
    const std::string sized = client.response();
    ASSERT_TRUE(sized.starts_with("HTTP/1.1 200")) << sized;
    EXPECT_TRUE(sized.ends_with("size=60000"));
    ASSERT_TRUE(client.send("POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    const std::string chunked = client.response();
    ASSERT_TRUE(chunked.starts_with("HTTP/1.1 200")) << chunked;
    EXPECT_TRUE(chunked.ends_with("size=11"));
    ASSERT_TRUE(client.send("HEAD /fine HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const std::string head = client.header_only();
    ASSERT_TRUE(head.starts_with("HTTP/1.1 200")) << head;
    EXPECT_EQ(header_value(head, "content-length"), "2");
    ASSERT_TRUE(client.send(get("/fine", false)));
    const std::string last = client.response();
    ASSERT_TRUE(last.starts_with("HTTP/1.1 200")) << last;
    EXPECT_EQ(header_value(last, "connection"), "close");
    EXPECT_TRUE(client.closed_by_peer());
}

// The drain: a request in flight finishes (and closes its connection), and a connection that has
// not sent its first request yet is still answered.
TEST(HttpConnections, TheDrainLetsRequestsInFlightAndNewConnectionsFinish) {
    Served served(limits_with(2));
    Client in_flight(served.port());
    ASSERT_TRUE(in_flight.send(get("/hold")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.holding() == 1; }));
    Client fresh(served.port());
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.connections == 2; }));
    served.begin_drain();
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(fresh.send(get("/fine")));
    const std::string answered = fresh.response();
    ASSERT_TRUE(answered.starts_with("HTTP/1.1 200")) << answered;
    EXPECT_EQ(header_value(answered, "connection"), "close");
    EXPECT_TRUE(fresh.closed_by_peer());
    served.release();
    const std::string finished = in_flight.response();
    ASSERT_TRUE(finished.starts_with("HTTP/1.1 200")) << finished;
    EXPECT_EQ(header_value(finished, "connection"), "close");
    EXPECT_TRUE(in_flight.closed_by_peer());
}

// What an idle connection costs lake (docs/serving/http.md, "Memory"): measured, and bounded so a
// regression shows.
TEST(HttpConnections, AnIdleConnectionCostsLittleMemory) {
    raise_descriptor_limit(4096);
    // With lake's own header limits (lake#1108), not boost::http's defaults.
    Served served(limits_with(2), lake::api::default_max_header_bytes);
    constexpr int count = 1000;
    std::vector<std::unique_ptr<Client>> idle;
    idle.reserve(count);
    for (int i = 0; i < 10; ++i) {
        idle.push_back(std::make_unique<Client>(served.port()));
        ASSERT_TRUE(idle.back()->send(get("/fine")));
        ASSERT_TRUE(idle.back()->response().starts_with("HTTP/1.1 200"));
    }
    const std::uint64_t before = resident_bytes();
    for (int i = 10; i < count; ++i) {
        idle.push_back(std::make_unique<Client>(served.port()));
        ASSERT_TRUE(idle.back()->connected()) << i;
        ASSERT_TRUE(idle.back()->send(get("/fine")));
        ASSERT_TRUE(idle.back()->response().starts_with("HTTP/1.1 200")) << i;
    }
    const std::uint64_t after = resident_bytes();
    const std::uint64_t each = after > before ? (after - before) / (count - 10) : 0;
    std::cout << "[ idle connection ] " << each / 1024 << " KiB resident each over " << count - 10
              << " connections\n";
    EXPECT_LT(each, 256U * 1024U);
}

// Re-review A: a body trickled a byte at a time, each byte well inside the I/O timeout, still
// empties the transfer-rate bucket and is cut off.
TEST(HttpConnections, ATrickledBodyIsCutOffByTheTransferRate) {
    auto limits = limits_with(2);
    limits.io_timeout = 60s;
    limits.rate_grace = 1s;
    Served served(std::move(limits));
    Client trickle(served.port(), 8s);
    trickle.set_send_timeout(2s);
    ASSERT_TRUE(trickle.send("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n"));
    std::atomic<bool> stop{false};
    std::thread sender([&] {
        for (int i = 0; i < 40 && !stop.load(); ++i) {
            std::this_thread::sleep_for(300ms);
            if (!trickle.send("x")) {
                return;
            }
        }
    });
    const auto started = Clock::now();
    EXPECT_TRUE(trickle.closed_by_peer()) << "a byte every 300 ms holds the worker";
    EXPECT_LT(Clock::now() - started, 6s);
    stop.store(true);
    sender.join();
    EXPECT_GE(served.stats().slow_transfers, 1U);
    EXPECT_EQ(served.stats().io_timeouts, 0U);
}

// Re-review A: a client that reads a response a few bytes at a time is cut off the same way.
TEST(HttpConnections, ASlowReaderIsCutOffByTheTransferRate) {
    auto limits = limits_with(2);
    limits.io_timeout = 60s;
    limits.rate_grace = 1s;
    Served served(std::move(limits));
    Client reader(served.port(), 200ms);
    ASSERT_TRUE(reader.send(get("/big")));
    const auto deadline = Clock::now() + 30s;
    while (served.stats().slow_transfers == 0 && Clock::now() < deadline) {
        char bytes[64];
        static_cast<void>(::recv(reader.descriptor(), bytes, sizeof(bytes), 0));
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_GE(served.stats().slow_transfers, 1U) << "64 bytes every 100 ms keeps a worker";
    EXPECT_EQ(served.stats().io_timeouts, 0U);
    EXPECT_TRUE(served.eventually([](const auto& stats) { return stats.active_requests == 0; }));
}

// Re-review B: only a capability declared a long poll leaves its worker; `wait_ms` from the
// caller alone never does.
TEST(HttpConnections, AWaitMsAloneNeverLeavesTheWorker) {
    lake::api::Registry registry;
    ASSERT_TRUE(registry.add(lake::api::Capability{
        .name = "ordinary",
        .title = "Ordinary",
        .description = "Takes a wait_ms it does not wait for.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"wait_ms", {{"type", "integer"}, {"minimum", 0}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = lake::api::CapabilityKind::Read,
        .requires_auth = false,
        .async_handler = [](const nlohmann::json&, lake::api::ApiContext&)
            -> liquid::runtime::task<lake::api::ApiResult> {
            co_return lake::api::ApiResult::success(nlohmann::json::object());
        }}));
    int left = 0;
    lake::api::ApiContext context;
    context.leave_worker = [&left](const std::string&) {
        ++left;
        return true;
    };
    ASSERT_TRUE(liquid::runtime::block_on(registry.call_async("ordinary", {{"wait_ms", 25000}}, context))->ok);
    EXPECT_EQ(left, 0);
}

// Re-review B: the real long polls are declared, and a repository write takes no field it does
// not read (so no `wait_ms`).
TEST(HttpConnections, TheLongPollsAreDeclaredAndWritesAreStrict) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    for (const char* name : {"receive_agent_tasks", "receive_agent_events", "receive_build_events",
                             "receive_app_events", "browse_stream", "tail_experiment_events"}) {
        const auto capability = registry.find(name);
        ASSERT_TRUE(capability) << name;
        EXPECT_TRUE(capability->long_poll) << name;
    }
    std::size_t long_polls = 0;
    for (const auto& capability : registry.list()) {
        long_polls += capability->long_poll ? 1 : 0;
    }
    EXPECT_EQ(long_polls, 6U);
    // tail_experiment_events waits 10 s when no wait_ms is sent, and leaves its worker for it.
    EXPECT_EQ(registry.find("tail_experiment_events")->default_wait_ms, 10000);
    const auto write = registry.find("commit_repository_files");
    ASSERT_TRUE(write);
    EXPECT_EQ(write->input_schema.value("additionalProperties", true), false);
    const auto refused = lake::api::validate_input(
        *write, {{"id", "00000000-0000-4000-8000-000000000000"}, {"wait_ms", 25000}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input") << refused.error.message;
}

// Re-review B: the blocking pool's queue is bounded; past it run_blocking answers busy at once.
TEST(HttpConnections, TheBlockingPoolRefusesPastItsBound) {
    auto& limit = lake::api::detail::blocking_task_limit();
    const int kept = limit.load();
    limit.store(0);
    const auto refused = liquid::runtime::block_on(lake::api::run_blocking(
        []() -> std::expected<int, liquid::error> { return 1; }));
    limit.store(kept);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::busy);
    const auto ran = liquid::runtime::block_on(lake::api::run_blocking(
        []() -> std::expected<int, liquid::error> { return 1; }));
    ASSERT_TRUE(ran);
    EXPECT_EQ(*ran, 1);
}

// Re-review C: requests that left their worker are capped in all and per caller (an
// unauthenticated caller by its address); past either cap, 429 with Retry-After closes the
// connection.
TEST(HttpConnections, LongRequestsAreCappedInAllAndPerCaller) {
    auto limits = limits_with(2);
    limits.max_long_requests = 5;
    limits.max_long_requests_per_caller = 3;
    Served served(std::move(limits));
    std::vector<std::unique_ptr<Client>> parked;
    const auto park = [&](const std::string& who) {
        parked.push_back(std::make_unique<Client>(served.port()));
        EXPECT_TRUE(parked.back()->send(get(who.empty() ? "/park" : "/park?who=" + who)));
    };
    const auto refused = [&](const std::string& who) {
        Client client(served.port());
        EXPECT_TRUE(client.send(get(who.empty() ? "/park" : "/park?who=" + who)));
        const std::string answer = client.response();
        EXPECT_TRUE(answer.starts_with("HTTP/1.1 429")) << who << ": " << answer;
        EXPECT_EQ(header_value(answer, "retry-after"), "1");
        EXPECT_EQ(header_value(answer, "connection"), "close");
        EXPECT_TRUE(client.closed_by_peer());
    };
    for (int i = 0; i < 3; ++i) {
        park("alice");
    }
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.parked() == 3; }));
    refused("alice");
    park("");
    park("");
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.parked() == 5; }));
    refused("bob");
    EXPECT_EQ(served.stats().long_refusals, 2U);
    served.release();
    for (auto& client : parked) {
        EXPECT_TRUE(client->response().starts_with("HTTP/1.1 200"));
    }
}

TEST(HttpConnections, AnUnauthenticatedCallerIsCappedByItsAddress) {
    auto limits = limits_with(2);
    limits.max_long_requests_per_caller = 2;
    Served served(std::move(limits));
    Client first(served.port());
    Client second(served.port());
    ASSERT_TRUE(first.send(get("/park")));
    ASSERT_TRUE(second.send(get("/park")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.parked() == 2; }));
    Client third(served.port());
    ASSERT_TRUE(third.send(get("/park")));
    EXPECT_TRUE(third.response().starts_with("HTTP/1.1 429"));
    // A named caller from the same address has its own count.
    Client named(served.port());
    ASSERT_TRUE(named.send(get("/park?who=carol")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.parked() == 3; }));
    served.release();
    EXPECT_TRUE(first.response().starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(named.response().starts_with("HTTP/1.1 200"));
}

// Re-review nit: the worker is given back before an unread body is discarded, so the discard's
// wait never holds it.
TEST(HttpConnections, TheWorkerIsFreeWhileAnUnreadBodyIsDiscarded) {
    Served served(limits_with(1));
    Client slow(served.port());
    ASSERT_TRUE(slow.send("POST /ignore HTTP/1.1\r\nHost: localhost\r\nContent-Length: 60000\r\n\r\n" +
                          std::string(1000, 'z')));
    ASSERT_TRUE(slow.response().starts_with("HTTP/1.1 200"));
    Client next(served.port());
    const auto started = Clock::now();
    ASSERT_TRUE(next.send(get("/fine")));
    EXPECT_TRUE(next.response().starts_with("HTTP/1.1 200"));
    EXPECT_LT(Clock::now() - started, 500ms) << "the discard of another request held the worker";
}

// Re-review nit: a request still not admitted when the admission wait runs out is answered 503
// and never dispatched to wait again while holding a worker.
TEST(HttpConnections, ARequestNotAdmittedInTimeIsAnsweredWithoutAWorker) {
    auto limits = limits_with(1);
    limits.admitted = [](std::string_view) { return false; };
    limits.admission_wait = 200ms;
    Served served(std::move(limits));
    Client client(served.port());
    ASSERT_TRUE(client.send(get("/hold")));
    const std::string answer = client.response();
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 503")) << answer;
    EXPECT_NE(answer.find("store_unavailable"), std::string::npos);
    EXPECT_EQ(served.holding(), 0) << "the route ran";
    EXPECT_EQ(served.stats().admission_timeouts, 1U);
}

// Re-review nits: a client that half-closes after a complete request is still answered, and a
// route that refuses without reading the body never invites it with `100 Continue`.
TEST(HttpConnections, AHalfCloseIsAnsweredAndARefusalSendsNoContinue) {
    Served served(limits_with(1));
    Client holder(served.port());
    ASSERT_TRUE(holder.send(get("/hold")));
    ASSERT_TRUE(served.eventually([&served](const auto&) { return served.holding() == 1; }));
    Client half(served.port());
    ASSERT_TRUE(half.send(get("/fine")));
    half.shutdown_send();
    ASSERT_TRUE(served.eventually([](const auto& stats) { return stats.waiting_requests == 1; }));
    served.release();
    EXPECT_TRUE(holder.response().starts_with("HTTP/1.1 200"));
    const std::string answered = half.response();
    EXPECT_TRUE(answered.starts_with("HTTP/1.1 200")) << answered;
    EXPECT_EQ(served.stats().gone_before_dispatch, 0U);

    Client refused(served.port());
    ASSERT_TRUE(refused.send("POST /busy HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n"
                             "Expect: 100-continue\r\n\r\n"));
    const std::string first = refused.header_only();
    EXPECT_TRUE(first.starts_with("HTTP/1.1 503")) << first;
    EXPECT_EQ(header_value(first, "connection"), "close");
}
