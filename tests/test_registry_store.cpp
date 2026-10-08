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
#include <optional>
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
#include "lake/control/oci_records.hpp"
#include "lake/control/workspace_store.hpp"
#include <liquid/store.hpp>
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

    void SetUp(const std::function<void(lake::api::ServerOptions&)>& configure = {}) {
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
        if (configure) {
            configure(options);
        }
        ASSERT_NO_FATAL_FAILURE(start());
    }

    void start() {
        server = std::make_unique<lake::api::ApiServer>(options);
        ASSERT_TRUE(server->start()) << server->reason();
        for (int waited = 0; waited < 200 && !server->store_root_open(); ++waited) {
            std::this_thread::sleep_for(20ms);
        }
        ASSERT_TRUE(server->store_root_open());
    }

    // A new lake process on the same volume, control store and workspace Zettas.
    void restart() {
        server->stop();
        server.reset();
        ASSERT_NO_FATAL_FAILURE(start());
    }

    // The workspace's records, opened as lake opens them (tests reach in to alter a record).
    lake::control::WorkspaceRecords records_of(const std::string& customer, const std::string& workspace) {
        auto records = lake::control::WorkspaceRecords::open(
            [service = workspaces->service, customer, workspace](std::string capability, bool read,
                                                                 json input) {
                return service->call(customer, workspace, std::move(capability), read, std::move(input));
            },
            workspace, kek);
        EXPECT_TRUE(records) << records.error().message;
        return std::move(*records);
    }

    std::optional<lake::control::OciBlob> blob_record(const std::string& customer,
                                                      const std::string& workspace,
                                                      const std::string& digest) {
        auto records = records_of(customer, workspace);
        auto found = lake::control::settled(
            liquid::runtime::block_on(lake::control::get_oci_blob(records, customer, digest)));
        EXPECT_TRUE(found);
        return found ? *found : std::nullopt;
    }

    // Rewrites a blob record to name `object`, as a damaged or wrong record would.
    void name_object(const std::string& customer, const std::string& workspace,
                     const std::string& digest, const std::string& object) {
        auto records = records_of(customer, workspace);
        auto found = lake::control::settled(
            liquid::runtime::block_on(lake::control::get_oci_blob(records, customer, digest)));
        ASSERT_TRUE(found && *found);
        lake::control::OciBlob blob = **found;
        blob.object = object;
        const std::string key = records.index_of(lake::control::OciBlobCodec::collection,
                                                 "customer:" + customer + "\ndigest:" + digest);
        auto written = lake::control::settled(liquid::runtime::block_on(records.put(
            lake::control::OciBlobCodec::collection, key, lake::control::OciBlobCodec::lookup(blob),
            lake::control::OciBlobCodec::encode(blob),
            lake::control::RecordCondition{.if_revision = blob.revision}, blob.lake_revision + 1)));
        ASSERT_TRUE(written) << written.error().message;
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

    // Liquid leases (roots) in the workspace's bucket; their object names are visible to Zetta.
    std::size_t leases(const std::string& workspace) {
        std::size_t count = 0;
        for (const auto& [key, bytes] : zetta->objects(endpoint(workspace))) {
            count += key.starts_with("leases/") && !key.contains(".lake-tail-") ? 1 : 0;
        }
        return count;
    }

    // Rewrites a blob record as one written before lake rooted image blobs.
    void unroot(const std::string& customer, const std::string& workspace, const std::string& digest) {
        auto records = records_of(customer, workspace);
        auto found = lake::control::settled(
            liquid::runtime::block_on(lake::control::get_oci_blob(records, customer, digest)));
        ASSERT_TRUE(found && *found);
        lake::control::OciBlob blob = **found;
        blob.rooted = false;
        const std::string key = records.index_of(lake::control::OciBlobCodec::collection,
                                                 "customer:" + customer + "\ndigest:" + digest);
        auto written = lake::control::settled(liquid::runtime::block_on(records.put(
            lake::control::OciBlobCodec::collection, key, lake::control::OciBlobCodec::lookup(blob),
            lake::control::OciBlobCodec::encode(blob),
            lake::control::RecordCondition{.if_revision = blob.revision}, blob.lake_revision + 1)));
        ASSERT_TRUE(written) << written.error().message;
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
        for (const char* directory : {"uploads", "claimed"}) {
            std::error_code ignored;
            for (const auto& entry : std::filesystem::directory_iterator(registry() / directory, ignored)) {
                static_cast<void>(entry);
                ++count;
            }
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
    // Another space's image, in a workspace not switched yet, shares one of alpha's layers: its
    // record still needs the registry's file after alpha's copy is verified.
    rig.push_image("tenant-two/waiting", "v1", R"({"waiting":1})", {layer_bytes(100, 35), layers[1]});
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
    EXPECT_EQ(plan["value"]["rows"], 9U) << plan;
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
    for (const std::string& blob : {config, layers[0]}) {
        EXPECT_TRUE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(blob))).string())) << verified;
    }
    EXPECT_FALSE(rig.blob_record(rig.one, rig.alpha, sha256(layers[1]))->object.empty())
        << "alpha's copy of the shared layer is in its store and verified";
    EXPECT_FALSE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(layers[1]))).string()))
        << "gamma's record of the same digest is not copied, so the registry's file is still needed";
    EXPECT_FALSE(holds_marker(rig.zetta->objects(RegistryRig::endpoint(rig.alpha))));

    // The copy removed nothing; once the registry's files are gone (a later, approved step), the
    // copied image still pulls whole, from the workspace store alone.
    EXPECT_EQ(rig.instance_blobs().size(), 7U);
    for (const std::string& path : paths) {
        std::filesystem::remove(path);
    }
    EXPECT_EQ(exchange(rig.port(), "GET", "/v2/tenant-one/legacy/manifests/v1").body, legacy.dump());
    EXPECT_EQ(rig.get_blob("tenant-two/waiting", sha256(layers[1])).body, layers[1])
        << "gamma still reads the shared layer from the registry";
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

// move_workspace_store `provision`: a workspace switched before a switch created the store of a
// workspace without repositories has none, and a switched workspace's store is never created
// implicitly. Provision creates it only when the workspace never held anything; a repository row
// or objects in the bucket are a conflict, and a repeated call changes nothing.
TEST(RegistryStore, ProvisionCreatesTheStoreOnlyOfASwitchedWorkspaceThatNeverHeldAnything) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    // Switched by a lake that did not create the store: store_moved_at set, no bucket.
    for (const std::string& workspace : {rig.alpha, rig.beta, rig.gamma}) {
        ASSERT_TRUE(rig.store->mark_workspace_store_moved(workspace));
    }
    const auto provision = [&](const std::string& workspace) {
        return rig.call("move_workspace_store", {{"workspace_id", workspace}, {"mode", "provision"}});
    };
    const json refused_create = rig.call("create_repository", {{"customer_id", rig.one},
                                                                {"name", "before"},
                                                                {"workspace_id", rig.alpha},
                                                                {"visibility", "private"}});
    EXPECT_FALSE(refused_create.value("ok", true)) << refused_create;
    EXPECT_FALSE(rig.zetta->has_bucket(RegistryRig::endpoint(rig.alpha)));

    const json provisioned = provision(rig.alpha);
    ASSERT_TRUE(provisioned.value("ok", false)) << provisioned.dump();
    EXPECT_TRUE(provisioned["value"]["created"].get<bool>());
    EXPECT_EQ(provisioned["value"]["refused_creations"].size(), 1U)
        << "the row the refused creation tombstoned held nothing";
    const auto bucket = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    EXPECT_TRUE(bucket.contains(std::string(lake::control::workspace_store_key_object)));

    // A repeated call changes nothing, in this lake and in the next one (whose store is not open
    // yet: the bucket holds objects, the store opens with its key, so nothing is created).
    const json again = provision(rig.alpha);
    ASSERT_TRUE(again.value("ok", false)) << again;
    EXPECT_FALSE(again["value"]["created"].get<bool>());
    EXPECT_EQ(rig.zetta->objects(RegistryRig::endpoint(rig.alpha)), bucket);
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const json reopened = provision(rig.alpha);
    ASSERT_TRUE(reopened.value("ok", false)) << reopened;
    EXPECT_FALSE(reopened["value"]["created"].get<bool>());
    EXPECT_GT(reopened["value"]["objects"].get<std::uint64_t>(), 0U);
    const auto reopened_bucket = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    EXPECT_EQ(reopened_bucket.at(std::string(lake::control::workspace_store_key_object)),
              bucket.at(std::string(lake::control::workspace_store_key_object)))
        << "the data key is the one first created";

    // The workspace now creates and writes repositories, and holds images.
    const json created = rig.call("create_repository", {{"customer_id", rig.one},
                                                         {"name", "after"},
                                                         {"workspace_id", rig.alpha},
                                                         {"visibility", "private"}});
    ASSERT_TRUE(created.value("ok", false)) << created;
    const std::string repository = created["value"]["id"];
    const json committed = rig.call(
        "commit_repository_files",
        {{"id", repository},
         {"revision", "main"},
         {"message", "Add a file"},
         {"operations", json::array({{{"type", "file"}, {"path", "hello.txt"}, {"content", "aGVsbG8="}}})}});
    ASSERT_TRUE(committed.value("ok", false)) << committed;
    const Reply file = exchange(rig.port(), "GET",
                                "/v1/repository_file?id=" + repository + "&ref=main&path=hello.txt");
    EXPECT_EQ(file.status, 200U) << file.body;
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "provisioned"));
    rig.push_image("tenant-one/provisioned", "v1", R"({"provisioned":1})", {layer_bytes(1000, 61)});
    // The workspace now holds data: provision refuses and changes nothing.
    const auto holding = rig.zetta->objects(RegistryRig::endpoint(rig.alpha));
    EXPECT_EQ(provision(rig.alpha)["error"]["code"], "conflict");
    EXPECT_EQ(rig.zetta->objects(RegistryRig::endpoint(rig.alpha)), holding);

    // Any other repository row, live or tombstoned inside its retention, refuses it.
    const auto held = rig.store->create_repository({rig.one, "held", "", "private", rig.beta});
    ASSERT_TRUE(held);
    json with_repository = provision(rig.beta);
    EXPECT_EQ(with_repository["error"]["code"], "conflict") << with_repository;
    EXPECT_NE(with_repository["error"]["message"].get<std::string>().find("held, live"),
              std::string::npos);
    ASSERT_TRUE(rig.store->delete_repository(
        held->id, lake::control::RepositoryDeletion{.deleted_at = "2026-10-01T00:00:00Z",
                                                    .retention_until = "2026-11-01T00:00:00Z"}));
    with_repository = provision(rig.beta);
    EXPECT_EQ(with_repository["error"]["code"], "conflict") << with_repository;
    EXPECT_NE(with_repository["error"]["message"].get<std::string>().find("held, tombstoned"),
              std::string::npos);
    EXPECT_FALSE(rig.zetta->objects(RegistryRig::endpoint(rig.beta))
                     .contains(std::string(lake::control::workspace_store_key_object)));

    // A bucket that holds objects (a wiped key, a store that held data) refuses it.
    rig.zetta->set_object(RegistryRig::endpoint(rig.gamma), "packs/pack-1.pack", "left over");
    const json with_objects = provision(rig.gamma);
    EXPECT_EQ(with_objects["error"]["code"], "conflict") << with_objects;
    EXPECT_NE(with_objects["error"]["message"].get<std::string>().find("1 objects"), std::string::npos);
    EXPECT_EQ(rig.zetta->objects(RegistryRig::endpoint(rig.gamma)).size(), 1U) << "nothing was written";

    // A workspace whose records name an image blob in its store held data: refused, though it
    // has no repository and its bucket is gone.
    const std::string delta = rig.workspaces->create(rig.two, "delta");
    rig.zetta->add_node(RegistryRig::endpoint(delta), "token-" + delta);
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(delta));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.two, delta, "kept"));
    const std::string kept = layer_bytes(2000, 62);
    rig.push_image("tenant-two/kept", "v1", R"({"kept":1})", {kept});
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    rig.zetta->drop_bucket(RegistryRig::endpoint(delta));
    const json with_blobs = provision(delta);
    EXPECT_EQ(with_blobs["error"]["code"], "conflict") << with_blobs;
    EXPECT_NE(with_blobs["error"]["message"].get<std::string>().find("2 image blobs"),
              std::string::npos) << with_blobs;
    EXPECT_FALSE(rig.zetta->has_bucket(RegistryRig::endpoint(delta))) << "nothing was created";

    // Only the operator provisions: a person's token is not the operator credential.
    const auto user = rig.store->create_user({"taap:registry-store-person", "", "Person"});
    ASSERT_TRUE(user);
    const auto token = rig.store->create_token({user->id, "lake", "person", "test", {"*"}, 300, false});
    ASSERT_TRUE(token) << token.error().message;
    Reply foreign = exchange(rig.port(), "POST", "/v1/move_workspace_store",
                             json{{"workspace_id", rig.alpha}, {"mode", "provision"}}.dump(),
                             {{"Content-Type", "application/json"}}, "Bearer " + token->secret);
    EXPECT_EQ(foreign.status, 403U) << foreign.body;
    EXPECT_EQ(json::parse(foreign.body, nullptr, false)["error"]["code"], "unauthorized") << foreign.body;
}

// ---- review of PR #528 ------------------------------------------------------------------------

// A PATCH to an upload while its completion is checking and storing it cannot reach the bytes:
// the completion moved them away first, so the PATCH is refused and the stored object is exactly
// the bytes whose digest was checked. (Before, the PATCH appended to the file between the digest
// check and the store's read of it, and the record mapped the digest to other bytes for good.)
TEST(RegistryStore, APatchDuringCompletionCannotChangeTheStoredBytes) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "raced"));
    const std::string bytes = layer_bytes(3 * 1024 * 1024, 71);
    Reply started = exchange(rig.port(), "POST", "/v2/tenant-one/raced/blobs/uploads/");
    ASSERT_EQ(started.status, 202U);
    const std::string location = started.headers.at("location");
    Reply patched = exchange(rig.port(), "PATCH", location, bytes,
                             {{"Content-Type", "application/octet-stream"},
                              {"Content-Range", "0-" + std::to_string(bytes.size() - 1)}});
    ASSERT_EQ(patched.status, 202U);
    // Every request to the workspace's Zetta is slow, so the completion is still storing when the
    // second PATCH arrives.
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 150ms);
    Reply completed;
    std::thread completion([&] {
        completed = exchange(rig.port(), "PUT", location + "?digest=" + sha256(bytes));
    });
    std::this_thread::sleep_for(400ms);
    const std::string extra = "appended while completing";
    Reply raced = exchange(rig.port(), "PATCH", location, extra,
                           {{"Content-Type", "application/octet-stream"},
                            {"Content-Range", std::to_string(bytes.size()) + "-" +
                                                  std::to_string(bytes.size() + extra.size() - 1)}});
    completion.join();
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 0ms);
    EXPECT_EQ(raced.status, 400U) << "the completing upload is no longer reachable: " << raced.body;
    EXPECT_EQ(oci_code(raced), "BLOB_UPLOAD_INVALID");
    ASSERT_EQ(completed.status, 201U) << completed.body;
    EXPECT_EQ(rig.blob_record(rig.one, rig.alpha, sha256(bytes))->size, bytes.size());
    const std::string config = R"({"raced":1})";
    ASSERT_EQ(rig.push_blob("tenant-one/raced", config).status, 201U);
    ASSERT_EQ(rig.publish("tenant-one/raced", "v1", RegistryRig::manifest(config, {{sha256(bytes), bytes.size()}}))
                  .status,
              201U);
    EXPECT_TRUE(rig.get_blob("tenant-one/raced", sha256(bytes)).body == bytes);
    EXPECT_EQ(rig.staged_uploads(), 0U);
}

// A record that names an object the store does not hold answers 500 to GET and HEAD, never 200
// and never the instance registry; one that names another object's bytes is repaired by a
// correct push of the blob, as is the missing one.
TEST(RegistryStore, ARecordNamingAMissingOrWrongObjectIsAnErrorUntilARePushRepairsIt) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "damaged"));
    const std::string first = layer_bytes(5000, 81);
    const std::string second = layer_bytes(6000, 82);
    rig.push_image("tenant-one/damaged", "v1", R"({"damaged":1})", {first, second});

    rig.name_object(rig.one, rig.alpha, sha256(first), "blake3:" + std::string(64, '0'));
    EXPECT_EQ(rig.get_blob("tenant-one/damaged", sha256(first)).status, 500U);
    Reply headed = rig.get_blob("tenant-one/damaged", sha256(first), "HEAD");
    EXPECT_EQ(headed.status, 500U);
    EXPECT_EQ(rig.push_blob("tenant-one/damaged", first).status, 201U);
    EXPECT_TRUE(rig.get_blob("tenant-one/damaged", sha256(first)).body == first) << "a re-push repairs it";
    EXPECT_EQ(rig.get_blob("tenant-one/damaged", sha256(first), "HEAD").status, 200U);

    // A record naming the other layer's object serves the wrong bytes until a push corrects it.
    const std::string other = rig.blob_record(rig.one, rig.alpha, sha256(second))->object;
    rig.name_object(rig.one, rig.alpha, sha256(first), other);
    EXPECT_TRUE(rig.get_blob("tenant-one/damaged", sha256(first)).body == second.substr(0, first.size()));
    EXPECT_EQ(rig.push_blob("tenant-one/damaged", first).status, 201U);
    EXPECT_NE(rig.blob_record(rig.one, rig.alpha, sha256(first))->object, other);
    const auto superseded = rig.blob_record(rig.one, rig.alpha, sha256(first))->superseded;
    EXPECT_NE(std::ranges::find(superseded, other), superseded.end())
        << "the root of the object it named before is recorded for later";
    EXPECT_TRUE(rig.get_blob("tenant-one/damaged", sha256(first)).body == first);
}

namespace {

// A pull that reads its status line and headers, then stops reading until it is closed.
struct PausedPull {
    int socket = -1;
    unsigned status = 0;
    std::string head;
    ~PausedPull() {
        if (socket >= 0) {
            ::close(socket);
        }
    }
};

std::unique_ptr<PausedPull> start_pull(unsigned short port, const std::string& path) {
    auto pull = std::make_unique<PausedPull>();
    pull->socket = ::socket(AF_INET, SOCK_STREAM, 0);
    int small = 4096;
    ::setsockopt(pull->socket, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(pull->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return pull;
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: " +
                                std::string(admin) + "\r\n\r\n";
    ::send(pull->socket, request.data(), request.size(), 0);
    std::string head;
    char byte = 0;
    while (head.find("\r\n\r\n") == std::string::npos && ::recv(pull->socket, &byte, 1, 0) == 1) {
        head += byte;
    }
    if (head.size() > 12) {
        pull->status = static_cast<unsigned>(std::stoul(head.substr(9, 3)));
    }
    pull->head = head;
    if (pull->status != 200) {
        ::close(pull->socket); // a refused pull holds no connection
        pull->socket = -1;
    }
    return pull;
}

} // namespace

// A pull holds one of its workspace lane's admissions for its whole stream, so pulls that stall
// (a slow client, a long put holding the lane) never park more of the blocking pool than the lane
// admits: past that, pulls are refused at once with 503, and other workspaces keep answering.
// (Before, only a pull's first window was admitted, and its later windows waited for the lane
// without limit.)
TEST(RegistryStore, PullsCountAgainstTheLaneForTheirWholeStreamAndOtherWorkspacesAnswer) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.beta));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "busy"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.beta, "calm"));
    const std::string layer = layer_bytes(24 * 1024 * 1024, 91);
    rig.push_image("tenant-one/busy", "v1", R"({"busy":1})", {layer});
    const std::string calm = layer_bytes(4096, 92);
    rig.push_image("tenant-one/calm", "v1", R"({"calm":1})", {calm});

    // Forty pulls that stop reading after their headers.
    std::vector<std::unique_ptr<PausedPull>> pulls;
    for (int index = 0; index < 40; ++index) {
        pulls.push_back(start_pull(rig.port(), "/v2/tenant-one/busy/blobs/" + sha256(layer)));
    }
    const auto admitted = std::ranges::count_if(pulls, [](const auto& pull) { return pull->status == 200; });
    const auto refused = std::ranges::count_if(pulls, [](const auto& pull) { return pull->status == 503; });
    EXPECT_GE(admitted, 1);
    EXPECT_LE(admitted, 8) << "a stalled pull keeps its stream slot";
    EXPECT_EQ(admitted + refused, 40);
    const auto calm_started = std::chrono::steady_clock::now();
    EXPECT_TRUE(rig.get_blob("tenant-one/calm", sha256(calm)).body == calm);
    EXPECT_LT(std::chrono::steady_clock::now() - calm_started, 3s);
    pulls.clear(); // the clients go away; their streams end and give their admissions back

    // A long put holds alpha's lane while forty pulls arrive: they are refused or wait their
    // turn, and beta answers meanwhile.
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 100ms);
    Reply put;
    std::thread putter([&] { put = rig.push_blob("tenant-one/busy", layer_bytes(2 * 1024 * 1024, 93)); });
    std::this_thread::sleep_for(300ms);
    std::vector<std::thread> pullers;
    std::atomic<int> unavailable{0};
    for (int index = 0; index < 40; ++index) {
        pullers.emplace_back([&] {
            if (rig.get_blob("tenant-one/busy", sha256(layer), "HEAD").status == 503) {
                ++unavailable;
            }
        });
    }
    const auto beta_started = std::chrono::steady_clock::now();
    EXPECT_TRUE(rig.get_blob("tenant-one/calm", sha256(calm)).body == calm);
    EXPECT_LT(std::chrono::steady_clock::now() - beta_started, 3s) << "another workspace still answers";
    for (std::thread& puller : pullers) {
        puller.join();
    }
    putter.join();
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 0ms);
    EXPECT_EQ(put.status, 201U) << put.body;
    EXPECT_GE(unavailable.load(), 32);
    // Afterwards the lane admits pulls again.
    Reply after;
    for (int attempt = 0; attempt < 50 && after.status != 200; ++attempt) {
        after = rig.get_blob("tenant-one/busy", sha256(layer), "HEAD");
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_EQ(after.status, 200U);
}

// `instance_paths` is advisory and only given when nothing could have been missed: a digest the
// instance's own repositories use is never in it, a routed workspace whose records cannot be read
// withholds the list, and so does a tenant tag still in the instance registry, whose blobs it
// still needs.
TEST(RegistryStore, InstancePathsLeaveOutWhatIsStillNeededAndAreWithheldWhenIncomplete) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "service"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.two, rig.gamma, "other"));
    const std::string shared = layer_bytes(3000, 101);
    const std::string own = layer_bytes(3000, 102);
    const std::string config = R"({"service":1})";
    rig.push_image("tenant-one/service", "v1", config, {shared, own});
    rig.push_image("tenant-two/other", "v1", R"({"other":1})", {layer_bytes(500, 103)});
    // The instance's own repository uses the shared layer.
    rig.push_image("instance-tool", "v1", R"({"tool":1})", {shared});
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.gamma));
    ASSERT_TRUE(rig.copy("copy").value("ok", false));
    json verified = rig.copy("verify");
    ASSERT_TRUE(verified.value("ok", false)) << verified;
    ASSERT_TRUE(verified["value"]["instance_paths"].is_array()) << verified;
    std::set<std::string> paths;
    for (const json& path : verified["value"]["instance_paths"]) {
        paths.insert(path.get<std::string>());
    }
    EXPECT_TRUE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(own))).string())) << verified;
    EXPECT_FALSE(paths.contains((rig.registry() / "blobs" / hex_of(sha256(shared))).string()))
        << "the instance's repository uses it";

    // A routed workspace that cannot be read: no list.
    rig.workspaces->status(rig.gamma, "Pending");
    verified = rig.copy("verify");
    ASSERT_TRUE(verified.value("ok", false)) << verified;
    EXPECT_TRUE(verified["value"]["instance_paths"].is_null()) << verified;
    EXPECT_FALSE(verified["value"].value("instance_paths_withheld", std::string()).empty());
    rig.workspaces->ready(rig.gamma);

    // A legacy tag of a space that no longer exists never withholds the list.
    std::filesystem::create_directories(rig.registry() / "manifests" / "gone-space" / "img");
    std::ofstream(rig.registry() / "manifests" / "gone-space" / "img" / "v1.json")
        << json{{"digest", sha256(own)}}.dump();
    verified = rig.copy("verify");
    ASSERT_TRUE(verified.value("ok", false)) << verified;
    ASSERT_TRUE(verified["value"]["instance_paths"].is_array()) << verified;
    std::filesystem::remove_all(rig.registry() / "manifests" / "gone-space");

    // A tenant tag the instance registry still holds and its workspace does not (oci_tags not
    // complete): no list.
    const std::string legacy_manifest = R"({"schemaVersion":2,"config":{"digest":")" + sha256(config) +
                                        R"(","size":13},"layers":[{"digest":")" + sha256(own) +
                                        R"(","size":3000}]})";
    std::filesystem::create_directories(rig.registry() / "manifests" / "tenant-one" / "service");
    std::ofstream(rig.registry() / "blobs" / hex_of(sha256(legacy_manifest)), std::ios::binary) << legacy_manifest;
    std::ofstream(rig.registry() / "manifests" / "tenant-one" / "service" / "legacy.json")
        << json{{"digest", sha256(legacy_manifest)}}.dump();
    verified = rig.copy("verify");
    ASSERT_TRUE(verified.value("ok", false)) << verified;
    EXPECT_TRUE(verified["value"]["instance_paths"].is_null()) << verified;
}

// Quota: a new blob past the space's storage quota is 507 and leaves no staged file; a re-push of
// a blob the store already holds adds nothing and is not refused.
TEST(RegistryStore, TheQuotaCountsOnlyNewBytesAndARefusedUploadLeavesNothingStaged) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "quota"));
    const std::string held = layer_bytes(10000, 111);
    ASSERT_EQ(rig.push_blob("tenant-one/quota", held).status, 201U);
    ASSERT_TRUE(rig.store->set_storage_quota(rig.one, 1));
    Reply again = rig.push_blob("tenant-one/quota", held);
    EXPECT_EQ(again.status, 201U) << again.body;
    Reply refused = rig.push_blob("tenant-one/quota", layer_bytes(10000, 112));
    EXPECT_EQ(refused.status, 507U) << refused.body;
    EXPECT_EQ(oci_code(refused), "DENIED");
    EXPECT_EQ(rig.staged_uploads(), 0U);
}

// Uploads abandoned longer than the maximum age are removed by the maintenance thread.
TEST(RegistryStore, AbandonedUploadsAreSweptAfterTheirMaximumAge) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp([](lake::api::ServerOptions& options) {
        options.registry_upload_max_age = std::chrono::seconds(1);
    }));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "abandoned"));
    Reply started = exchange(rig.port(), "POST", "/v2/tenant-one/abandoned/blobs/uploads/");
    ASSERT_EQ(started.status, 202U);
    ASSERT_EQ(exchange(rig.port(), "PATCH", started.headers.at("location"), "never finished",
                       {{"Content-Type", "application/octet-stream"}, {"Content-Range", "0-13"}})
                  .status,
              202U);
    EXPECT_EQ(rig.staged_uploads(), 1U);
    // The owner loop (drain, which lake's main loop calls) schedules the sweep.
    for (int waited = 0; waited < 100 && rig.staged_uploads() != 0; ++waited) {
        rig.server->drain();
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_EQ(rig.staged_uploads(), 0U);
}

// Usage keys stored blobs per workspace: a digest one workspace stores and another still reads
// from the instance registry counts in both places.
TEST(RegistryStore, UsageCountsAStoredDigestPerWorkspace) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "stored"));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.beta, "registry"));
    const std::string layer = layer_bytes(7000, 121);
    const json in_store = rig.push_image("tenant-one/stored", "v1", R"({"s":1})", {layer});
    const json in_registry = rig.push_image("tenant-one/registry", "v1", R"({"r":1})", {layer});
    Reply usage = exchange(rig.port(), "GET", "/v1/metering_usage?customer_id=" + rig.one);
    const json storage = json::parse(usage.body)["value"]["summary"]["storage"];
    EXPECT_EQ(storage["images_in_workspace_stores_bytes"], layer.size() + 7) << storage;
    EXPECT_EQ(storage["images_bytes"], in_store.dump().size() + in_registry.dump().size() +
                                           layer.size() + 7) << storage;
}

// Image blobs are rooted in liquid (a lease), so a liquid collection of the workspace's store,
// which keeps only what a root reaches, does not remove them.
TEST(RegistryStore, ALiquidCollectionOfTheWorkspaceStoreKeepsImageBlobs) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "rooted"));
    const std::string layer = layer_bytes(300 * 1024, 131);
    rig.push_image("tenant-one/rooted", "v1", R"({"rooted":1})", {layer});
    rig.server->stop();
    rig.server.reset();
    {
        const std::string endpoint = RegistryRig::endpoint(rig.alpha);
        auto client = std::make_shared<lake::control::ZettaObjectClient>(
            rig.zetta, lake::control::ObjectEndpoint{endpoint, "token-" + rig.alpha});
        liquid::store_options options;
        options.backend = std::make_shared<lake::control::ZettaObjectBackend>(
            client, lake::control::ZettaObjectBackendOptions{.workspace_id = rig.alpha,
                                                              .instance_kek = std::string(kek)});
        options.owner = std::make_shared<lake::control::ZettaStoreOwner>(client, 30s);
        options.owner_wait = 5s;
        auto store = liquid::runtime::block_on(
            liquid::store::open(rig.root / "workspaces" / rig.alpha, std::move(options)));
        ASSERT_TRUE(store) << store.error().message;
        auto collected = liquid::runtime::block_on(store->collect());
        ASSERT_TRUE(collected) << collected.error().message;
        EXPECT_GE(collected->roots, 2U) << "each image blob is a root";
        ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    }
    ASSERT_NO_FATAL_FAILURE(rig.start());
    EXPECT_TRUE(rig.get_blob("tenant-one/rooted", sha256(layer)).body == layer);
}

// ---- second review of PR #528 -----------------------------------------------------------------

// Pulls have their own eight stream slots per workspace, apart from the admissions of other
// requests: with eight pulls of one workspace in flight a push to it still succeeds, a ninth pull
// is refused at once with `Connection: close`, and a stalled reader is ended by the idle timeout,
// freeing its slot.
TEST(RegistryStore, PullsHaveTheirOwnSlotsAndAStalledReaderTimesOut) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp([](lake::api::ServerOptions& options) {
        options.registry_stream_idle_timeout = std::chrono::milliseconds(1500);
    }));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "pulled"));
    const std::string layer = layer_bytes(24 * 1024 * 1024, 141);
    rig.push_image("tenant-one/pulled", "v1", R"({"pulled":1})", {layer});
    const std::string path = "/v2/tenant-one/pulled/blobs/" + sha256(layer);

    std::vector<std::unique_ptr<PausedPull>> pulls;
    for (int index = 0; index < 8; ++index) {
        pulls.push_back(start_pull(rig.port(), path));
        ASSERT_EQ(pulls.back()->status, 200U) << index;
    }
    // A push into the same workspace is not refused by the pulls.
    Reply pushed = rig.push_blob("tenant-one/pulled", layer_bytes(4096, 142));
    EXPECT_EQ(pushed.status, 201U) << pushed.body;
    // A ninth pull is refused, and its connection closed.
    auto ninth = start_pull(rig.port(), path);
    EXPECT_EQ(ninth->status, 503U);
    std::string head = ninth->head;
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return std::tolower(c); });
    EXPECT_NE(head.find("connection: close"), std::string::npos) << ninth->head;

    // The stalled readers are ended by the idle timeout; their slots are free again.
    Reply after;
    for (int attempt = 0; attempt < 100 && after.status != 200; ++attempt) {
        std::this_thread::sleep_for(100ms);
        after = rig.get_blob("tenant-one/pulled", sha256(layer), "HEAD");
    }
    EXPECT_EQ(after.status, 200U) << "the idle timeout released the stalled pulls";
    auto fresh = start_pull(rig.port(), path);
    EXPECT_EQ(fresh->status, 200U);
}

// lake#534: a streaming pull gives its request worker back, so four stalled pulls on a lake with
// two workers neither wait for a worker nor keep the next request from one.
TEST(RegistryStore, AStreamingPullGivesItsWorkerBack) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp([](lake::api::ServerOptions& options) {
        options.worker_coroutines = 2;
        options.worker_wait = std::chrono::milliseconds(300);
    }));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "stalled"));
    const std::string layer = layer_bytes(8 * 1024 * 1024, 151);
    rig.push_image("tenant-one/stalled", "v1", R"({"stalled":1})", {layer});
    const std::string path = "/v2/tenant-one/stalled/blobs/" + sha256(layer);
    std::vector<std::unique_ptr<PausedPull>> pulls;
    for (int index = 0; index < 4; ++index) {
        pulls.push_back(start_pull(rig.port(), path));
        ASSERT_EQ(pulls.back()->status, 200U) << index;
    }
    const Reply head = rig.get_blob("tenant-one/stalled", sha256(layer), "HEAD");
    EXPECT_EQ(head.status, 200U) << head.body;
}

// At most 24 pulls run in the process, fewer than the server's workers, and once they do a
// pull of another workspace is refused at once while that workspace's other requests answer.
TEST(RegistryStore, WithTheGlobalPullCapReachedOtherWorkspacesStillAnswer) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string delta = rig.workspaces->create(rig.two, "delta");
    rig.zetta->add_node(RegistryRig::endpoint(delta), "token-" + delta);
    const std::vector<std::pair<std::string, std::string>> spaces = {
        {rig.one, rig.alpha}, {rig.one, rig.beta}, {rig.two, rig.gamma}, {rig.two, delta}};
    const std::string layer = layer_bytes(16 * 1024 * 1024, 151);
    std::vector<std::string> repositories;
    for (std::size_t index = 0; index < spaces.size(); ++index) {
        const auto& [space, workspace] = spaces[index];
        ASSERT_NO_FATAL_FAILURE(rig.switch_store(workspace));
        const std::string name = "image" + std::to_string(index);
        ASSERT_NO_FATAL_FAILURE(rig.route(space, workspace, name));
        const std::string repository =
            std::string(space == rig.one ? "tenant-one/" : "tenant-two/") + name;
        rig.push_image(repository, "v1", R"({"i":)" + std::to_string(index) + "}", {layer});
        repositories.push_back(repository);
    }
    std::vector<std::unique_ptr<PausedPull>> pulls;
    for (std::size_t index = 0; index < 3; ++index) {
        for (int pull = 0; pull < 8; ++pull) {
            pulls.push_back(start_pull(rig.port(), "/v2/" + repositories[index] + "/blobs/" + sha256(layer)));
            ASSERT_EQ(pulls.back()->status, 200U) << index << " " << pull;
        }
    }
    auto refused = start_pull(rig.port(), "/v2/" + repositories[3] + "/blobs/" + sha256(layer));
    EXPECT_EQ(refused->status, 503U) << "the process runs at most 24 pulls";
    const auto started = std::chrono::steady_clock::now();
    Reply manifest = exchange(rig.port(), "GET", "/v2/" + repositories[3] + "/manifests/v1");
    EXPECT_EQ(manifest.status, 200U) << manifest.body;
    Reply pushed = rig.push_blob(repositories[3], layer_bytes(4096, 152));
    EXPECT_EQ(pushed.status, 201U) << pushed.body;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
}

// A root is created once per object a record names: re-pushing a blob adds no lease, and the copy
// roots an object already in place that was stored before lake rooted image blobs, once.
TEST(RegistryStore, ImageBlobRootsAreCreatedOnceAndTheCopyRootsOlderObjects) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "roots"));
    const std::string layer = layer_bytes(5000, 161);
    ASSERT_EQ(rig.push_blob("tenant-one/roots", layer).status, 201U);
    EXPECT_TRUE(rig.blob_record(rig.one, rig.alpha, sha256(layer))->rooted);
    const std::size_t rooted = rig.leases(rig.alpha);
    for (int again = 0; again < 3; ++again) {
        ASSERT_EQ(rig.push_blob("tenant-one/roots", layer).status, 201U);
    }
    EXPECT_EQ(rig.leases(rig.alpha), rooted) << "a re-push adds no lease";

    ASSERT_NO_FATAL_FAILURE(rig.unroot(rig.one, rig.alpha, sha256(layer)));
    json copied = rig.copy("copy");
    ASSERT_TRUE(copied.value("ok", false)) << copied;
    EXPECT_EQ(copied["value"]["rooted"], 1U) << copied;
    EXPECT_TRUE(rig.blob_record(rig.one, rig.alpha, sha256(layer))->rooted);
    EXPECT_EQ(rig.leases(rig.alpha), rooted + 1);
    copied = rig.copy("copy");
    EXPECT_EQ(copied["value"]["rooted"], 0U) << copied;
    EXPECT_EQ(rig.leases(rig.alpha), rooted + 1);
}

// After a store error a client may retry, so the claimed bytes go back under the upload id and a
// retry of the same completion succeeds. A completion's claim also restarts the upload's age, so
// the sweep never removes the bytes a slow completion is storing.
TEST(RegistryStore, AClaimedUploadSurvivesARetryableErrorAndTheSweepDuringItsCompletion) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp([](lake::api::ServerOptions& options) {
        options.registry_upload_max_age = std::chrono::seconds(30);
    }));
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "retried"));
    const std::string bytes = layer_bytes(2 * 1024 * 1024, 171);
    Reply started = exchange(rig.port(), "POST", "/v2/tenant-one/retried/blobs/uploads/");
    ASSERT_EQ(started.status, 202U);
    const std::string location = started.headers.at("location");
    ASSERT_EQ(exchange(rig.port(), "PATCH", location, bytes,
                       {{"Content-Type", "application/octet-stream"},
                        {"Content-Range", "0-" + std::to_string(bytes.size() - 1)}})
                  .status,
              202U);
    // The workspace's Zetta stops answering for the completion: a store error, retried later.
    rig.zetta->drop_next(1000);
    Reply failed = exchange(rig.port(), "PUT", location + "?digest=" + sha256(bytes));
    rig.zetta->drop_next(0);
    EXPECT_NE(failed.status, 201U);
    EXPECT_NE(failed.status, 400U) << failed.body;
    EXPECT_EQ(rig.staged_uploads(), 1U) << "the bytes are back under the upload id";
    Reply retried;
    for (int attempt = 0; attempt < 20 && retried.status != 201; ++attempt) {
        retried = exchange(rig.port(), "PUT", location + "?digest=" + sha256(bytes));
        if (retried.status != 201) {
            std::this_thread::sleep_for(200ms);
        }
    }
    EXPECT_EQ(retried.status, 201U) << retried.body;
    EXPECT_TRUE(rig.blob_record(rig.one, rig.alpha, sha256(bytes)).has_value());

    // An upload last written an hour ago is claimed by a slow completion; the sweep runs meanwhile.
    started = exchange(rig.port(), "POST", "/v2/tenant-one/retried/blobs/uploads/");
    const std::string second = started.headers.at("location");
    const std::string other = layer_bytes(2 * 1024 * 1024, 172);
    ASSERT_EQ(exchange(rig.port(), "PATCH", second, other,
                       {{"Content-Type", "application/octet-stream"},
                        {"Content-Range", "0-" + std::to_string(other.size() - 1)}})
                  .status,
              202U);
    const std::string id = second.substr(second.rfind('/') + 1);
    std::filesystem::last_write_time(rig.registry() / "uploads" / id,
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 100ms);
    Reply completed;
    std::thread completion([&] { completed = exchange(rig.port(), "PUT", second + "?digest=" + sha256(other)); });
    std::size_t claimed = 0;
    for (int waited = 0; waited < 20 && claimed == 0; ++waited) {
        std::this_thread::sleep_for(50ms);
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(rig.registry() / "claimed", ignored)) {
            static_cast<void>(entry);
            ++claimed;
        }
    }
    ASSERT_EQ(claimed, 1U);
    rig.server->drain(); // schedules the sweep
    std::this_thread::sleep_for(300ms);
    std::size_t still = 0;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(rig.registry() / "claimed", ignored)) {
        static_cast<void>(entry);
        ++still;
    }
    completion.join();
    rig.zetta->hang(RegistryRig::endpoint(rig.alpha), 0ms);
    EXPECT_EQ(still, 1U) << "the sweep measured the claimed upload from its claim";
    EXPECT_EQ(completed.status, 201U) << completed.body;
}

// lake#536: once lake drains, image blob work for a workspace store is refused and the released
// lease is never taken again. A pull (GET) and a HEAD are refused at the stream admission
// (admit_workspace_stream) with 503 and Retry-After; the operator's copy, which roots an unrooted
// blob through root_workspace_blob (a lane hold without admission), is refused by that path's own
// drain check.
TEST(RegistryStore, ADrainingLakeRefusesBlobWorkAndNeverRetakesTheLease) {
    RegistryRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_NO_FATAL_FAILURE(rig.switch_store(rig.alpha));
    ASSERT_NO_FATAL_FAILURE(rig.route(rig.one, rig.alpha, "drained"));
    const std::string layer = layer_bytes(70 * 1024, 7);
    rig.push_image("tenant-one/drained", "v1", R"({"architecture":"amd64","os":"linux"})", {layer});
    ASSERT_EQ(rig.get_blob("tenant-one/drained", sha256(layer), "HEAD").status, 200U);
    ASSERT_NO_FATAL_FAILURE(rig.unroot(rig.one, rig.alpha, sha256(layer)));
    const std::string endpoint = RegistryRig::endpoint(rig.alpha);
    ASSERT_FALSE(rig.zetta->lease(endpoint).holder.empty());

    rig.server->begin_drain();
    for (int waited = 0; waited < 300 && !rig.zetta->lease(endpoint).holder.empty(); ++waited) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(rig.zetta->lease(endpoint).holder.empty()) << "released at the drain";
    const auto fence = rig.zetta->lease(endpoint).fence;
    for (const char* method : {"HEAD", "GET"}) {
        Reply refused = rig.get_blob("tenant-one/drained", sha256(layer), method);
        EXPECT_EQ(refused.status, 503U) << method << " " << refused.body;
        EXPECT_EQ(refused.headers["retry-after"], "1") << method;
    }
    const json copied = rig.copy("copy");
    EXPECT_NE(copied.dump().find("lake is stopping"), std::string::npos) << copied.dump();
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(rig.zetta->lease(endpoint).holder.empty());
    EXPECT_EQ(rig.zetta->lease(endpoint).fence, fence) << "nothing took the lease again";
}
