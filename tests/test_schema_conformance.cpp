#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

#include <liquid/liquid.hpp>
#include <nlohmann/json.hpp>

#include "http1/client.hpp"
#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "objects/records.hpp"
#include "temporary_directory.hpp"
#include "stream_fixture.hpp"
#include "experiment_fixture.hpp"

extern char** environ;

namespace {
using nlohmann::json;

std::vector<std::byte> bytes(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

template <class Result> auto checked(Result result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

class HttpEvidence {
public:
    explicit HttpEvidence(unsigned short port) : url_("http://127.0.0.1:" + std::to_string(port)) {
        document["openapi"] = request("GET", "/v1/openapi.json").second;
        document["tools"] =
            request("POST", "/mcp", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})
                .second.at("result")
                .at("tools");
        document["calls"] = json::array();
        document["rest"] = json::array();
    }

    std::pair<unsigned, json>
    request(const std::string& method, const std::string& path, const json& input = nullptr) {
        const auto response = checked(liquid::runtime::block_on(liquid::http1::request(
            {.url = url_,
             .method = method,
             .suffix = path,
             .headers = {{"Authorization", "Bearer " + bearer},
                         {"Content-Type", "application/json"}},
             .body = input.is_null() ? std::vector<std::byte>{} : bytes(input.dump())})));
        const std::string body(reinterpret_cast<const char*>(response.body.data()),
                               response.body.size());
        return {response.status, json::parse(body)};
    }

    json call(const std::string& name, const json& input = json::object(), bool success = true) {
        const auto [status, reply] = request("POST",
                                             "/mcp",
                                             {{"jsonrpc", "2.0"},
                                              {"id", 1},
                                              {"method", "tools/call"},
                                              {"params", {{"name", name}, {"arguments", input}}}});
        EXPECT_EQ(status, 200U) << reply;
        const auto& result = reply.at("result");
        const auto& envelope = result.at("structuredContent");
        EXPECT_EQ(result.at("isError"), !envelope.at("ok").get<bool>());
        EXPECT_EQ(json::parse(result.at("content").at(0).at("text").get<std::string>()), envelope);
        document["calls"].push_back({{"name", name}, {"arguments", input}, {"result", result}});
        if (success && !envelope.at("ok").get<bool>()) {
            throw std::runtime_error(name + ": " + envelope.dump());
        }
        return envelope.value("value", json::object());
    }

    void read(const std::string& name, const std::string& query = "") {
        const auto [status, response] = request("GET", "/v1/" + name + query);
        EXPECT_EQ(status, 200U) << response;
        document["rest"].push_back({{"name", name}, {"status", status}, {"body", response}});
    }

    json document;
    // The operator credential by default; a person's session secret for capabilities that
    // refuse an operator (organization self-service).
    std::string bearer = "schema-test";

private:
    std::string url_;
};

std::string prepare_conflict(const std::filesystem::path& root,
                             const std::string& customer,
                             const std::string& repository) {
    auto store = checked(liquid::runtime::block_on(liquid::store::open(root / customer / "store")));
    auto source = checked(liquid::runtime::block_on(liquid::repository::create(
        root / customer / "repositories" / repository, {.store = &store})));
    std::vector<liquid::digest> references;
    for (const std::string value : {"base", "left", "right"}) {
        const auto written = liquid::runtime::block_on(source.write("document.txt", bytes(value)));
        if (!written) {
            throw std::runtime_error(written.error().message);
        }
        const auto tree = checked(liquid::runtime::block_on(source.root()));
        references.push_back(tree.entries.at(0).ref);
    }
    const auto conflict = checked(liquid::runtime::block_on(source.record_conflict(
        {.base = references.at(0),
         .left = references.at(1),
         .right = references.at(2),
         .region = {.path = "document.txt", .bytes = liquid::conflict_byte_range{0, 4}},
         .kind = liquid::conflict_kind::text,
         .explanation = "Both writers changed the same text."})));
    checked(liquid::runtime::block_on(source.cut("record schema fixture conflict")));
    return liquid::to_string(conflict);
}

std::string signed_attestation() {
    liquid::attestation statement{
        .subject = liquid::digest{liquid::algorithm::blake3, {}},
        .profile = "authorship/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = liquid::attestation_subject_kind::drip,
        .attributes = {{"author", "taap:1:ed25519:schema-subject"}},
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

void check_uploads(HttpEvidence& http, const std::string& repository) {
    const std::vector<std::pair<json, std::string>> malformed{
        {json::array({1}), "/files/0"},
        {json::array({nullptr}), "/files/0"},
        {json::array({json::object()}), "/files/0/name"},
        {json::array({{{"name", "sample.txt"}}}), "/files/0/content"},
        {json::array({{{"name", 1}, {"content", ""}}}), "/files/0/name"},
        {json::array({{{"name", "sample.txt"}, {"content", 1}}}), "/files/0/content"},
        {json::array({{{"name", "sample.txt"}, {"content", "%%%="}}}), "/files/0/content"},
        {json::array({{{"name", "sample.txt"}, {"content", ""}, {"extra", true}}}),
         "/files/0/extra"},
        {json::array({{{"name", "sample.txt"}, {"content", ""}}, false}), "/files/1"},
    };
    for (const auto& [files, path] : malformed) {
        const json input{
            {"repository_id", repository}, {"ref", "main"}, {"path", ""}, {"files", files}};
        const auto [status, body] = http.request("POST", "/v1/upload_drive_files", input);
        EXPECT_EQ(status, 400U) << body;
        EXPECT_EQ(body.at("error").at("code"), "invalid_input");
        EXPECT_EQ(body.at("error").at("path"), path);
        http.call("upload_drive_files", input, false);
        const auto& error =
            http.document.at("calls").back().at("result").at("structuredContent").at("error");
        EXPECT_EQ(error.at("code"), "invalid_input");
        EXPECT_EQ(error.at("path"), path);
        http.document["rest"].push_back(
            {{"name", "upload_drive_files"}, {"status", status}, {"body", body}});
    }
    http.call("upload_drive_files",
              {{"repository_id", repository},
               {"ref", "main"},
               {"path", "shared"},
               {"files",
                {{{"name", "empty.txt"}, {"content", ""}},
                 {{"name", "sample.txt"}, {"content", "aGVsbG8="}}}}});
}

void check_membership(HttpEvidence& http, const json& user) {
    const auto organization =
        http.call("create_organization", {{"display_name", "Schema members"}});
    const auto membership = http.call("create_membership",
                                      {{"organization_id", organization.at("id")},
                                       {"principal_type", "user"},
                                       {"principal_id", user.at("id")},
                                       {"principal_taap_id", user.at("taap_id")},
                                       {"role", "member"},
                                       {"attestation_digest", "sha512-256:schema-membership"}});
    http.call("remove_member", {{"id", membership.at("id")}});
}

// create_organization_space and convert_personal_space serve a signed-in person and refuse the
// operator credential, so they are called with a person's session.
void check_person_organizations(HttpEvidence& http, lake::control::Store& store) {
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto session = checked(store.sign_up_password({"lake",
                                                         "schema-person-" + suffix + "@example.test",
                                                         "correct horse battery staple",
                                                         "Schema person",
                                                         "schema",
                                                         "{}"}));
    const std::string personal =
        checked(store.list_customers(2, session.session.user_id)).at(0).id;
    const std::string operator_bearer = http.bearer;
    http.bearer = session.secret;
    http.call("create_organization_space", {{"display_name", "Schema organization"}});
    http.call("convert_personal_space",
              {{"customer_id", personal}, {"display_name", "Schema converted"}});
    http.bearer = operator_bearer;
}

void check_proofs(HttpEvidence& http) {
    std::ifstream input(LAKE_TEST_SOURCE_DIR "/tests/fixtures/schema-tuf.json");
    const auto metadata = json::parse(input);
    http.call("initialize_tuf", {{"root", metadata.at("root")}});
    http.call("publish_tuf_metadata",
              {{"targets", metadata.at("targets")},
               {"snapshot", metadata.at("snapshot")},
               {"timestamp", metadata.at("timestamp")}});
    const auto attestation = signed_attestation();
    http.call("append_transparency_entries", {{"attestations", {attestation, attestation}}});
    http.call("publish_transparency_checkpoint");
    http.call("transparency_inclusion_proof", {{"leaf_index", 0}, {"tree_size", 2}});
    http.call("transparency_consistency_proof", {{"old_tree_size", 1}, {"new_tree_size", 2}});
    http.call("transparency_proof_bundle", {{"leaf_index", 0}});
    http.read("transparency_inclusion_proof", "?leaf_index=0&tree_size=2");
    http.read("transparency_consistency_proof", "?old_tree_size=1&new_tree_size=2");
    http.read("transparency_proof_bundle", "?leaf_index=0");
}

void validate_evidence(const json& document) {
    const auto path = lake::test::fixture_path("schema-responses.json");
    std::ofstream(path) << document.dump(2);
    if (const char* retained = std::getenv("LAKE_SCHEMA_EVIDENCE")) {
        std::ofstream(retained) << document.dump(2);
    }
    std::vector<std::string> arguments{
        LAKE_TEST_PYTHON, LAKE_TEST_SOURCE_DIR "/tests/validate_api_schema.py", path.string()};
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    ASSERT_EQ(::posix_spawn(&pid, argv.front(), nullptr, nullptr, argv.data(), environ), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SchemaConformance, EveryToolAndRequiredOutputUsesHttpContract) {
    // 32 bytes of hex, as LAKE_INSTANCE_KEK is: workspace records (Streams ownership) derive from it.
    constexpr const char* kek = "736368656d612d636f6e666f726d616e63652d696e7374616e63652d6b656b31";
    const auto root = lake::test::fixture_path("schema-server");
    const auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::string suffix = lake::control::new_uuid_v4();
    const auto user =
        checked(store->create_user({"taap:1:ed25519:schema-" + suffix, "", "Schema owner"}));
    const auto customer =
        checked(store->create_customer({"individual", user.id, "", "schema-" + suffix}));
    const auto repository =
        checked(store->create_repository({customer.id, "schema-fixture", "", "private"}));
    const auto conflict = prepare_conflict(root, customer.id, repository.id);
    lake::test::StreamWorkspace streams(store, customer.id);
    lake::test::ExperimentWorkspace experiments(store, customer.id);
    experiments.caller->fallback = streams.caller;
    lake::api::ApiServer server({.port = 0,
                                 .node_name = "schema-test",
                                 .store = store,
                                 .workspaces = experiments.service,
                                 .admin_token = "schema-test",
                                 .instance_kek = kek,
                                 .store_root = root.string()});
    ASSERT_TRUE(server.start()) << server.reason();
    HttpEvidence http(server.port());
    for (const auto& tool : http.document.at("tools")) {
        http.call(tool.at("name"), json::object(), false);
    }
    http.call("server_info");
    for (const auto& [name, input] : lake::test::experiment_calls(customer.id, experiments.id)) {
        http.call(name, input);
        const auto [status, body] = http.request("POST", "/v1/" + name, input);
        EXPECT_EQ(status, 200U);
        http.document["rest"].push_back({{"name", name}, {"status", status}, {"body", body}});
    }
    json stream{{"customer_id", customer.id}, {"workspace_id", experiments.id},
                {"zetta", "workspace"}, {"topic", "schema-orders"}};
    http.call("create_stream", stream);
    http.call("list_streams", {{"customer_id", customer.id}, {"workspace_id", experiments.id}});
    http.call("get_stream", stream);
    http.call("browse_stream", stream);
    auto stream_publish = stream;
    stream_publish["payload"] = "{}";
    http.call("publish_stream_message", stream_publish);
    stream_publish["sequence"] = 1;
    stream_publish["expected_revision"] = 1;
    http.call("update_stream_message", stream_publish);
    auto stream_update = stream;
    stream_update["settings"] = {{"max_events", 100}};
    http.call("update_stream", stream_update);
    auto stream_delete = stream;
    stream_delete["before_sequence"] = 2;
    stream_delete["confirm_count"] = 1;
    http.call("delete_stream_messages", stream_delete);
    stream["confirm_name"] = "schema-orders";
    http.call("delete_stream", stream);
    http.read("server_info");
    http.call("set_storage_quota", {{"customer_id", customer.id}, {"quota_bytes", 0}});
    http.call("metering_usage", {{"customer_id", customer.id}});
    http.read("metering_usage", "?customer_id=" + customer.id);
    http.call("list_conflicts", {{"id", repository.id}});
    http.read("list_conflicts", "?id=" + repository.id);
    http.call("resolve_conflict",
              {{"repository_id", repository.id},
               {"ref", "main"},
               {"path", "document.txt"},
               {"conflict_id", conflict},
               {"resolution", "left"}});
    http.call(
        "create_drive_folder",
        {{"repository_id", repository.id}, {"ref", "main"}, {"path", ""}, {"name", "shared"}});
    check_uploads(http, repository.id);
    const auto link = http.call("create_drive_share_link",
                                {{"repository_id", repository.id},
                                 {"ref", "main"},
                                 {"path", "shared"},
                                 {"role", "read"},
                                 {"expires_at", "2099-01-01T00:00:00Z"}});
    const json linked_file{
        {"token", link.at("token")}, {"ref", "main"}, {"path", "shared/sample.txt"}};
    http.call("preview_drive_share_link", linked_file);
    http.call("download_drive_share_link", linked_file);
    check_membership(http, lake::control::to_json(user));
    check_person_organizations(http, *store);
    check_proofs(http);
    validate_evidence(http.document);
}
} // namespace
