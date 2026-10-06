// lake#383 phase B (decision 54 S5): a tenant image's blobs live in its workspace's liquid store.
//
// A real lake server runs with workspace stores on FakeZettaObjects (Zetta's object routes, as
// tests/zetta_object_fixture.hpp specifies them) and workspace records on RecordWorkspaces. Each
// test switches its workspaces to their own stores (move_workspace_store) and pushes and pulls
// images through the OCI Distribution routes over a plain socket, so headers and bodies are seen
// as a client sees them, and a large layer is read back without the client keeping it. See
// docs/api/registry.md#where-a-tenant-image-lives.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"
#include "zetta_object_fixture.hpp"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view kek = "5a1e2d3c4b5a69788796a5b4c3d2e1f000112233445566778899aabbccdd0383";
constexpr std::string_view marker = "LAKE-383B-LAYER-PLAINTEXT-7d2f-never-in-a-bucket";
constexpr std::string_view admin = "Bearer registry-store-admin";

std::string sha256(std::string_view bytes) {
    return "sha256:" + lake::control::identity_crypto::sha256_hex(bytes);
}

// Pseudo-random bytes (incompressible, no repeats), with the marker at the start.
std::string layer_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    std::string output(size, '\0');
    for (std::size_t index = 0; index < size; index += 8) {
        const std::uint64_t value = generator();
        for (std::size_t part = 0; part < 8 && index + part < size; ++part) {
            output[index + part] = static_cast<char>((value >> (part * 8)) & 0xffU);
        }
    }
    std::copy_n(marker.data(), std::min(marker.size(), size), output.begin());
    return output;
}

// The resident set size of this process now.
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

struct Reply {
    unsigned status = 0;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
    std::uint64_t body_bytes = 0;
};

// One HTTP/1.1 exchange over a plain socket. With `sink`, the body is handed over as it arrives and
// not kept.
Reply exchange(unsigned short port, const std::string& method, const std::string& path,
               const std::string& body = {}, std::vector<std::pair<std::string, std::string>> headers = {},
               std::string_view authorization = admin,
               const std::function<void(std::string_view)>& sink = {}) {
    Reply reply;
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return reply;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return reply;
    }
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!authorization.empty()) {
        request += "Authorization: " + std::string(authorization) + "\r\n";
    }
    for (const auto& [name, value] : headers) {
        request += name + ": " + value + "\r\n";
    }
    request += "\r\n";
    request += body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return reply;
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string head;
    bool in_body = false;
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        std::string_view piece(buffer.data(), static_cast<std::size_t>(count));
        if (!in_body) {
            head.append(piece);
            const std::size_t end = head.find("\r\n\r\n");
            if (end == std::string::npos) {
                continue;
            }
            in_body = true;
            const std::string rest = head.substr(end + 4);
            head.resize(end);
            piece = rest;
            std::size_t line_end = head.find("\r\n");
            const std::string status_line = head.substr(0, line_end);
            reply.status = static_cast<unsigned>(std::stoul(status_line.substr(9, 3)));
            while (line_end != std::string::npos) {
                const std::size_t next = head.find("\r\n", line_end + 2);
                const std::string line = head.substr(line_end + 2, next == std::string::npos
                                                                       ? std::string::npos
                                                                       : next - line_end - 2);
                if (const std::size_t colon = line.find(':'); colon != std::string::npos) {
                    std::string name = line.substr(0, colon);
                    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                    reply.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
                }
                line_end = next;
            }
            reply.body_bytes += rest.size();
            if (sink) {
                sink(rest);
            } else {
                reply.body += rest;
            }
            continue;
        }
        reply.body_bytes += piece.size();
        if (sink) {
            sink(piece);
        } else {
            reply.body.append(piece);
        }
    }
    ::close(socket_fd);
    return reply;
}

std::string oci_code(const Reply& reply) {
    const json body = json::parse(reply.body, nullptr, false);
    return body.is_object() && body.contains("errors") ? body["errors"][0].value("code", "") : "";
}

// SHA-256 over bytes handed over in pieces.
struct StreamHash {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    StreamHash() {
        EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr);
    }
    void update(std::string_view bytes) {
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size());
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        unsigned int size = 0;
        EVP_DigestFinal_ex(context.get(), digest.data(), &size);
        std::string output = "sha256:";
        for (const unsigned char byte : digest) {
            output += std::format("{:02x}", byte);
        }
        return output;
    }
};

bool holds_marker(const std::map<std::string, std::string>& objects) {
    return std::ranges::any_of(objects, [](const auto& object) {
        return object.second.find(marker) != std::string::npos;
    });
}

// Two spaces: `tenant-one` with workspaces alpha and beta, `tenant-two` with gamma, each workspace
// Zetta in the fake at `http://<workspace>.test`.
struct RegistryRig {
    std::shared_ptr<lake::control::InMemoryStore> store = std::make_shared<lake::control::InMemoryStore>();
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces;
    std::shared_ptr<lake::test::FakeZettaObjects> zetta = std::make_shared<lake::test::FakeZettaObjects>();
    std::string one, two;            // the spaces' customer ids
    std::string alpha, beta, gamma;  // their workspaces
    std::filesystem::path root =
        lake::test::fixture_path("registry-store-" + lake::control::new_uuid_v4());
    lake::api::ServerOptions options;
    std::unique_ptr<lake::api::ApiServer> server;

    void SetUp() {
        const auto first = store->create_user({"taap:registry-store-one", "", "Registry One"});
        const auto second = store->create_user({"taap:registry-store-two", "", "Registry Two"});
        ASSERT_TRUE(first && second);
        const auto space_one = store->create_customer({"individual", first->id, "", "tenant-one"});
        const auto space_two = store->create_customer({"individual", second->id, "", "tenant-two"});
        ASSERT_TRUE(space_one && space_two);
        one = space_one->id;
        two = space_two->id;
        workspaces = std::make_unique<lake::test::RecordWorkspaces>(store);
        alpha = workspaces->create(one, "alpha");
        beta = workspaces->create(one, "beta");
        gamma = workspaces->create(two, "gamma");
        for (const std::string& workspace : {alpha, beta, gamma}) {
            zetta->add_node(endpoint(workspace), "token-" + workspace);
        }
        options.port = 0;
        options.node_name = "registry-store";
        options.store = store;
        options.workspaces = workspaces->service;
        options.object_transport = zetta;
        options.admin_token = "registry-store-admin";
        options.instance_kek = std::string(kek);
        options.store_root = root.string();
        server = std::make_unique<lake::api::ApiServer>(options);
        ASSERT_TRUE(server->start()) << server->reason();
        for (int waited = 0; waited < 200 && !server->store_root_open(); ++waited) {
            std::this_thread::sleep_for(20ms);
        }
        ASSERT_TRUE(server->store_root_open());
    }

    ~RegistryRig() {
        if (server) {
            server->stop();
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    static std::string endpoint(const std::string& workspace) {
        return "http://" + workspace + ".test";
    }

    unsigned short port() const {
        return server->port();
    }

    std::filesystem::path registry() const {
        return root / "registry-store" / "registry";
    }

    json call(const std::string& capability, const json& input) {
        Reply reply = exchange(port(), "POST", "/v1/" + capability, input.dump(),
                                     {{"Content-Type", "application/json"}});
        return json::parse(reply.body, nullptr, false);
    }

    // The workspace's repositories (none here) move to its own store: copy, then switch.
    void switch_store(const std::string& workspace) {
        for (const std::string mode : {"copy", "switch"}) {
            const json moved =
                call("move_workspace_store", {{"workspace_id", workspace}, {"mode", mode}});
            ASSERT_TRUE(moved.value("ok", false)) << mode << moved;
        }
        ASSERT_FALSE(store->get_workspace(workspace)->store_moved_at.empty());
    }

    void route(const std::string& space, const std::string& workspace, const std::string& name) {
        const json created = call("create_image_repository",
                                  {{"customer_id", space}, {"workspace_id", workspace}, {"name", name}});
        ASSERT_TRUE(created.value("ok", false)) << created;
    }

    // A monolithic upload: POST, then PUT with the whole body.
    Reply push_blob(const std::string& repository, const std::string& contents) {
        Reply started = exchange(port(), "POST", "/v2/" + repository + "/blobs/uploads/");
        if (started.status != 202) {
            return started;
        }
        return exchange(port(), "PUT", started.headers.at("location") + "?digest=" + sha256(contents),
                        contents, {{"Content-Type", "application/octet-stream"}});
    }

    // A chunked upload: POST, one PATCH per piece with its Content-Range, then an empty PUT.
    Reply push_chunked(const std::string& repository, const std::vector<std::string>& pieces,
                       const std::string& digest) {
        Reply started = exchange(port(), "POST", "/v2/" + repository + "/blobs/uploads/");
        if (started.status != 202) {
            return started;
        }
        const std::string location = started.headers.at("location");
        std::uint64_t offset = 0;
        for (const std::string& piece : pieces) {
            Reply patched = exchange(
                port(), "PATCH", location, piece,
                {{"Content-Type", "application/octet-stream"},
                 {"Content-Range", std::to_string(offset) + "-" + std::to_string(offset + piece.size() - 1)}});
            if (patched.status != 202) {
                return patched;
            }
            offset += piece.size();
        }
        return exchange(port(), "PUT", location + "?digest=" + digest, {},
                        {{"Content-Type", "application/octet-stream"}});
    }

    static json manifest(const std::string& config, const std::vector<std::pair<std::string, std::uint64_t>>& layers) {
        json document{{"schemaVersion", 2},
                      {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                      {"config",
                       {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                        {"digest", sha256(config)},
                        {"size", config.size()}}},
                      {"layers", json::array()}};
        for (const auto& [digest, size] : layers) {
            document["layers"].push_back(
                {{"mediaType", "application/vnd.oci.image.layer.v1.tar"}, {"digest", digest}, {"size", size}});
        }
        return document;
    }

    Reply publish(const std::string& repository, const std::string& tag, const json& document) {
        return exchange(port(), "PUT", "/v2/" + repository + "/manifests/" + tag, document.dump(),
                        {{"Content-Type", "application/vnd.oci.image.manifest.v1+json"}});
    }

    // Pushes a config and each layer monolithically, then the manifest; answers the manifest.
    json push_image(const std::string& repository, const std::string& tag, const std::string& config,
                    const std::vector<std::string>& layers) {
        EXPECT_EQ(push_blob(repository, config).status, 201U);
        std::vector<std::pair<std::string, std::uint64_t>> described;
        for (const std::string& layer : layers) {
            Reply pushed = push_blob(repository, layer);
            EXPECT_EQ(pushed.status, 201U) << pushed.body;
            EXPECT_EQ(pushed.headers["docker-content-digest"], sha256(layer));
            described.emplace_back(sha256(layer), layer.size());
        }
        const json document = manifest(config, described);
        Reply published = publish(repository, tag, document);
        EXPECT_EQ(published.status, 201U) << published.body;
        return document;
    }

    Reply get_blob(const std::string& repository, const std::string& digest,
                   const std::string& method = "GET") {
        return exchange(port(), method, "/v2/" + repository + "/blobs/" + digest);
    }

    // Blob files the instance registry holds.
    std::vector<std::string> instance_blobs() const {
        std::vector<std::string> names;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(registry() / "blobs", ignored)) {
            names.push_back(entry.path().filename().string());
        }
        std::ranges::sort(names);
        return names;
    }

    std::size_t staged_uploads() const {
        std::size_t count = 0;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(registry() / "uploads", ignored)) {
            static_cast<void>(entry);
            ++count;
        }
        return count;
    }

    json copy(const std::string& mode) {
        return call("copy_workspace_records", {{"collection", "oci_blobs"}, {"mode", mode}});
    }
};

std::string hex_of(const std::string& digest) {
    return digest.substr(7);
}

} // namespace

// Push and pull of a multi-layer image through a routed, switched workspace: every blob becomes an
// object of the workspace's liquid store (only ciphertext in its Zetta), nothing reaches the
// instance registry, each layer comes back byte for byte (GET and HEAD), and usage counts the
// layers once, in the store.
TEST(RegistryStore, AMultiLayerImageLivesInItsWorkspaceStoreAndPullsByteIdentical) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "service"));
    const auto bucket_before = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));

    const std::string config = R"({"architecture":"amd64","os":"linux"})";
    const std::vector<std::string> layers = {layer_bytes(6 * 1024 * 1024 + 123, 1),
                                             layer_bytes(70 * 1024, 2), layer_bytes(1, 3)};
    const json document = rig.push_image("tenant-one/service", "v1", config, layers);

    Reply pulled_manifest =
        exchange(rig.port(), "GET", "/v2/tenant-one/service/manifests/v1");
    ASSERT_EQ(pulled_manifest.status, 200U) << pulled_manifest.body;
    EXPECT_EQ(pulled_manifest.body, document.dump());
    for (const std::string& layer : layers) {
        Reply pulled = rig.get_blob("tenant-one/service", sha256(layer));
        ASSERT_EQ(pulled.status, 200U) << pulled.body.substr(0, 200);
        EXPECT_EQ(pulled.headers["docker-content-digest"], sha256(layer));
        EXPECT_EQ(pulled.headers["content-length"], std::to_string(layer.size()));
        EXPECT_TRUE(pulled.body == layer) << "layer of " << layer.size() << " bytes differs";
        Reply headed = rig.get_blob("tenant-one/service", sha256(layer), "HEAD");
        EXPECT_EQ(headed.status, 200U);
        EXPECT_EQ(headed.headers["content-length"], std::to_string(layer.size()));
        EXPECT_TRUE(headed.body.empty());
    }
    Reply config_blob = rig.get_blob("tenant-one/service", sha256(config));
    EXPECT_EQ(config_blob.body, config);

    // The blobs are in the workspace's store, encrypted; none is in the instance registry, and no
    // staged upload is left on lake's volume.
    EXPECT_TRUE(rig.instance_blobs().empty());
    EXPECT_EQ(rig.staged_uploads(), 0U);
    const auto bucket = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    const auto total = [](const std::map<std::string, std::string>& objects) {
        std::int64_t bytes = 0;
        for (const auto& [key, value] : objects) {
            bytes += static_cast<std::int64_t>(value.size());
        }
        return bytes;
    };
    EXPECT_GT(total(bucket) - total(bucket_before), static_cast<std::int64_t>(layers[0].size()))
        << "the layers were written into the workspace's bucket";
    EXPECT_FALSE(holds_marker(bucket)) << "the workspace Zetta holds only ciphertext";
    EXPECT_EQ(rig.workspaces->records(rig.alpha).rows("oci_blobs").size(), 4U);

    // A second push of the same layer stores nothing new (liquid deduplicates it).
    const std::int64_t settled = total(rig.zetta->objects(RegistryRig::endpoint(rig.alpha)));
    EXPECT_EQ(rig.push_blob("tenant-one/service", layers[0]).status, 201U);
    EXPECT_LT(total(rig.zetta->objects(RegistryRig::endpoint(rig.alpha))) - settled, 256 * 1024)
        << "no second copy of the layer";

    // Usage: the layers count once, in the workspace store; the manifest stays an image byte.
    Reply usage = exchange(rig.port(), "GET", "/v1/metering_usage?customer_id=" + rig.one);
    const json storage = json::parse(usage.body)["value"]["summary"]["storage"];
    std::uint64_t blob_bytes = config.size();
    for (const std::string& layer : layers) {
        blob_bytes += layer.size();
    }
    EXPECT_EQ(storage["images_bytes"], document.dump().size()) << storage;
    EXPECT_EQ(storage["images_in_workspace_stores_bytes"], blob_bytes) << storage;
}

// A chunked upload (PATCH ranges, then an empty PUT) lands in the workspace store; bytes that do
// not match the digest are refused and nothing is recorded.
TEST(RegistryStore, AChunkedUploadLandsInTheStoreAndAWrongDigestIsRefused) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "chunked"));
    const std::string whole = layer_bytes(5 * 1024 * 1024 + 7, 11);
    const std::vector<std::string> pieces = {whole.substr(0, 1024 * 1024),
                                             whole.substr(1024 * 1024, 3 * 1024 * 1024),
                                             whole.substr(4 * 1024 * 1024)};
    Reply completed = rig.push_chunked("tenant-one/chunked", pieces, sha256(whole));
    ASSERT_EQ(completed.status, 201U) << completed.body;
    EXPECT_EQ(completed.headers["docker-content-digest"], sha256(whole));

    Reply wrong = rig.push_chunked("tenant-one/chunked", {"one", "two"}, sha256("onetwo!"));
    EXPECT_EQ(wrong.status, 400U) << wrong.body;
    EXPECT_EQ(oci_code(wrong), "BLOB_UPLOAD_INVALID");
    EXPECT_EQ(rig.workspaces->records(rig.alpha).rows("oci_blobs").size(), 1U)
        << "a refused upload is not recorded";

    const std::string config = R"({"chunked":true})";
    ASSERT_EQ(rig.push_blob("tenant-one/chunked", config).status, 201U);
    ASSERT_EQ(rig.publish("tenant-one/chunked", "v1",
                          RegistryRig::manifest(config, {{sha256(whole), whole.size()}}))
                  .status,
              201U);
    Reply pulled = rig.get_blob("tenant-one/chunked", sha256(whole));
    ASSERT_EQ(pulled.status, 200U);
    EXPECT_TRUE(pulled.body == whole);
    EXPECT_TRUE(rig.instance_blobs().empty());
}

// A 300 MiB layer is pushed in 16 MiB chunks and pulled back without lake holding it: the pull's
// resident set grows by far less than the layer while the client hashes it as it arrives.
TEST(RegistryStore, ALargeLayerIsStreamedNeverHeldWhole) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "large"));
    constexpr std::size_t piece = 16 * 1024 * 1024;
    constexpr std::size_t pieces = 300 / 16 + 1; // 304 MiB
    StreamHash whole;
    // Generated one piece at a time and hashed, then sent in one chunked upload.
    Reply started = exchange(rig.port(), "POST", "/v2/tenant-one/large/blobs/uploads/");
    ASSERT_EQ(started.status, 202U);
    const std::string location = started.headers.at("location");
    std::uint64_t offset = 0;
    for (std::size_t index = 0; index < pieces; ++index) {
        const std::string bytes = layer_bytes(piece, 100 + index);
        whole.update(bytes);
        Reply patched = exchange(
            rig.port(), "PATCH", location, bytes,
            {{"Content-Type", "application/octet-stream"},
             {"Content-Range", std::to_string(offset) + "-" + std::to_string(offset + bytes.size() - 1)}});
        ASSERT_EQ(patched.status, 202U) << patched.body;
        offset += bytes.size();
    }
    const std::string digest = whole.finish();
    Reply completed = exchange(rig.port(), "PUT", location + "?digest=" + digest);
    ASSERT_EQ(completed.status, 201U) << completed.body;
    const std::string config = R"({"large":true})";
    ASSERT_EQ(rig.push_blob("tenant-one/large", config).status, 201U);
    ASSERT_EQ(rig.publish("tenant-one/large", "v1", RegistryRig::manifest(config, {{digest, offset}})).status,
              201U);

    const std::uint64_t before = resident_bytes();
    std::atomic<std::uint64_t> peak{before};
    std::atomic<bool> pulling{true};
    std::thread sampler([&] {
        while (pulling.load()) {
            peak.store(std::max(peak.load(), resident_bytes()));
            std::this_thread::sleep_for(2ms);
        }
    });
    StreamHash received;
    Reply pulled = exchange(rig.port(), "GET", "/v2/tenant-one/large/blobs/" + digest, {}, {},
                                  admin, [&](std::string_view bytes) { received.update(bytes); });
    pulling.store(false);
    sampler.join();
    ASSERT_EQ(pulled.status, 200U);
    EXPECT_EQ(pulled.headers["content-length"], std::to_string(offset));
    EXPECT_EQ(pulled.body_bytes, offset);
    EXPECT_EQ(received.finish(), digest);
    const std::uint64_t growth = peak.load() > before ? peak.load() - before : 0;
    EXPECT_LT(growth, std::uint64_t{128} * 1024 * 1024)
        << "the pull grew the resident set by " << growth / (1024 * 1024) << " MiB for a "
        << offset / (1024 * 1024) << " MiB layer";
    std::cout << "[ large layer ] " << offset / (1024 * 1024) << " MiB pulled; resident growth "
              << growth / (1024 * 1024) << " MiB\n";
}

// A blob belongs to the workspace it was pushed into: another workspace of the same space, or
// another space, can neither publish it by its digest nor read it through its own repository.
TEST(RegistryStore, ABlobOfAnotherWorkspaceOrSpaceIsRefused) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    for (const std::string& workspace : {rig.alpha, rig.beta, rig.gamma}) {
        ASSERT_NO_FATAL_FAILURE(rig.switch_store(workspace));
    }
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "service"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.beta, "borrow"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.two, rig.gamma, "thief"));
    const std::string secret = layer_bytes(200 * 1024, 21);
    rig.push_image("tenant-one/service", "v1", R"({"alpha":1})", {secret});

    for (const std::string repository : {"tenant-one/borrow", "tenant-two/thief"}) {
        const std::string config = R"({"borrow":1})";
        ASSERT_EQ(rig.push_blob(repository, config).status, 201U);
        Reply published =
            rig.publish(repository, "v1", RegistryRig::manifest(config, {{sha256(secret), secret.size()}}));
        EXPECT_EQ(published.status, 400U) << repository << published.body;
        EXPECT_EQ(oci_code(published), "MANIFEST_BLOB_UNKNOWN") << published.body;
        EXPECT_EQ(rig.get_blob(repository, sha256(secret)).status, 404U) << repository;
    }
    // The other workspaces' stores never received the layer.
    for (const std::string& workspace : {rig.beta, rig.gamma}) {
        EXPECT_FALSE(holds_marker(rig.zetta->objects(RegistryRig::endpoint(workspace))));
        EXPECT_EQ(rig.workspaces->records(workspace).rows("oci_blobs").size(), 1U) << "only its config";
    }
    EXPECT_EQ(rig.get_blob("tenant-one/service", sha256(secret)).body.size(), secret.size());
}

// The one-shot copy (copy_workspace_records, collection oci_blobs). Images pushed while their
// workspaces were served from the space store are in the instance registry; after the switch they
// are still served from there until the copy. Plan writes nothing and reports what cannot move: a
// workspace not switched, a blob the registry lost, bytes that are not their digest. Copy streams
// each blob into its workspace store and names it in the record; a rerun copies nothing twice.
// Verify reads every object back and checks its SHA-256, and lists the registry files left
// behind. After that, pulls never read the instance registry.
TEST(RegistryStore, TheCopyMovesInstanceBlobsIntoTheirStoresAndVerifiesThem) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "legacy"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.beta, "damaged"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.two, rig.gamma, "waiting"));
    const std::string config = R"({"legacy":1})";
    const std::vector<std::string> layers = {layer_bytes(5 * 1024 * 1024 + 9, 31), layer_bytes(3000, 32)};
    const json legacy = rig.push_image("tenant-one/legacy", "v1", config, layers);
    const std::string lost = layer_bytes(4000, 33);
    const std::string corrupt = layer_bytes(5000, 34);
    rig.push_image("tenant-one/damaged", "v1", R"({"damaged":1})", {lost, corrupt});
    rig.push_image("tenant-two/waiting", "v1", R"({"waiting":1})", {layer_bytes(100, 35)});
    // Phase A: every blob is in the instance registry.
    EXPECT_EQ(rig.instance_blobs().size(), 8U);

    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.beta));
    // Until the copy, a switched workspace's blobs are still read from the instance registry.
    EXPECT_TRUE(rig.get_blob("tenant-one/legacy", sha256(layers[0])).body == layers[0]);
    std::filesystem::remove(rig.registry() / "blobs" / hex_of(sha256(lost)));
    {
        std::ofstream damaged(rig.registry() / "blobs" / hex_of(sha256(corrupt)), std::ios::binary | std::ios::trunc);
        damaged << "not the bytes";
    }

    const auto bucket_before = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    const json plan = rig.copy("plan");
    ASSERT_TRUE(plan.value("ok", false)) << plan;
    EXPECT_EQ(plan["value"]["rows"], 8U) << plan;
    std::map<std::string, std::string> outcomes;
    for (const json& item : plan["value"]["unresolved"]) {
        outcomes[item["id"]] = item["outcome"];
    }
    EXPECT_EQ(outcomes[rig.beta + "/" + sha256(lost)], "blob_missing") << plan;
    EXPECT_EQ(outcomes[rig.gamma + "/" + sha256(layer_bytes(100, 35))], "workspace_not_switched") << plan;
    EXPECT_EQ(rig.zetta->objects(RegistryRig::endpoint(rig.alpha)), bucket_before) << "plan writes nothing";

    const json copied = rig.copy("copy");
    ASSERT_TRUE(copied.value("ok", false)) << copied;
    EXPECT_EQ(copied["value"]["copied"], 4U) << copied; // alpha's config and two layers, beta's config
    for (const json& item : copied["value"]["unresolved"]) {
        outcomes[item["id"]] = item["outcome"];
    }
    EXPECT_EQ(outcomes[rig.beta + "/" + sha256(corrupt)], "blob_corrupt") << copied;
    const json rerun = rig.copy("copy");
    EXPECT_EQ(rerun["value"]["copied"], 0U) << rerun;
    EXPECT_EQ(rerun["value"]["already_present"], 4U) << rerun;

    const json verified = rig.copy("verify");
    ASSERT_TRUE(verified.value("ok", false)) << verified;
    EXPECT_EQ(verified["value"]["verified"], 4U) << verified;
    EXPECT_TRUE(verified["value"]["mismatched"].empty()) << verified;
    EXPECT_FALSE(verified["value"]["complete"].get<bool>()) << "unresolved rows keep it incomplete";
    std::set<std::string> paths;
    for (const json& path : verified["value"]["instance_paths"]) {
        paths.insert(path.get<std::string>());
    }
    for (const std::string& blob : {config, layers[0], layers[1]}) {
        EXPECT_TRUE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(blob))).string())) << verified;
    }
    EXPECT_FALSE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(corrupt))).string()))
        << "a blob not yet copied still needs its file";
    EXPECT_FALSE(holds_marker(rig.zetta->objects(RegistryRig::endpoint(rig.alpha))));

    // The copy removed nothing; once the registry's files are gone (a later, approved step), the
    // copied image still pulls whole, from the workspace store alone.
    EXPECT_EQ(rig.instance_blobs().size(), 7U);
    for (const std::string& path : paths) {
        std::filesystem::remove(path);
    }
    EXPECT_EQ(exchange(rig.port(), "GET", "/v2/tenant-one/legacy/manifests/v1").body, legacy.dump());
    for (const std::string& blob : {config, layers[0], layers[1]}) {
        Reply pulled = rig.get_blob("tenant-one/legacy", sha256(blob));
        EXPECT_EQ(pulled.status, 200U);
        EXPECT_TRUE(pulled.body == blob);
    }
}

// npm-, Python- and debuginfod-style instance packages and one-component OCI repositories stay
// with the instance on lake's volume, whatever the workspaces' stores hold.
TEST(RegistryStore, InstancePackagesStayInTheInstanceRegistry) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    const auto bucket_before = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    const std::string config = R"({"instance":true})";
    const std::string layer = layer_bytes(2 * 1024 * 1024, 41);
    const json document = rig.push_image("lake-fixture", "latest", config, {layer});
    EXPECT_EQ(exchange(rig.port(), "GET", "/v2/lake-fixture/manifests/latest", {}, {}, "").body,
              document.dump());
    EXPECT_TRUE(exchange(rig.port(), "GET", "/v2/lake-fixture/blobs/" + sha256(layer), {}, {}, "").body ==
                layer);
    const auto blobs = rig.instance_blobs();
    EXPECT_NE(std::ranges::find(blobs, hex_of(sha256(layer))), blobs.end());

    const std::string build_id = lake::control::identity_crypto::sha256_hex("lake-383b").substr(0, 40);
    const std::string executable = "an executable for lake#383 phase B";
    EXPECT_EQ(exchange(rig.port(), "PUT", "/_lake/debug/" + build_id + "/executable", executable,
                       {{"Content-Type", "application/octet-stream"}})
                  .status,
              201U);
    EXPECT_EQ(exchange(rig.port(), "GET", "/buildid/" + build_id + "/executable", {}, {}, "").body,
              executable);
    EXPECT_EQ(rig.zetta->objects(RegistryRig::endpoint(rig.alpha)), bucket_before)
        << "instance packages never reach a workspace store";
}

// Deleting a workspace (the lake#514 cascade) takes its image blobs with its store: its routes
// go, its store is closed and its lease released, pulls answer 404, and nothing of it is in the
// instance registry.
TEST(RegistryStore, DeletingAWorkspaceTakesItsImageBlobsWithItsStore) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.beta));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "doomed"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.beta, "kept"));
    const std::string layer = layer_bytes(300 * 1024, 51);
    rig.push_image("tenant-one/doomed", "v1", R"({"doomed":1})", {layer});
    rig.push_image("tenant-one/kept", "v1", R"({"kept":1})", {layer});
    ASSERT_EQ(rig.get_blob("tenant-one/doomed", sha256(layer)).status, 200U);
    EXPECT_FALSE(rig.zetta->lease(RegistryRig::endpoint(rig.alpha)).holder.empty());

    const json deleted = rig.call("delete_workspace", {{"customer_id", rig.one},
                                                        {"workspace_id", rig.alpha},
                                                        {"confirm", "alpha"}});
    ASSERT_TRUE(deleted.value("ok", false)) << deleted;
    EXPECT_TRUE(rig.zetta->lease(RegistryRig::endpoint(rig.alpha)).holder.empty())
        << "the deletion closed the store and released its lease";
    EXPECT_FALSE(rig.store->get_oci_repository_route(rig.one, "doomed"));
    EXPECT_EQ(rig.get_blob("tenant-one/doomed", sha256(layer)).status, 404U);
    EXPECT_EQ(exchange(rig.port(), "GET", "/v2/tenant-one/doomed/manifests/v1").status, 404U);
    EXPECT_TRUE(rig.instance_blobs().empty());
    // The other workspace's copy of the same layer is its own and stays.
    EXPECT_TRUE(rig.get_blob("tenant-one/kept", sha256(layer)).body == layer);
}
