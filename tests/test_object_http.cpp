// lake#544: the workspace store's keep-alive client (src/control/object_http.cpp) against a small
// loopback HTTP/1.1 server that counts its connections.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <memory>
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
// answered; `/slow` is answered after 100 ms.
class LoopbackServer {
public:
    LoopbackServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 64);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
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
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }
    int accepted() const {
        return accepted_.load();
    }
    int requests() const {
        return requests_.load();
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
            const bool close = line.find("/close") != std::string::npos;
            const std::string body = line.substr(0, line.rfind(' ')) + " " + std::to_string(length);
            std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n" + (close ? "Connection: close\r\n" : "") +
                                 "\r\n" + body;
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
        std::lock_guard lock(mutex_);
        std::erase(open_, socket);
        ::close(socket);
    }

    int listener_ = -1;
    int port_ = 0;
    std::thread acceptor_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> requests_{0};
    std::mutex mutex_;
    std::vector<int> open_;
    std::vector<std::thread> threads_;
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

} // namespace
