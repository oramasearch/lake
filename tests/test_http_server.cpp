#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "lake/api/server.hpp"

namespace {

class StderrPipe {
public:
    StderrPipe() {
        if (::pipe(pipe_) != 0) {
            return;
        }
        original_ = ::dup(STDERR_FILENO);
        if (original_ < 0) {
            close_pipe();
            return;
        }
        std::fflush(stderr);
        std::cerr.flush();
        if (::dup2(pipe_[1], STDERR_FILENO) < 0) {
            ::close(original_);
            original_ = -1;
            close_pipe();
            return;
        }
        ::close(pipe_[1]);
        pipe_[1] = -1;
        capturing_ = true;
    }

    ~StderrPipe() {
        restore();
        close_pipe();
    }

    [[nodiscard]] bool ready() const {
        return capturing_;
    }

    std::string finish() {
        restore();
        std::string output;
        char buffer[256];
        for (;;) {
            const ssize_t count = ::read(pipe_[0], buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(pipe_[0]);
        pipe_[0] = -1;
        return output;
    }

private:
    void restore() {
        if (!capturing_) {
            return;
        }
        std::fflush(stderr);
        std::cerr.flush();
        static_cast<void>(::dup2(original_, STDERR_FILENO));
        ::close(original_);
        original_ = -1;
        capturing_ = false;
    }

    void close_pipe() {
        for (int& descriptor : pipe_) {
            if (descriptor >= 0) {
                ::close(descriptor);
                descriptor = -1;
            }
        }
    }

    int pipe_[2]{-1, -1};
    int original_{-1};
    bool capturing_{false};
};

int connect_loopback(unsigned short port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return -1;
    }
    return socket_fd;
}

bool send_all(int socket_fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(socket_fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string read_response(int socket_fd, bool wait_for_close) {
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
        if (wait_for_close) {
            continue;
        }
        const std::size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }
        std::string headers = response.substr(0, header_end);
        std::ranges::transform(headers, headers.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        constexpr std::string_view prefix = "\r\ncontent-length: ";
        const std::size_t length_start = headers.find(prefix);
        if (length_start == std::string::npos) {
            return {};
        }
        const std::size_t length_end = headers.find("\r\n", length_start + prefix.size());
        if (length_end == std::string::npos) {
            return {};
        }
        const char* first = headers.data() + length_start + prefix.size();
        const char* last = headers.data() + length_end;
        std::size_t body_size = 0;
        const auto parsed = std::from_chars(first, last, body_size);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return {};
        }
        const std::size_t expected_size = header_end + 4 + body_size;
        if (response.size() >= expected_size) {
            response.resize(expected_size);
            return response;
        }
    }
    return wait_for_close ? response : std::string{};
}

} // namespace

TEST(HttpServer, IdleKeepAliveCloseWritesNothingToStderr) {
    StderrPipe captured_stderr;
    ASSERT_TRUE(captured_stderr.ready());

    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.node_name = "idle-close-test";
    options.admin_token = "idle-close-admin";
    options.instance_kek = "idle-close-instance-kek";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const int keep_alive = connect_loopback(server.port());
    ASSERT_GE(keep_alive, 0);
    ASSERT_TRUE(send_all(keep_alive,
                         "GET /healthz HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: keep-alive\r\n\r\n"));
    const std::string first_response = read_response(keep_alive, false);
    ASSERT_NE(first_response.find(" 200 "), std::string::npos) << first_response;
    ::close(keep_alive);

    const int confirmation = connect_loopback(server.port());
    ASSERT_GE(confirmation, 0);
    ASSERT_TRUE(send_all(confirmation,
                         "GET /healthz HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n\r\n"));
    const std::string confirmation_response = read_response(confirmation, true);
    ::close(confirmation);
    ASSERT_NE(confirmation_response.find(" 200 "), std::string::npos) << confirmation_response;

    server.stop();
    EXPECT_EQ(captured_stderr.finish(), "");
}
