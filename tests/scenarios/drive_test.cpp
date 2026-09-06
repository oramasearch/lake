#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <liquid/drive.hpp>
#include <liquid/identity.hpp>
#include <liquid/repository.hpp>
#include <liquid/runtime.hpp>
#include <liquid/sync.hpp>
#include <liquid/vfs.hpp>

#include "../temporary_directory.hpp"
#include "lake/api/server.hpp"
#include "lake/control/postgres_store.hpp"

extern char** environ;

namespace {

using nlohmann::json;

constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

class ScenarioRoot {
public:
    ScenarioRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("drive-scenario-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~ScenarioRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct SignedUpUser {
    std::string id;
    std::string taap_id;
    std::string secret;
};

struct ProvisionedRepository {
    SignedUpUser owner;
    lake::control::Customer customer;
    lake::control::Repository repository;
};

std::string base64url(const std::vector<std::byte>& bytes) {
    const auto* input = reinterpret_cast<const unsigned char*>(bytes.data());
    std::string encoded(((bytes.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), input, static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::string signed_identity_document(const std::filesystem::path& root, std::string_view label) {
    const std::filesystem::path home = root / ("identity-" + std::string(label));
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    if (!signer) {
        return {};
    }
    const std::filesystem::path document_path =
        home / "identity" / (signer->identity().fingerprint + ".identity.cbor");
    std::ifstream input(document_path, std::ios::binary);
    std::vector<std::byte> document;
    for (char character; input.get(character);) {
        document.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return base64url(document);
}

std::string http_request(unsigned short port,
                         std::string_view method,
                         std::string_view path,
                         std::string_view body = {},
                         std::string_view authorization = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    std::string request = std::string(method) + " " + std::string(path) +
                          " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!authorization.empty()) {
        request += "Authorization: " + std::string(authorization) + "\r\n";
    }
    request += "\r\n" + std::string(body);
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

json response_json(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return {};
    }
    return json::parse(response.substr(separator + 4), nullptr, false);
}

void write_text(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int run_process(const std::vector<std::string>& arguments) {
    std::vector<char*> raw;
    raw.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);
    pid_t process = -1;
    const int spawned =
        ::posix_spawnp(&process, raw.front(), nullptr, nullptr, raw.data(), environ);
    if (spawned != 0) {
        return spawned;
    }
    int status = 0;
    if (::waitpid(process, &status, 0) < 0 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

class DriveScenario : public testing::Test {
protected:
    void SetUp() override {
        database_uri_ = std::getenv("LAKE_TEST_DB_URI");
        if (database_uri_ == nullptr || *database_uri_ == '\0') {
            GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
        }
        store_ = lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
        const auto ready = store_->ping();
        ASSERT_TRUE(ready) << ready.error().message;
        lake::api::ServerOptions options;
        options.port = 0;
        options.node_name = "drive-scenario";
        options.admin_token = "drive-scenario-admin";
        options.instance_kek = instance_kek;
        options.store_root = (root_.path() / "lake").string();
        options.store = store_;
        server_ = std::make_unique<lake::api::ApiServer>(std::move(options));
        ASSERT_TRUE(server_->start()) << server_->reason();
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    SignedUpUser sign_up(std::string_view label) {
        const std::string email =
            std::string(label) + "-" + lake::control::new_uuid_v4() + "@example.test";
        const json input{{"email", email},
                         {"password", "correct horse battery staple"},
                         {"display_name", std::string(label)},
                         {"device_name", "drive client"},
                         {"identity_document", signed_identity_document(root_.path(), label)}};
        const std::string response =
            http_request(server_->port(), "POST", "/v1/sign_up_password", input.dump());
        EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
        const json result = response_json(response);
        EXPECT_TRUE(result.value("ok", false)) << result.dump();
        if (!result.value("ok", false)) {
            return {};
        }
        const json& value = result.at("value");
        const std::string user_id = value.at("session").at("user_id");
        const auto user = store_->get_user(user_id);
        EXPECT_TRUE(user) << user.error().message;
        if (!user) {
            return {};
        }
        return {user_id, user->taap_id, value.at("secret")};
    }

    ProvisionedRepository provision(std::string_view label) {
        SignedUpUser owner = sign_up(label);
        const std::string suffix = lake::control::new_uuid_v4();
        const auto customer =
            store_->create_customer({"individual", owner.id, "", "drive-" + suffix.substr(0, 8)});
        EXPECT_TRUE(customer) << customer.error().message;
        if (!customer) {
            return {};
        }
        const auto repository =
            store_->create_repository({customer->id, "project", "Drive scenario", "private"});
        EXPECT_TRUE(repository) << repository.error().message;
        if (!customer || !repository) {
            return {};
        }
        const auto object = store_->find_policy_object("repository", repository->id);
        EXPECT_TRUE(object) << object.error().message;
        if (object) {
            const auto grant = store_->create_grant({owner.taap_id,
                                                     object->id,
                                                     "admin",
                                                     "attestation:" + suffix,
                                                     "drive-scenario-admin",
                                                     "request:" + suffix});
            EXPECT_TRUE(grant) << grant.error().message;
        }
        return {std::move(owner), *customer, *repository};
    }

    [[nodiscard]] std::string repository_url(const lake::control::Repository& repository) const {
        return "http://127.0.0.1:" + std::to_string(server_->port()) + "/v1/repositories/" +
               repository.id;
    }

    [[nodiscard]] std::string bearer(const SignedUpUser& user) const {
        return "Bearer " + user.secret;
    }

    ScenarioRoot root_;
    const char* database_uri_{};
    std::shared_ptr<lake::control::Store> store_;
    std::unique_ptr<lake::api::ApiServer> server_;
};

TEST_F(DriveScenario, UserSyncsSharesAndSeesHistoryEndToEnd) {
    const ProvisionedRepository fixture = provision("owner");
    ASSERT_FALSE(fixture.owner.secret.empty());
    const SignedUpUser recipient = sign_up("recipient");
    ASSERT_FALSE(recipient.secret.empty());
    const std::filesystem::path folder = root_.path() / "owner-drive";
    write_text(folder / "shared-project" / "notes.txt", "shared through drive\n");

    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder,
                                   repository_url(fixture.repository),
                                   {.bearer_token = fixture.owner.secret, .durable = false}));
    ASSERT_TRUE(client) << client.error().message;
    const auto synced = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(synced) << synced.error().message;

    const json share_input{{"repository_id", fixture.repository.id},
                           {"ref", "main"},
                           {"path", "shared-project"},
                           {"principal_taap_id", recipient.taap_id},
                           {"role", "read"},
                           {"attestation_digest", "attestation:drive-recipient"}};
    const std::string shared = http_request(server_->port(),
                                            "POST",
                                            "/v1/share_drive_folder",
                                            share_input.dump(),
                                            bearer(fixture.owner));
    ASSERT_NE(shared.find(" 200 "), std::string::npos) << shared;
    ASSERT_TRUE(response_json(shared).value("ok", false)) << shared;

    const std::string browse_path =
        "/v1/drive_browse?repository_id=" + fixture.repository.id + "&ref=main&path=shared-project";
    const std::string browsed =
        http_request(server_->port(), "GET", browse_path, {}, bearer(recipient));
    ASSERT_NE(browsed.find(" 200 "), std::string::npos) << browsed;
    const json browse = response_json(browsed);
    ASSERT_TRUE(browse.value("ok", false)) << browse.dump();
    ASSERT_EQ(browse.at("value").at("items").size(), 1U);
    EXPECT_EQ(browse.at("value").at("items").at(0).at("path"), "shared-project/notes.txt");

    const std::string history_api_path =
        "/v1/drive_history?repository_id=" + fixture.repository.id +
        "&ref=main&path=shared-project/notes.txt";
    const json history_api = response_json(
        http_request(server_->port(), "GET", history_api_path, {}, bearer(recipient)));
    ASSERT_TRUE(history_api.value("ok", false)) << history_api.dump();
    ASSERT_FALSE(history_api.at("value").at("items").empty());
    const std::string change = history_api.at("value").at("items").at(0).at("id");
    const std::string history_path = "/drive/history?repository_id=" + fixture.repository.id +
                                     "&ref=main&path=shared-project/notes.txt";
    const std::string history =
        http_request(server_->port(), "GET", history_path, {}, bearer(recipient));
    EXPECT_NE(history.find(" 200 "), std::string::npos) << history;
    EXPECT_NE(history.find("shared-project/notes.txt"), std::string::npos);
    EXPECT_NE(history.find(change), std::string::npos);
}

TEST_F(DriveScenario, ConcurrentOfflineEditsKeepBothNamedVersionsInHistory) {
    const ProvisionedRepository fixture = provision("conflict-owner");
    ASSERT_FALSE(fixture.owner.secret.empty());
    const std::filesystem::path first_folder = root_.path() / "first-client";
    const std::filesystem::path second_folder = root_.path() / "second-client";
    write_text(first_folder / "shared-project" / "notes.txt", "base version\n");
    std::filesystem::create_directories(second_folder);

    auto first = liquid::runtime::block_on(
        liquid::drive_client::open(first_folder,
                                   repository_url(fixture.repository),
                                   {.bearer_token = fixture.owner.secret, .durable = false}));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(first->sync()));
    auto second = liquid::runtime::block_on(
        liquid::drive_client::open(second_folder,
                                   repository_url(fixture.repository),
                                   {.bearer_token = fixture.owner.secret, .durable = false}));
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(second->sync()));

    write_text(first_folder / "shared-project" / "notes.txt", "first client edit\n");
    write_text(second_folder / "shared-project" / "notes.txt", "second client edit\n");
    ASSERT_TRUE(liquid::runtime::block_on(first->sync()));
    const auto conflicted = liquid::runtime::block_on(second->sync());
    ASSERT_TRUE(conflicted) << conflicted.error().message;
    EXPECT_TRUE(std::ranges::any_of(conflicted->paths, [](const auto& path) {
        return path.kind == liquid::drive_reconciliation_kind::content_conflict;
    }));

    const std::string original = "shared-project/notes.txt";
    const std::string retained = "shared-project/notes (remote conflict).txt";
    EXPECT_EQ(read_text(second_folder / original), "second client edit\n");
    EXPECT_EQ(read_text(second_folder / retained), "first client edit\n");

    const std::string browse_path =
        "/v1/drive_browse?repository_id=" + fixture.repository.id + "&ref=main&path=shared-project";
    const json browse =
        response_json(http_request(server_->port(), "GET", browse_path, {}, bearer(fixture.owner)));
    ASSERT_TRUE(browse.value("ok", false)) << browse.dump();
    std::vector<std::string> paths;
    for (const json& item : browse.at("value").at("items")) {
        paths.push_back(item.at("path"));
    }
    EXPECT_NE(std::ranges::find(paths, original), paths.end());
    EXPECT_NE(std::ranges::find(paths, retained), paths.end());

    const std::string api_path =
        "/v1/drive_history?repository_id=" + fixture.repository.id + "&ref=main&path=" + original;
    const json history_api =
        response_json(http_request(server_->port(), "GET", api_path, {}, bearer(fixture.owner)));
    ASSERT_TRUE(history_api.value("ok", false)) << history_api.dump();
    const auto& items = history_api.at("value").at("items");
    const auto conflict_entry = std::ranges::find_if(items, [](const json& item) {
        return item.value("new_conflicts", json::array()).size() >= 2;
    });
    ASSERT_NE(conflict_entry, items.end()) << history_api.dump();
    const std::vector<std::string> values = conflict_entry->at("new_conflicts");

    const std::string history_path =
        "/drive/history?repository_id=" + fixture.repository.id + "&ref=main&path=" + original;
    const std::string history =
        http_request(server_->port(), "GET", history_path, {}, bearer(fixture.owner));
    EXPECT_NE(history.find(" 200 "), std::string::npos) << history;
    for (const std::string& value : values) {
        EXPECT_NE(history.find(value), std::string::npos) << history;
    }
}

TEST_F(DriveScenario, SyncedFolderPullsAsRepositoryAndBuildsProject) {
    const ProvisionedRepository fixture = provision("developer-owner");
    ASSERT_FALSE(fixture.owner.secret.empty());
    const std::filesystem::path folder = root_.path() / "developer-drive";
    write_text(folder / "project" / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "project(drive_sample LANGUAGES CXX)\n"
               "add_executable(drive_sample main.cpp)\n");
    write_text(folder / "project" / "main.cpp", "int main() { return 0; }\n");

    auto drive = liquid::runtime::block_on(
        liquid::drive_client::open(folder,
                                   repository_url(fixture.repository),
                                   {.bearer_token = fixture.owner.secret, .durable = false}));
    ASSERT_TRUE(drive) << drive.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(drive->sync()));

    auto clone = liquid::runtime::block_on(
        liquid::repository::create(root_.path() / "developer-repository"));
    ASSERT_TRUE(clone) << clone.error().message;
    liquid::sync_endpoint endpoint(
        repository_url(fixture.repository),
        liquid::sync_endpoint_options{.bearer_token = fixture.owner.secret});
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*clone));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const std::filesystem::path checkout = root_.path() / "developer-checkout";
    const auto materialised = liquid::runtime::block_on(liquid::vfs::materialise(*clone, checkout));
    ASSERT_TRUE(materialised) << materialised.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(checkout / "project" / "CMakeLists.txt"));
    EXPECT_TRUE(std::filesystem::is_regular_file(checkout / "project" / "main.cpp"));

    const std::filesystem::path source = checkout / "project";
    const std::filesystem::path build = source / "build";
    EXPECT_EQ(
        run_process(
            {LAKE_TEST_CMAKE_COMMAND, "-S", source.string(), "-B", build.string(), "-G", "Ninja"}),
        0);
    EXPECT_EQ(run_process({LAKE_TEST_CMAKE_COMMAND, "--build", build.string()}), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(build / "drive_sample"));
    EXPECT_EQ(run_process({(build / "drive_sample").string()}), 0);
}

} // namespace
