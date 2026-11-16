#include "lake/control/dns_txt.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace lake::control {
namespace {

constexpr std::uint16_t type_txt = 16;
constexpr std::uint16_t class_in = 1;
constexpr std::size_t header_size = 12;
// The largest UDP answer read; a bigger one comes back truncated and is asked again over TCP.
constexpr std::size_t udp_answer_limit = 4096;
// A write to a closed TCP connection fails with EPIPE instead of raising SIGPIPE: Linux takes a
// flag per send, macOS a socket option (set in ask_tcp).
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

std::uint16_t read16(std::span<const std::uint8_t> message, std::size_t at) {
    return static_cast<std::uint16_t>((message[at] << 8) | message[at + 1]);
}

// Moves `at` past the (possibly compressed) name that starts there, or fails.
bool skip_name(std::span<const std::uint8_t> message, std::size_t& at) {
    for (int labels = 0; labels < 128; ++labels) {
        if (at >= message.size()) {
            return false;
        }
        const std::uint8_t length = message[at];
        if (length == 0) {
            ++at;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (at + 1 >= message.size()) {
                return false;
            }
            at += 2; // a pointer ends the name
            return true;
        }
        if ((length & 0xC0) != 0) {
            return false;
        }
        at += 1 + length;
    }
    return false;
}

// A file descriptor closed on every path.
class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] int get() const {
        return fd_;
    }

private:
    int fd_;
};

struct Server {
    sockaddr_storage address{};
    socklen_t length = 0;
};

std::expected<Server, std::string> server_address(const std::string& literal) {
    Server server;
    auto* v4 = reinterpret_cast<sockaddr_in*>(&server.address);
    if (::inet_pton(AF_INET, literal.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(53);
        server.length = sizeof(sockaddr_in);
        return server;
    }
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&server.address);
    if (::inet_pton(AF_INET6, literal.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(53);
        server.length = sizeof(sockaddr_in6);
        return server;
    }
    return std::unexpected("name server " + literal + " is not an IP address");
}

// Waits until `fd` is ready for `events` or `deadline` passes.
bool wait_for(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return false;
        }
        pollfd entry{fd, events, 0};
        const int ready = ::poll(&entry, 1, static_cast<int>(left.count()));
        if (ready > 0) {
            return (entry.revents & events) != 0;
        }
        if (ready == 0 || errno != EINTR) {
            return false;
        }
    }
}

std::expected<TxtAnswer, std::string> ask_udp(const Server& server,
                                              const std::vector<std::uint8_t>& query,
                                              std::uint16_t id,
                                              std::chrono::steady_clock::time_point deadline) {
    Socket socket(::socket(server.address.ss_family, SOCK_DGRAM, 0));
    if (socket.get() < 0) {
        return std::unexpected(std::string("could not open a UDP socket: ") + std::strerror(errno));
    }
    if (::connect(
            socket.get(), reinterpret_cast<const sockaddr*>(&server.address), server.length) != 0) {
        return std::unexpected(std::string("could not reach the name server: ") +
                               std::strerror(errno));
    }
    if (::send(socket.get(), query.data(), query.size(), 0) != static_cast<ssize_t>(query.size())) {
        return std::unexpected(std::string("could not send the DNS query: ") +
                               std::strerror(errno));
    }
    std::vector<std::uint8_t> answer(udp_answer_limit);
    // An answer with another id (a late answer to an earlier question) is read past.
    for (;;) {
        if (!wait_for(socket.get(), POLLIN, deadline)) {
            return std::unexpected(std::string("the name server did not answer in time"));
        }
        const ssize_t read = ::recv(socket.get(), answer.data(), answer.size(), 0);
        if (read < 0) {
            return std::unexpected(std::string("could not read the DNS answer: ") +
                                   std::strerror(errno));
        }
        const std::span<const std::uint8_t> message(answer.data(), static_cast<std::size_t>(read));
        if (message.size() >= 2 && read16(message, 0) != id) {
            continue;
        }
        return decode_txt_answer(message, id);
    }
}

bool send_all(int fd,
              const std::uint8_t* data,
              std::size_t size,
              std::chrono::steady_clock::time_point deadline) {
    while (size > 0) {
        if (!wait_for(fd, POLLOUT, deadline)) {
            return false;
        }
        const ssize_t sent = ::send(fd, data, size, send_flags);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool read_all(int fd,
              std::uint8_t* data,
              std::size_t size,
              std::chrono::steady_clock::time_point deadline) {
    while (size > 0) {
        if (!wait_for(fd, POLLIN, deadline)) {
            return false;
        }
        const ssize_t read = ::recv(fd, data, size, 0);
        if (read <= 0) {
            if (read < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        data += read;
        size -= static_cast<std::size_t>(read);
    }
    return true;
}

// The same question over TCP (RFC 7766): each message is preceded by its two-octet length.
std::expected<TxtAnswer, std::string> ask_tcp(const Server& server,
                                              const std::vector<std::uint8_t>& query,
                                              std::uint16_t id,
                                              std::chrono::steady_clock::time_point deadline) {
    Socket socket(::socket(server.address.ss_family, SOCK_STREAM, 0));
    if (socket.get() < 0) {
        return std::unexpected(std::string("could not open a TCP socket: ") + std::strerror(errno));
    }
    const int flags = ::fcntl(socket.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        return std::unexpected(std::string("could not open a TCP socket: ") + std::strerror(errno));
    }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (::connect(
            socket.get(), reinterpret_cast<const sockaddr*>(&server.address), server.length) != 0) {
        if (errno != EINPROGRESS || !wait_for(socket.get(), POLLOUT, deadline)) {
            return std::unexpected(std::string("could not reach the name server over TCP"));
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            return std::unexpected(std::string("could not reach the name server over TCP"));
        }
    }
    std::vector<std::uint8_t> framed;
    framed.reserve(query.size() + 2);
    framed.push_back(static_cast<std::uint8_t>(query.size() >> 8));
    framed.push_back(static_cast<std::uint8_t>(query.size() & 0xFF));
    framed.insert(framed.end(), query.begin(), query.end());
    if (!send_all(socket.get(), framed.data(), framed.size(), deadline)) {
        return std::unexpected(std::string("could not send the DNS query over TCP"));
    }
    std::uint8_t length[2];
    if (!read_all(socket.get(), length, sizeof(length), deadline)) {
        return std::unexpected(std::string("the name server did not answer over TCP in time"));
    }
    std::vector<std::uint8_t> answer(static_cast<std::size_t>((length[0] << 8) | length[1]));
    if (!read_all(socket.get(), answer.data(), answer.size(), deadline)) {
        return std::unexpected(std::string("the name server did not answer over TCP in time"));
    }
    return decode_txt_answer(answer, id);
}

std::uint16_t random_id() {
    std::random_device device;
    return static_cast<std::uint16_t>(device() & 0xFFFF);
}

} // namespace

std::expected<std::vector<std::uint8_t>, std::string> encode_txt_query(std::string_view name,
                                                                       std::uint16_t id) {
    if (name.ends_with('.')) {
        name.remove_suffix(1);
    }
    if (name.empty() || name.size() > 253) {
        return std::unexpected(std::string("not a DNS name"));
    }
    std::vector<std::uint8_t> message{static_cast<std::uint8_t>(id >> 8),
                                      static_cast<std::uint8_t>(id & 0xFF),
                                      0x01, // recursion desired
                                      0x00,
                                      0x00,
                                      0x01, // one question
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00};
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? name.size() : dot;
        const std::size_t length = end - start;
        if (length == 0 || length > 63) {
            return std::unexpected(std::string("not a DNS name"));
        }
        message.push_back(static_cast<std::uint8_t>(length));
        message.insert(message.end(),
                       name.begin() + static_cast<std::ptrdiff_t>(start),
                       name.begin() + static_cast<std::ptrdiff_t>(end));
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    message.push_back(0);
    message.push_back(0);
    message.push_back(type_txt);
    message.push_back(0);
    message.push_back(class_in);
    return message;
}

std::expected<TxtAnswer, std::string> decode_txt_answer(std::span<const std::uint8_t> message,
                                                        std::uint16_t id) {
    if (message.size() < header_size) {
        return std::unexpected(std::string("the DNS answer is too short"));
    }
    if (read16(message, 0) != id) {
        return std::unexpected(std::string("the DNS answer is for another question"));
    }
    const std::uint16_t flags = read16(message, 2);
    if ((flags & 0x8000) == 0) {
        return std::unexpected(std::string("the DNS message is not an answer"));
    }
    TxtAnswer answer;
    if ((flags & 0x0200) != 0) {
        answer.truncated = true;
        return answer;
    }
    const int rcode = flags & 0x000F;
    if (rcode == 3) { // NXDOMAIN: the name does not exist, so it holds no record
        return answer;
    }
    if (rcode != 0) {
        return std::unexpected("the name server refused the DNS question (rcode " +
                               std::to_string(rcode) + ")");
    }
    const std::uint16_t questions = read16(message, 4);
    const std::uint16_t answers = read16(message, 6);
    std::size_t at = header_size;
    for (std::uint16_t index = 0; index < questions; ++index) {
        if (!skip_name(message, at) || at + 4 > message.size()) {
            return std::unexpected(std::string("the DNS answer is malformed"));
        }
        at += 4;
    }
    for (std::uint16_t index = 0; index < answers; ++index) {
        if (!skip_name(message, at) || at + 10 > message.size()) {
            return std::unexpected(std::string("the DNS answer is malformed"));
        }
        const std::uint16_t type = read16(message, at);
        const std::uint16_t klass = read16(message, at + 2);
        const std::uint16_t length = read16(message, at + 8);
        at += 10;
        if (at + length > message.size()) {
            return std::unexpected(std::string("the DNS answer is malformed"));
        }
        // A CNAME on the way to the TXT records is read past; only TXT records are answered.
        if (type == type_txt && klass == class_in) {
            std::string record;
            std::size_t inner = at;
            while (inner < at + length) {
                const std::uint8_t piece = message[inner];
                if (inner + 1 + piece > at + length) {
                    return std::unexpected(std::string("the DNS answer is malformed"));
                }
                record.append(reinterpret_cast<const char*>(message.data() + inner + 1), piece);
                inner += 1 + piece;
            }
            answer.records.push_back(std::move(record));
        }
        at += length;
    }
    return answer;
}

std::vector<std::string> resolv_conf_nameservers(std::string_view text) {
    std::vector<std::string> servers;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string keyword;
        std::string address;
        if (words >> keyword >> address && keyword == "nameserver") {
            // A scoped IPv6 address (fe80::1%eth0) is not used: inet_pton cannot read it.
            if (!address.contains('%')) {
                servers.push_back(address);
            }
        }
    }
    return servers;
}

std::expected<std::vector<std::string>, std::string>
lookup_txt(const std::string& name, std::chrono::milliseconds per_server) {
    std::vector<std::string> servers;
    if (std::ifstream file("/etc/resolv.conf"); file) {
        std::ostringstream text;
        text << file.rdbuf();
        servers = resolv_conf_nameservers(text.str());
    }
    if (servers.empty()) {
        servers.push_back("127.0.0.1");
    }
    std::string last_error = "no name server answered";
    for (const std::string& literal : servers) {
        const auto server = server_address(literal);
        if (!server) {
            last_error = server.error();
            continue;
        }
        const std::uint16_t id = random_id();
        const auto query = encode_txt_query(name, id);
        if (!query) {
            return std::unexpected(query.error());
        }
        const auto deadline = std::chrono::steady_clock::now() + per_server;
        auto answer = ask_udp(*server, *query, id, deadline);
        if (answer && answer->truncated) {
            answer = ask_tcp(*server, *query, id, std::chrono::steady_clock::now() + per_server);
            if (answer && answer->truncated) {
                answer = std::unexpected(std::string("the DNS answer was truncated over TCP"));
            }
        }
        if (answer) {
            return std::move(answer->records);
        }
        last_error = answer.error();
    }
    return std::unexpected(last_error);
}

} // namespace lake::control
