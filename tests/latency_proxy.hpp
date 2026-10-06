#pragma once

// A TCP proxy on loopback that delays every byte by half a round trip in each direction, so a
// client sees a network round trip like the cluster's (lake#527). Each connection forwards both
// directions independently: a byte read at t is written at t + rtt/2, so concurrent requests on
// separate connections overlap as they would on a real network, and a sequence of dependent
// requests pays the round trip each time.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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
    LatencyProxy(int target_port, std::chrono::microseconds round_trip)
        : target_port_(target_port), half_(round_trip / 2) {
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

private:
    using clock = std::chrono::steady_clock;

    // One direction of one connection: bytes read from `from` are written to `to` after `delay`.
    struct Pipe {
        int from = -1;
        int to = -1;
        std::chrono::microseconds delay{};
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
                queue.emplace_back(clock::now() + delay, buffer.substr(0, static_cast<std::size_t>(got)));
                ready.notify_all();
            }
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
            auto down = std::make_shared<Pipe>();
            down->from = server;
            down->to = client;
            down->delay = half_;
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
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Pipe>> pipes_;
    std::vector<std::thread> threads_;
};

} // namespace lake::test
