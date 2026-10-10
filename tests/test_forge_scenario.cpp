#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liquid/build.hpp>
#include <liquid/forge.hpp>
#include <liquid/repository.hpp>
#include <liquid/runtime.hpp>
#include <liquid/sync.hpp>

#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/postgres_store.hpp"
#include "temporary_directory.hpp"
#include "repository_test_support.hpp"
#include "workspace_fixture.hpp"

namespace {

class ScenarioRoot final {
public:
    ScenarioRoot() : path_(lake::test::fixture_path("forge-scenario-" + std::to_string(++next_))) {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    ~ScenarioRoot() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto host_platform() -> liquid::platform {
#if defined(__APPLE__) && defined(__aarch64__)
    return {"macos", "arm64"};
#elif defined(__APPLE__)
    return {"macos", "x86_64"};
#elif defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

auto send_all(int socket_fd, std::string_view value) -> bool {
    auto sent = std::size_t{};
    while (sent < value.size()) {
        const auto count = ::send(socket_fd, value.data() + sent, value.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

auto http_get(unsigned short port, std::string_view target, std::string_view bearer)
    -> std::string {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return {};
    }
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    const std::string request = "GET " + std::string{target} +
                                " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                "Authorization: Bearer " + std::string{bearer} + "\r\n\r\n";
    if (!send_all(socket_fd, request)) {
        ::close(socket_fd);
        return {};
    }
    auto response = std::string{};
    auto buffer = std::array<char, 4096>{};
    for (;;) {
        const auto count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

// The head of one ref, read through the `refs` capability: liquid's repository is the only record
// of refs (lake#319).
auto ref_head(unsigned short port,
              std::string_view bearer,
              const std::string& repository_id,
              std::string_view name) -> lake::control::Result<std::string> {
    const std::string response = http_get(port, "/v1/refs?repository_id=" + repository_id, bearer);
    const auto separator = response.find("\r\n\r\n");
    const auto body = separator == std::string::npos
                          ? nlohmann::json()
                          : nlohmann::json::parse(response.substr(separator + 4), nullptr, false);
    if (!body.is_object() || !body.contains("value")) {
        return std::unexpected(lake::control::StoreError{"store_error", response});
    }
    for (const auto& reference : body.at("value").at("items")) {
        if (reference.at("name") == name) {
            return reference.at("head").get<std::string>();
        }
    }
    return std::unexpected(lake::control::StoreError{"not_found", "reference not found"});
}

auto repository_has_yaml(liquid::repository& repository, liquid::change_id change)
    -> liquid::result<bool> {
    const auto snapshot = liquid::runtime::block_on(repository.at(change));
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    const auto differences =
        liquid::runtime::block_on(repository.diff(liquid::tree{}, snapshot->root, {.text = false}));
    if (!differences) {
        return std::unexpected(differences.error());
    }
    return std::ranges::any_of(*differences, [](const liquid::tree_diff_entry& entry) {
        return entry.path.ends_with(".yml") || entry.path.ends_with(".yaml");
    });
}

auto record_graph_result(liquid::repository& repository, liquid::change_id change)
    -> liquid::result<liquid::result_id> {
    const auto changes = liquid::runtime::block_on(repository.log({.from = change}));
    if (!changes) {
        return std::unexpected(changes.error());
    }
    const auto selected = std::ranges::find_if(*changes, [&](const liquid::change& candidate) {
        return liquid::to_string(candidate.id.value) == liquid::to_string(change.value);
    });
    if (selected == changes->end()) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "change not found", "forge scenario"});
    }
    const auto output = bytes("forge scenario graph output\n");
    const auto content = liquid::hash(liquid::algorithm::blake3, output);
    if (!content) {
        return std::unexpected(content.error());
    }
    const liquid::blob remote_output{
        .chunks = {*content},
        .size = output.size(),
        .chunk_sizes = {output.size()},
        .content = *content,
        .remote = liquid::remote_blob_location{"forge-scenario", "graph-runner"},
    };
    const auto artifact = liquid::runtime::block_on(repository.store_remote_blob(remote_output));
    if (!artifact) {
        return std::unexpected(artifact.error());
    }
    const liquid::action_input input{
        .command = {"forge-graph-build", "src/review.cpp", "build/forge-proof.txt"},
        .outputs = {"build/forge-proof.txt"},
        .inputs = selected->root,
        .toolchain = selected->root,
        .platform = host_platform(),
    };
    const liquid::action_result result{
        .outputs = {{"build/forge-proof.txt", 0100644, *artifact}},
        .stdout_output = *artifact,
        .stderr_output = *artifact,
        .timing = {.queued = std::chrono::milliseconds{1},
                   .sandbox = std::chrono::milliseconds{2},
                   .exec = std::chrono::milliseconds{3},
                   .hash = std::chrono::milliseconds{1},
                   .store = std::chrono::milliseconds{1}},
        .worker = "graph-runner",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}},
    };
    return liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, input, result));
}

struct GraphEvidence {
    liquid::result_id result;
    std::string command;
    bool verified{};
    std::string reason;
};

auto record_lake_graph_evidence(const std::filesystem::path& root,
                                const std::string& customer_id,
                                const std::string& repository_id,
                                liquid::change_id change) -> liquid::result<GraphEvidence> {
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root / customer_id / "store"));
    if (!customer_store) {
        return std::unexpected(customer_store.error());
    }
    liquid::repository_options options;
    options.store = &*customer_store;
    auto repository = liquid::runtime::block_on(
        liquid::repository::open(root / customer_id / "repositories" / repository_id, options));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    const auto recorded = record_graph_result(*repository, change);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }
    const auto stored = liquid::runtime::block_on(liquid::build::result(*repository, *recorded));
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const auto action = liquid::runtime::block_on(repository->action(stored->action));
    if (!action) {
        return std::unexpected(action.error());
    }
    const auto provenance =
        liquid::runtime::block_on(liquid::build::verify_provenance(*repository, *recorded));
    if (!provenance) {
        return std::unexpected(provenance.error());
    }
    return GraphEvidence{*recorded,
                         action->command.empty() ? "" : action->command.front(),
                         provenance->verified,
                         provenance->reason};
}

auto reference_outcome(const liquid::sync_report& report, std::string_view name)
    -> const liquid::sync_ref_outcome* {
    const auto outcome =
        std::ranges::find(report.references, name, &liquid::sync_ref_outcome::name);
    return outcome == report.references.end() ? nullptr : &*outcome;
}

} // namespace

TEST(PostgresForgeScenario, DripsReviewRebaseAndGraphEvidenceLandEndToEnd) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    ScenarioRoot temporary;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    std::shared_ptr<lake::control::Store> control =
        lake::control::make_postgres_store(database_uri, instance_kek);
    ASSERT_TRUE(control->ping());
    const std::string suffix = lake::control::new_uuid_v4();
    const auto owner = control->create_user({"taap:1:ed25519:forge-author-" + suffix,
                                             "forge-author-" + suffix + "@example.test",
                                             "Forge Author"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "forge-author-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "forge-scenario", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;

    auto author =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "author"));
    ASSERT_TRUE(author) << author.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        author->write("src/review.cpp", bytes("namespace forge {\nint value = 0;\n}\n"))));
    const auto base = liquid::runtime::block_on(author->cut("Base forge source"));
    ASSERT_TRUE(base) << base.error().message;

    lake::api::ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-forge-scenario";
    options.admin_token = "postgres-forge-scenario-admin";
    options.instance_kek = instance_kek;
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    // Reviews and their comments are forge records in the repository's workspace Zetta (lake#341).
    lake::test::RecordWorkspaces workspaces(control, database_uri);
    workspaces.ready(repository->workspace_id);
    options.workspaces = workspaces.service;
    auto server = std::make_unique<lake::api::ApiServer>(options);
    ASSERT_TRUE(server->start()) << server->reason();
    const std::string repository_url =
        "http://127.0.0.1:" + std::to_string(server->port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint sync(repository_url,
                               liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    ASSERT_TRUE(liquid::runtime::block_on(sync.push(*author)));

    ASSERT_TRUE(liquid::runtime::block_on(
        author->write("src/review.cpp", bytes("namespace forge {\nint reviewed = 1;\n}\n"))));
    const auto candidate = liquid::runtime::block_on(author->cut("Review forge change"));
    ASSERT_TRUE(candidate) << candidate.error().message;
    const auto drips = liquid::runtime::block_on(author->drips());
    ASSERT_TRUE(drips) << drips.error().message;
    EXPECT_GE(drips->size(), 2U);
    ASSERT_TRUE(liquid::runtime::block_on(author->set_ref("review/forge", *candidate)));
    ASSERT_TRUE(liquid::runtime::block_on(author->set_ref("main", *base)));
    ASSERT_TRUE(liquid::runtime::block_on(sync.push(*author)));
    const auto unlanded_head = ref_head(server->port(), options.admin_token, repository->id, "main");
    ASSERT_TRUE(unlanded_head) << unlanded_head.error().message;
    EXPECT_EQ(*unlanded_head, liquid::to_string(base->value));

    liquid::forge_client reviewer(repository_url, {.bearer_token = options.admin_token});
    const auto review =
        liquid::runtime::block_on(reviewer.create_review(*candidate, {"taap:1:ed25519:reviewer"}));
    ASSERT_TRUE(review) << review.error().message;
    const auto opened = liquid::runtime::block_on(reviewer.get_review(review->id));
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(opened->state, liquid::forge_review_state::open);
    EXPECT_EQ(liquid::to_string(opened->change.value), liquid::to_string(candidate->value));
    // What the change page reads (lake#561): the change and its open review with its reviewer.
    const std::string candidate_change = http_get(
        server->port(),
        "/v1/get_change?id=" + repository->id + "&change_id=" + liquid::to_string(candidate->value),
        options.admin_token);
    EXPECT_NE(candidate_change.find(" 200 "), std::string::npos) << candidate_change;
    EXPECT_NE(candidate_change.find("Review forge change"), std::string::npos);
    const std::string reviews =
        http_get(server->port(),
                 "/v1/list_reviews?repository_id=" + repository->id + "&limit=100",
                 options.admin_token);
    EXPECT_NE(reviews.find(" 200 "), std::string::npos) << reviews;
    EXPECT_NE(reviews.find("taap:1:ed25519:reviewer"), std::string::npos) << reviews;

    const auto comment = liquid::runtime::block_on(reviewer.create_comment(
        review->id,
        "Keep the reviewed assignment.",
        {.change = *candidate, .path = "src/review.cpp", .line = 2, .column = 1}));
    ASSERT_TRUE(comment) << comment.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(author->write(
        "src/review.cpp", bytes("// current main\nnamespace forge {\nint reviewed = 1;\n}\n"))));
    const auto rebased = liquid::runtime::block_on(author->cut("Rebase forge change"));
    ASSERT_TRUE(rebased) << rebased.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(author->set_ref("review/forge", *rebased)));
    ASSERT_TRUE(liquid::runtime::block_on(author->set_ref("main", *base)));
    ASSERT_TRUE(liquid::runtime::block_on(sync.push(*author)));
    server->stop();
    server.reset();

    const auto graph = record_lake_graph_evidence(
        temporary.path() / "lake", customer->id, repository->id, *rebased);
    ASSERT_TRUE(graph) << graph.error().message;
    EXPECT_EQ(graph->command, "forge-graph-build");
    EXPECT_TRUE(graph->verified) << graph->reason;
    const auto has_yaml = repository_has_yaml(*author, *rebased);
    ASSERT_TRUE(has_yaml) << has_yaml.error().message;
    EXPECT_FALSE(*has_yaml);

    server = std::make_unique<lake::api::ApiServer>(options);
    ASSERT_TRUE(server->start()) << server->reason();
    const std::string rebased_repository_url =
        "http://127.0.0.1:" + std::to_string(server->port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint rebased_sync(
        rebased_repository_url, liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    liquid::forge_client rebased_reviewer(rebased_repository_url,
                                          {.bearer_token = options.admin_token});

    const auto mapped =
        liquid::runtime::block_on(rebased_reviewer.advance_anchors(*author, review->id, *rebased));
    ASSERT_TRUE(mapped) << mapped.error().message;
    ASSERT_EQ(mapped->size(), 1U);
    EXPECT_EQ(mapped->front().body, "Keep the reviewed assignment.");
    EXPECT_EQ(liquid::to_string(mapped->front().anchor.change.value),
              liquid::to_string(rebased->value));
    EXPECT_EQ(mapped->front().anchor.line, 3U);
    ASSERT_TRUE(mapped->front().mapped);
    EXPECT_TRUE(mapped->front().mapped->moved);
    const auto approved = liquid::runtime::block_on(
        rebased_reviewer.set_review_state(review->id, liquid::forge_review_state::approved));
    ASSERT_TRUE(approved) << approved.error().message;

    ASSERT_TRUE(liquid::runtime::block_on(author->set_ref("main", *rebased)));
    const auto landed = liquid::runtime::block_on(rebased_sync.push(*author));
    ASSERT_TRUE(landed) << landed.error().message;
    const auto* main_outcome = reference_outcome(*landed, "main");
    ASSERT_NE(main_outcome, nullptr);
    EXPECT_EQ(main_outcome->outcome, liquid::sync_ref_outcome_kind::applied);
    const auto landed_head = ref_head(server->port(), options.admin_token, repository->id, "main");
    ASSERT_TRUE(landed_head) << landed_head.error().message;
    EXPECT_EQ(*landed_head, liquid::to_string(rebased->value));

    const std::string landed_change = http_get(
        server->port(),
        "/v1/get_change?id=" + repository->id + "&change_id=" + liquid::to_string(rebased->value),
        options.admin_token);
    EXPECT_NE(landed_change.find(" 200 "), std::string::npos) << landed_change;
    EXPECT_NE(landed_change.find("Rebase forge change"), std::string::npos);
    // The change's checks: its build result, provenance and graph evidence.
    const std::string checks = http_get(server->port(),
                                        "/v1/list_artifacts?id=" + repository->id + "&limit=100",
                                        options.admin_token);
    EXPECT_NE(checks.find(" 200 "), std::string::npos) << checks;
    EXPECT_NE(checks.find("build/forge-proof.txt"), std::string::npos) << checks;
    EXPECT_NE(checks.find("slsa_provenance/1"), std::string::npos);
    EXPECT_NE(checks.find(liquid::to_string(graph->result.value)), std::string::npos);
    server->stop();
}
