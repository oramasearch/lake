#pragma once

// A TCP proxy on loopback that delays every byte by half a round trip in each direction, so a
// client sees a network round trip like the cluster's (lake#527). Each connection forwards both
// directions independently: a byte read at t is written at t + rtt/2, so concurrent requests on
// separate connections overlap as they would on a real network, and a sequence of dependent
// requests pays the round trip each time.
//
// With `durable_delay` (lake#535), a request that asks Zetta to fsync (a POST `:barrier`, or a
// write with `durable=true` in its query) is held that much longer before it is forwarded, as a
// disk whose fsync costs that long would hold its answer. The proxy reads each request's line and
// headers to find it and skips its body by Content-Length.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lake::test {

class LatencyProxy {
public:
    LatencyProxy(int target_port, std::chrono::microseconds round_trip,
                 std::chrono::microseconds durable_delay = std::chrono::microseconds(0))
        : target_port_(target_port), half_(round_trip / 2), durable_delay_(durable_delay) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 64);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~LatencyProxy() {
        stopping_.store(true);
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::vector<std::shared_ptr<Pipe>> pipes;
        {
            std::lock_guard lock(mutex_);
            pipes = pipes_;
        }
        for (const auto& pipe : pipes) {
            pipe->stop();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    LatencyProxy(const LatencyProxy&) = delete;
    LatencyProxy& operator=(const LatencyProxy&) = delete;

    int port() const {
        return port_;
    }

    // Requests that asked for an fsync, as the proxy saw them (counted when `durable_delay` is set).
    std::uint64_t durable_requests() const {
        return durable_requests_.load();
    }

    // The next `count` answers are lost (lake#544): the first bytes of each from Zetta are dropped
    // and that connection is closed both ways, so the request was applied and the client never
    // hears.
    void lose_next_answers(int count) {
        lose_answers_.fetch_add(count);
    }

private:
    using clock = std::chrono::steady_clock;

    // One direction of one connection: bytes read from `from` are written to `to` after `delay`.
    // A request pipe with `durable_delay` adds it to the bytes that end a durable request's headers.
    struct Pipe {
        int from = -1;
        int to = -1;
        std::chrono::microseconds delay{};
        std::chrono::microseconds durable_delay{};
        std::atomic<std::uint64_t>* durable_requests = nullptr;
        // Set on an answer pipe: answers still to lose.
        std::atomic<int>* lose_answers = nullptr;
        // The request being read: its line and headers so far, then the body bytes still to skip.
        std::string head;
        std::uint64_t body_left = 0;
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::pair<clock::time_point, std::string>> queue;
        bool closed = false;
        std::atomic<bool> stopped{false};

        void stop() {
            stopped.store(true);
            ::shutdown(from, SHUT_RDWR);
            ::shutdown(to, SHUT_RDWR);
            std::lock_guard lock(mutex);
            closed = true;
            ready.notify_all();
        }

        void read_loop() {
            std::string buffer(256 * 1024, '\0');
            for (;;) {
                const ssize_t got = ::recv(from, buffer.data(), buffer.size(), 0);
                std::lock_guard lock(mutex);
                if (got <= 0) {
                    closed = true;
                    ready.notify_all();
                    return;
                }
                if (lose_answers != nullptr) {
                    int left = lose_answers->load();
                    while (left > 0 && !lose_answers->compare_exchange_weak(left, left - 1)) {
                    }
                    if (left > 0) {
                        // The answer is dropped and the client's connection closed.
                        ::shutdown(to, SHUT_RDWR);
                        ::shutdown(from, SHUT_RDWR);
                        closed = true;
                        queue.clear();
                        ready.notify_all();
                        return;
                    }
                }
                std::string_view bytes(buffer.data(), static_cast<std::size_t>(got));
                auto at = clock::now() + delay;
                if (durable_delay.count() > 0) {
                    // Every durable request whose headers end in these bytes holds them back; the
                    // queue is in order, so later bytes wait behind them.
                    at += durable_delay * static_cast<std::int64_t>(durable_requests_in(bytes));
                }
                queue.emplace_back(at, std::string(bytes));
                ready.notify_all();
            }
        }

        // How many requests that ask for an fsync end their headers within `bytes`.
        std::uint64_t durable_requests_in(std::string_view bytes) {
            std::uint64_t found = 0;
            while (!bytes.empty()) {
                if (body_left > 0) {
                    const std::size_t skip = static_cast<std::size_t>(std::min<std::uint64_t>(body_left, bytes.size()));
                    body_left -= skip;
                    bytes.remove_prefix(skip);
                    continue;
                }
                const std::size_t before = head.size();
                head.append(bytes);
                const std::size_t end = head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    return found;
                }
                const std::size_t used = end + 4 - before;
                bytes.remove_prefix(used);
                const std::string_view request(head.data(), end);
                const std::string_view line = request.substr(0, request.find("\r\n"));
                if (line.find(":barrier") != std::string_view::npos ||
                    line.find("durable=true") != std::string_view::npos) {
                    ++found;
                    if (durable_requests != nullptr) {
                        durable_requests->fetch_add(1);
                    }
                }
                std::string lower(request);
                std::ranges::transform(lower, lower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (const std::size_t length = lower.find("\r\ncontent-length:"); length != std::string::npos) {
                    std::size_t digit = length + 17;
                    while (digit < lower.size() && lower[digit] == ' ') {
                        ++digit;
                    }
                    std::from_chars(lower.data() + digit, lower.data() + lower.size(), body_left);
                }
                head.clear();
            }
            return found;
        }

        void write_loop() {
            for (;;) {
                std::pair<clock::time_point, std::string> item;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return !queue.empty() || closed; });
                    if (queue.empty()) {
                        ::shutdown(to, SHUT_WR);
                        return;
                    }
                    item = std::move(queue.front());
                    queue.pop_front();
                }
                std::this_thread::sleep_until(item.first);
                std::size_t sent = 0;
                while (sent < item.second.size()) {
                    const ssize_t wrote = ::send(to, item.second.data() + sent, item.second.size() - sent, 0);
                    if (wrote <= 0) {
                        return;
                    }
                    sent += static_cast<std::size_t>(wrote);
                }
            }
        }
    };

    void accept_loop() {
        while (!stopping_.load()) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            const int server = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<std::uint16_t>(target_port_));
            if (::connect(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                ::close(client);
                ::close(server);
                continue;
            }
            int yes = 1;
            ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            ::setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            auto up = std::make_shared<Pipe>();
            up->from = client;
            up->to = server;
            up->delay = half_;
            up->durable_delay = durable_delay_;
            up->durable_requests = &durable_requests_;
            auto down = std::make_shared<Pipe>();
            down->from = server;
            down->to = client;
            down->delay = half_;
            down->lose_answers = &lose_answers_;
            std::lock_guard lock(mutex_);
            pipes_.push_back(up);
            pipes_.push_back(down);
            for (const auto& pipe : {up, down}) {
                threads_.emplace_back([pipe] { pipe->read_loop(); });
                threads_.emplace_back([pipe] { pipe->write_loop(); });
            }
            // Both sockets close once both directions are done; the threads hold the pipes.
            threads_.emplace_back([up, down, client, server] {
                const auto closed = [](Pipe& pipe) {
                    std::lock_guard pipe_lock(pipe.mutex);
                    return pipe.closed && pipe.queue.empty();
                };
                while (!(closed(*up) && closed(*down)) && !up->stopped.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ::close(client);
                ::close(server);
            });
        }
    }

    int target_port_;
    std::chrono::microseconds half_;
    std::chrono::microseconds durable_delay_;
    std::atomic<std::uint64_t> durable_requests_{0};
    std::atomic<int> lose_answers_{0};
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Pipe>> pipes_;
    std::vector<std::thread> threads_;
};

} // namespace lake::test
