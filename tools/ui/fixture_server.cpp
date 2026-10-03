#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
#include "lake/audit/audit.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/search/index.hpp"
// The keyed record store tests use for workspace Zettas: drive links and application events are
// workspace records.
#include "../../tests/record_fixture.hpp"
// Secrets live in workspace records (lake#328); the fixture writes them as lake does.
#include "../../src/api/secret_records_api.hpp"

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

// The fixture's workspace Zettas: only lake's own records (drive share links, lake#350; secrets,
// lake#328), one
// record store per workspace endpoint. Every other workspace capability is unknown here.
class FixtureWorkspaces final : public lake::control::WorkspaceCaller {
public:
    liquid::runtime::task<lake::control::ZettaControlResult<nlohmann::json>>
    call(std::string url, std::string, std::string capability, bool, nlohmann::json input) override {
        std::lock_guard lock(mutex_);
        if (lake::test::RecordTables::handles(capability, input)) {
            co_return records_[url].run(capability, input);
        }
        co_return lake::control::ZettaControlResult<nlohmann::json>(std::unexpected(
            lake::control::ZettaControlError{"unknown_capability", "not in the UI fixture"}));
    }

private:
    std::mutex mutex_;
    std::map<std::string, lake::test::RecordTables> records_;
};

// A registered route pattern as a requestable path: the workspace parameter names a fixture
// workspace, and any other parameter or wildcard segment is the literal `fixture`.
std::string concrete_route(std::string_view pattern, std::string_view workspace_id) {
    std::string output;
    for (const auto segment : std::views::split(pattern, '/')) {
        const std::string_view part(segment.begin(), segment.end());
        if (part.empty()) {
            continue;
        }
        output += '/';
        if (part == ":workspace_id") {
            output += workspace_id;
        } else if (part.starts_with(':') || part.starts_with('*')) {
            output += "fixture";
        } else {
            output += part;
        }
    }
    return output.empty() || pattern.ends_with('/') ? output + '/' : output;
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
    if (argc != 3 && argc != 4) {
        std::println(stderr, "usage: lake-ui-fixture STORE_ROOT MANIFEST [PORT]");
        return 2;
    }
    const std::filesystem::path store_root = argv[1];
    const std::filesystem::path manifest = argv[2];
    const std::string admin_token = "ui-fixture-admin";
    // Hexadecimal, as LAKE_INSTANCE_KEK is: workspace records are sealed under keys derived from it.
    const std::string instance_kek =
        "75692d666978747572652d696e7374616e63652d6b656b2d3030303030303030";

    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto issued = require(store->sign_up_password({"lake",
                                                         "ui-fixture@example.test",
                                                         "correct horse battery staple",
                                                         "UI Fixture",
                                                         "fixture browser",
                                                         "{}"}),
                                "create fixture identity");
    const auto principal = require(store->context_for(issued.secret), "read fixture identity");
    const auto shared_recipient_issued =
        require(store->sign_up_password({"lake",
                                         "shared-recipient@example.test",
                                         "correct horse battery staple",
                                         "Shared Recipient",
                                         "fixture browser",
                                         "{}"}),
                "create shared recipient");
    const auto shared_recipient =
        require(store->context_for(shared_recipient_issued.secret), "read shared recipient");
    // Password sign-up provisions the user's personal customer (docs/api/provisioning.md); the
    // fixture uses that one rather than creating a second, which the store refuses.
    const auto personal_customers =
        require(store->list_customers(1, principal.user_id), "read individual customer");
    if (personal_customers.empty()) {
        std::println(stderr, "lake-ui-fixture: sign-up provisioned no individual customer");
        return 1;
    }
    const auto& individual_customer = personal_customers.front();
    const auto organization =
        require(store->create_organization(
                    {"taap:1:ed25519:ui-fixture-organization", "Orama Research", ""}),
                "create organization");
    const auto organization_customer =
        require(store->create_customer({"organization", "", organization.id, "orama-research"}),
                "create organization customer");
    // One ready workspace of the organization: it holds the application's lifecycle events.
    const auto console_workspace =
        require(store->create_workspace({organization_customer.id, "console", "prod", ""}),
                "create workspace");
    const auto console_service =
        require(store->get_service(console_workspace.zetta_service_id), "read workspace service");
    if (const auto ready =
            store->set_service_status(console_service.id, "Ready", "", "http://console.test");
        !ready) {
        std::println(stderr, "lake-ui-fixture: mark workspace ready: {}", ready.error().message);
        return 1;
    }
    // Every workspace of the fixture is served by one WorkspaceService over FixtureWorkspaces.
    const auto service_dir = store_root / "ui-fixture-services";
    std::filesystem::create_directories(service_dir);
    std::ofstream(service_dir / (console_service.slug + ".json"))
        << nlohmann::json{{"kind", "zetta"}, {"token", "ui-fixture-workspace"}}.dump();
    const auto workspaces = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{service_dir.string(), 0},
        store,
        std::make_shared<FixtureWorkspaces>());
    const std::string application_id = lake::control::new_uuid_v4();
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
                     .workspace_id = console_workspace.id}),
                "create application");
    const auto application_account =
        require(store->sign_up_password({application.pool_id,
                                         "app-user@example.test",
                                         "correct horse battery staple",
                                         "Application User",
                                         "fixture browser",
                                         "{}"}),
                "create application user");
    {
        auto records = lake::control::WorkspaceRecords::open(
            [workspaces, customer = organization_customer.id, id = console_workspace.id](
                std::string capability, bool read, nlohmann::json input) {
                return workspaces->call(customer, id, std::move(capability), read, std::move(input));
            },
            console_workspace.id, instance_kek);
        if (!records) {
            std::println(stderr, "lake-ui-fixture: open workspace records: {}",
                         records.error().message);
            return 1;
        }
        // The application's user and organization live in its workspace (lake#353, lake#354).
        const auto account =
            require(store->get_user(application_account.session.user_id), "read application user");
        lake::control::ApplicationUser user;
        user.application_id = application.id;
        user.user_id = account.id;
        user.taap_id = account.taap_id;
        user.email = account.email;
        user.display_name = account.display_name;
        user.status = "active";
        user.own_pool = true;
        lake::control::ApplicationOrganization app_organization;
        app_organization.application_id = application.id;
        app_organization.taap_id = "taap:1:ed25519:ui-fixture-app-organization";
        app_organization.name = "Example Workspace";
        app_organization.metadata = {{"plan", "enterprise"}};
        auto user_written = liquid::runtime::block_on(
            lake::control::create_application_user(*records, std::move(user)));
        auto organization_written = liquid::runtime::block_on(
            lake::control::put_new_application_organization(*records, std::move(app_organization)));
        if (!user_written || !*user_written || !organization_written || !*organization_written) {
            std::println(stderr, "lake-ui-fixture: create application user or organization failed");
            return 1;
        }
        // Its signing key lives in the same workspace (lake#352), enveloped by the store.
        auto application_key = lake::control::apps::generate_signing_key(application.id, 1);
        if (!application_key) {
            std::println(stderr, "lake-ui-fixture: create application signing key: {}",
                         application_key.error());
            return 1;
        }
        application_key->encrypted_seed =
            require(store->encrypt_application_credential("application.jwt." + application.id,
                                                          application_key->seed),
                    "envelope application signing key");
        application_key->seed.clear();
        auto ring = liquid::runtime::block_on(lake::control::create_application_signing_keys(
            *records, {.application_id = application.id, .keys = {*application_key}}));
        if (!ring || !*ring) {
            std::println(stderr, "lake-ui-fixture: create application signing key failed");
            return 1;
        }
        lake::control::ApplicationWebhookEvents events(*records);
        auto appended = liquid::runtime::block_on(events.append(
            {.application_id = application.id,
             .event = "user.created",
             .payload = {{"email", "app-user@example.test"}, {"source", "ui-fixture"}}}));
        if (!appended || !*appended) {
            std::println(stderr, "lake-ui-fixture: create application event failed");
            return 1;
        }
    }
    // Each repository belongs to one workspace of its space (lake#315).
    const auto research_workspace = require(
        store->create_workspace({organization_customer.id, "research", "dev", ""}),
        "create workspace");
    const auto personal_workspace = require(
        store->create_workspace({individual_customer.id, "personal", "dev", ""}),
        "create personal workspace");
    const auto repository =
        require(store->create_repository({organization_customer.id,
                                          "lake-design",
                                          "Design-system fixture with history and build evidence.",
                                          "public",
                                          research_workspace.id}),
                "create public repository");
    require(store->create_repository({individual_customer.id,
                                      "private-notes",
                                      "A private repository for list density.",
                                      "private",
                                      personal_workspace.id}),
            "create private repository");
    const auto team = require(
        store->create_team({organization.id, "platform", "Platform", "admin", "ui-fixture-team"}),
        "create team");
    const auto membership = require(store->create_membership({organization.id,
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
    // The organization's secret mount, in its console workspace (lake#328).
    lake::api::ApiContext secrets_context;
    secrets_context.store = store.get();
    secrets_context.workspaces = workspaces.get();
    secrets_context.instance_kek = instance_kek;
    lake::control::SecretMount mount_draft;
    mount_draft.customer_id = organization_customer.id;
    mount_draft.workspace_id = console_workspace.id;
    mount_draft.organization_id = organization.id;
    mount_draft.name = "production";
    mount_draft.description = "Production service credentials.";
    auto created_mount = lake::control::settled(liquid::runtime::block_on(
        lake::api::create_secret_mount(secrets_context, mount_draft)));
    if (!created_mount) {
        std::println(stderr, "lake-ui-fixture: create secret mount: {}",
                     created_mount.error().message);
        return 1;
    }
    const auto mount = *created_mount;
    {
        auto secrets = lake::api::MountSecrets::open(secrets_context, mount.id);
        if (!secrets) {
            std::println(stderr, "lake-ui-fixture: open secret mount: {}", secrets.error().message);
            return 1;
        }
        auto put = lake::control::settled(liquid::runtime::block_on(
            secrets->put({mount.id,
                          "search-api",
                          {{"token", "fixture-value"}},
                          {{"owner", "platform"}, {"rotation", "quarterly"}},
                          0})));
        auto key = lake::control::settled(
            liquid::runtime::block_on(secrets->create_key("artifact-signing")));
        if (!put || !key) {
            std::println(stderr, "lake-ui-fixture: create secret and key: {}",
                         !put ? put.error().message : key.error().message);
            return 1;
        }
    }

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
    require(liquid::runtime::block_on(liquid_repository.write(
                "docs/nested/hello.txt", bytes("Hello from the shared child folder.\n"))),
            "write nested shared document");
    require(liquid::runtime::block_on(
                liquid_repository.write("conflict.txt", bytes("primary version\nshared line\n"))),
            "write conflict source");
    require(liquid::runtime::block_on(
                liquid_repository.write("document.pdf", bytes("%PDF-1.7\n%%EOF\n"))),
            "write PDF fixture");
    require(liquid::runtime::block_on(liquid_repository.write("clip.mp4", bytes("fixture video"))),
            "write video fixture");
    require(liquid::runtime::block_on(liquid_repository.write("sound.mp3", bytes("fixture audio"))),
            "write audio fixture");
    require(
        liquid::runtime::block_on(liquid_repository.write("archive.zip", bytes("fixture archive"))),
        "write fallback fixture");
    const auto initial_change =
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
    auto conflict_repository = require(
        liquid::runtime::block_on(liquid::repository::create(
            store_root / organization_customer.id / "repositories" / (repository.id + "-conflict"),
            repository_options)),
        "create conflict repository");
    require(liquid::runtime::block_on(conflict_repository.write(
                "conflict.txt", bytes("alternate version\nshared line\n"))),
            "write alternate conflict value");
    require(liquid::runtime::block_on(conflict_repository.cut("Alternate conflict value")),
            "record alternate conflict value");
    require(liquid::runtime::block_on(
                liquid_repository.combine(std::move(conflict_repository), "conflict.txt")),
            "combine conflict value");
    require(liquid::runtime::block_on(liquid_repository.cut("Record conflict values")),
            "record conflict values");
    const auto conflicts =
        require(liquid::runtime::block_on(liquid_repository.conflicts()), "read conflict values");
    if (conflicts.size() != 1U || conflicts.front().region.path != "conflict.txt" ||
        conflicts.front().kind != liquid::conflict_kind::structural ||
        conflicts.front().explanation.empty()) {
        std::println(stderr, "lake-ui-fixture: conflict fixture was not preserved");
        return 1;
    }
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
    require(store->create_grant({shared_recipient.taap_id,
                                 docs_object.id,
                                 "read",
                                 "ui-fixture-shared-recipient",
                                 "admin",
                                 "ui-fixture-shared-recipient"}),
            "create shared recipient grant");
    // The folder's link lives in the organization's workspace Zetta; centrally only its grant.
    const auto& workspace = research_workspace;
    const auto workspace_zetta =
        require(store->get_service(workspace.zetta_service_id), "read workspace Zetta");
    if (const auto ready = store->set_service_status(
            workspace_zetta.id, "Ready", "", "http://" + workspace.id + ".fixture");
        !ready) {
        std::println(stderr, "lake-ui-fixture: mark workspace ready: {}", ready.error().message);
        return 1;
    }
    std::ofstream(service_dir / (workspace_zetta.slug + ".json"))
        << nlohmann::json{{"kind", "zetta"}, {"token", "ui-fixture-workspace"}}.dump();
    lake::control::DriveShareLink share_link;
    share_link.id = lake::control::new_uuid_v4();
    const std::string share_token =
        lake::control::drive_share_token(workspace.id, share_link.id, "ui-fixture-share-secret");
    share_link.repository_id = repository.id;
    share_link.object_id = docs_object.id;
    share_link.node_id = docs_node_id;
    share_link.path = "docs";
    share_link.role = "read";
    share_link.token_hash = share_token_hash(share_token);
    share_link.expires_at = "2030-01-01T00:00:00Z";
    share_link.created_by_taap_id = "admin";
    {
        auto records = require(
            lake::control::WorkspaceRecords::open(
                [workspaces, customer = organization_customer.id, id = workspace.id](
                    std::string capability, bool read, nlohmann::json input) {
                    return workspaces->call(customer, id, std::move(capability), read,
                                            std::move(input));
                },
                workspace.id, instance_kek),
            "open workspace records");
        lake::control::DriveShareLinks links(records);
        require(lake::control::settled(liquid::runtime::block_on(links.append(
                    share_link, lake::control::RecordWrite{share_link.id, 0}))),
                "create drive share link");
    }
    require(store->create_grant({"share-link:" + share_link.id,
                                 docs_object.id,
                                 "read",
                                 "drive_share_link/" + share_link.id,
                                 "admin",
                                 "ui-fixture-share"}),
            "create drive share link grant");
    // The repository's forge records live in its workspace Zetta (lake#341).
    auto forge_records = require(
        lake::control::WorkspaceRecords::open(
            [workspaces, customer = organization_customer.id, id = workspace.id](
                std::string capability, bool read, nlohmann::json input) {
                return workspaces->call(customer, id, std::move(capability), read,
                                        std::move(input));
            },
            workspace.id, instance_kek),
        "open forge records");
    lake::control::ForgeRecords forge(forge_records);
    const auto forge_record = [&forge, &repository](std::string kind, nlohmann::json value) {
        lake::control::ForgeRecord record;
        record.repository_id = repository.id;
        record.kind = std::move(kind);
        record.value = std::move(value);
        return lake::control::settled(liquid::runtime::block_on(forge.append(std::move(record))));
    };
    require(forge_record("export",
                 {{"include_git", false}, {"manifest", {{"format", "lake_export_manifest/1"}}}}),
            "create export record");
    const std::string change_id = liquid::to_string(change.value);
    const auto fixture_issue = require(
        forge_record("issue",
             {{"title", "Keep source anchors stable"},
              {"body",
               "## Context\n\nSource links should survive a new change and remain safe to share."},
              {"state", "open"},
              {"labels", {"forge", "accessibility"}},
              {"assignees", {principal.taap_id}},
              {"references", {change_id}}}),
        "create issue record");
    require(forge_record("issue_comment",
                 {{"issue_id", fixture_issue.id},
                  {"body", "Confirmed with keyboard navigation and a **fresh** source view."}}),
            "create issue comment");
    const auto fixture_review =
        require(forge_record("review",
                                            {{"change_id", change_id},
                                             {"state", "approved"},
                                             {"reviewers", {principal.taap_id}}}),
                "create review record");
    require(forge_record("review_comment",
                 {{"review_id", fixture_review.id},
                  {"body", "The updated wording is clear and the line anchor remains stable."},
                  {"anchor",
                   {{"change", change_id}, {"path", "README.md"}, {"line", 3}, {"column", 1}}}}),
            "create review comment");
    require(forge_record("release",
                 {{"tag", "v1.0.0"},
                  {"change_id", change_id},
                  {"notes", "## Forge release\n\nIncludes repository and review pages."},
                  {"artifacts",
                   {{{"id", liquid::to_string(artifact.value)},
                     {"action", "cmake --build build"},
                     {"outputs", 1},
                     {"attestations",
                      {{{"id", "ui-fixture-attestation"},
                        {"profile", "slsa_provenance/1"},
                        {"subject", liquid::to_string(artifact.value)},
                        {"issuer", "ui-fixture"}}}}}}}}),
            "create release record");
    const auto fixture_webhook =
        require(forge_record("webhook",
                                            {{"url", "https://hooks.example.test/lake"},
                                             {"events", {"review", "release"}},
                                             {"secret", "fixture-webhook-secret"}}),
                "create webhook record");
    require(forge_record("webhook_delivery",
                                        {{"webhook_id", fixture_webhook.id},
                                         {"event", "review"},
                                         {"payload", {{"change_id", change_id}}},
                                         {"signature", "sha256=fixture"},
                                         {"attempt", 2},
                                         {"status", 503},
                                         {"error", "fixture receiver unavailable"},
                                         {"delivered", false}}),
            "create webhook delivery record");
    require(forge_record("branch_protection",
                                        {{"ref", "main"},
                                         {"required_review", true},
                                         {"required_verification", true},
                                         {"fast_forward_only", true},
                                         {"allowed_pushers", {principal.taap_id}},
                                         {"policy_object_id", repository_object.id}}),
            "create branch protection record");
    require(lake::audit::record(*store,
                                {.customer_id = organization_customer.id,
                                 .kind = "forge.branch_protection.refused",
                                 .principal = "taap:1:ed25519:untrusted",
                                 .object = "repository/" + repository.id + "/ref/main",
                                 .action = "reference_update",
                                 .outcome = "denied",
                                 .request_id = "ui-fixture-refusal",
                                 .method = "bearer",
                                 .device = "fixture client",
                                 .details = {{"repository", repository.id},
                                             {"reference", "main"},
                                             {"caller", "taap:1:ed25519:untrusted"},
                                             {"rule", "required_review"},
                                             {"reason", "an approved review is required"}}},
                                {.instance_kek = instance_kek}),
            "create branch protection audit event");

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
        {"settings", issued.secret, "/settings"},
        {"customers", operator_token, "/customers"},
        {"organizations", operator_token, "/organizations"},
        {"organization", operator_token, std::format("/organization?id={}", organization.id)},
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
        {"team", operator_token, std::format("/team?id={}", team.id)},
        {"sso",
         operator_token,
         std::format("/sso?organization_id={}&provider_id={}", organization.id, provider.id)},
        {"members",
         operator_token,
         std::format("/members?organization_id={}&team_id={}", organization.id, team.id)},
        {"member", operator_token, std::format("/member?id={}", membership.id)},
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
         std::format("/change?repository_id={}&id={}", repository.id, change_id)},
        {"issues", operator_token, std::format("/issues?repository_id={}", repository.id)},
        {"issue",
         operator_token,
         std::format("/issue?repository_id={}&id={}", repository.id, fixture_issue.id)},
        {"releases", operator_token, std::format("/releases?repository_id={}", repository.id)},
        {"webhooks", operator_token, std::format("/webhooks?repository_id={}", repository.id)},
        {"protection", operator_token, std::format("/protection?repository_id={}", repository.id)},
        {"conflicts", operator_token, std::format("/conflicts?repository_id={}", repository.id)},
        {"artifacts", operator_token, std::format("/artifacts?repository_id={}", repository.id)},
        {"artifact",
         operator_token,
         std::format(
             "/artifact?repository_id={}&id={}", repository.id, liquid::to_string(artifact.value))},
        {"search", operator_token, std::format("/search?query=Orama&repository={}", repository.id)},
        {"secret-mounts",
         operator_token,
         std::format("/secret-mounts?workspace_id={}", console_workspace.id)},
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
        {"drive-grid",
         operator_token,
         std::format("/drive?repository_id={}&ref={}&path=&view=grid", repository.id, ref)},
        {"drive-preview",
         operator_token,
         std::format("/drive/preview?repository_id={}&ref={}&path=README.md", repository.id, ref)},
        {"drive-history",
         operator_token,
         std::format("/drive/history?repository_id={}&path=README.md", repository.id)},
        {"drive-version-diff",
         operator_token,
         std::format("/drive/history/diff?repository_id={}&ref={}&path=README.md&left={}&right={}",
                     repository.id,
                     ref,
                     liquid::to_string(initial_change.value),
                     liquid::to_string(change.value))},
        {"drive-conflict",
         operator_token,
         std::format(
             "/drive/conflict?repository_id={}&ref={}&path=conflict.txt", repository.id, ref)},
        {"drive-share",
         operator_token,
         std::format("/drive/share?repository_id={}&ref={}&path=docs&workspace_id={}",
                     repository.id, ref, workspace.id)},
        {"drive-share-link",
         operator_token,
         std::format("/drive/share/link?repository_id={}&ref={}&path=docs&workspace_id={}&link_id={}",
                     repository.id,
                     ref,
                     workspace.id,
                     share_link.id)},
        {"shared-with-me", shared_recipient_issued.secret, "/shared-with-me"},
        {"shared-link", "-", std::format("/share/{}", share_token)},
    };
    write_manifest(manifest, pages);

    lake::api::ServerOptions options;
    options.listen_address = "127.0.0.1";
    options.port = argc == 4 ? static_cast<unsigned short>(std::stoul(argv[3])) : 4902;
    options.node_name = "ui-fixture";
    options.admin_token = admin_token;
    options.instance_kek = instance_kek;
    options.store_root = store_root.string();
    options.store = store;
    options.workspaces = workspaces;
    options.search = std::make_shared<lake::search::Service>(store_root);
    lake::api::ApiServer server(options);
    if (!server.start()) {
        std::println(stderr, "lake-ui-fixture: cannot start: {}", server.reason());
        return 1;
    }
    // Every GET route the server registered, made concrete in the research workspace, beside
    // the manifest: tools/ui/test-fixture-pages.sh requests each one (lake#497).
    {
        const auto routes_path = std::filesystem::path(manifest.string() + ".routes");
        std::ofstream routes(routes_path);
        if (!routes) {
            std::println(stderr, "lake-ui-fixture: cannot write {}", routes_path.string());
            return 1;
        }
        for (const std::string& pattern : server.get_route_patterns()) {
            routes << concrete_route(pattern, research_workspace.id) << '\n';
        }
    }
    std::println("{{\"event\":\"startup\",\"pages\":{},\"port\":{}}}", pages.size(), server.port());
    // Callers read the port from this line through a pipe or file, where stdout is not
    // line-buffered.
    std::fflush(stdout);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!stop_requested.load()) {
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    server.stop();
    return 0;
}
