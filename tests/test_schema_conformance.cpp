// SPDX-License-Identifier: MIT

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

#include "api/agent_task_records_api.hpp"
#include "experiment_fixture.hpp"
#include "http1/client.hpp"
#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/network.hpp"
#include "mcp_test_server.hpp"
#include "objects/records.hpp"
#include "one_thread_io.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"
#include "tunnel_fixture.hpp"
#include "zetta_object_fixture.hpp"

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

// A payment provider whose invoices have a request to send (lake#857), so resend_invoice answers
// in the contract; payments are recorded by hand, as the manual provider's are.
struct SchemaPaymentProvider final : lake::api::PaymentProvider {
    std::string_view name() const override {
        return "schema";
    }
    std::string_view title() const override {
        return "Schema";
    }
    std::expected<lake::api::PaymentRequest, std::string>
    request_payment(const lake::api::PaymentAccount&,
                    const lake::control::Invoice& invoice) override {
        return lake::api::PaymentRequest{"schema-" + invoice.period, "Pay by wire.", {}, "open"};
    }
    bool records_payments_by_hand() const override {
        return true;
    }
    // The balance's checkouts (lake#1114), so start_top_up and set_auto_top_up answer.
    std::expected<lake::api::CheckoutSession, std::string>
    create_balance_checkout(const lake::api::PaymentAccount&,
                            const lake::api::BalanceCheckoutRequest& request) override {
        return lake::api::CheckoutSession{"cs_schema_" + request.purpose,
                                          "https://checkout.schema.test/" + request.purpose};
    }
};

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
        const auto response = checked(lake::test::http_request(
            {.url = url_,
             .method = method,
             .suffix = path,
             .headers = {{"Authorization", "Bearer " + bearer},
                         {"Content-Type", "application/json"}},
             .body = input.is_null() ? std::vector<std::byte>{} : bytes(input.dump())}));
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
        // `path` or `name` (lake#587): the content is the one field every item needs.
        {json::array({json::object()}), "/files/0/content"},
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
                                       {"role", "member"}});
    http.call("remove_member", {{"id", membership.at("id")}});
}

// Coupon codes (lake#1156): the operator creates, lists and disables a code; a person who signed
// up after it redeems it for their own account.
void check_coupons(HttpEvidence& http, lake::control::Store& store) {
    const auto created = http.call("create_coupon",
                                   {{"code", "schema-50"},
                                    {"amount_micros", 50'000'000},
                                    {"credit_days", 90},
                                    {"max_redemptions", 10},
                                    {"campaign", "schema"}});
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto session =
        checked(store.sign_up_password({"lake",
                                        "schema-coupon-" + suffix + "@example.test",
                                        "correct horse battery staple",
                                        "Schema coupon",
                                        "schema",
                                        "{}"}));
    const std::string personal = checked(store.list_customers(2, session.session.user_id)).at(0).id;
    const std::string operator_bearer = http.bearer;
    http.bearer = session.secret;
    http.call("redeem_coupon", {{"customer_id", personal}, {"code", "SCHEMA50"}});
    http.bearer = operator_bearer;
    http.call("list_coupons");
    http.read("list_coupons", "?campaign=schema");
    http.call("disable_coupon", {{"coupon_id", created.at("coupon").at("id")}});
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

// The web app's capabilities (lake#561): the reads it needs and the Ask Orama and Agent Factory
// conversation, as a person with write on the space.
void check_web_app(HttpEvidence& http,
                   lake::control::InMemoryStore& store,
                   lake::test::ExperimentWorkspace& experiments,
                   const std::string& kek,
                   const lake::control::Customer& customer,
                   const lake::control::Repository& repository) {
    const json space{{"customer_id", customer.id}, {"workspace_id", experiments.id}};
    const auto with = [&](const json& extra) {
        json input = space;
        input.update(extra);
        return input;
    };
    http.call("my_permissions",
              {{"object_type", "customer"},
               {"object_id", customer.id},
               {"actions", {"read", "write", "promote"}}});
    http.call("resolve_model", {{"namespace", customer.name}, {"name", repository.name}});
    auto& answers = experiments.caller->answers;
    answers["capture_intent"] = {{"input_id", "intent-1"}};
    answers["compile_intent"] = {{"name", "schema-graph"}};
    answers["get_graph"] = {
        {"graph_id", "graph-1"},
        {"name", "schema-graph"},
        {"project", "schema"},
        {"nodes",
         {{{"id", "load"}, {"kind", "connect"}, {"name", "Load"}},
          {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}}},
        {"edges", {{{"from", "load"}, {"to", "train"}, {"kind", "data_flow"}}}}};
    http.call("compile_pipeline", with({{"project", "schema"}, {"intent", "Predict churn"}}));
    http.call("render_pipeline",
              with({{"project", "schema"}, {"graph", "schema-graph"}, {"step", "train"}}));

    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto session = checked(store.sign_up_password({"lake",
                                                         "schema-asker-" + suffix + "@example.test",
                                                         "correct horse battery staple",
                                                         "Schema asker",
                                                         "schema",
                                                         "{}"}));
    const auto person = checked(store.get_user(session.session.user_id));
    const auto object = checked(store.find_policy_object("customer", customer.id));
    checked(store.create_grant(
        {person.taap_id, object.id, "write", "schema-asker-attestation", "admin", "schema"}));
    const std::string operator_bearer = http.bearer;
    http.bearer = session.secret;
    http.call("viewer", {{"workspace_id", experiments.id}});

    // An agent of the space answers at once; its proposal runs only through the conversation.
    answers["get_agent"] = {{"id", "schema-agent"}, {"runtime", "native"}};
    answers["chat"] = {{"reply", "You have one repository."},
                       {"proposals", {{{"capability", "server_info"}, {"input", json::object()}}}},
                       {"reads", json::array()},
                       {"delegations", json::array()},
                       {"trace_id", ""}};
    const json sent = http.call("send_ask_message",
                                with({{"agent", "schema-agent"}, {"message", "What do I have?"}}));
    const json asked =
        http.call("get_ask_conversation", with({{"conversation_id", sent.at("conversation_id")}}))
            .at("messages")
            .back();
    http.call("confirm_ask_proposal",
              with({{"conversation_id", sent.at("conversation_id")},
                    {"message_id", sent.at("reply_id")},
                    {"index", 0},
                    {"input_sha256", asked.at("proposal_calls").at(0).at("input_sha256")}}));
    // The Quickstart's model step (lake#1182): the newest quickstart of this workspace, made
    // earlier in the test, talked through with the space's own chat agent, which answers at once.
    http.call("start_quickstart_conversation", with({{"agent", "schema-agent"}}));

    // Background turns: the Project Agent's build of a project, and an Agent Creator Agent proposal.
    const auto task = [&](std::string agent, std::string kind, std::string conversation) {
        return checked(store.create_agent_task({.id = lake::control::new_uuid_v4(),
                                                .customer_id = customer.id,
                                                .agent = std::move(agent),
                                                .kind = std::move(kind),
                                                .conversation_id = std::move(conversation),
                                                .workspace_id = experiments.id,
                                                .scopes = {"workspace_query"},
                                                .requested_by_user_id = person.id,
                                                .requested_by_taap_id = person.taap_id}));
    };
    task("agent-project", "build:schema", "");
    http.call("latest_agent_task", with({{"subject", "build:schema"}}));
    // A pending workspace scan, reported as list_opportunities' last_scan (lake#1195).
    task("agent-autosuggest", "discover", "");
    http.call("list_opportunities", with(json::object()));
    const std::string conversation = lake::control::new_uuid_v4();
    auto factory = task("agent-creator", "factory", conversation);
    lake::api::ApiContext context;
    context.store = &store;
    context.workspaces = experiments.service.get();
    context.instance_kek = kek;
    context.workspace_id = experiments.id;
    const auto must = [](auto completed) {
        const auto settled = lake::control::settled(std::move(completed));
        if (!settled) {
            throw std::runtime_error(settled.error().message);
        }
    };
    must(liquid::runtime::block_on(lake::api::write_agent_task_request(context, factory)));
    checked(store.claim_agent_task(factory.id, "agent-creator", std::string(64, 'a')));
    factory = checked(store.record_agent_task_event(factory.id, "agent-creator", "Succeeded"));
    const json answer{{"ok", true},
                      {"result",
                       {{"kind", "proposal"},
                        {"proposal",
                         {{"type", "native"},
                          {"agent", {{"name", "Schema watcher"}, {"purpose", "Watch renewals"}}}}},
                        {"defects", json::array()}}}};
    must(liquid::runtime::block_on(
        lake::api::write_agent_task_event(context, factory, "Succeeded", "Done", answer)));
    http.call(
        "settle_ask_turn",
        with({{"conversation_id", conversation}, {"task_id", factory.id}, {"kind", "factory"}}));
    const json messages =
        http.call("get_ask_conversation", with({{"conversation_id", conversation}})).at("messages");
    const json proposed = messages.back();
    answers["create_agent"] = {{"id", "agent-schema"}, {"name", "Schema watcher"}};
    http.call("approve_agent_proposal",
              with({{"conversation_id", conversation},
                    {"message_id", proposed.at("id")},
                    {"proposal_sha256", proposed.at("payload").at("proposal_sha256")}}));
    // Deleting the conversation (lake#645): the plan, then the deletion.
    http.call("delete_ask_conversation",
              with({{"conversation_id", conversation}, {"dry_run", true}}));
    http.call("delete_ask_conversation", with({{"conversation_id", conversation}}));
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

void check_network(HttpEvidence& http,
                   lake::control::Store& store,
                   const lake::control::Customer& customer,
                   const lake::control::User& user,
                   const std::string& workspace_id) {
    const auto session = checked(store.sign_up_password({"lake",
                                                         "schema-tunnel@example.test",
                                                         "correct horse battery staple",
                                                         "Tunnel developer",
                                                         "schema",
                                                         "{}"}));
    const auto developer = checked(store.get_user(session.session.user_id));
    const auto object = checked(store.find_policy_object("customer", customer.id));
    checked(store.create_grant({developer.taap_id,
                                object.id,
                                "read",
                                "schema-network-attestation",
                                "admin",
                                "schema-network-grant"}));
    const auto workspace = checked(store.get_workspace(workspace_id));
    const auto app = checked(store.create_web_app({.customer_id = customer.id,
                                                   .workspace_id = workspace_id,
                                                   .name = "schema-network-client",
                                                   .mode = "image",
                                                   .image = "client:v1",
                                                   .port = 3000,
                                                   .host = "schema-network-client.test"}));
    const json scope{{"customer_id", customer.id}, {"workspace_id", workspace_id}};
    auto connection_input = scope;
    connection_input.update({{"source_kind", "app"},
                             {"source_id", app.id},
                             {"target_kind", "service"},
                             {"target_id", workspace.zetta_service_id},
                             {"ports", json::array({4800})},
                             {"credential_scope", {{"access", "read"}}}});
    const auto connection = http.call("create_network_connection", connection_input);
    http.call("list_network_connections", scope);
    auto rotate = scope;
    rotate["connection_id"] = connection.at("id");
    http.call("rotate_network_connection_credential", rotate);

    auto egress_input = scope;
    egress_input.update({{"dns_name", "schema.example.test"}, {"ports", json::array({443})}});
    http.call("create_network_egress_rule", egress_input);
    http.call("list_network_egress_rules", scope);

    auto tunnel_input = scope;
    tunnel_input.update(
        {{"user_id", developer.id},
         {"target_kind", "service"},
         {"target_id", workspace.zetta_service_id},
         {"port", 4800},
         {"expires_at",
          lake::control::iso8601_at(std::chrono::system_clock::now() + std::chrono::hours(1))}});
    const auto tunnel = http.call("grant_network_tunnel", tunnel_input);
    auto& memory = dynamic_cast<lake::control::InMemoryStore&>(store);
    const auto observed =
        memory.set_network_tunnel_grant_status(tunnel.at("id"),
                                               {.phase = "Ready",
                                                .observed_generation = 1,
                                                .endpoint = "relay.test:7443",
                                                .relay_ca_pem = tunnel.at("client_ca_pem")});
    ASSERT_TRUE(observed);
    http.call("list_network_tunnel_grants", scope);
    http.call("list_network_peers", scope);
    http.call("network_rules", scope);
    http.call("list_network_tunnel_sessions", scope);
    http.call("network_activity", scope);

    auto credentials = scope;
    credentials.update(
        {{"tunnel_grant_id", tunnel.at("id")}, {"csr_pem", lake::test::tunnel_csr().pem}});
    // Even the operator cannot obtain a recipient's certificate.
    http.call("issue_network_tunnel_certificate", credentials, false);
    const auto operator_bearer = http.bearer;
    http.bearer = session.secret;
    http.call("issue_network_tunnel_certificate", credentials);
    const auto [status, body] =
        http.request("POST", "/v1/issue_network_tunnel_certificate", credentials);
    EXPECT_EQ(status, 200U) << body;
    http.document["rest"].push_back(
        {{"name", "issue_network_tunnel_certificate"}, {"status", status}, {"body", body}});
    http.bearer = operator_bearer;

    auto remove = scope;
    remove["connection_id"] = connection.at("id");
    http.call("remove_network_connection", remove);
    auto revoke = scope;
    revoke["tunnel_grant_id"] = tunnel.at("id");
    http.call("revoke_network_tunnel", revoke);
    http.bearer = session.secret;
    http.call("issue_network_tunnel_certificate", credentials, false);
    http.bearer = operator_bearer;
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
    // 32 bytes of hex, as LAKE_INSTANCE_KEK is: workspace records (Streams ownership, drive links) derive from it.
    constexpr const char* kek = "736368656d612d636f6e666f726d616e63652d696e7374616e63652d6b656b31";
    const auto root = lake::test::fixture_path("schema-server");
    const auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::string suffix = lake::control::new_uuid_v4();
    const auto user =
        checked(store->create_user({"taap:1:ed25519:schema-" + suffix, "", "Schema owner"}));
    const auto customer =
        checked(store->create_customer({"individual", user.id, "", "schema-" + suffix}));
    const auto repository =
        checked(store->create_repository({customer.id, "schema-fixture", "", "private",
                                          lake::test::repository_workspace(*store, customer.id)}));
    const auto conflict = prepare_conflict(root, customer.id, repository.id);
    lake::test::StreamWorkspace streams(store, customer.id);
    lake::test::ExperimentWorkspace experiments(store, customer.id);
    experiments.caller->fallback = streams.caller;
    // Billing reads model usage even with an empty price list, so every fixture workspace answers.
    const auto repository_home = checked(store->get_workspace(repository.workspace_id));
    const auto repository_service = checked(store->get_service(repository_home.zetta_service_id));
    ASSERT_TRUE(
        store->set_service_status(repository_service.id, "Ready", "", "http://repositories.test"));
    std::ofstream(std::filesystem::path(experiments.service->config().service_dir) /
                  (repository_service.slug + ".json"))
        << json{{"kind", "zetta"}, {"token", "repository-fixture-secret"}}.dump();
    const auto objects = std::make_shared<lake::test::FakeZettaObjects>();
    objects->add_node("http://experiments.test", "experiment-service-secret");
    lake::control::BillingPlan gateway_prices;
    gateway_prices.id = "usage-200001-00000000000000001";
    gateway_prices.name = "Usage";
    gateway_prices.currency = "USD";
    // An agent turn calls its workspace's model, gpt-6.1-sol when none is chosen (lake#1230); a
    // model with long-context rates is served only with them published.
    gateway_prices.prices = {{"model_tokens", 6, 1, 0, "gpt-5.5/input"},
                             {"model_tokens", 36, 1, 0, "gpt-5.5/output"},
                             {"model_tokens", 12, 1, 0, "gpt-5.5@long/input"},
                             {"model_tokens", 54, 1, 0, "gpt-5.5@long/output"},
                             {"model_tokens", 3, 1, 0, "gpt-6.1-sol/input"},
                             {"model_tokens", 13, 1, 0, "gpt-6.1-sol/output"},
                             {"model_tokens", 6, 1, 0, "gpt-6.1-sol@long/input"},
                             {"model_tokens", 20, 1, 0, "gpt-6.1-sol@long/output"}};
    checked(store->put_billing_plan(gateway_prices));
    lake::api::ApiServer server(
        {.port = 0,
         .node_name = "schema-test",
         .store = store,
         .workspaces = experiments.service,
         // Workspace files (drives) in the workspace Zetta's object routes, faked.
         .object_transport = objects,
         // Workspace methodologies build their image by the build path (lake#655).
         .methodology_runtime_image = "registry.test/oramasearch/methodology-runtime:1",
         .internal_url = "http://lake.schema.test",
         .admin_token = "schema-test",
         .instance_kek = kek,
         // A fake: tests never call OpenAI (docs/api/transcripts.md).
         .transcriber = [](const lake::api::TranscriptionRequest&)
             -> std::expected<lake::api::Transcription, lake::api::ApiError> {
             return lake::api::Transcription{"schema words", 1.5, "gpt-4o-transcribe"};
         },
         // MCP connections reach the local test server (docs/api/mcp-connections.md).
         .mcp_allow_private_addresses = true,
         // A fake model provider: the model gateway never calls OpenAI in tests (lake#248).
         .model_provider =
             [](const lake::api::ModelProviderRequest&) {
                 return lake::api::ModelProviderResponse{
                     200,
                     R"({"object":"chat.completion","model":"gpt-5.5","choices":[],)"
                     R"("usage":{"prompt_tokens":3,"completion_tokens":1}})",
                     {}};
             },
         // Billing on (lake#564), so its capabilities are in the contract too.
         .billing = true,
         .payment_providers =
             lake::api::fixed_payment_providers(std::make_shared<SchemaPaymentProvider>()),
         .store_root = root.string()});
    ASSERT_TRUE(server.start()) << server.reason();
    // The account has topped up (lake#1114), so the calls that start paid work run past the
    // balance's gate; PaidWork.* covers the gate itself.
    {
        lake::control::BalanceCredit credit;
        credit.customer_id = customer.id;
        credit.amount_micros = 20'000'000;
        credit.provider = "stripe";
        credit.reference = "cs_schema_first_top_up";
        checked(store->apply_balance_credit(credit));
    }
    HttpEvidence http(server.port());
    for (const auto& tool : http.document.at("tools")) {
        http.call(tool.at("name"), json::object(), false);
    }
    http.call("server_info");
    check_network(http, *store, customer, user, experiments.id);
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
    {
        // The model gateway answers only an agent run's own credential (docs/api/model-gateway.md).
        lake::control::AgentTask task;
        task.id = lake::control::new_uuid_v4();
        task.customer_id = customer.id;
        task.agent = "agent-ask-orama";
        task.kind = "turn";
        task.workspace_id = experiments.id;
        task.scopes = {"get_agent_task"};
        task.requested_by_user_id = user.id;
        checked(store->create_agent_task(task));
        const std::string credential = "lat." + task.id + ".0123456789abcdef0123456789abcdef";
        checked(store->claim_agent_task(
            task.id, task.agent, lake::control::identity_crypto::sha256_hex(credential)));
        const std::string operator_bearer = http.bearer;
        http.bearer = credential;
        http.call("model_completion",
                  {{"endpoint", "chat/completions"},
                   {"request",
                    {{"model", "gpt-5.5"},
                     {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}}}});
        http.bearer = operator_bearer;
    }
    const json transcript_scope{{"customer_id", customer.id}, {"workspace_id", experiments.id}};
    auto dictation = transcript_scope;
    dictation.update({{"name", "memo.webm"}, {"audio_base64", "c3BlZWNo"}});
    http.call("transcribe", dictation);
    auto transcript_input = transcript_scope;
    transcript_input.update({{"title", "Schema call"}, {"source", "text"}, {"text", "words"}});
    const auto transcript = http.call("create_transcript", transcript_input);
    auto from_audio = transcript_scope;
    from_audio.update(
        {{"title", "Schema memo"}, {"source", "recording"}, {"audio_base64", "c3BlZWNo"}});
    http.call("create_transcript", from_audio);
    http.call("list_transcripts", transcript_scope);
    http.read("list_transcripts",
              "?customer_id=" + customer.id + "&workspace_id=" + experiments.id + "&q=schema");
    auto transcript_id = transcript_scope;
    transcript_id["id"] = transcript.at("id");
    http.call("get_transcript", transcript_id);
    auto transcript_edit = transcript_id;
    transcript_edit["title"] = "Schema call, edited";
    http.call("update_transcript", transcript_edit);
    http.call("delete_transcript", transcript_id);
    // MCP connections against a local test MCP server (docs/api/mcp-connections.md, lake#652).
    lake::test::McpTestServer mcp_server;
    auto mcp_input = transcript_scope;
    mcp_input.update({{"name", "Schema MCP"}, {"url", mcp_server.url()}});
    const auto mcp = http.call("create_mcp_connection", mcp_input);
    http.call("list_mcp_connections", transcript_scope);
    auto mcp_id = transcript_scope;
    mcp_id["id"] = mcp.at("id");
    http.call("get_mcp_connection", mcp_id);
    auto mcp_tools = mcp_id;
    mcp_tools["tools"] = {{"echo", {{"allowed", true}}}, {"create_ticket", {{"allowed", true}}}};
    http.call("update_mcp_connection", mcp_tools);
    http.call("refresh_mcp_connection", mcp_id);
    http.call("test_mcp_connection", mcp_id);
    http.call("list_mcp_connection_tools", transcript_scope);
    auto mcp_call = transcript_scope;
    mcp_call.update(
        {{"connection_id", mcp.at("id")}, {"tool", "echo"}, {"arguments", {{"text", "hi"}}}});
    http.call("read_mcp_tool", mcp_call);
    mcp_call["tool"] = "create_ticket";
    mcp_call["arguments"] = {{"title", "Schema ticket"}};
    http.call("call_mcp_tool", mcp_call);
    auto mcp_grant = transcript_scope;
    mcp_grant.update({{"connection_id", mcp.at("id")},
                      {"tool", "create_ticket"},
                      {"agent", "agent-1"},
                      {"auto_approve", true}});
    http.call("set_mcp_tool_auto_approve", mcp_grant);
    http.call("list_mcp_connection_calls", mcp_id);
    http.call("delete_mcp_connection", mcp_id);
    // A workspace's own solutions (docs/api/solutions.md, lake#597).
    auto solution_input = transcript_scope;
    solution_input.update({{"name", "Schema solution"},
                           {"problem", "Late payments"},
                           {"decision", "Who to call first"},
                           {"measurement", {{"metric", "days sales outstanding"}}},
                           {"methodologies", json::array({"M-1"})}});
    const auto solution = http.call("create_solution", solution_input);
    http.call("list_solutions", transcript_scope);
    // An agent's definition revisions (docs/api/train-agents.md, lake#653): none for an unknown agent.
    auto revisions = transcript_scope;
    revisions["agent_id"] = "schema-agent";
    http.call("list_agent_revisions", revisions);
    auto solution_id = transcript_scope;
    solution_id["id"] = solution.at("solution").at("id");
    http.call("get_solution", solution_id);
    auto solution_edit = solution_id;
    solution_edit["name"] = "Schema solution, edited";
    http.call("update_solution", solution_edit);
    const auto copy = http.call("duplicate_solution", solution_id);
    auto copy_id = transcript_scope;
    copy_id["id"] = copy.at("solution").at("id");
    http.call("delete_solution", copy_id);
    // The quickstart process (docs/api/quickstart.md, lake#1128) over canned engine answers.
    auto& engine = experiments.caller->answers;
    engine["upload_dataset"] = {{"database", "datasets"}, {"table", "schema_movies"}, {"rows", 2}};
    engine["get_dataset"] = {{"name", "schema_movies"},
                             {"database", "datasets"},
                             {"rows", 2},
                             {"columns",
                              json::array({{{"name", "title"}, {"type", "string"}},
                                           {{"name", "popular"}, {"type", "integer"}}})}};
    engine["create_index"] = {{"index_id", "index-schema"},
                              {"fields", json::array({"popular", "title"})},
                              {"exposed", false}};
    engine["expose_index"] = {{"index_id", "index-schema"}, {"exposed", true}};
    engine["train_model"] = {{"run_id", "run-schema"}, {"status", "running"}};
    engine["get_run"] = {{"id", "run-schema"}, {"status", "succeeded"}};
    engine["list_models"] = {
        {"models", json::array({{{"id", "model-quickstart"}, {"run_id", "run-schema"}}})}};
    engine["expose_model"] = {{"model_id", "model-quickstart"}, {"exposed", true}};
    auto quickstart = transcript_scope;
    quickstart.update({{"content", "title,popular\nAlien,1\nBig,0\n"},
                       {"format", "csv"},
                       {"name", "schema_movies"}});
    const auto started = http.call("start_quickstart", quickstart);
    auto quickstart_id = transcript_scope;
    quickstart_id["quickstart_id"] = started.at("quickstart_id");
    http.call("quickstart_snippets", quickstart_id);
    // Browser keys (docs/api/browser-keys.md, lake#1152): bound to the quickstart's index.
    engine["get_index"] = {{"index_id", "index-schema"}, {"exposed", true}};
    auto browser_key = transcript_scope;
    browser_key.update(
        {{"index_id", "index-schema"}, {"origins", json::array({"https://shop.example.test"})}});
    const auto made_key = http.call("create_browser_key", browser_key);
    auto keyed_snippets = quickstart_id;
    keyed_snippets["origins"] = json::array({"https://shop.example.test"});
    http.call("quickstart_snippets", keyed_snippets);
    http.call("list_browser_keys", {{"customer_id", transcript_scope.at("customer_id")}});
    http.call(
        "revoke_browser_key",
        {{"customer_id", transcript_scope.at("customer_id")}, {"id", made_key.at("key").at("id")}});
    auto quickstart_training = quickstart_id;
    quickstart_training["target"] = "popular";
    http.call("train_quickstart", quickstart_training);
    http.call("quickstart_status", quickstart_id);
    http.call("train_quickstart", quickstart_id);
    // A workspace's own methodologies (docs/api/methodologies.md, lake#655): the engine's
    // registry is canned; the code is committed to a real repository of the workspace.
    json own_methodology{{"id", "W-0000000a"},
                         {"name", "Schema payers"},
                         {"source", "workspace"},
                         {"editable", true},
                         {"code", {{"repository_id", ""}, {"revision", "main"}, {"path", ""}}},
                         {"validation", {{"status", "not_validated"}}},
                         {"network", false}};
    experiments.caller->answers["register_methodology"] = {{"methodology", own_methodology}};
    auto methodology_draft = transcript_scope;
    methodology_draft["description"] = "Which customers will pay late?";
    http.call("draft_methodology", methodology_draft);
    auto methodology_input = methodology_draft;
    methodology_input["name"] = "Schema payers";
    http.call("create_methodology", methodology_input);
    for (const auto& repository : http.call("list_repositories", {{"customer_id", customer.id}})
                                      .value("items", json::array())) {
        if (repository.value("name", std::string()).starts_with("methodology-schema-payers")) {
            own_methodology["code"]["repository_id"] = repository.at("id");
        }
    }
    ASSERT_FALSE(own_methodology["code"]["repository_id"].get<std::string>().empty());
    experiments.caller->answers["get_methodology"] = {{"methodology", own_methodology}};
    experiments.caller->answers["update_methodology"] = {{"methodology", own_methodology}};
    experiments.caller->answers["delete_methodology"] = {{"removed", true}, {"id", "W-0000000a"}};
    auto methodology_id = transcript_scope;
    methodology_id["id"] = "W-0000000a";
    auto with_source = methodology_id;
    with_source["include_source"] = true;
    http.call("get_methodology", with_source);
    auto methodology_network = methodology_id;
    methodology_network["network"] = true;
    http.call("update_methodology", methodology_network);
    http.call("duplicate_methodology", methodology_id);
    const auto validating = http.call("validate_methodology", methodology_id);
    const std::string validate_run = validating.at("run").at("id");
    own_methodology["validation"] = {{"status", "passed"}, {"revision", "main"}};
    experiments.caller->answers["get_methodology"] = {{"methodology", own_methodology}};
    auto methodology_train = methodology_id;
    methodology_train.update({{"dataset", "datasets.schema_payers"}, {"target", "late"}});
    const auto training = http.call("run_methodology", methodology_train);
    http.call("list_methodology_runs", transcript_scope);
    auto methodology_run = transcript_scope;
    methodology_run["run_id"] = training.at("run").at("id");
    // Held back by the operator (orama-operator#293, migration 0082): the phase is Queued.
    lake::control::MethodologyRunStatus queued;
    queued.phase = "Queued";
    queued.message = "Queued: 2 of this workspace's methodology runs are already running (the limit "
                     "is 2); this run is next.";
    ASSERT_TRUE(store->put_methodology_run_status(training.at("run").at("id"), queued));
    const auto queued_run = http.call("get_methodology_run", methodology_run);
    EXPECT_EQ(queued_run.at("run").at("phase"), "Queued");
    EXPECT_EQ(queued_run.at("run").at("message"), queued.message);
    // The runner's side, with the run's own credential (the operator writes its digest).
    const std::string run_secret = "lmr." + validate_run + "." + std::string(48, 'f');
    lake::control::MethodologyRunStatus staging;
    staging.phase = "Staging";
    staging.credential_sha256 = lake::control::identity_crypto::sha256_hex(run_secret);
    ASSERT_TRUE(store->put_methodology_run_status(validate_run, staging));
    const std::string methodology_operator = http.bearer;
    http.bearer = run_secret;
    http.call("methodology_run_input", {{"run_id", validate_run}});
    http.call(
        "methodology_run_output",
        {{"run_id", validate_run}, {"result", {{"status", "ok"}, {"findings", json::array()}}}});
    // An artifact over 24 MiB, in chunks, with the training run's credential (lake#690).
    const std::string train_secret =
        "lmr." + training.at("run").at("id").get<std::string>() + "." + std::string(48, 'e');
    lake::control::MethodologyRunStatus collecting;
    collecting.phase = "Collecting";
    collecting.credential_sha256 = lake::control::identity_crypto::sha256_hex(train_secret);
    ASSERT_TRUE(store->put_methodology_run_status(training.at("run").at("id"), collecting));
    experiments.caller->answers["append_methodology_artifact"] = {{"received", 3}};
    http.bearer = train_secret;
    http.call("methodology_run_artifact",
              {{"run_id", training.at("run").at("id")}, {"offset", 0}, {"chunk_base64", "YWJj"}});
    // A served model (lake#690): its deployment, and the stage's side with its own credential.
    http.bearer = methodology_operator;
    auto served_model = transcript_scope;
    served_model["model_id"] = "model-schema";
    http.call("get_methodology_deployment", served_model);
    http.call("list_methodology_deployments", transcript_scope);
    lake::control::NewMethodologyDeployment served;
    served.customer_id = customer.id;
    served.workspace_id = experiments.id;
    served.methodology_id = "W-0000000a";
    served.model_id = "model-schema";
    served.run_id = training.at("run").at("id");
    served.runtime_image = "registry.test/oramasearch/methodology-runtime:1";
    served.cpu_millis = 1000;
    served.memory_bytes = 1 << 30;
    const auto deployment = store->create_methodology_deployment(served);
    ASSERT_TRUE(deployment) << deployment.error().message;
    const std::string deployment_secret = "lmd." + deployment->id + "." + std::string(48, 'd');
    lake::control::MethodologyDeploymentStatus deployment_staging;
    deployment_staging.phase = "Staging";
    deployment_staging.credential_sha256 = lake::control::identity_crypto::sha256_hex(deployment_secret);
    ASSERT_TRUE(store->put_methodology_deployment_status(deployment->id, deployment_staging));
    experiments.caller->answers["read_model_weights"] = {{"model_id", "model-schema"},
                                                         {"offset", 0},
                                                         {"chunk_base64", "YWJj"},
                                                         {"next_offset", nullptr},
                                                         {"total_bytes", 3},
                                                         {"sha256", std::string(64, 'a')}};
    experiments.caller->answers["get_model"] = {
        {"id", "model-schema"},
        {"kind", "python:W-0000000a"},
        {"feature_schema", {{"features", {"days"}}, {"artifact_format", "bytes"}}}};
    http.bearer = deployment_secret;
    http.call("methodology_deployment_input", {{"deployment_id", deployment->id}});
    http.bearer = methodology_operator;
    http.call("get_methodology_deployment", served_model);
    http.call("stop_methodology_run", methodology_run);
    http.call("delete_methodology", methodology_id);
    // Drives (docs/api/drives.md): a workspace-files drive in the workspace Zetta's fake object
    // store, and a virtual one.
    auto drive_input = transcript_scope;
    drive_input.update({{"name", "Schema files"}, {"storage", "files"}});
    const auto drive = http.call("create_drive", drive_input);
    auto drive_scope = transcript_scope;
    drive_scope["drive_id"] = drive.at("id");
    auto virtual_input = transcript_scope;
    virtual_input.update({{"name", "Schema links"}, {"storage", "virtual"}});
    http.call("create_drive", virtual_input);
    http.call("list_drives", transcript_scope);
    http.read("list_drives", "?customer_id=" + customer.id + "&workspace_id=" + experiments.id);
    auto drive_id = transcript_scope;
    drive_id["id"] = drive.at("id");
    http.call("get_drive", drive_id);
    auto drive_rename = drive_id;
    drive_rename["name"] = "Schema files, renamed";
    http.call("rename_drive", drive_rename);
    auto folder_input = drive_scope;
    folder_input["path"] = "Reports/2026";
    const auto folder = http.call("create_drive_folder_entry", folder_input);
    auto small_input = drive_scope;
    small_input["files"] = json::array({{{"path", "notes.txt"}, {"content", "aGVsbG8="}}});
    const auto small = http.call("upload_drive_entries", small_input);
    auto begin_input = drive_scope;
    begin_input.update({{"path", "Reports/data.csv"}, {"size", 4}});
    const auto begun = http.call("begin_drive_upload", begin_input);
    auto chunk = drive_scope;
    chunk.update({{"id", begun.at("entry").at("id")}, {"offset", 0}, {"content", "YSxiCg=="}});
    http.call("append_drive_upload", chunk);
    auto finish = drive_scope;
    finish["id"] = begun.at("entry").at("id");
    http.call("finish_drive_upload", finish);
    http.call("list_drive_entries", drive_scope);
    http.read("list_drive_entries",
              "?customer_id=" + customer.id + "&workspace_id=" + experiments.id +
                  "&drive_id=" + drive.at("id").get<std::string>());
    auto entry_id = drive_scope;
    entry_id["id"] = small.at("items").at(0).at("id");
    http.call("get_drive_entry", entry_id);
    http.call("read_drive_file", entry_id);
    http.call("recent_drive_entries", transcript_scope);
    auto search_input = transcript_scope;
    search_input["q"] = "notes";
    http.call("search_drives", search_input);
    http.read("search_drives",
              "?customer_id=" + customer.id + "&workspace_id=" + experiments.id + "&q=report");
    auto shortcut_input = drive_scope;
    shortcut_input.update(
        {{"name", "Orama"}, {"target", {{"kind", "url"}, {"url", "https://orama.test/"}}}});
    http.call("create_drive_shortcut", shortcut_input);
    auto move_input = entry_id;
    move_input["parent_id"] = folder.at("id");
    http.call("move_drive_entry", move_input);
    http.call("delete_drive_entry", entry_id);
    auto drive_delete = drive_id;
    drive_delete["confirm_name"] = "Schema files, renamed";
    http.call("delete_drive", drive_delete);
    http.read("server_info");
    http.call("set_storage_quota", {{"customer_id", customer.id}, {"quota_bytes", 0}});
    http.call("metering_usage", {{"customer_id", customer.id}});
    http.read("metering_usage", "?customer_id=" + customer.id);
    // Usage over time (lake#564): a reading, the limits and setting one.
    http.call("set_usage_limit",
              {{"customer_id", customer.id}, {"dimension", "calls"}, {"limit", 1000}});
    http.call("usage_limits", {{"customer_id", customer.id}});
    http.call("usage_over_time",
              {{"customer_id", customer.id},
               {"from", "2026-10-01T00:00:00Z"},
               {"to", "2026-10-03T00:00:00Z"},
               {"granularity", "day"}});
    http.read("usage_over_time",
              "?customer_id=" + customer.id +
                  "&from=2026-10-01T00:00:00Z&to=2026-10-02T00:00:00Z&granularity=hour");
    // Billing (lake#564, lake#635): the price list, a preview, and a month invoiced and paid.
    http.call(
        "set_price_list",
        {{"prices",
          json::array(
              {{{"dimension", "calls"}, {"unit_micros", 1000}},
               {{"dimension", "model_tokens"}, {"class", "gpt-5.5/input"}, {"unit_micros", 6}},
               {{"dimension", "model_tokens"},
                {"class", "gpt-5.5/output"},
                {"unit_micros", 36}},
               {{"dimension", "model_tokens"}, {"class", "gpt-6.1-sol/input"}, {"unit_micros", 3}},
               {{"dimension", "model_tokens"},
                {"class", "gpt-6.1-sol/output"},
                {"unit_micros", 13}}})}});
    http.call("price_list");
    http.read("price_list", "");
    http.call("price_list", {{"include_history", true}});
    http.call("billing_account", {{"customer_id", customer.id}});
    http.read("billing_account", "?customer_id=" + customer.id);
    http.call("set_billing_contact",
              {{"customer_id", customer.id}, {"email", "billing@example.com"}, {"country", "DE"}});
    http.call("bill_preview", {{"customer_id", customer.id}});
    // The prepaid balance (lake#1114): this account topped up above.
    http.call("account_balance", {{"customer_id", customer.id}});
    http.read("account_balance", "?customer_id=" + customer.id);
    http.call("refresh_account_balance", {{"customer_id", customer.id}});
    http.call("set_balance_overdraft",
              {{"customer_id", customer.id}, {"overdraft_micros", 75'000'000}});
    http.call("set_balance_overdraft", {{"customer_id", customer.id}});
    // Credits from Orama (lake#1134): given, then its unused remainder revoked.
    const auto granted = http.call("grant_balance_credit",
                                   {{"customer_id", customer.id},
                                    {"amount_micros", 5'000'000},
                                    {"reason", "schema conformance"},
                                    {"expires_at", "2030-01-01T00:00:00Z"}});
    http.call("revoke_balance_credit",
              {{"credit_id", granted.at("credit").at("id")}, {"reason", "schema conformance"}});
    check_coupons(http, *store);
    http.call("start_top_up", {{"customer_id", customer.id}, {"amount_micros", 25'000'000}});
    http.call("set_auto_top_up", {{"customer_id", customer.id}, {"enabled", true}});
    http.call("set_auto_top_up", {{"customer_id", customer.id}, {"enabled", false}});
    // January at a list with no rates, so its invoice reads no workspace's usage. Issued to an
    // account that has not topped up, so it is collected through the provider and can be resent.
    const auto payer_user = checked(store->create_user(
        {"taap:1:ed25519:schema-invoices", "invoices@example.test", "Invoices"}));
    const auto payer =
        checked(store->create_customer({"individual", payer_user.id, "", "schema-invoices"}));
    checked(store->set_billing_contact({payer.id, "invoices@example.com"}));
    http.call("set_price_list", {{"effective_from", "2026-01"}, {"prices", json::array()}});
    http.call("issue_invoice", {{"customer_id", payer.id}, {"period", "2026-01"}});
    lake::control::Invoice month;
    month.customer_id = customer.id;
    month.plan_id = "usage-202602-00000000000000001";
    month.plan_name = "Usage";
    month.currency = "USD";
    month.provider = "manual";
    month.lines = {
        {"usage", "calls", "API and MCP Calls", "calls", 1000, 0, 1000, 1000, 1, 1000000}};
    month.period = "2026-02";
    const auto due = checked(store->create_invoice(month));
    http.call("list_invoices", {{"customer_id", customer.id}});
    http.read("list_invoices", "?customer_id=" + customer.id);
    const auto invoices = checked(store->list_invoices(payer.id));
    const std::string january = invoices.back().id;
    http.call("get_invoice", {{"customer_id", payer.id}, {"invoice_id", january}});
    http.read("get_invoice", "?customer_id=" + payer.id + "&invoice_id=" + january);
    http.call("resend_invoice", {{"invoice_id", january}});
    http.call("record_invoice_payment", {{"invoice_id", january}, {"reference", "W1"}});
    http.call("void_invoice", {{"invoice_id", due.id}, {"note", "issued twice"}});
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
    http.call("download_drive_file",
              {{"repository_id", repository.id}, {"ref", "main"}, {"path", "shared/sample.txt"}});
    http.call("drive_share_people",
              {{"repository_id", repository.id}, {"ref", "main"}, {"path", "shared"}});
    http.call("issue_assignees", {{"repository_id", repository.id}});
    const auto link = http.call("create_drive_share_link",
                                {{"repository_id", repository.id},
                                 {"workspace_id", experiments.id},
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
    check_web_app(http, *store, experiments, kek, customer, repository);
    check_proofs(http);
    validate_evidence(http.document);
}
} // namespace
