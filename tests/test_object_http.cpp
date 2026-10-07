// lake#544: the workspace store's keep-alive client (src/control/object_http.cpp) against a small
// loopback HTTP/1.1 server that counts its connections.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lake/control/workspace_store.hpp"

namespace {

using namespace std::chrono_literals;
using lake::control::ObjectRequest;
using lake::control::ObjectResponse;

// Answers each request with its method, path and body length in the body. A path containing
// `/close` is answered with `Connection: close` and the connection closed; `/hang` is never
// answered; `/slow` is answered after 100 ms. The framing cases: `/chunked` answers in chunks,
// `/short` promises 100 bytes, sends 10 and closes, `/toclose` sends a body with no length and
// closes, `/nocontent` answers 204, `/extra` sends a stale second answer after its own, and
// `/continue` sends a 100 Continue before its answer.
class LoopbackServer {
public:
    explicit LoopbackServer(bool v6 = false) : v6_(v6) {
        int yes = 1;
        if (v6) {
            listener_ = ::socket(AF_INET6, SOCK_STREAM, 0);
            ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_loopback;
            bound_ = ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            ::listen(listener_, 64);
            socklen_t length = sizeof(address);
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
            port_ = ntohs(address.sin6_port);
        } else {
            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bound_ = ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            ::listen(listener_, 64);
            socklen_t length = sizeof(address);
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
            port_ = ntohs(address.sin_port);
        }
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    bool bound() const {
        return bound_;
    }

    ~LoopbackServer() {
        stopping_.store(true);
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        acceptor_.join();
        close_connections();
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            threads.swap(threads_);
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }

    int port() const {
        return port_;
    }
    std::string url(std::string_view path) const {
        return std::string(v6_ ? "http://[::1]:" : "http://127.0.0.1:") + std::to_string(port_) + std::string(path);
    }
    int accepted() const {
        return accepted_.load();
    }
    int requests() const {
        return requests_.load();
    }
    // Connections that ended, from either side.
    int ended() const {
        return ended_.load();
    }
    // Closes every open connection from the server's side, as a restarted Zetta would.
    void close_connections() {
        std::lock_guard lock(mutex_);
        for (const int socket : open_) {
            ::shutdown(socket, SHUT_RDWR);
        }
    }

private:
    void accept_loop() {
        while (!stopping_.load()) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            accepted_.fetch_add(1);
            std::lock_guard lock(mutex_);
            open_.push_back(client);
            threads_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(int socket) {
        std::string buffer;
        char chunk[64 * 1024];
        for (;;) {
            std::size_t end = buffer.find("\r\n\r\n");
            while (end == std::string::npos) {
                const auto got = ::recv(socket, chunk, sizeof(chunk), 0);
                if (got <= 0) {
                    return finish(socket);
                }
                buffer.append(chunk, static_cast<std::size_t>(got));
                end = buffer.find("\r\n\r\n");
            }
            std::string head = buffer.substr(0, end);
            buffer.erase(0, end + 4);
            std::string lower = head;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::size_t length = 0;
            if (const auto at = lower.find("\r\ncontent-length:"); at != std::string::npos) {
                std::size_t digit = at + 17;
                while (digit < lower.size() && lower[digit] == ' ') {
                    ++digit;
                }
                std::from_chars(lower.data() + digit, lower.data() + lower.size(), length);
            }
            while (buffer.size() < length) {
                const auto got = ::recv(socket, chunk, sizeof(chunk), 0);
                if (got <= 0) {
                    return finish(socket);
                }
                buffer.append(chunk, static_cast<std::size_t>(got));
            }
            buffer.erase(0, length);
            requests_.fetch_add(1);
            const std::string line = head.substr(0, head.find("\r\n"));
            if (line.find("/hang") != std::string::npos) {
                continue;
            }
            if (line.find("/slow") != std::string::npos) {
                std::this_thread::sleep_for(100ms);
            }
            bool close = line.find("/close") != std::string::npos;
            const std::string body = line.substr(0, line.rfind(' ')) + " " + std::to_string(length);
            std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n" + (close ? "Connection: close\r\n" : "") +
                                 "\r\n" + body;
            if (line.find("/chunked") != std::string::npos) {
                answer = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n";
            } else if (line.find("/short") != std::string::npos) {
                answer = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789";
                close = true;
            } else if (line.find("/toclose") != std::string::npos) {
                answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the end";
                close = true;
            } else if (line.find("/nocontent") != std::string::npos) {
                answer = "HTTP/1.1 204 No Content\r\n\r\n";
            } else if (line.find("/extra") != std::string::npos) {
                answer += "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstale";
            } else if (line.find("/continue") != std::string::npos) {
                answer = "HTTP/1.1 100 Continue\r\n\r\n" + answer;
            }
            std::size_t sent = 0;
            while (sent < answer.size()) {
                const auto wrote = ::send(socket, answer.data() + sent, answer.size() - sent, 0);
                if (wrote <= 0) {
                    return finish(socket);
                }
                sent += static_cast<std::size_t>(wrote);
            }
            if (close) {
                return finish(socket);
            }
        }
    }

    void finish(int socket) {
        ended_.fetch_add(1);
        std::lock_guard lock(mutex_);
        std::erase(open_, socket);
        ::close(socket);
    }

    bool v6_ = false;
    bool bound_ = false;
    int listener_ = -1;
    int port_ = 0;
    std::thread acceptor_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> requests_{0};
    std::atomic<int> ended_{0};
    std::mutex mutex_;
    std::vector<int> open_;
    std::vector<std::thread> threads_;
};

// A listener that never accepts, its queue filled, so a new dial to it gets no answer at all: a
// peer that is gone without a word, as a node that vanished is.
class SilentListener {
public:
    SilentListener() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 1);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        // Fill the queue: dials that complete without an accept, until one does not.
        for (int index = 0; index < 64; ++index) {
            const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
            ::fcntl(socket, F_SETFL, O_NONBLOCK);
            ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            fillers_.push_back(socket);
            pollfd waiting{socket, POLLOUT, 0};
            if (::poll(&waiting, 1, 200) == 0) {
                break;
            }
        }
    }
    ~SilentListener() {
        for (const int socket : fillers_) {
            ::close(socket);
        }
        ::close(listener_);
    }
    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

private:
    int listener_ = -1;
    int port_ = 0;
    std::vector<int> fillers_;
};

ObjectRequest request_to(std::string url, std::string method = "GET", std::string body = {}) {
    ObjectRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    request.headers = {{"Authorization", "Bearer secret-token-544"}};
    request.body = std::move(body);
    request.timeout_seconds = 5;
    return request;
}

ObjectResponse send(lake::control::ObjectTransport& transport, ObjectRequest request) {
    auto answered = liquid::runtime::block_on(transport.exchange(std::move(request)));
    EXPECT_TRUE(answered);
    return answered ? std::move(*answered) : ObjectResponse{};
}

TEST(ObjectHttp, SequentialRequestsShareOneConnectionAndOneResolution) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    const std::string base = "http://localhost:" + std::to_string(server.port());
    for (int index = 0; index < 20; ++index) {
        const std::string body(index % 2 == 0 ? 0 : 1024 * 1024 + index, 'x');
        auto answer = send(*transport, request_to(base + "/v1/objects/b/k" + std::to_string(index),
                                                  index % 2 == 0 ? "GET" : "PUT", body));
        EXPECT_TRUE(answer.error.empty()) << answer.error;
        EXPECT_EQ(answer.status, 200);
        EXPECT_EQ(answer.body, std::string(index % 2 == 0 ? "GET" : "PUT") + " /v1/objects/b/k" +
                                   std::to_string(index) + " " + std::to_string(body.size()));
    }
    const auto stats = transport->stats();
    EXPECT_EQ(server.accepted(), 1);
    EXPECT_EQ(stats.connections, 1U);
    EXPECT_EQ(stats.resolutions, 1U);
    EXPECT_EQ(stats.reused, 19U);
}

TEST(ObjectHttp, ConcurrentRequestsEachHaveAConnectionAndKeepIt) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    const auto wave = [&] {
        std::vector<std::thread> callers;
        for (int index = 0; index < 6; ++index) {
            callers.emplace_back([&, index] {
                auto answer = send(*transport, request_to(server.url("/slow/" + std::to_string(index))));
                EXPECT_EQ(answer.status, 200) << answer.error;
            });
        }
        for (auto& caller : callers) {
            caller.join();
        }
    };
    wave();
    const int after_first = server.accepted();
    EXPECT_GE(after_first, 2) << "the requests overlapped, so they could not share one connection";
    EXPECT_LE(after_first, 6);
    wave();
    wave();
    // The second and third waves find the first wave's connections kept.
    EXPECT_LE(server.accepted(), 6);
    EXPECT_EQ(transport->stats().resolutions, 1U);
}

TEST(ObjectHttp, AConnectionTheServerClosedIsNotReused) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    EXPECT_EQ(send(*transport, request_to(server.url("/a"))).status, 200);
    server.close_connections();
    std::this_thread::sleep_for(50ms);
    // A PATCH, which the transport must not send on a closed connection and then lose.
    auto answer = send(*transport, request_to(server.url("/b"), "PATCH", "appended"));
    EXPECT_TRUE(answer.error.empty()) << answer.error;
    EXPECT_EQ(answer.status, 200);
    const auto stats = transport->stats();
    EXPECT_EQ(stats.closed, 1U);
    EXPECT_EQ(stats.connections, 2U);
    EXPECT_EQ(stats.reused, 0U);
    EXPECT_EQ(server.requests(), 2);
}

TEST(ObjectHttp, AConnectionAnsweredWithCloseIsNotKept) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    EXPECT_EQ(send(*transport, request_to(server.url("/close"))).status, 200);
    EXPECT_EQ(send(*transport, request_to(server.url("/after"))).status, 200);
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().reused, 0U);
    EXPECT_EQ(transport->stats().closed, 0U) << "never kept, so never found closed";
}

TEST(ObjectHttp, AConnectionIdleTooLongIsClosedRatherThanReused) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport({.idle_limit = 0s});
    EXPECT_EQ(send(*transport, request_to(server.url("/a"))).status, 200);
    EXPECT_EQ(send(*transport, request_to(server.url("/b"))).status, 200);
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().closed, 1U);
}

TEST(ObjectHttp, ATimedOutRequestsConnectionIsDroppedAndTheBearerNeverShows) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    auto request = request_to(server.url("/hang"));
    request.timeout_seconds = 1;
    auto answer = send(*transport, std::move(request));
    EXPECT_EQ(answer.status, 0);
    EXPECT_NE(answer.error.find("timed out"), std::string::npos) << answer.error;
    EXPECT_EQ(answer.error.find("secret-token-544"), std::string::npos);
    // The connection the hung request held is not reused: its answer could still arrive.
    EXPECT_EQ(send(*transport, request_to(server.url("/next"))).status, 200);
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().reused, 0U);
}

TEST(ObjectHttp, NoListenerIsATransportFailure) {
    int port = 0;
    {
        LoopbackServer server;
        port = server.port();
    }
    auto transport = lake::control::make_pooled_object_transport();
    auto answer = send(*transport, request_to("http://127.0.0.1:" + std::to_string(port) + "/x"));
    EXPECT_EQ(answer.status, 0);
    EXPECT_NE(answer.error.find("could not connect"), std::string::npos) << answer.error;
    EXPECT_EQ(answer.error.find("secret-token-544"), std::string::npos);
}

TEST(ObjectHttp, ARequestThatTimesOutOnAKeptConnectionDropsTheOriginsOtherKeptConnections) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    // Two kept connections.
    std::thread first([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/1"))).status, 200); });
    std::thread second([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/2"))).status, 200); });
    first.join();
    second.join();
    ASSERT_EQ(server.accepted(), 2);
    // A kept connection that does not answer: the peer may be gone, and so the other one too.
    auto hung = request_to(server.url("/hang"));
    hung.timeout_seconds = 1;
    EXPECT_NE(send(*transport, std::move(hung)).error, "");
    const auto reused = transport->stats().reused;
    EXPECT_EQ(reused, 1U);
    EXPECT_EQ(send(*transport, request_to(server.url("/next"))).status, 200);
    EXPECT_EQ(server.accepted(), 3) << "the next request dials afresh";
    EXPECT_EQ(transport->stats().reused, reused);
}

TEST(ObjectHttp, ADialThatGetsNoAnswerFailsWithinTheConnectTimeoutAndResolvesAgain) {
    SilentListener silent;
    auto transport = lake::control::make_pooled_object_transport({.connect_timeout = 300ms});
    for (int attempt = 1; attempt <= 2; ++attempt) {
        auto request = request_to(silent.url("/x"));
        request.timeout_seconds = 20;
        const auto started = std::chrono::steady_clock::now();
        auto answer = send(*transport, std::move(request));
        EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
        EXPECT_EQ(answer.status, 0);
        EXPECT_NE(answer.error.find("could not connect"), std::string::npos) << answer.error;
    }
    EXPECT_EQ(transport->stats().resolutions, 2U) << "a dial that timed out evicts the resolution";
}

TEST(ObjectHttp, AStoreWhoseZettaIsSilentFailsWithinABoundedTime) {
    SilentListener silent;
    auto transport = lake::control::make_pooled_object_transport({.connect_timeout = 300ms});
    lake::control::ZettaObjectClient client(transport, lake::control::ObjectEndpoint{silent.url(""), "token"});
    const auto started = std::chrono::steady_clock::now();
    // An idempotent read: four attempts, each cut by the connect timeout.
    auto read = liquid::runtime::block_on(client.get("packs/a", 0, 16));
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, liquid::errc::unavailable);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
}

TEST(ObjectHttp, AnOriginNoLongerUsedHasItsKeptConnectionClosed) {
    LoopbackServer used_once;
    auto transport = lake::control::make_pooled_object_transport({.idle_limit = 200ms, .sweep_interval = 50ms});
    EXPECT_EQ(send(*transport, request_to(used_once.url("/a"))).status, 200);
    EXPECT_EQ(used_once.ended(), 0);
    // Nothing is sent to that origin again; the sweep still closes what is kept for it.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (used_once.ended() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(used_once.ended(), 1);
    EXPECT_EQ(transport->stats().closed, 1U);
}

TEST(ObjectHttp, TheOldestKeptConnectionsAreSweptWhileNewerOnesStay) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport({.idle_limit = 400ms, .sweep_interval = 50ms});
    // Two kept connections; then only one is used, and the other ages out.
    std::thread first([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/1"))).status, 200); });
    std::thread second([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/2"))).status, 200); });
    first.join();
    second.join();
    ASSERT_EQ(server.accepted(), 2);
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until) {
        EXPECT_EQ(send(*transport, request_to(server.url("/again"))).status, 200);
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(server.ended(), 1) << "the connection not used for 400 ms was closed";
    EXPECT_EQ(server.accepted(), 2);
}

TEST(ObjectHttp, AConnectionPastThePoolsBoundIsClosedAndCounted) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport({.idle_per_origin = 1});
    std::thread first([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/1"))).status, 200); });
    std::thread second([&] { EXPECT_EQ(send(*transport, request_to(server.url("/slow/2"))).status, 200); });
    first.join();
    second.join();
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().overflow, 1U);
}

TEST(ObjectHttp, DestroyingTheTransportAnswersTheRequestsItHolds) {
    auto server = std::make_shared<LoopbackServer>();
    auto transport = lake::control::make_pooled_object_transport();
    auto answered = std::make_shared<std::promise<ObjectResponse>>();
    auto future = answered->get_future();
    std::thread caller([server, transport, answered]() mutable {
        auto request = request_to(server->url("/hang"));
        request.timeout_seconds = 60;
        auto result = liquid::runtime::block_on(transport->exchange(std::move(request)));
        answered->set_value(result ? std::move(*result) : ObjectResponse{});
    });
    while (server->requests() == 0) {
        std::this_thread::sleep_for(10ms);
    }
    {
        // The caller holds its own reference through the call; drop it from here and stop the
        // transport by hand the way its last owner going would.
        auto stopping = std::move(transport);
        stopping->shutdown();
    }
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready) << "a waiting caller was never answered";
    const ObjectResponse response = future.get();
    EXPECT_EQ(response.status, 0);
    EXPECT_NE(response.error, "");
    caller.join();
}

TEST(ObjectHttp, ARequestThatAsksForAFreshConnectionDialsOne) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    EXPECT_EQ(send(*transport, request_to(server.url("/a"))).status, 200);
    auto fresh = request_to(server.url("/b"), "PUT", "once");
    fresh.fresh_connection = true;
    EXPECT_EQ(send(*transport, std::move(fresh)).status, 200);
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().reused, 0U);
    // Its connection is kept afterwards like any other.
    EXPECT_EQ(send(*transport, request_to(server.url("/c"))).status, 200);
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(transport->stats().reused, 1U);
}

// Records each request's path and whether it asked for a new connection, and answers 500.
class RecordingTransport final : public lake::control::ObjectTransport {
public:
    liquid::runtime::task<ObjectResponse> exchange(ObjectRequest request) override {
        std::lock_guard lock(mutex_);
        const auto path = request.url.substr(request.url.find("/v1/"));
        seen_.emplace_back(request.method + " " + path.substr(0, path.find('?')) +
                               (path.find("if_absent=true") != std::string::npos ? " if_absent" : ""),
                           request.fresh_connection);
        co_return ObjectResponse{500, R"({"ok":false,"error":{"code":"internal","message":"no"}})", {}};
    }
    std::vector<std::pair<std::string, bool>> seen() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, bool>> seen_;
};

TEST(ObjectHttp, TheClientAsksForAFreshConnectionExactlyForTheRequestsItDoesNotRetry) {
    auto recording = std::make_shared<RecordingTransport>();
    lake::control::ZettaObjectClient client(recording, lake::control::ObjectEndpoint{"http://zetta:4800", "token"});
    static_cast<void>(liquid::runtime::block_on(client.put("k", "bytes", std::nullopt, true)));
    static_cast<void>(liquid::runtime::block_on(client.usage()));
    static_cast<void>(liquid::runtime::block_on(client.acquire("holder", 30s, 0ms)));
    static_cast<void>(liquid::runtime::block_on(client.renew("holder", 1, 30s, 1s)));
    static_cast<void>(liquid::runtime::block_on(client.put("k", "bytes", std::nullopt, false)));
    static_cast<void>(liquid::runtime::block_on(client.get("k", 0, 4)));
    static_cast<void>(liquid::runtime::block_on(client.remove("k", std::nullopt)));
    std::map<std::string, std::set<bool>> fresh;
    for (const auto& [request, asked] : recording->seen()) {
        fresh[request].insert(asked);
    }
    const std::map<std::string, std::set<bool>> expected{
        {"PUT /v1/objects/lake_store/k if_absent", {true}},
        {"GET /v1/objects/lake_store:usage", {true}},
        {"POST /v1/objects/lake_store:acquire", {true}},
        {"POST /v1/objects/lake_store:renew", {true}},
        {"PUT /v1/objects/lake_store/k", {false}},
        {"GET /v1/objects/lake_store/k", {false}},
        {"DELETE /v1/objects/lake_store/k", {false}},
    };
    EXPECT_EQ(fresh, expected);
}

TEST(ObjectHttp, AnUnknownMethodIsRefusedAndNothingIsSent) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    auto answer = send(*transport, request_to(server.url("/brew"), "BREW"));
    EXPECT_EQ(answer.status, 0);
    EXPECT_NE(answer.error.find("method"), std::string::npos) << answer.error;
    EXPECT_EQ(server.requests(), 0);
}

TEST(ObjectHttp, AHeaderThatCannotBeSentFailsTheRequestRatherThanBeingDropped) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    auto split = request_to(server.url("/a"));
    split.headers.emplace_back("X-Lake-Note", "one\r\nX-Injected: two");
    auto answer = send(*transport, std::move(split));
    EXPECT_EQ(answer.status, 0);
    EXPECT_NE(answer.error.find("X-Lake-Note"), std::string::npos) << answer.error;
    auto named = request_to(server.url("/b"));
    named.headers.emplace_back("Bad Name", "value");
    answer = send(*transport, std::move(named));
    EXPECT_EQ(answer.status, 0);
    EXPECT_EQ(answer.error.find("secret-token-544"), std::string::npos);
    EXPECT_EQ(server.requests(), 0);
}

TEST(ObjectHttp, AnInterimAnswerLeavesItsConnectionUnkept) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    static_cast<void>(send(*transport, request_to(server.url("/continue"))));
    auto after = send(*transport, request_to(server.url("/after")));
    EXPECT_EQ(after.status, 200) << after.error;
    EXPECT_EQ(after.body, "GET /after 0") << "an earlier answer was served to a later request";
    EXPECT_EQ(transport->stats().reused, 0U);
}

TEST(ObjectHttp, AnIpv6AddressConnects) {
    LoopbackServer server(/*v6=*/true);
    if (!server.bound()) {
        GTEST_SKIP() << "no IPv6 loopback";
    }
    auto transport = lake::control::make_pooled_object_transport();
    auto answer = send(*transport, request_to(server.url("/v6")));
    EXPECT_EQ(answer.status, 200) << answer.error;
    EXPECT_EQ(answer.body, "GET /v6 0");
}

TEST(ObjectHttp, AnswersAreFramedAndNoBytePassesToTheNextRequest) {
    LoopbackServer server;
    auto transport = lake::control::make_pooled_object_transport();
    const auto expect_next = [&](const std::string& path) {
        auto next = send(*transport, request_to(server.url(path)));
        EXPECT_EQ(next.status, 200) << next.error;
        EXPECT_EQ(next.body, "GET " + path + " 0");
    };
    auto chunked = send(*transport, request_to(server.url("/chunked")));
    EXPECT_EQ(chunked.status, 200) << chunked.error;
    EXPECT_EQ(chunked.body, "helloworld");
    expect_next("/after-chunked");
    EXPECT_EQ(server.accepted(), 1) << "a chunked answer read to its end leaves its connection kept";

    auto empty = send(*transport, request_to(server.url("/nocontent")));
    EXPECT_EQ(empty.status, 204) << empty.error;
    EXPECT_EQ(empty.body, "");
    expect_next("/after-204");
    EXPECT_EQ(server.accepted(), 1);

    auto extra = send(*transport, request_to(server.url("/extra")));
    EXPECT_EQ(extra.status, 200) << extra.error;
    EXPECT_EQ(extra.body, "GET /extra 0");
    expect_next("/after-extra");

    const int before_short = server.accepted();
    auto cut = send(*transport, request_to(server.url("/short")));
    EXPECT_EQ(cut.status, 0) << "a body shorter than its Content-Length is a transport failure";
    EXPECT_NE(cut.error, "");
    expect_next("/after-short");
    EXPECT_GT(server.accepted(), before_short);

    auto to_close = send(*transport, request_to(server.url("/toclose")));
    EXPECT_EQ(to_close.status, 200) << to_close.error;
    EXPECT_EQ(to_close.body, "until the end");
    const int after_close = server.accepted();
    const auto reused = transport->stats().reused;
    expect_next("/after-toclose");
    EXPECT_EQ(server.accepted(), after_close + 1) << "a body that ran to the close leaves nothing to keep";
    EXPECT_EQ(transport->stats().reused, reused);
}

} // namespace
