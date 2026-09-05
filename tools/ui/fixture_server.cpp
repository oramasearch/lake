#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include <openssl/hmac.h>

#include <liquid/liquid.hpp>

#include "lake/api/server.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/search/index.hpp"

namespace {

std::atomic<bool> stop_requested{false};

void on_signal(int) {
    stop_requested.store(true);
}

std::vector<std::byte> bytes(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

template <typename T, typename E>
T require(std::expected<T, E> result, std::string_view operation) {
    if (!result) {
        std::println(stderr, "lake-ui-fixture: {}: {}", operation, result.error().message);
        std::exit(1);
    }
    return std::move(*result);
}

void require(liquid::result<void> result, std::string_view operation) {
    if (!result) {
        std::println(stderr, "lake-ui-fixture: {}: {}", operation, result.error().message);
        std::exit(1);
    }
}

liquid::platform host_platform() {
#if defined(__APPLE__)
    return {"macos", "arm64"};
#elif defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

std::string node_id_text(const liquid::node_id& node) {
    std::string output;
    output.reserve(node.size() * 2);
    for (const std::byte value : node) {
        output += std::format("{:02x}", std::to_integer<unsigned int>(value));
    }
    return output;
}

std::string share_token_hash(std::string_view token) {
    static constexpr std::string_view key = "lake-drive-share-link/1";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(token.data()),
         token.size(),
         digest,
         &size);
    std::string output;
    output.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        output += std::format("{:02x}", digest[index]);
    }
    return output;
}

void write_manifest(const std::filesystem::path& path,
                    const std::vector<std::tuple<std::string, std::string, std::string>>& pages) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) {
        std::println(stderr, "lake-ui-fixture: cannot write {}", path.string());
        std::exit(1);
    }
    for (const auto& [slug, token, route] : pages) {
        output << slug << '\t' << token << '\t' << route << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: lake-ui-fixture STORE_ROOT MANIFEST");
        return 2;
    }
    const std::filesystem::path store_root = argv[1];
    const std::filesystem::path manifest = argv[2];
    const std::string admin_token = "ui-fixture-admin";
    const std::string instance_kek = "ui-fixture-instance-kek";
    const std::string share_token = "ui-fixture-share-token";

    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto issued = require(store->sign_up_password({"lake",
                                                         "ui-fixture@example.test",
                                                         "correct horse battery staple",
                                                         "UI Fixture",
                                                         "fixture browser",
                                                         "{}"}),
                                "create fixture identity");
    const auto principal = require(store->context_for(issued.secret), "read fixture identity");
    const auto individual_customer =
        require(store->create_customer({"individual", principal.user_id, "", "ui-fixture"}),
                "create individual customer");
    const auto organization =
        require(store->create_organization(
                    {"taap:1:ed25519:ui-fixture-organization", "Orama Research", ""}),
                "create organization");
    const auto organization_customer =
        require(store->create_customer({"organization", "", organization.id, "orama-research"}),
                "create organization customer");
    const std::string application_id = lake::control::new_uuid_v4();
    auto application_key = lake::control::apps::generate_signing_key(application_id, 1);
    if (!application_key) {
        std::println(
            stderr, "lake-ui-fixture: create application signing key: {}", application_key.error());
        return 1;
    }
    const auto application =
        require(store->create_application(
                    {.id = application_id,
                     .customer_id = organization_customer.id,
                     .pool_id = "app:" + application_id,
                     .name = "Orama Console",
                     .allowed_origins = {"https://console.example.test"},
                     .redirect_uris = {"https://console.example.test/callback"},
                     .custom_sign_in_domain = "login.console.example.test",
                     .theme = {{"accent_color", "#0f62fe"}, {"background_color", "#f8f8fa"}},
                     .signing_key = *application_key}),
                "create application");
    require(store->sign_up_password({application.pool_id,
                                     "app-user@example.test",
                                     "correct horse battery staple",
                                     "Application User",
                                     "fixture browser",
                                     "{}"}),
            "create application user");
    require(store->create_application_organization({application.id,
                                                    "taap:1:ed25519:ui-fixture-app-organization",
                                                    "Example Workspace",
                                                    {{"plan", "enterprise"}}}),
            "create application organization");
    require(store->create_application_webhook_event(
                application.id,
                "user.created",
                {{"email", "app-user@example.test"}, {"source", "ui-fixture"}}),
            "create application event");
    const auto repository =
        require(store->create_repository({organization_customer.id,
                                          "lake-design",
                                          "Design-system fixture with history and build evidence.",
                                          "public"}),
                "create public repository");
    require(store->create_repository({individual_customer.id,
                                      "private-notes",
                                      "A private repository for list density.",
                                      "private"}),
            "create private repository");
    const auto team = require(
        store->create_team({organization.id, "platform", "Platform", "admin", "ui-fixture-team"}),
        "create team");
    require(store->create_membership({organization.id,
                                      team.id,
                                      "user",
                                      principal.user_id,
                                      principal.taap_id,
                                      "admin",
                                      "ui-fixture-membership",
                                      "admin",
                                      "ui-fixture-membership"}),
            "create membership");
    require(store->create_invitation({organization.id,
                                      team.id,
                                      "email",
                                      "reviewer@example.test",
                                      "member",
                                      "2030-01-01T00:00:00Z",
                                      "admin",
                                      "ui-fixture-invitation"}),
            "create invitation");
    const auto provider =
        require(store->register_sso_provider({organization.id,
                                              "lake",
                                              "Workforce OIDC",
                                              "oidc",
                                              "https://identity.example.test",
                                              "",
                                              "",
                                              "lake-ui-fixture",
                                              "fixture-secret",
                                              "fixture-scim-token",
                                              {{"email", "email"}, {"display_name", "name"}},
                                              true}),
                "create SSO provider");
    require(store->create_federation_issuer(
                {organization.id, "https://identity.example.test", "email"}),
            "create federation issuer");
    const auto mount = require(store->create_secret_mount({organization_customer.id,
                                                           organization.id,
                                                           "production",
                                                           "Production service credentials.",
                                                           "admin",
                                                           "ui-fixture-mount"},
                                                          instance_kek),
                               "create secret mount");
    require(store->put_secret({mount.id,
                               "search-api",
                               {{"token", "fixture-value"}},
                               {{"owner", "platform"}, {"rotation", "quarterly"}},
                               0},
                              instance_kek),
            "create secret");
    require(store->create_encryption_key({mount.id, "artifact-signing"}, instance_kek),
            "create encryption key");

    auto customer_store = require(liquid::runtime::block_on(liquid::store::open(
                                      store_root / organization_customer.id / "store")),
                                  "open customer store");
    liquid::repository_options repository_options;
    repository_options.store = &customer_store;
    auto liquid_repository =
        require(liquid::runtime::block_on(liquid::repository::create(
                    store_root / organization_customer.id / "repositories" / repository.id,
                    repository_options)),
                "create liquid repository");
    require(liquid::runtime::block_on(liquid_repository.write(
                "README.md", bytes("# Lake fixture\n\nA searchable repository.\n"))),
            "write README.md");
    require(liquid::runtime::block_on(liquid_repository.write(
                "docs/architecture.md",
                bytes("# Architecture\n\nServer-rendered pages and liquid data.\n"))),
            "write architecture document");
    require(liquid::runtime::block_on(liquid_repository.cut("Initial repository content")),
            "record initial change");
    require(liquid::runtime::block_on(liquid_repository.write(
                "README.md", bytes("# Lake fixture\n\nA searchable Orama repository.\n"))),
            "update README.md");
    require(liquid::runtime::block_on(
                liquid_repository.write("src/example.cpp",
                                        bytes("#include <print>\n\nint main() {\n"
                                              "    std::println(\"hello lake\");\n"
                                              "}\n"))),
            "write example source");
    const auto change =
        require(liquid::runtime::block_on(liquid_repository.cut("Add the Orama example")),
                "record second change");
    const std::vector<std::byte> artifact_bytes = bytes("fixture artifact output");
    const auto artifact_content =
        require(liquid::hash(liquid::algorithm::blake3, artifact_bytes), "hash artifact");
    const liquid::blob remote_artifact{
        .chunks = {artifact_content},
        .size = artifact_bytes.size(),
        .chunk_sizes = {artifact_bytes.size()},
        .content = artifact_content,
        .remote = liquid::remote_blob_location{"fixture", "ui-worker"},
    };
    const auto artifact_blob =
        require(liquid::runtime::block_on(liquid_repository.store_remote_blob(remote_artifact)),
                "store artifact metadata");
    const auto history =
        require(liquid::runtime::block_on(liquid_repository.log()), "read repository history");
    const liquid::action_input build_input{
        .command = {"cmake", "--build", "build"},
        .outputs = {"build/lake"},
        .inputs = history.front().root,
        .toolchain = history.front().root,
        .platform = host_platform(),
    };
    auto result_value = liquid::action_result{
        .outputs = {{"build/lake", 0100755, artifact_blob}},
        .stdout_output = artifact_blob,
        .stderr_output = artifact_blob,
        .timing = {.queued = std::chrono::milliseconds{2},
                   .sandbox = std::chrono::milliseconds{5},
                   .exec = std::chrono::milliseconds{31},
                   .hash = std::chrono::milliseconds{3},
                   .store = std::chrono::milliseconds{4}},
        .worker = "ui-worker",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{1'800'000'000}},
        .cache = liquid::cache_status::hit,
    };
    result_value.started_at = std::chrono::sys_seconds{std::chrono::seconds{1'799'999'960}};
    result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{1'800'000'000}};
    const auto artifact = require(liquid::runtime::block_on(liquid::build::record_remote_result(
                                      liquid_repository, build_input, result_value)),
                                  "record artifact");
    const auto references =
        require(liquid::runtime::block_on(liquid_repository.refs()), "read references");
    const std::string ref = references.front().name;

    const auto repository_object =
        require(store->find_policy_object("repository", repository.id), "find repository object");
    require(store->create_grant({principal.taap_id,
                                 repository_object.id,
                                 "read",
                                 "ui-fixture-repository-read",
                                 "admin",
                                 "ui-fixture-grant"}),
            "create repository grant");
    const auto root_tree =
        require(liquid::runtime::block_on(liquid_repository.root()), "read root tree");
    const auto docs_entry = std::ranges::find(root_tree.entries, "docs", &liquid::tree_entry::name);
    if (docs_entry == root_tree.entries.end()) {
        std::println(stderr, "lake-ui-fixture: docs folder is absent");
        return 1;
    }
    const std::string docs_node_id = node_id_text(docs_entry->node);
    const auto docs_object =
        require(store->create_policy_object({"path",
                                             repository.id + ":" + docs_node_id,
                                             repository_object.id,
                                             "admin",
                                             "ui-fixture-drive-object",
                                             true}),
                "create drive folder object");
    require(store->create_drive_share_link({repository.id,
                                            docs_object.id,
                                            docs_node_id,
                                            "docs",
                                            "read",
                                            share_token_hash(share_token),
                                            "2030-01-01T00:00:00Z",
                                            "admin",
                                            "ui-fixture-share"}),
            "create drive share link");
    require(store->create_forge_record(
                {repository.id,
                 "export",
                 {{"include_git", false}, {"manifest", {{"format", "lake_export_manifest/1"}}}}}),
            "create export record");

    const std::string operator_token = admin_token;
    const std::vector<std::tuple<std::string, std::string, std::string>> pages = {
        {"home", operator_token, "/"},
        {"login", "-", "/login"},
        {"sign-up", "-", "/sign-up"},
        {"sign-in", "-", "/sign-in"},
        {"mfa", "-", "/mfa"},
        {"passkey-sign-in", "-", "/passkey-sign-in"},
        {"sessions", issued.secret, "/sessions"},
        {"passkeys", issued.secret, "/passkeys"},
        {"tokens", issued.secret, "/tokens"},
        {"customers", operator_token, "/customers"},
        {"organizations", operator_token, "/organizations"},
        {"applications", operator_token, "/applications"},
        {"application", operator_token, std::format("/application?id={}", application.id)},
        {"application-users",
         operator_token,
         std::format("/application/users?id={}", application.id)},
        {"application-organizations",
         operator_token,
         std::format("/application/organizations?id={}", application.id)},
        {"application-events",
         operator_token,
         std::format("/application/events?id={}", application.id)},
        {"application-sign-up", "-", std::format("/apps/{}/sign-up", application.id)},
        {"application-sign-in", "-", std::format("/apps/{}/sign-in", application.id)},
        {"application-mfa", "-", std::format("/apps/{}/mfa", application.id)},
        {"application-passkey", "-", std::format("/apps/{}/passkey-sign-in", application.id)},
        {"teams", operator_token, std::format("/teams?organization_id={}", organization.id)},
        {"sso",
         operator_token,
         std::format("/sso?organization_id={}&provider_id={}", organization.id, provider.id)},
        {"members",
         operator_token,
         std::format("/members?organization_id={}&team_id={}", organization.id, team.id)},
        {"invitations",
         operator_token,
         std::format("/invitations?organization_id={}&team_id={}", organization.id, team.id)},
        {"grants", operator_token, std::format("/grants?object_id={}", repository_object.id)},
        {"access", operator_token, std::format("/access?object_id={}", repository_object.id)},
        {"repositories", operator_token, "/repositories"},
        {"repository", operator_token, std::format("/repository?id={}", repository.id)},
        {"exports", operator_token, std::format("/exports?repository_id={}", repository.id)},
        {"repository-tree",
         operator_token,
         std::format("/repository/tree?id={}&ref={}", repository.id, ref)},
        {"file",
         operator_token,
         std::format("/file?repository_id={}&ref={}&path=README.md", repository.id, ref)},
        {"changes", operator_token, std::format("/changes?repository_id={}", repository.id)},
        {"change",
         operator_token,
         std::format(
             "/change?repository_id={}&id={}", repository.id, liquid::to_string(change.value))},
        {"conflicts", operator_token, std::format("/conflicts?repository_id={}", repository.id)},
        {"artifacts", operator_token, std::format("/artifacts?repository_id={}", repository.id)},
        {"artifact",
         operator_token,
         std::format(
             "/artifact?repository_id={}&id={}", repository.id, liquid::to_string(artifact.value))},
        {"search", operator_token, std::format("/search?query=Orama&repository={}", repository.id)},
        {"secret-mounts",
         operator_token,
         std::format("/secret-mounts?customer_id={}", organization_customer.id)},
        {"secrets", operator_token, std::format("/secrets?mount_id={}&name=search-api", mount.id)},
        {"encryption-keys", operator_token, std::format("/encryption-keys?mount_id={}", mount.id)},
        {"metering",
         operator_token,
         std::format("/metering?customer_id={}", organization_customer.id)},
        {"audit", operator_token, "/audit"},
        {"transparency", "-", "/transparency"},
        {"drive",
         operator_token,
         std::format("/drive?repository_id={}&ref={}&path=docs", repository.id, ref)},
        {"drive-preview",
         operator_token,
         std::format("/drive/preview?repository_id={}&ref={}&path=README.md", repository.id, ref)},
        {"drive-history",
         operator_token,
         std::format("/drive/history?repository_id={}&path=README.md", repository.id)},
        {"drive-share",
         operator_token,
         std::format("/drive/share?repository_id={}&ref={}&path=docs", repository.id, ref)},
        {"shared-link", "-", std::format("/share/{}", share_token)},
    };
    write_manifest(manifest, pages);

    lake::api::ServerOptions options;
    options.listen_address = "127.0.0.1";
    options.port = 4902;
    options.node_name = "ui-fixture";
    options.admin_token = admin_token;
    options.instance_kek = instance_kek;
    options.store_root = store_root.string();
    options.store = store;
    options.search = std::make_shared<lake::search::Service>(store_root);
    lake::api::ApiServer server(options);
    if (!server.start()) {
        std::println(stderr, "lake-ui-fixture: cannot start: {}", server.reason());
        return 1;
    }
    std::println("{{\"event\":\"startup\",\"pages\":{},\"port\":{}}}", pages.size(), server.port());
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!stop_requested.load()) {
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    server.stop();
    return 0;
}
