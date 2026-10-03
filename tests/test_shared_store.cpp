// Two lake processes and one store root (lake#467).
//
// A rollout that overlaps a single-replica Lake runs a second lake process on the store volume
// before the first one exits. The libliquid customer stores are single-process by design (their
// pack append position, object map and chunk index live in process memory), so lake takes an
// exclusive owner lock on the store root and a second process refuses to start. These tests
// start real lake binaries against one store root and one PostgreSQL control database, write to
// every store-backed subsystem while the owner drains on SIGTERM, and then check the store and
// that every acknowledged write reads back from the process that takes over.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <liquid/liquid.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/server.hpp"
#include "lake/api/store_owner.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/transparency/log.hpp"
#include "objects/records.hpp"
#include "postgres_fixture.hpp"
#include "temporary_directory.hpp"

extern char** environ;

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* admin_token = "shared-store-admin";

#if defined(MSG_NOSIGNAL)
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0; // SO_NOSIGPIPE is set on the socket instead
#endif

std::vector<std::byte> bytes_of(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

std::string text_of(const std::vector<std::byte>& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

template <class Result> auto checked(Result result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

unsigned short free_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (descriptor < 0 ||
        ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        return 0;
    }
    ::close(descriptor);
    return ntohs(address.sin_port);
}

struct HttpResponse {
    int status = 0; // 0: no response (refused, reset, or cut off)
    std::string headers;
    std::string body;
};

HttpResponse http(unsigned short port,
                  const std::string& method,
                  const std::string& path,
                  const std::string& body = {},
                  const std::string& content_type = "application/json") {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return {};
    }
    timeval timeout{30, 0};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
    const int no_sigpipe = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return {};
    }
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
                          "Connection: close\r\nAuthorization: Bearer " + admin_token +
                          "\r\nContent-Type: " + content_type +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            ::send(descriptor, request.data() + sent, request.size() - sent, send_flags);
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[8192];
    for (;;) {
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    const std::size_t separator = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.1 ") || separator == std::string::npos) {
        return {};
    }
    HttpResponse output;
    output.status = std::atoi(response.c_str() + 9);
    output.headers = response.substr(0, separator + 2);
    output.body = response.substr(separator + 4);
    // A response cut off by the shutdown is not an acknowledgement.
    const std::string length_header = "\r\nContent-Length: ";
    const std::size_t length = output.headers.find(length_header);
    if (length != std::string::npos &&
        static_cast<std::size_t>(std::atoll(output.headers.c_str() + length +
                                            length_header.size())) != output.body.size()) {
        return {};
    }
    return output;
}

std::string header(const HttpResponse& response, const std::string& name) {
    const std::string prefix = "\r\n" + name + ": ";
    const std::size_t start = response.headers.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value = start + prefix.size();
    return response.headers.substr(value, response.headers.find("\r\n", value) - value);
}

json call(unsigned short port, const std::string& capability, const json& input) {
    const HttpResponse response = http(port, "POST", "/v1/" + capability, input.dump());
    if (response.status != 200 && response.status != 201) {
        throw std::runtime_error(capability + " answered " + std::to_string(response.status) +
                                 ": " + response.body);
    }
    const json envelope = json::parse(response.body);
    if (!envelope.value("ok", false)) {
        throw std::runtime_error(capability + " failed: " + response.body);
    }
    return envelope.at("value");
}

// A lake binary run against a store root, with its stderr kept for the refusal message.
class LakeChild {
public:
    LakeChild(const std::filesystem::path& store_root,
              const std::string& database,
              const std::string& kek,
              const std::filesystem::path& stderr_path)
        : port_(free_port()), stderr_path_(stderr_path) {
        std::vector<std::string> arguments{LAKE_TEST_SERVER_PATH,
                                           "--listen",
                                           "127.0.0.1:" + std::to_string(port_),
                                           "--control-db",
                                           database,
                                           "--store-root",
                                           store_root.string(),
                                           "--admin-token",
                                           admin_token,
                                           "--instance-kek",
                                           kek};
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, stderr_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        if (::posix_spawn(&pid_, argv.front(), &actions, nullptr, argv.data(), environ) != 0) {
            pid_ = -1;
        }
        posix_spawn_file_actions_destroy(&actions);
    }

    ~LakeChild() {
        if (pid_ > 0 && !status_) {
            ::kill(pid_, SIGKILL);
            static_cast<void>(::waitpid(pid_, nullptr, 0));
        }
    }

    LakeChild(const LakeChild&) = delete;
    LakeChild& operator=(const LakeChild&) = delete;

    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    // Ready on /readyz, or the process exited first (then exit_status() is set).
    bool wait_ready(std::chrono::milliseconds limit = 20s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (exited()) {
                return false;
            }
            if (http(port_, "GET", "/readyz").status == 200) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    bool exited() {
        if (status_) {
            return true;
        }
        int status = 0;
        if (pid_ > 0 && ::waitpid(pid_, &status, WNOHANG) == pid_) {
            status_ = status;
            return true;
        }
        return false;
    }

    std::optional<int> wait_exit(std::chrono::milliseconds limit = 30s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!exited() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        return status_;
    }

    void terminate() const {
        ::kill(pid_, SIGTERM);
    }

    [[nodiscard]] std::string stderr_text() const {
        std::ifstream input(stderr_path_);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

private:
    unsigned short port_;
    std::filesystem::path stderr_path_;
    pid_t pid_ = -1;
    std::optional<int> status_;
};

std::string signed_attestation(std::uint64_t sequence) {
    // A distinct subject per sequence makes every appended entry unique.
    const std::string subject = "shared-store attestation " + std::to_string(sequence);
    liquid::attestation statement{
        .subject = checked(liquid::hash(liquid::algorithm::blake3, bytes_of(subject))),
        .profile = "authorship/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = liquid::attestation_subject_kind::drip,
        .attributes = {{"author", "taap:1:ed25519:shared-store"}},
    };
    auto signer = checked(liquid::runtime::block_on(liquid::identity_detail::local_signer()));
    statement.issuer = signer.identity();
    const auto body = checked(liquid::objects::encode_attestation_body(statement));
    const auto signature = checked(liquid::runtime::block_on(signer.sign("attestation", body)));
    statement.signature = checked(liquid::encode_attestation_signature(signature));
    const auto encoded = checked(liquid::encode_attestation(statement));
    return lake::control::identity_crypto::base64url_encode(
        std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
}

std::string base64(std::string_view value) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    std::size_t index = 0;
    for (; index + 2 < value.size(); index += 3) {
        const unsigned triple = (static_cast<unsigned char>(value[index]) << 16U) |
                                (static_cast<unsigned char>(value[index + 1]) << 8U) |
                                static_cast<unsigned char>(value[index + 2]);
        for (const unsigned shift : {18U, 12U, 6U, 0U}) {
            output += alphabet[(triple >> shift) & 63U];
        }
    }
    if (index + 1 == value.size()) {
        const unsigned single = static_cast<unsigned char>(value[index]) << 16U;
        output += alphabet[(single >> 18U) & 63U];
        output += alphabet[(single >> 12U) & 63U];
        output += "==";
    } else if (index + 2 == value.size()) {
        const unsigned pair = (static_cast<unsigned char>(value[index]) << 16U) |
                              (static_cast<unsigned char>(value[index + 1]) << 8U);
        output += alphabet[(pair >> 18U) & 63U];
        output += alphabet[(pair >> 12U) & 63U];
        output += alphabet[(pair >> 6U) & 63U];
        output += '=';
    }
    return output;
}

// What each writer had acknowledged (a complete 2xx response, or a completed push).
struct Acknowledged {
    std::mutex mutex;
    std::map<std::string, std::string> pushed;       // repository path -> contents
    std::map<std::string, std::string> drive;        // drive path -> contents
    std::map<std::string, std::string> manifests;    // OCI tag -> manifest
    std::map<std::string, std::string> blobs;        // OCI digest -> contents
    std::map<std::uint64_t, std::string> log_leaves; // leaf index -> attestation
};

struct Fixture {
    std::string customer_id;
    std::string pushed_repository;
    std::string drive_repository;
};

Fixture provision(unsigned short port) {
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const json user = call(port,
                           "create_user",
                           {{"taap_id", "taap:1:ed25519:shared-store-" + suffix},
                            {"display_name", "Shared store"}});
    const json customer = call(port,
                               "create_customer",
                               {{"customer_type", "individual"},
                                {"user_id", user.at("id")},
                                {"name", "shared-store-" + suffix}});
    const std::string customer_id = customer.at("id");
    const json pushed =
        call(port, "create_repository", {{"customer_id", customer_id}, {"name", "pushed"}});
    const json drive =
        call(port, "create_repository", {{"customer_id", customer_id}, {"name", "drive"}});
    return {customer_id, pushed.at("id"), drive.at("id")};
}

// Writers run against the serving process until it stops answering. Every write is unique, so a
// lost or overwritten write shows up as a missing or different value when it is read back.
class Workload {
public:
    Workload(std::vector<unsigned short> ports,
             Fixture fixture,
             std::filesystem::path scratch,
             Acknowledged& acknowledged)
        : ports_(std::move(ports)), fixture_(std::move(fixture)), scratch_(std::move(scratch)),
          acknowledged_(acknowledged) {}

    void start() {
        for (std::size_t lane = 0; lane < ports_.size(); ++lane) {
            threads_.emplace_back([this, lane] { push_repository(lane); });
            threads_.emplace_back([this, lane] { write_drive(lane); });
            threads_.emplace_back([this, lane] { upload_blobs(lane); });
            threads_.emplace_back([this, lane] { append_log(lane); });
        }
    }

    // Writers stop once their process has stopped answering, or when asked.
    void stop() {
        stopping_ = true;
        for (std::thread& thread : threads_) {
            thread.join();
        }
        threads_.clear();
    }

    [[nodiscard]] std::uint64_t attempts() const {
        return attempts_.load();
    }
    [[nodiscard]] std::uint64_t failures() const {
        return failures_.load();
    }
    [[nodiscard]] std::vector<std::string> errors() {
        std::lock_guard lock(errors_mutex_);
        return errors_;
    }

private:
    bool keep_going(std::uint64_t& consecutive_failures) const {
        // After the process stops answering, a few retries cover the drain window.
        return !stopping_.load() && consecutive_failures < 20;
    }

    void failed(std::uint64_t& consecutive_failures, const std::string& error) {
        ++consecutive_failures;
        ++failures_;
        std::lock_guard lock(errors_mutex_);
        if (errors_.size() < 50) {
            errors_.push_back(error);
        }
        std::this_thread::sleep_for(25ms);
    }

    void push_repository(std::size_t lane) {
        const std::filesystem::path local = scratch_ / ("push-source-" + std::to_string(lane));
        auto source = liquid::runtime::block_on(liquid::repository::create(local));
        if (!source) {
            std::lock_guard lock(errors_mutex_);
            errors_.push_back("push source: " + source.error().message);
            return;
        }
        const std::string url = "http://127.0.0.1:" + std::to_string(ports_[lane]) +
                                "/v1/repositories/" + fixture_.pushed_repository;
        liquid::sync_endpoint endpoint(url, liquid::sync_endpoint_options{.bearer_token = admin_token});
        std::map<std::string, std::string> written;
        std::uint64_t consecutive = 0;
        for (std::uint64_t sequence = 0; keep_going(consecutive); ++sequence) {
            ++attempts_;
            const std::string path = "lane-" + std::to_string(lane) + "/" +
                                     std::to_string(sequence) + ".txt";
            const std::string contents = "push " + std::to_string(lane) + "/" +
                                         std::to_string(sequence) + " " +
                                         std::string(4096 + sequence % 977, 'p');
            if (!liquid::runtime::block_on(source->write(path, bytes_of(contents))) ||
                !liquid::runtime::block_on(source->cut("push " + path))) {
                failed(consecutive, "local cut failed");
                continue;
            }
            written.emplace(path, contents);
            const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
            if (!pushed) {
                failed(consecutive, "push: " + pushed.error().message);
                continue;
            }
            consecutive = 0;
            // A completed push publishes every change cut so far.
            std::lock_guard lock(acknowledged_.mutex);
            acknowledged_.pushed.insert(written.begin(), written.end());
        }
    }

    void write_drive(std::size_t lane) {
        std::uint64_t consecutive = 0;
        for (std::uint64_t sequence = 0; keep_going(consecutive); ++sequence) {
            ++attempts_;
            const std::string name =
                "drive-" + std::to_string(lane) + "-" + std::to_string(sequence) + ".txt";
            const std::string contents = "drive " + name + " " + std::string(512, 'd');
            const HttpResponse response =
                http(ports_[lane],
                     "POST",
                     "/v1/upload_drive_files",
                     json{{"repository_id", fixture_.drive_repository},
                          {"ref", "main"},
                          {"path", ""},
                          {"files", {{{"name", name}, {"content", base64(contents)}}}}}
                         .dump());
            if (response.status != 200 ||
                !json::parse(response.body, nullptr, false).value("ok", false)) {
                failed(consecutive,
                       "drive: " + std::to_string(response.status) + " " + response.body);
                continue;
            }
            consecutive = 0;
            std::lock_guard lock(acknowledged_.mutex);
            acknowledged_.drive.emplace(name, contents);
        }
    }

    // One image per iteration: a config blob and a layer blob, then a manifest tag that names
    // them. The registry serves a blob only through a manifest of its repository.
    void upload_blobs(std::size_t lane) {
        std::uint64_t consecutive = 0;
        const auto upload = [&](const std::string& contents) -> std::optional<std::string> {
            const std::string digest =
                "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
            const HttpResponse started = http(ports_[lane],
                                              "POST",
                                              "/v2/shared-store/blobs/uploads/",
                                              "",
                                              "application/octet-stream");
            const std::string location = header(started, "Location");
            if (started.status != 202 || location.empty()) {
                failed(consecutive, "blob start: " + std::to_string(started.status));
                return std::nullopt;
            }
            const HttpResponse completed = http(ports_[lane],
                                                "PUT",
                                                location + "?digest=" + digest,
                                                contents,
                                                "application/octet-stream");
            if (completed.status != 201) {
                failed(consecutive, "blob put: " + std::to_string(completed.status));
                return std::nullopt;
            }
            return digest;
        };
        for (std::uint64_t sequence = 0; keep_going(consecutive); ++sequence) {
            ++attempts_;
            const std::string tag = "t-" + std::to_string(lane) + "-" + std::to_string(sequence);
            const std::string config = json{{"architecture", "amd64"}, {"os", "linux"},
                                            {"tag", tag}}.dump();
            const std::string layer = "layer " + tag + " " + lake::control::new_uuid_v4() +
                                      std::string(2048, 'b');
            const auto config_digest = upload(config);
            if (!config_digest) {
                continue;
            }
            const auto layer_digest = upload(layer);
            if (!layer_digest) {
                continue;
            }
            const std::string manifest =
                json{{"schemaVersion", 2},
                     {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                     {"config",
                      {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                       {"digest", *config_digest},
                       {"size", config.size()}}},
                     {"layers",
                      {{{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                        {"digest", *layer_digest},
                        {"size", layer.size()}}}}}
                    .dump();
            const HttpResponse published = http(ports_[lane],
                                                "PUT",
                                                "/v2/shared-store/manifests/" + tag,
                                                manifest,
                                                "application/vnd.oci.image.manifest.v1+json");
            if (published.status != 201) {
                failed(consecutive, "manifest put: " + std::to_string(published.status));
                continue;
            }
            consecutive = 0;
            std::lock_guard lock(acknowledged_.mutex);
            acknowledged_.manifests.emplace(tag, manifest);
            acknowledged_.blobs.emplace(*config_digest, config);
            acknowledged_.blobs.emplace(*layer_digest, layer);
        }
    }

    void append_log(std::size_t lane) {
        std::uint64_t consecutive = 0;
        for (std::uint64_t sequence = 0; keep_going(consecutive); ++sequence) {
            ++attempts_;
            const std::string attestation = signed_attestation(lane * 1'000'000 + sequence);
            const HttpResponse response =
                http(ports_[lane],
                     "POST",
                     "/v1/append_transparency_entries",
                     json{{"attestations", {attestation}}}.dump());
            if (response.status != 200) {
                failed(consecutive, "log: " + std::to_string(response.status));
                continue;
            }
            const json envelope = json::parse(response.body, nullptr, false);
            const json result = envelope.is_object() && envelope.value("ok", false)
                                    ? envelope.value("value", json::object())
                                    : json::object();
            if (!result.contains("first_index")) {
                failed(consecutive, "log: unexpected reply " + response.body);
                continue;
            }
            consecutive = 0;
            std::lock_guard lock(acknowledged_.mutex);
            acknowledged_.log_leaves.emplace(result.at("first_index").get<std::uint64_t>(),
                                             attestation);
        }
    }

    std::vector<unsigned short> ports_;
    Fixture fixture_;
    std::filesystem::path scratch_;
    Acknowledged& acknowledged_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> attempts_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::mutex errors_mutex_;
    std::vector<std::string> errors_;
};

// Every acknowledged write, read back through the process that took over the store.
void expect_acknowledged_writes_read_back(unsigned short port,
                                          const Fixture& fixture,
                                          const std::filesystem::path& scratch,
                                          Acknowledged& acknowledged) {
    std::lock_guard lock(acknowledged.mutex);
    for (const auto& [repository, expected] :
         {std::pair{fixture.pushed_repository, &acknowledged.pushed},
          std::pair{fixture.drive_repository, &acknowledged.drive}}) {
        auto destination = liquid::runtime::block_on(
            liquid::repository::create(scratch / ("read-back-" + repository)));
        ASSERT_TRUE(destination) << destination.error().message;
        liquid::sync_endpoint endpoint("http://127.0.0.1:" + std::to_string(port) +
                                           "/v1/repositories/" + repository,
                                       liquid::sync_endpoint_options{.bearer_token = admin_token});
        const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
        ASSERT_TRUE(pulled) << pulled.error().message;
        for (const auto& [path, contents] : *expected) {
            const auto read = liquid::runtime::block_on(destination->read(path));
            ASSERT_TRUE(read) << repository << " lost acknowledged " << path << ": "
                              << read.error().message;
            EXPECT_EQ(text_of(*read), contents) << repository << " changed " << path;
        }
    }
    for (const auto& [tag, manifest] : acknowledged.manifests) {
        const HttpResponse pulled = http(port,
                                         "GET",
                                         "/v2/shared-store/manifests/" + tag,
                                         "",
                                         "application/vnd.oci.image.manifest.v1+json");
        EXPECT_EQ(pulled.status, 200) << "lost acknowledged manifest " << tag;
        EXPECT_EQ(pulled.body, manifest) << "changed manifest " << tag;
    }
    for (const auto& [digest, contents] : acknowledged.blobs) {
        const HttpResponse blob = http(
            port, "GET", "/v2/shared-store/blobs/" + digest, "", "application/octet-stream");
        EXPECT_EQ(blob.status, 200) << "lost acknowledged blob " << digest;
        EXPECT_EQ(blob.body, contents) << "changed blob " << digest;
    }
    const HttpResponse checkpoint = http(port, "GET", "/v1/transparency_log_configuration");
    EXPECT_EQ(checkpoint.status, 200) << checkpoint.body;
}

// The store check: libliquid reopens every customer store and walks reachability from its roots
// without deleting anything, and the transparency log's entries recompute its recorded root.
void expect_store_consistent(const std::filesystem::path& store_root,
                             const Fixture& fixture,
                             const std::filesystem::path& scratch,
                             Acknowledged& acknowledged) {
    auto store = liquid::runtime::block_on(
        liquid::store::open(store_root / fixture.customer_id / "store"));
    ASSERT_TRUE(store) << "customer store does not reopen: " << store.error().message;
    const auto collected =
        liquid::runtime::block_on(store->collect(liquid::collection_options{.dry_run = true}));
    ASSERT_TRUE(collected) << "customer store reachability walk failed: "
                           << collected.error().message;
    EXPECT_GT(collected->live_objects, 0U);

    lake::transparency::TiledLog log(store_root, "lake");
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    const auto batch = log.replication_batch(0);
    ASSERT_TRUE(batch) << batch.error().message;
    lake::transparency::TiledLog rebuilt(scratch / "rebuilt-log", "lake");
    const auto applied = rebuilt.apply_replication_batch(*batch);
    ASSERT_TRUE(applied) << "the log's entries do not rebuild its tree: " << applied.error().message;
    EXPECT_EQ(applied->tree_size, state->tree_size);
    EXPECT_EQ(applied->root_hash, state->root_hash);
    std::lock_guard lock(acknowledged.mutex);
    for (const auto& [index, attestation] : acknowledged.log_leaves) {
        ASSERT_LT(index, state->tree_size) << "lost acknowledged log leaf " << index;
        const auto proof = log.inclusion_proof(index, state->tree_size);
        EXPECT_TRUE(proof) << "no inclusion proof for leaf " << index;
    }
}

} // namespace

TEST(StoreOwner, OneLockPerStoreRootNamesItsHolder) {
    const auto root = lake::test::fixture_path("store-owner-lock");
    auto first = lake::api::StoreOwnerLock::acquire(root, "first");
    ASSERT_TRUE(first) << first.error();
    const auto second = lake::api::StoreOwnerLock::acquire(root, "second");
    ASSERT_FALSE(second);
    EXPECT_NE(second.error().find("in use by another lake process"), std::string::npos)
        << second.error();
    EXPECT_NE(second.error().find("pid " + std::to_string(::getpid())), std::string::npos)
        << second.error();
    EXPECT_NE(second.error().find("node first"), std::string::npos) << second.error();
    first->release();
    const auto third = lake::api::StoreOwnerLock::acquire(root, "third");
    EXPECT_TRUE(third) << third.error();
}

TEST(StoreOwner, SecondServerOnOneStoreRootRefusesToStart) {
    const auto root = lake::test::fixture_path("store-owner-servers");
    lake::api::ServerOptions options;
    options.port = 0;
    options.store_root = root.string();
    lake::api::ApiServer first(options);
    ASSERT_TRUE(first.start()) << first.reason();
    lake::api::ApiServer second(options);
    EXPECT_FALSE(second.start());
    EXPECT_NE(second.reason().find("in use by another lake process"), std::string::npos)
        << second.reason();
    first.stop();
    lake::api::ApiServer third(options);
    EXPECT_TRUE(third.start()) << third.reason();
    third.stop();
}

// The overlap a RollingUpdate would create: the owner serves writes to every store-backed
// subsystem, a second process is started on the same store root, then the owner gets SIGTERM
// and drains while the writes continue; a successor then takes the store over.
TEST(StoreOwner, OverlappingProcessRefusesAndAcknowledgedWritesSurviveTheDrain) {
    const char* database = lake::test::postgres_uri();
    if (!database) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const char* kek_variable = std::getenv("LAKE_INSTANCE_KEK");
    ASSERT_NE(kek_variable, nullptr);
    const std::string kek = kek_variable;
    const auto scratch = lake::test::fixture_path("shared-store-" + lake::control::new_uuid_v4());
    const auto store_root = scratch / "store";
    std::filesystem::create_directories(store_root);

    LakeChild owner(store_root, database, kek, scratch / "owner.stderr");
    ASSERT_TRUE(owner.wait_ready()) << owner.stderr_text();
    const Fixture fixture = provision(owner.port());

    Acknowledged acknowledged;
    Workload workload({owner.port()}, fixture, scratch, acknowledged);
    workload.start();
    std::this_thread::sleep_for(1500ms);

    // The surge process: it must not open the store while the owner is alive.
    LakeChild surge(store_root, database, kek, scratch / "surge.stderr");
    EXPECT_FALSE(surge.wait_ready(15s)) << "a second lake process served on a held store root";
    const auto refused = surge.wait_exit(5s);
    ASSERT_TRUE(refused.has_value()) << "the second process neither served nor exited";
    EXPECT_TRUE(WIFEXITED(*refused) && WEXITSTATUS(*refused) != 0);
    const std::string refusal = surge.stderr_text();
    EXPECT_NE(refusal.find("is in use by another lake process"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Recreate"), std::string::npos) << refusal;

    // The owner drains under load.
    std::this_thread::sleep_for(500ms);
    owner.terminate();
    const auto drained = owner.wait_exit(30s);
    workload.stop();
    ASSERT_TRUE(drained.has_value()) << "the owner did not exit after SIGTERM";
    EXPECT_TRUE(WIFEXITED(*drained) && WEXITSTATUS(*drained) == 0) << owner.stderr_text();

    {
        std::lock_guard lock(acknowledged.mutex);
        EXPECT_FALSE(acknowledged.pushed.empty());
        EXPECT_FALSE(acknowledged.drive.empty());
        EXPECT_FALSE(acknowledged.blobs.empty());
        EXPECT_FALSE(acknowledged.log_leaves.empty());
        std::println(stderr,
                     "shared store: {} attempts, {} acknowledged pushes, {} drive files, {} blobs, "
                     "{} log leaves, {} failures",
                     workload.attempts(),
                     acknowledged.pushed.size(),
                     acknowledged.drive.size(),
                     acknowledged.blobs.size(),
                     acknowledged.log_leaves.size(),
                     workload.failures());
    }

    // The successor takes the store over once the owner has exited.
    {
        LakeChild successor(store_root, database, kek, scratch / "successor.stderr");
        ASSERT_TRUE(successor.wait_ready()) << successor.stderr_text();
        expect_acknowledged_writes_read_back(successor.port(), fixture, scratch, acknowledged);
        successor.terminate();
        ASSERT_TRUE(successor.wait_exit(30s).has_value());
    }
    expect_store_consistent(store_root, fixture, scratch, acknowledged);
}

