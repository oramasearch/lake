// Lake's records in workspace Zettas (docs/architecture/workspace-records.md, lake#370), with Ask
// Orama conversations as the first collection and Design → Opportunities as the first mutable one
// (lake#376): sealing, structural isolation between workspaces and spaces, conditional writes,
// workspace deletion, and the operator's one-shot copy.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <pqxx/pqxx>

#include "api/agent_stream_runs.hpp"
#include "api/forge_records_api.hpp"
#include "api/mcp_connection_internal.hpp"
#include "api/secret_records_api.hpp"
#include "api/workspace_bus.hpp"
#include "api/workspace_records_api.hpp"
#include "audit_test_support.hpp"
#include "control/secrets_crypto.hpp"
#include "lake/api/agent_stream_triggers.hpp"
#include "lake/api/audit_recorder.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/mcp_client.hpp"
#include "lake/api/mcp_connections.hpp"
#include "lake/api/metering_recorder.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/transcription.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/api/workspace_settings.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/agent_stream_triggers.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/application_authorization_codes.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/mcp_connections.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/methodologies.hpp"
#include "lake/control/oci_records.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/platform_records.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/repository_records.hpp"
#include "lake/control/secret_records.hpp"
#include "lake/control/solutions.hpp"
#include "lake/control/stream_owners.hpp"
#include "lake/control/transcripts.hpp"
#include "lake/control/workspace_audit.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/transparency/log.hpp"
#include "methodology_repositories.hpp"

namespace lake {
// Standard base64 for the chunked artifacts and served weights (lake#690).
inline std::string test_base64_encode(std::string_view bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}
inline std::string test_base64_decode(std::string_view text) {
    std::string out;
    unsigned value = 0;
    int bits = -8;
    for (const char c : text) {
        int digit = -1;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        else continue;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}
} // namespace lake
#include "mcp_test_server.hpp"
#include "postgres_fixture.hpp"
#include "record_fixture.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;
using lake::control::AskMessage;
using lake::control::AskMessages;
using lake::control::WorkspaceRecords;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";

template <class Task> auto run(Task task) {
    return lake::control::settled(liquid::runtime::block_on(std::move(task)));
}

// One workspace's storage, reached directly (no WorkspaceService) for the unit tests.
struct DirectRecords {
    std::shared_ptr<lake::test::RecordTables> tables = std::make_shared<lake::test::RecordTables>();
    std::string workspace_id = lake::control::new_uuid_v4();

    WorkspaceRecords open(std::string id = {}) {
        auto tables = this->tables;
        auto opened = WorkspaceRecords::open(
            [tables](std::string capability, bool read, json input)
                -> liquid::runtime::task<lake::control::ZettaControlResult<json>> {
                EXPECT_TRUE(lake::test::RecordTables::handles(capability, input)) << capability;
                EXPECT_EQ(read, capability == "get_record" || capability == "list_records")
                    << capability;
                co_return tables->run(capability, input);
            },
            id.empty() ? workspace_id : id, kek);
        EXPECT_TRUE(opened) << opened.error().message;
        return std::move(*opened);
    }
};

AskMessage message(std::string conversation, std::string user, std::string role,
                   std::string content, json payload = nullptr) {
    return {"", std::move(conversation), "customer-1", std::move(user), std::move(role),
            std::move(content), std::move(payload), ""};
}

} // namespace

TEST(WorkspaceRecords, AWorkspaceHoldsOnlySealedBodiesAndBlindIndexes) {
    DirectRecords direct;
    auto records = direct.open();
    AskMessages messages(records);
    const std::string conversation = lake::control::new_uuid_v4();
    const auto first = run(messages.append(message(conversation, "user-1", "user", "what runs?")));
    ASSERT_TRUE(first) << first.error().message;
    const auto second = run(messages.append(
        message(conversation, "user-1", "assistant", "two runs", {{"reads", json::array()}})));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_LT(first->at_us, second->at_us) << "appends in one process keep their order";

    const auto& rows = direct.tables->rows("ask_messages");
    ASSERT_EQ(rows.size(), 2U) << "one put_record per message, in collection lake_ask_messages";
    for (const auto& [key, row] : rows) {
        const std::string text = row.dump();
        for (const std::string& plain :
             std::vector<std::string>{"what runs?", "two runs", conversation, "user-1"}) {
            EXPECT_EQ(text.find(plain), std::string::npos) << "the workspace holds " << plain;
        }
        EXPECT_EQ(row.at("index").at("lookup"),
                  records.index_of(lake::control::AskMessageCodec::collection,
                                   lake::control::ask_conversation_lookup(conversation, "user-1")));
        EXPECT_EQ(row.at("order").get<std::string>().size(), 20U) << "a zero-padded instant";
    }
    EXPECT_EQ(direct.tables->collections.at("lake_ask_messages").indexes,
              json::array({"lookup"}));

    const auto read = run(messages.list(lake::control::ask_conversation_lookup(conversation, "user-1")));
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read->size(), 2U);
    EXPECT_EQ(read->at(0).content, "what runs?");
    EXPECT_EQ(read->at(1).payload["reads"], json::array());
    EXPECT_EQ(read->at(1).id, second->id);
    EXPECT_FALSE(read->at(0).created_at.empty());
    EXPECT_TRUE(run(messages.list(lake::control::ask_conversation_lookup(conversation, "user-2")))
                    ->empty())
        << "only the person who had the conversation reads it back";
    const auto newest = run(messages.list(
        lake::control::ask_conversation_lookup(conversation, "user-1"), 1));
    ASSERT_EQ(newest->size(), 1U);
    EXPECT_EQ(newest->front().content, "two runs") << "a limit keeps the newest";

    // Appending the same id again is refused by the engine (if_absent), never a second copy.
    const auto again = run(messages.append(message(conversation, "user-1", "user", "x"),
                                           lake::control::RecordWrite{first->id, 0}));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "conflict");
    EXPECT_TRUE(again.error().details.contains("revision"));
}

TEST(WorkspaceRecords, AMissingCollectionIsEmptyAndTheFirstWriteCreatesIt) {
    DirectRecords direct;
    auto records = direct.open();
    AskMessages messages(records);
    const auto none = run(messages.list(lake::control::ask_conversation_lookup("c", "u")));
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_TRUE(none->empty());
    EXPECT_TRUE(direct.tables->collections.empty()) << "reading creates nothing";
    ASSERT_TRUE(run(messages.append(message("c", "u", "user", "hello"))));
    EXPECT_TRUE(direct.tables->collections.contains("lake_ask_messages"));
    // Another replica's store of the same workspace finds the collection already there.
    auto again = direct.open();
    AskMessages more(again);
    ASSERT_TRUE(run(more.append(message("c", "u", "user", "again"))));
    EXPECT_EQ(run(more.list(lake::control::ask_conversation_lookup("c", "u")))->size(), 2U);
    EXPECT_EQ(std::ranges::count(direct.tables->calls, "create_collection"), 1);
}

TEST(WorkspaceRecords, ALongConversationIsListedAPageAtATimeAndALargeBodyIsOneRecord) {
    DirectRecords direct;
    auto records = direct.open();
    AskMessages messages(records);
    for (int index = 0; index < 1205; ++index) {
        ASSERT_TRUE(run(messages.append(message("c", "u", "user", std::to_string(index)))));
    }
    const auto all = run(messages.list(lake::control::ask_conversation_lookup("c", "u")));
    ASSERT_EQ(all->size(), 1205U) << "next_after is followed";
    EXPECT_EQ(all->front().content, "0");
    EXPECT_EQ(all->back().content, "1204");
    const auto newest = run(messages.list(lake::control::ask_conversation_lookup("c", "u"), 200));
    ASSERT_EQ(newest->size(), 200U);
    EXPECT_EQ(newest->front().content, "1005");

    std::string content;
    while (content.size() < 40'000) {
        content += lake::control::new_uuid_v4();
    }
    ASSERT_TRUE(run(messages.append(message("big", "u", "assistant", content))));
    const auto read = run(messages.list(lake::control::ask_conversation_lookup("big", "u")));
    ASSERT_EQ(read->size(), 1U);
    EXPECT_EQ(read->front().content, content);
}

TEST(WorkspaceRecords, ARecordOpensOnlyInTheWorkspaceThatWroteIt) {
    // Ownership is immutable: a record copied into another workspace's collection, or moved to
    // another lookup, does not open there (the AEAD binds workspace, collection, key, lookup,
    // instant, schema version and lake's revision).
    DirectRecords first;
    DirectRecords second;
    auto mine = first.open();
    AskMessages messages(mine);
    ASSERT_TRUE(run(messages.append(message("c", "u", "user", "private"))));
    auto other = second.open();
    second.tables->collections = first.tables->collections;
    AskMessages theirs(other);
    for (auto& [key, row] : second.tables->rows("ask_messages")) {
        row["index"]["lookup"] = other.index_of(lake::control::AskMessageCodec::collection,
                                                lake::control::ask_conversation_lookup("c", "u"));
    }
    EXPECT_TRUE(run(theirs.list(lake::control::ask_conversation_lookup("c", "u")))->empty())
        << "the same lookup in another workspace does not open the moved record";
    for (auto& [key, row] : first.tables->rows("ask_messages")) {
        row["index"]["lookup"] = mine.index_of(lake::control::AskMessageCodec::collection,
                                               lake::control::ask_conversation_lookup("other", "u"));
    }
    AskMessages again(mine);
    EXPECT_TRUE(run(again.list(lake::control::ask_conversation_lookup("other", "u")))->empty())
        << "a record moved to another conversation does not open there either";
}

TEST(WorkspaceRecords, MutableRecordsWriteConditionallyAndRetry) {
    DirectRecords direct;
    auto records = direct.open();
    const lake::control::RecordCollection counters{"test_counters", 1};
    const auto increment = [&] {
        return run(records.update(counters, "counter-1", "all",
                                  [](const std::optional<lake::control::StoredRecord>& current) {
                                      const int value =
                                          current ? current->body.value("n", 0) : 0;
                                      return std::optional<json>(json{{"n", value + 1}});
                                  }));
    };
    const auto created = increment();
    ASSERT_TRUE(created && *created) << (created ? "" : created.error().message);
    EXPECT_EQ((*created)->lake_revision, 1);
    const auto revision = (*created)->revision;

    // A stale writer loses: 409 with the live revision.
    ASSERT_TRUE(increment());
    const auto stale = run(records.put(counters, "counter-1", "all", {{"n", 99}},
                                       {.if_revision = revision}));
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "conflict");
    EXPECT_NE(stale.error().details.value("revision", std::int64_t{0}), revision);

    // A write that lands between update's read and write is retried, not lost.
    bool raced = false;
    const auto updated = run(records.update(
        counters, "counter-1", "all", [&](const std::optional<lake::control::StoredRecord>& current) {
            if (!raced) {
                raced = true;
                auto& row = direct.tables->collections.at("lake_test_counters").records.at("counter-1");
                row["revision"] = row["revision"].get<std::int64_t>() + 1000;
            }
            return std::optional<json>(json{{"n", current->body.value("n", 0) + 1}});
        }));
    ASSERT_TRUE(updated && *updated);
    EXPECT_EQ((*updated)->body["n"], 3);
    EXPECT_EQ((*updated)->lake_revision, 3);
    const auto got = run(records.get(counters, "counter-1"));
    ASSERT_TRUE(got && *got);
    EXPECT_EQ((**got).body["n"], 3);

    // Ownership moving (503 busy) is retried; a deletion is conditional too.
    direct.tables->busy_next = 2;
    ASSERT_TRUE(run(records.get(counters, "counter-1")));
    EXPECT_FALSE(run(records.remove(counters, "counter-1", revision)));
    ASSERT_TRUE(run(records.remove(counters, "counter-1", (**got).revision)));
    EXPECT_FALSE(*run(records.get(counters, "counter-1")));
    EXPECT_EQ(run(records.remove(counters, "counter-1")).error().code, "not_found");
}

TEST(WorkspaceRecords, OpeningRefusesAMissingKeyOrAWorkspaceThatIsNotAUuid) {
    const auto call = [](std::string, bool, json) -> liquid::runtime::task<
                          lake::control::ZettaControlResult<json>> { co_return json::object(); };
    EXPECT_EQ(WorkspaceRecords::open(call, "not-a-workspace", kek).error().code, "invalid_input");
    EXPECT_EQ(WorkspaceRecords::open(call, lake::control::new_uuid_v4(), "").error().code,
              "store_unavailable");
    EXPECT_EQ(WorkspaceRecords::zetta_collection(lake::control::AskMessageCodec::collection),
              "lake_ask_messages");
}

namespace {

lake::control::ClerkIdentity person(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity(std::string(kek));
    return {subject,
            subject + "@example.test",
            "",
            "browser",
            identity->taap_id,
            identity->public_key,
            identity->wrapped_seed,
            static_cast<std::int64_t>(std::time(nullptr))};
}

// Two spaces, two workspaces each, every workspace Ready at its own endpoint, over one store.
struct TwoSpaces {
    std::shared_ptr<lake::control::Store> store;
    std::filesystem::path dir =
        lake::test::fixture_path("workspace-records-" + lake::control::new_uuid_v4());
    std::shared_ptr<lake::test::StreamCaller> zetta = std::make_shared<lake::test::StreamCaller>();
    std::shared_ptr<lake::control::WorkspaceService> service;
    lake::api::Registry registry;
    struct Space {
        lake::control::Principal principal;
        std::string customer_id;
        std::vector<std::string> workspaces;
    };
    std::vector<Space> spaces;

    std::string database_uri; // empty: the in-memory store
    // Speech-to-text for transcribe and audio transcripts; empty: not configured.
    lake::api::Transcriber transcriber;
    // Repositories and image builds, for workspace methodologies (lake#655); unset: none.
    lake::api::SyncStore* sync_store = nullptr;
    std::string methodology_runtime_image;
    // The runner image of stdio MCP connections (lake#659); empty: none.
    std::string mcp_runner_image;
    std::string apps_wildcard;
    // Served methodology models' predictions (lake#690); empty: none.
    lake::api::MethodologyServer methodology_server;
    std::string internal_url;
    std::string public_host;
    // MCP connections' transport (lake#652); null: none. Tests reach a local test server.
    std::shared_ptr<lake::api::McpHttp> mcp_http;
    lake::api::UsageRecorder* usage_recorder = nullptr;
    lake::api::AuditRecorder* audit_recorder = nullptr;

    // What the operator reports for a service.
    void status(const std::string& service_id, const std::string& phase,
                const std::string& endpoint) {
        if (database_uri.empty()) {
            EXPECT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_service_status(
                service_id, phase, "", endpoint));
            return;
        }
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO service_status (service_id, phase, endpoint) VALUES ($1::uuid, "
                       "$2, $3) ON CONFLICT (service_id) DO UPDATE SET phase = EXCLUDED.phase, "
                       "endpoint = EXCLUDED.endpoint",
                       service_id, phase, endpoint);
        tx.commit();
    }

    TwoSpaces(std::shared_ptr<lake::control::Store> backing, std::string uri)
        : store(std::move(backing)), database_uri(std::move(uri)) {
        std::filesystem::create_directories(dir);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{dir.string(), 0}, store, zetta);
        lake::api::register_core_capabilities(registry, "test");
        for (const std::string name : {"first", "second"}) {
            const auto session =
                store->sign_in_clerk(person(name + "-" + lake::control::new_uuid_v4()));
            EXPECT_TRUE(session);
            Space space;
            space.principal = *store->context_for(session->secret);
            space.customer_id = store->list_customers(10, space.principal.user_id)->front().id;
            for (const std::string workspace : {"home", "lab"}) {
                const auto created =
                    store->create_workspace({space.customer_id, workspace, "dev", ""});
                EXPECT_TRUE(created) << created.error().message;
                ready(created->id);
                space.workspaces.push_back(created->id);
            }
            spaces.push_back(std::move(space));
        }
    }

    void ready(const std::string& workspace_id) {
        // The namespace the operator reports the workspace runs in.
        if (database_uri.empty()) {
            EXPECT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_workspace_namespace(
                workspace_id, "dev-ws-" + workspace_id));
        } else {
            pqxx::connection connection(database_uri);
            pqxx::work tx(connection);
            tx.exec_params("UPDATE workspaces SET namespace = $2 WHERE id = $1::uuid",
                           workspace_id, "dev-ws-" + workspace_id);
            tx.commit();
        }
        const auto workspace = store->get_workspace(workspace_id);
        const auto zetta_service = store->get_service(workspace->zetta_service_id);
        status(zetta_service->id, "Ready", "http://" + workspace_id + ".test");
        std::ofstream(dir / (zetta_service->slug + ".json"))
            << json{{"kind", "zetta"}, {"token", "token-" + workspace_id}}.dump();
    }

    lake::api::ApiContext context(const Space& space) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = service.get();
        context.instance_kek = std::string(kek);
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = space.principal.user_id;
        context.acting_identity = space.principal.taap_id;
        context.credential_session_id = space.principal.session_id;
        context.scopes = space.principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        context.transcriber = transcriber;
        context.sync_store = sync_store;
        context.methodology_runtime_image = methodology_runtime_image;
        context.mcp_runner_image = mcp_runner_image;
        context.apps_wildcard = apps_wildcard;
        context.methodology_server = methodology_server;
        context.internal_url = internal_url;
        context.public_host = public_host;
        context.mcp_http = mcp_http;
        context.mcp_allow_private_addresses = true;
        context.external_origin = "https://lake.test";
        context.usage_recorder = usage_recorder;
        context.audit_recorder = audit_recorder;
        return context;
    }

    // What the Ask page does when the person sends a message in a workspace.
    void say(const Space& space, const std::string& workspace_id, const std::string& conversation,
             std::string text) {
        auto context = this->context(space);
        auto records = lake::api::workspace_records(context, space.customer_id, workspace_id);
        ASSERT_TRUE(records) << records.error().message;
        AskMessages messages(*records);
        ASSERT_TRUE(run(messages.append({"", conversation, space.customer_id,
                                         space.principal.user_id, "user", std::move(text),
                                         nullptr, ""})));
    }

    lake::api::ApiResult read(const Space& space, json input) {
        return call(space, "get_ask_conversation", std::move(input));
    }

    lake::api::ApiResult call(const Space& space, const std::string& capability, json input) {
        auto context = this->context(space);
        const auto completed =
            liquid::runtime::block_on(registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    }

    // A Discover scan Auto-Suggest Agent finished in a workspace: one suggestion and one decline.
    std::string scan(const Space& space, const std::string& workspace_id, const std::string& intent) {
        const auto task = store->create_agent_task({.customer_id = space.customer_id,
                                                    .agent = "agent-autosuggest",
                                                    .kind = "discover",
                                                    .workspace_id = workspace_id,
                                                    .body = {{"message", "scan"}},
                                                    .scopes = {"list_datasets"},
                                                    .requested_by_user_id = space.principal.user_id});
        EXPECT_TRUE(task) << task.error().message;
        EXPECT_TRUE(store->claim_agent_task(task->id, "agent-autosuggest", "sha"));
        const json profile{
            {"suggested", json::array({{{"intent", intent},
                                        {"solution", "S-1"},
                                        {"methodology", "gbdt"},
                                        {"why_this_data_supports_it", "a label exists"}}})},
            {"declined", json::array({{{"intent", intent + " later"}, {"reason", "no timestamp"}}})}};
        EXPECT_TRUE(store->record_agent_task_event(task->id, "agent-autosuggest", "Succeeded"));
        // The scan's answer is a record in its workspace, as publish_agent_event keeps it
        // (lake#375).
        auto context = this->context(space);
        auto records = lake::api::workspace_records(context, space.customer_id, workspace_id);
        EXPECT_TRUE(records) << records.error().message;
        EXPECT_TRUE(run(lake::control::record_agent_task_content(
            *records, {.task_id = task->id,
                       .customer_id = space.customer_id,
                       .agent = "agent-autosuggest",
                       .result = {{"ok", true}, {"result", {{"project_profile", profile}}}},
                       .phase = "Succeeded"})));
        return task->id;
    }

    json opportunities(const Space& space, std::size_t workspace, std::string task = {}) {
        json input{{"customer_id", space.customer_id},
                   {"workspace_id", space.workspaces[workspace]}};
        if (!task.empty()) {
            input["task_id"] = task;
        }
        const auto listed = call(space, "list_opportunities", input);
        EXPECT_TRUE(listed.ok) << listed.error.message;
        return listed.value.value("opportunities", json::array());
    }
};

class WorkspaceRecordStores : public testing::TestWithParam<bool> {
protected:
    std::shared_ptr<lake::control::Store> make_store() {
        if (!GetParam()) {
            return std::make_shared<lake::control::InMemoryStore>();
        }
        const char* uri = lake::test::postgres_uri();
        if (uri == nullptr || *uri == '\0') {
            return nullptr;
        }
        // The runner's database is private to this test (tests/test_main.cpp).
        database_uri_ = uri;
        return lake::control::make_postgres_store(database_uri_, std::string(kek));
    }
    void check_delegation_offboarding(bool suspended);
    void check_stream_offboarding(bool suspended);
    std::string database_uri_;
};

} // namespace

TEST_P(WorkspaceRecordStores, ConversationsStayInTheirWorkspaceAndSpace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string conversation = lake::control::new_uuid_v4();
    t.say(first, first.workspaces[0], conversation, "home question");
    t.say(first, first.workspaces[1], conversation, "lab question");
    t.say(second, second.workspaces[0], conversation, "another space");

    const auto read = [&](const auto& space, std::size_t workspace) {
        return t.read(space, {{"customer_id", space.customer_id},
                              {"workspace_id", space.workspaces[workspace]},
                              {"conversation_id", conversation}});
    };
    auto home = read(first, 0);
    ASSERT_TRUE(home.ok) << home.error.message;
    ASSERT_EQ(home.value["messages"].size(), 1U);
    EXPECT_EQ(home.value["messages"][0]["content"], "home question");
    auto lab = read(first, 1);
    ASSERT_EQ(lab.value["messages"].size(), 1U);
    EXPECT_EQ(lab.value["messages"][0]["content"], "lab question")
        << "the same conversation id in another workspace of the space is another conversation";
    auto theirs = read(second, 0);
    ASSERT_EQ(theirs.value["messages"].size(), 1U);
    EXPECT_EQ(theirs.value["messages"][0]["content"], "another space");
    EXPECT_TRUE(read(second, 1).value["messages"].empty());

    // Each message is stored only at its own workspace's endpoint.
    for (const auto& space : t.spaces) {
        for (const auto& workspace : space.workspaces) {
            const auto& rows =
                t.zetta->records["http://" + workspace + ".test"].rows("ask_messages");
            EXPECT_EQ(rows.size(), workspace == second.workspaces[1] ? 0U : 1U) << workspace;
        }
    }

    // Another space's workspace, or another space's id, is refused.
    const auto foreign = t.read(second, {{"customer_id", second.customer_id},
                                         {"workspace_id", first.workspaces[0]},
                                         {"conversation_id", conversation}});
    EXPECT_FALSE(foreign.ok);
    EXPECT_EQ(foreign.error.code, "invalid_input");
    EXPECT_EQ(foreign.error.path, "/customer_id");
    const auto trespass = t.read(second, {{"customer_id", first.customer_id},
                                          {"workspace_id", first.workspaces[0]},
                                          {"conversation_id", conversation}});
    EXPECT_FALSE(trespass.ok);
    EXPECT_EQ(trespass.error.code, "unauthorized") << "no read on another space";

    // Missing, malformed and unknown workspace ids name the field.
    const auto missing = t.read(first, {{"customer_id", first.customer_id},
                                        {"conversation_id", conversation}});
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.path, "/workspace_id");
    for (const std::string& bad :
         std::vector<std::string>{"not-a-workspace", lake::control::new_uuid_v4()}) {
        const auto refused = t.read(first, {{"customer_id", first.customer_id},
                                            {"workspace_id", bad},
                                            {"conversation_id", conversation}});
        EXPECT_EQ(refused.error.code, "invalid_input") << bad;
        EXPECT_EQ(refused.error.path, "/workspace_id") << bad;
    }

    // A workspace that is not ready refuses instead of falling back anywhere.
    const auto lab_workspace = t.store->get_workspace(first.workspaces[1]);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + first.workspaces[1] + ".test");
    EXPECT_EQ(read(first, 1).error.code, "workspace_not_ready");

    // Deleting a workspace takes its conversations with it; the other workspace keeps its own.
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    const auto deleted = read(first, 1);
    EXPECT_EQ(deleted.error.code, "invalid_input");
    EXPECT_EQ(deleted.error.path, "/workspace_id");
    EXPECT_EQ(read(first, 0).value["messages"].size(), 1U);
}

TEST_P(WorkspaceRecordStores, OpportunitiesStayInTheWorkspaceTheirScanRanIn) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string home_scan = t.scan(first, first.workspaces[0], "Predict churn");
    const std::string lab_scan = t.scan(first, first.workspaces[1], "Group customers");
    const std::string theirs_scan = t.scan(second, second.workspaces[0], "Forecast demand");

    // Reading a finished scan saves its Opportunities in the scan's own workspace, once.
    auto home = t.opportunities(first, 0, home_scan);
    ASSERT_EQ(home.size(), 2U);
    EXPECT_EQ(home[0]["intent"], "Predict churn");
    EXPECT_EQ(home[0]["status"], "new");
    EXPECT_EQ(home[1]["status"], "not_yet");
    EXPECT_EQ(home[0]["id"], lake::control::opportunity_id(home_scan, 1));
    EXPECT_EQ(t.opportunities(first, 0, home_scan).size(), 2U) << "a second settle writes nothing";
    // The lab workspace lists its own latest scan (lake#1195), never the home scan, even when
    // named.
    const auto lab = t.opportunities(first, 1, home_scan);
    ASSERT_EQ(lab.size(), 2U);
    for (const auto& row : lab) {
        EXPECT_EQ(row["task_id"], lab_scan)
            << "a scan of another workspace is not saved here, even when named";
    }
    EXPECT_EQ(t.opportunities(first, 1, lab_scan).size(), 2U);
    EXPECT_EQ(t.opportunities(second, 0, theirs_scan).size(), 2U);
    EXPECT_TRUE(t.opportunities(second, 1).empty());
    for (const auto& space : t.spaces) {
        for (const auto& workspace : space.workspaces) {
            const auto& rows =
                t.zetta->records["http://" + workspace + ".test"].rows("opportunities");
            EXPECT_EQ(rows.size(), workspace == second.workspaces[1] ? 0U : 2U) << workspace;
            for (const auto& [key, row] : rows) {
                EXPECT_EQ(row.dump().find("churn"), std::string::npos) << "sealed at rest";
            }
        }
    }
    // A later scan is listed first.
    const std::string newer = t.scan(first, first.workspaces[0], "Score leads");
    home = t.opportunities(first, 0, newer);
    ASSERT_EQ(home.size(), 4U);
    EXPECT_EQ(home[0]["intent"], "Score leads");
    EXPECT_EQ(home[2]["intent"], "Predict churn");

    // Dismissing is a conditional write in the owning workspace only.
    const std::string id = home[2]["id"];
    const auto dismiss = [&](const auto& space, std::size_t workspace, const std::string& target) {
        return t.call(space, "dismiss_opportunity", {{"customer_id", space.customer_id},
                                                     {"workspace_id", space.workspaces[workspace]},
                                                     {"opportunity_id", target}});
    };
    EXPECT_EQ(dismiss(first, 1, id).error.code, "not_found") << "another workspace of the space";
    EXPECT_EQ(dismiss(second, 0, id).error.code, "not_found") << "another space";
    EXPECT_EQ(dismiss(first, 0, "not-an-id").error.code, "not_found");
    const auto dismissed = dismiss(first, 0, id);
    ASSERT_TRUE(dismissed.ok) << dismissed.error.message;
    EXPECT_EQ(dismissed.value["status"], "dismissed");
    EXPECT_EQ(dismissed.value["intent"], "Predict churn") << "nothing but the status changes";
    EXPECT_EQ(t.opportunities(first, 0)[2]["status"], "dismissed");

    // Creating a project from another workspace's Opportunity finds none.
    const auto elsewhere = t.call(first, "create_projects_from_opportunities",
                                  {{"customer_id", first.customer_id},
                                   {"workspace_id", first.workspaces[1]},
                                   {"opportunity_ids", json::array({id})}});
    EXPECT_EQ(elsewhere.error.code, "not_found");

    // Another space's workspace, or another space's id, is refused.
    const auto foreign = t.call(second, "list_opportunities", {{"customer_id", second.customer_id},
                                                               {"workspace_id", first.workspaces[0]}});
    EXPECT_EQ(foreign.error.code, "invalid_input");
    EXPECT_EQ(foreign.error.path, "/customer_id");
    const auto trespass = t.call(second, "list_opportunities", {{"customer_id", first.customer_id},
                                                                {"workspace_id", first.workspaces[0]}});
    EXPECT_FALSE(trespass.ok);
    EXPECT_EQ(trespass.error.code, "unauthorized") << "no read on another space";
    const auto trespass_write = t.call(second, "dismiss_opportunity",
                                       {{"customer_id", first.customer_id},
                                        {"workspace_id", first.workspaces[0]},
                                        {"opportunity_id", id}});
    EXPECT_EQ(trespass_write.error.code, "unauthorized") << "no write on another space";

    // Missing, malformed and unknown workspace ids name the field, for reads and writes.
    for (const std::string capability : {"list_opportunities", "dismiss_opportunity"}) {
        json input{{"customer_id", first.customer_id}};
        if (capability == "dismiss_opportunity") {
            input["opportunity_id"] = id;
        }
        const auto missing = t.call(first, capability, input);
        EXPECT_EQ(missing.error.code, "invalid_input") << capability;
        EXPECT_EQ(missing.error.path, "/workspace_id") << capability;
        for (const std::string& bad :
             std::vector<std::string>{"not-a-workspace", lake::control::new_uuid_v4()}) {
            input["workspace_id"] = bad;
            const auto refused = t.call(first, capability, input);
            EXPECT_EQ(refused.error.code, "invalid_input") << capability << " " << bad;
            EXPECT_EQ(refused.error.path, "/workspace_id") << capability << " " << bad;
        }
    }

    // A workspace that is not ready refuses instead of falling back anywhere.
    const auto lab_workspace = t.store->get_workspace(first.workspaces[1]);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + first.workspaces[1] + ".test");
    const auto not_ready = t.call(first, "list_opportunities",
                                  {{"customer_id", first.customer_id},
                                   {"workspace_id", first.workspaces[1]}});
    EXPECT_EQ(not_ready.error.code, "workspace_not_ready");

    // Deleting a workspace takes its Opportunities with it; the other workspace keeps its own.
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    const auto deleted = t.call(first, "list_opportunities", {{"customer_id", first.customer_id},
                                                              {"workspace_id", first.workspaces[1]}});
    EXPECT_EQ(deleted.error.code, "invalid_input");
    EXPECT_EQ(deleted.error.path, "/workspace_id");
    EXPECT_EQ(t.opportunities(first, 0).size(), 4U);
}

// lake#1195: a scan survives leaving the page. Opportunities lists the workspace's newest scan as
// `last_scan` (pending, running, then finished), and a finished scan's Opportunities are listed
// without its task id, so a page opened on any tab or device shows the progress and the results.
TEST_P(WorkspaceRecordStores, OpportunitiesReportTheLatestScanWithoutItsTaskId) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto listed = [&](std::size_t workspace) {
        const auto read = t.call(
            first,
            "list_opportunities",
            {{"customer_id", first.customer_id}, {"workspace_id", first.workspaces[workspace]}});
        EXPECT_TRUE(read.ok) << read.error.message;
        return read.value;
    };
    const auto before = listed(0);
    EXPECT_TRUE(before.at("last_scan").is_null()) << "no scan yet";
    EXPECT_TRUE(before.at("opportunities").empty());

    // A scan that was asked for and not yet claimed, then one the agent is working on.
    const auto asked = store->create_agent_task({.customer_id = first.customer_id,
                                                 .agent = "agent-autosuggest",
                                                 .kind = "discover",
                                                 .workspace_id = first.workspaces[0],
                                                 .body = {{"message", "scan"}},
                                                 .scopes = {"list_datasets"},
                                                 .requested_by_user_id = first.principal.user_id});
    ASSERT_TRUE(asked) << asked.error().message;
    auto pending = listed(0).at("last_scan");
    ASSERT_TRUE(pending.is_object());
    EXPECT_EQ(pending["task_id"], asked->id);
    EXPECT_EQ(pending["status"], "pending");
    EXPECT_FALSE(pending["started_at"].get<std::string>().empty());
    EXPECT_TRUE(pending["finished_at"].is_null());
    ASSERT_TRUE(store->claim_agent_task(asked->id, "agent-autosuggest", "sha"));
    const auto running = listed(0);
    EXPECT_EQ(running.at("last_scan")["status"], "running");
    EXPECT_TRUE(running.at("opportunities").empty()) << "nothing to list while it runs";
    EXPECT_TRUE(listed(1).at("last_scan").is_null()) << "a scan belongs to its own workspace";

    // A finished scan (its answer recorded, as publish_agent_event keeps it) is listed with no
    // task id, and reported finished.
    const std::string finished = t.scan(first, first.workspaces[0], "Predict churn");
    const auto done = listed(0);
    ASSERT_EQ(done.at("opportunities").size(), 2U);
    EXPECT_EQ(done.at("opportunities")[0]["task_id"], finished);
    EXPECT_EQ(done.at("last_scan")["task_id"], finished);
    EXPECT_EQ(done.at("last_scan")["status"], "succeeded");
    EXPECT_FALSE(done.at("last_scan")["finished_at"].is_null());
    EXPECT_EQ(listed(0).at("opportunities").size(), 2U) << "a second read writes nothing";
}

TEST_P(WorkspaceRecordStores, TranscriptsLiveInTheirWorkspaceAndKeepNoAudio) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto in = [&](const auto& space, std::size_t workspace, json input) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[workspace];
        return input;
    };

    // Without a key, speech-to-text is refused and nothing is written.
    const std::string audio = "data:audio/webm;codecs=opus;base64,c3BlZWNo"; // "speech"
    const auto unconfigured =
        t.call(first, "transcribe", in(first, 0, {{"name", "memo"}, {"audio_base64", audio}}));
    EXPECT_EQ(unconfigured.error.code, "not_supported");
    EXPECT_EQ(unconfigured.error.message, "Speech-to-text is not configured on this Lake.");
    const auto unconfigured_create =
        t.call(first,
               "create_transcript",
               in(first, 0, {{"title", "Memo"}, {"source", "recording"}, {"audio_base64", audio}}));
    EXPECT_EQ(unconfigured_create.error.code, "not_supported");

    // The fake transcriber: what it was sent, and the text it answers.
    std::vector<lake::api::TranscriptionRequest> heard;
    t.transcriber = [&heard](const lake::api::TranscriptionRequest& request)
        -> std::expected<lake::api::Transcription, lake::api::ApiError> {
        heard.push_back(request);
        if (request.audio == "silence") {
            return std::unexpected(
                lake::api::ApiError{"engine_error", "OpenAI could not transcribe the audio"});
        }
        return lake::api::Transcription{
            "We agreed to ship the budget on Friday.", 42.5, "gpt-4o-transcribe"};
    };

    const auto transcribed =
        t.call(first,
               "transcribe",
               in(first, 0, {{"name", "memo"}, {"audio_base64", audio}, {"language", "en"}}));
    ASSERT_TRUE(transcribed.ok) << transcribed.error.message;
    EXPECT_EQ(transcribed.value,
              (json{{"text", "We agreed to ship the budget on Friday."},
                    {"seconds", 42.5},
                    {"model", "gpt-4o-transcribe"}}));
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0].audio, "speech");
    EXPECT_EQ(heard[0].file_name, "memo.webm");
    EXPECT_EQ(heard[0].mime, "audio/webm");
    EXPECT_EQ(heard[0].language, "en");
    const auto failing = t.call(
        first, "transcribe", in(first, 0, {{"name", "x.wav"}, {"audio_base64", "c2lsZW5jZQ=="}}));
    EXPECT_EQ(failing.error.code, "engine_error");
    // A text file is refused before it reaches the speech service (lake#594).
    const std::size_t sent_before = heard.size();
    const auto text_file =
        t.call(first, "transcribe", in(first, 0, {{"name", "memo.txt"}, {"audio_base64", audio}}));
    EXPECT_EQ(text_file.error.code, "invalid_input");
    EXPECT_EQ(text_file.error.path, "/name");
    EXPECT_NE(text_file.error.message.find("memo.txt is not audio or video"), std::string::npos);
    EXPECT_EQ(heard.size(), sent_before) << "nothing was sent";
    for (const auto& space : t.spaces) {
        for (const auto& workspace : space.workspaces) {
            EXPECT_TRUE(
                t.zetta->records["http://" + workspace + ".test"].rows("transcripts").empty())
                << "transcribe keeps nothing";
        }
    }

    // From audio: transcribed first, the audio never kept.
    const auto from_audio = t.call(first,
                                   "create_transcript",
                                   in(first,
                                      0,
                                      {{"title", " Friday call "},
                                       {"source", "recording"},
                                       {"audio_base64", audio},
                                       {"audio_name", "call.m4a"},
                                       {"recorded_at", "2026-10-09T15:00:00Z"}}));
    ASSERT_TRUE(from_audio.ok) << from_audio.error.message;
    EXPECT_EQ(from_audio.value["title"], "Friday call");
    EXPECT_EQ(from_audio.value["text"], "We agreed to ship the budget on Friday.");
    EXPECT_EQ(from_audio.value["source"], "recording");
    EXPECT_EQ(from_audio.value["seconds"], 42.5);
    EXPECT_EQ(from_audio.value["recorded_at"], "2026-10-09T15:00:00Z");
    EXPECT_EQ(from_audio.value["created_by"], first.principal.user_id);
    // The author by name, from the control store (lake#579): this person signed in without a
    // display name, so it is their email; never their user id.
    const auto author = t.store->get_user(first.principal.user_id);
    ASSERT_TRUE(author);
    const std::string author_name =
        author->display_name.empty() ? author->email : author->display_name;
    ASSERT_FALSE(author_name.empty());
    EXPECT_EQ(from_audio.value["created_by_name"], author_name);
    EXPECT_TRUE(from_audio.value["language"].is_null());
    EXPECT_EQ(heard.back().file_name, "call.m4a");
    EXPECT_EQ(heard.back().mime, "audio/mp4");

    // From text, and from captions cleaned to what was said.
    const auto from_text = t.call(first,
                                  "create_transcript",
                                  in(first,
                                     0,
                                     {{"title", "Notes"},
                                      {"source", "text"},
                                      {"text", "Pasted notes about hiring."},
                                      {"language", "en"}}));
    ASSERT_TRUE(from_text.ok) << from_text.error.message;
    EXPECT_TRUE(from_text.value["seconds"].is_null());
    EXPECT_EQ(from_text.value["language"], "en");
    const auto from_vtt =
        t.call(first,
               "create_transcript",
               in(first,
                  0,
                  {{"title", "Standup captions"},
                   {"source", "upload"},
                   {"text",
                    "WEBVTT\n\n1\n00:00:00.000 --> 00:00:02.000\n<v Ana>Budget "
                    "is late\n\n2\n00:00:02.000 --> 00:00:04.000\nso we wait\n"}}));
    ASSERT_TRUE(from_vtt.ok) << from_vtt.error.message;
    EXPECT_EQ(from_vtt.value["text"], "Ana: Budget is late\nso we wait");
    EXPECT_EQ(heard.size(), 3U) << "text is never sent for transcription";

    // Sealed at rest, and no audio in the record.
    const auto& rows =
        t.zetta->records["http://" + first.workspaces[0] + ".test"].rows("transcripts");
    ASSERT_EQ(rows.size(), 3U);
    for (const auto& [key, row] : rows) {
        EXPECT_EQ(row.dump().find("Friday"), std::string::npos) << "sealed at rest";
        EXPECT_EQ(row.dump().find("c3BlZWNo"), std::string::npos);
    }
    {
        auto context = t.context(first);
        auto records =
            lake::api::workspace_records(context, first.customer_id, first.workspaces[0]);
        ASSERT_TRUE(records);
        const auto stored = run(records->get(lake::control::TranscriptCodec::collection,
                                             from_audio.value["id"].get<std::string>()));
        ASSERT_TRUE(stored && *stored);
        for (const auto& [field, value] : (*stored)->body.items()) {
            EXPECT_EQ(field.find("audio"), std::string::npos) << field;
        }
        EXPECT_EQ((*stored)->body.dump().find("speech"), std::string::npos);
    }

    // Refusals name the field.
    const auto neither =
        t.call(first, "create_transcript", in(first, 0, {{"title", "Empty"}, {"source", "text"}}));
    EXPECT_EQ(neither.error.code, "invalid_input");
    EXPECT_EQ(neither.error.path, "/text");
    const auto untitled = t.call(first,
                                 "create_transcript",
                                 in(first, 0, {{"title", " "}, {"source", "text"}, {"text", "x"}}));
    EXPECT_EQ(untitled.error.path, "/title");
    const auto bad_source =
        t.call(first,
               "create_transcript",
               in(first, 0, {{"title", "x"}, {"source", "fax"}, {"text", "x"}}));
    EXPECT_EQ(bad_source.error.code, "invalid_input");
    EXPECT_EQ(bad_source.error.path, "/source");
    const auto long_text =
        t.call(first,
               "create_transcript",
               in(first,
                  0,
                  {{"title", "x"},
                   {"source", "text"},
                   {"text", std::string(lake::api::max_transcript_text_bytes + 1, 'a')}}));
    EXPECT_EQ(long_text.error.code, "size_invalid");

    // Newest first; q filters title and text ignoring case; limit and cursor page.
    const auto list = [&](const auto& space, std::size_t workspace, json extra) {
        return t.call(space, "list_transcripts", in(space, workspace, std::move(extra)));
    };
    auto listed = list(first, 0, json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["transcripts"].size(), 3U);
    EXPECT_EQ(listed.value["transcripts"][0]["id"], from_vtt.value["id"]);
    EXPECT_EQ(listed.value["transcripts"][2]["id"], from_audio.value["id"]);
    EXPECT_EQ(listed.value["transcripts"][2]["excerpt"], "We agreed to ship the budget on Friday.");
    EXPECT_FALSE(listed.value["transcripts"][0].contains("text"));
    EXPECT_FALSE(listed.value.contains("next_cursor"));
    EXPECT_EQ(listed.value["total"], 3);
    EXPECT_EQ(listed.value["transcripts"][0]["created_by_name"], author_name);
    listed = list(first, 0, {{"q", "BUDGET"}});
    ASSERT_EQ(listed.value["transcripts"].size(), 2U);
    EXPECT_EQ(listed.value["transcripts"][0]["title"], "Standup captions");
    EXPECT_EQ(listed.value["transcripts"][1]["title"], "Friday call");
    EXPECT_EQ(list(first, 0, {{"q", "notes"}}).value["transcripts"].size(), 1U) << "the title";
    listed = list(first, 0, {{"limit", 2}});
    ASSERT_EQ(listed.value["transcripts"].size(), 2U);
    ASSERT_TRUE(listed.value.contains("next_cursor"));
    EXPECT_EQ(listed.value["total"], 3) << "total counts past the page (lake#574)";
    EXPECT_EQ(list(first, 0, {{"limit", 1}, {"q", "BUDGET"}}).value["total"], 2);
    const auto rest = list(first, 0, {{"limit", 2}, {"cursor", listed.value["next_cursor"]}});
    ASSERT_EQ(rest.value["transcripts"].size(), 1U);
    EXPECT_EQ(rest.value["transcripts"][0]["id"], from_audio.value["id"]);
    EXPECT_EQ(list(first, 0, {{"cursor", "nope"}}).error.path, "/cursor");
    EXPECT_TRUE(list(first, 1, json::object()).value["transcripts"].empty())
        << "another workspace of the space lists none";

    // Get, update and delete, in the owning workspace only.
    const std::string id = from_audio.value["id"];
    const auto got = t.call(first, "get_transcript", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value, from_audio.value);
    EXPECT_EQ(t.call(first, "get_transcript", in(first, 1, {{"id", id}})).error.code, "not_found");
    EXPECT_EQ(t.call(first, "get_transcript", in(first, 0, {{"id", "not-an-id"}})).error.code,
              "not_found");
    const auto renamed = t.call(
        first, "update_transcript", in(first, 0, {{"id", id}, {"title", "Friday budget call"}}));
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value["title"], "Friday budget call");
    EXPECT_EQ(renamed.value["text"], from_audio.value["text"]) << "only the title changes";
    EXPECT_EQ(renamed.value["created_at"], from_audio.value["created_at"]);
    const auto edited = t.call(
        first, "update_transcript", in(first, 0, {{"id", id}, {"text", "We ship on Monday."}}));
    ASSERT_TRUE(edited.ok);
    EXPECT_EQ(edited.value["text"], "We ship on Monday.");
    EXPECT_EQ(edited.value["title"], "Friday budget call");
    EXPECT_EQ(t.call(first, "get_transcript", in(first, 0, {{"id", id}})).value["text"],
              "We ship on Monday.");
    EXPECT_EQ(t.call(first, "update_transcript", in(first, 0, {{"id", id}})).error.code,
              "invalid_input");
    EXPECT_EQ(
        t.call(first, "update_transcript", in(first, 1, {{"id", id}, {"title", "x"}})).error.code,
        "not_found");
    EXPECT_EQ(t.call(first, "delete_transcript", in(first, 1, {{"id", id}})).error.code,
              "not_found");
    const auto deleted = t.call(first, "delete_transcript", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value, (json{{"deleted", true}}));
    EXPECT_EQ(t.call(first, "get_transcript", in(first, 0, {{"id", id}})).error.code, "not_found");
    EXPECT_EQ(t.call(first, "delete_transcript", in(first, 0, {{"id", id}})).error.code,
              "not_found");
    EXPECT_EQ(list(first, 0, json::object()).value["transcripts"].size(), 2U);

    // A search finds a word deep in the text, and its excerpt is the text around it (lake#633).
    const auto long_call = t.call(
        first,
        "create_transcript",
        in(first,
           0,
           {{"title", "Quarterly review"},
            {"source", "text"},
            {"text", std::string(400, 'a') + " Dana: churn is up in the basic plan. Next."}}));
    ASSERT_TRUE(long_call.ok) << long_call.error.message;
    listed = list(first, 0, {{"q", "Churn is up"}});
    ASSERT_EQ(listed.value["transcripts"].size(), 1U);
    EXPECT_EQ(listed.value["transcripts"][0]["id"], long_call.value["id"]);
    const std::string excerpt = listed.value["transcripts"][0]["excerpt"];
    EXPECT_NE(excerpt.find("churn is up in the basic plan"), std::string::npos) << excerpt;
    EXPECT_EQ(excerpt.rfind("…", 0), 0U) << excerpt;
    EXPECT_EQ(list(first, 0, json::object())
                  .value["transcripts"][0]["excerpt"]
                  .get<std::string>()
                  .rfind("aaa", 0),
              0U)
        << "without a query the excerpt is the start";
    EXPECT_TRUE(list(first, 1, {{"q", "churn"}}).value["transcripts"].empty())
        << "each workspace answers for itself; Search asks every workspace";

    // Another space is refused before any workspace is read.
    for (const std::string capability : {"list_transcripts",
                                         "get_transcript",
                                         "create_transcript",
                                         "update_transcript",
                                         "delete_transcript"}) {
        json input = in(first, 0, {{"id", from_text.value["id"]}});
        if (capability == "list_transcripts") {
            input.erase("id");
        }
        if (capability == "create_transcript") {
            input = in(first, 0, {{"title", "x"}, {"source", "text"}, {"text", "x"}});
        }
        if (capability == "update_transcript") {
            input["title"] = "x";
        }
        EXPECT_EQ(t.call(second, capability, input).error.code, "unauthorized") << capability;
    }
}

// A workspace's own AI solutions beside Orama's read-only catalogue (docs/api/solutions.md,
// lake#597): created, listed with Orama's, edited, duplicated and deleted in their own workspace;
// Orama's refused for edit and delete by the server; another workspace and another space refused.
TEST_P(WorkspaceRecordStores, SolutionsAreTheWorkspacesOwnAndOramasStayReadOnly) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto in = [&](const auto& space, std::size_t workspace, json input) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[workspace];
        return input;
    };

    // Orama's catalogue, as the workspace's engine answers it.
    const json churn{
        {"id", "S-1"},
        {"name", "Churn & Retention"},
        {"promise", "Find who may leave, explain why, and route the right retention work."},
        {"use_case", "Identify preventable churn early enough for a governed intervention."},
        {"composition",
         json::array({{{"methodology", "M-1"}, {"role", "primary"}, {"minimum_level", 1}},
                      {{"methodology", "M-5"}, {"role", "attachable"}, {"minimum_level", 2}}})},
        {"pipeline_order", json::array({"M-2", "M-1", "M-5"})},
        {"measurement",
         {{"design", "holdout"},
          {"outcome", {{"name", "retained revenue"}, {"unit", "USD"}, {"window_days", 90}}}}},
        {"measurement_design", "Permanent holdout with randomized treatment cells."},
        {"decision_policy_template", "Risk and treatment tiers with contact caps."},
        {"status", "catalogue"},
        {"runnable", true}};
    bool catalogue_listed = true;
    t.zetta->responder =
        [&](const std::string& capability,
            const json& input) -> std::optional<lake::control::ZettaControlResult<json>> {
        if (capability == "list_solutions") {
            if (!catalogue_listed) {
                return lake::control::ZettaControlResult<json>(std::unexpected(
                    lake::control::ZettaControlError{"unknown_capability", "unknown"}));
            }
            return lake::control::ZettaControlResult<json>(
                json{{"solutions", json::array({churn})}});
        }
        if (capability == "get_solution") {
            if (input.value("solution", std::string()) == "S-1") {
                return lake::control::ZettaControlResult<json>(json{{"solution", churn}});
            }
            return lake::control::ZettaControlResult<json>(std::unexpected(
                lake::control::ZettaControlError{"not_found", "no solution named"}));
        }
        return std::nullopt;
    };

    // Create: the catalogue's shape as far as a person can author it.
    const json late{{"name", " Late payments "},
                    {"problem", "Invoices are paid late and cash runs short."},
                    {"decision", "Which customers collections calls first each morning."},
                    {"measurement",
                     {{"metric", "days sales outstanding"}, {"baseline", "52"}, {"target", "40"}}},
                    {"methodologies", json::array({"M-1", {{"id", "M-3"}, {"minimum_level", 2}}})},
                    {"pipeline", "collections/late-payers"},
                    {"notes", "Start with the enterprise accounts."}};
    const auto created = t.call(first, "create_solution", in(first, 0, late));
    ASSERT_TRUE(created.ok) << created.error.message;
    const json own = created.value["solution"];
    const std::string id = own["id"];
    EXPECT_TRUE(lake::control::workspace_solution_id(id));
    EXPECT_EQ(own["source"], "workspace");
    EXPECT_EQ(own["editable"], true);
    EXPECT_EQ(own["name"], "Late payments") << "trimmed";
    EXPECT_EQ(own["decision"], late["decision"]);
    EXPECT_EQ(own["measurement"],
              (json{{"metric", "days sales outstanding"}, {"baseline", "52"}, {"target", "40"}}));
    EXPECT_EQ(own["methodologies"],
              (json::array(
                  {{{"id", "M-1"}, {"minimum_level", 1}}, {{"id", "M-3"}, {"minimum_level", 2}}})));
    EXPECT_EQ(own["pipeline"], "collections/late-payers");
    EXPECT_TRUE(own["duplicated_from"].is_null());
    EXPECT_TRUE(own["updated_at"].is_null());
    EXPECT_EQ(own["created_by"], first.principal.user_id);
    const auto author = t.store->get_user(first.principal.user_id);
    ASSERT_TRUE(author);
    EXPECT_EQ(own["created_by_name"],
              author->display_name.empty() ? author->email : author->display_name);

    // Sealed at rest in the workspace's own Zetta; no other workspace holds it.
    const auto& rows =
        t.zetta->records["http://" + first.workspaces[0] + ".test"].rows("solutions");
    ASSERT_EQ(rows.size(), 1U);
    for (const auto& [key, row] : rows) {
        EXPECT_EQ(row.dump().find("Late payments"), std::string::npos) << "sealed at rest";
        EXPECT_EQ(row.dump().find("collections"), std::string::npos) << "sealed at rest";
    }
    EXPECT_TRUE(
        t.zetta->records["http://" + first.workspaces[1] + ".test"].rows("solutions").empty());

    // Refusals name the field.
    json no_metric = late;
    no_metric["measurement"] = {{"baseline", "52"}};
    EXPECT_EQ(t.call(first, "create_solution", in(first, 0, no_metric)).error.path,
              "/measurement/metric");
    json blank_decision = late;
    blank_decision["decision"] = "  ";
    const auto blank = t.call(first, "create_solution", in(first, 0, blank_decision));
    EXPECT_EQ(blank.error.code, "invalid_input");
    EXPECT_EQ(blank.error.path, "/decision");
    json twice = late;
    twice["methodologies"] = json::array({"M-1", "M-1"});
    EXPECT_EQ(t.call(first, "create_solution", in(first, 0, twice)).error.path,
              "/methodologies/1/id");
    json too_high = late;
    too_high["methodologies"] = json::array({{{"id", "M-1"}, {"minimum_level", 5}}});
    EXPECT_EQ(t.call(first, "create_solution", in(first, 0, too_high)).error.path,
              "/methodologies/0/minimum_level");
    json long_name = late;
    long_name["name"] = std::string(lake::control::max_solution_name_bytes + 1, 'a');
    EXPECT_EQ(t.call(first, "create_solution", in(first, 0, long_name)).error.path, "/name");

    // List both kinds, each marked; `source` keeps one kind.
    const auto list = [&](std::size_t workspace, json extra) {
        return t.call(first, "list_solutions", in(first, workspace, std::move(extra)));
    };
    auto listed = list(0, json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["solutions"].size(), 2U);
    EXPECT_EQ(listed.value["solutions"][0]["id"], "S-1");
    EXPECT_EQ(listed.value["solutions"][0]["source"], "orama");
    EXPECT_EQ(listed.value["solutions"][0]["editable"], false);
    EXPECT_EQ(listed.value["solutions"][0]["promise"], churn["promise"]) << "the engine's fields";
    EXPECT_EQ(listed.value["solutions"][1], own);
    EXPECT_EQ(listed.value["catalogue"], (json{{"available", true}, {"message", nullptr}}));
    EXPECT_EQ(list(0, {{"source", "orama"}}).value["solutions"].size(), 1U);
    const auto mine = list(0, {{"source", "workspace"}});
    ASSERT_EQ(mine.value["solutions"].size(), 1U);
    EXPECT_EQ(mine.value["solutions"][0]["id"], id);
    EXPECT_EQ(list(0, {{"source", "everyone"}}).error.path, "/source");
    const auto other_workspace = list(1, json::object());
    ASSERT_EQ(other_workspace.value["solutions"].size(), 1U) << "another workspace: Orama's only";
    EXPECT_EQ(other_workspace.value["solutions"][0]["source"], "orama");

    // Read both kinds; yours only in its own workspace.
    const auto get = [&](std::size_t workspace, const std::string& which) {
        return t.call(first, "get_solution", in(first, workspace, {{"id", which}}));
    };
    const auto got_orama = get(0, "S-1");
    ASSERT_TRUE(got_orama.ok) << got_orama.error.message;
    EXPECT_EQ(got_orama.value["solution"]["source"], "orama");
    EXPECT_EQ(got_orama.value["solution"]["editable"], false);
    EXPECT_EQ(got_orama.value["solution"]["name"], "Churn & Retention");
    EXPECT_EQ(get(0, id).value["solution"], own);
    EXPECT_EQ(get(1, id).error.code, "not_found");
    EXPECT_EQ(get(0, "S-99").error.code, "not_found");
    EXPECT_EQ(get(0, lake::control::new_uuid_v4()).error.code, "not_found");

    // Edit yours: the fields sent replace theirs, the others are kept.
    const auto renamed = t.call(first,
                                "update_solution",
                                in(first,
                                   0,
                                   {{"id", id},
                                    {"name", "Late payers"},
                                    {"measurement", {{"metric", "DSO"}, {"target", "38"}}}}));
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value["solution"]["name"], "Late payers");
    EXPECT_EQ(renamed.value["solution"]["measurement"],
              (json{{"metric", "DSO"}, {"baseline", nullptr}, {"target", "38"}}));
    EXPECT_EQ(renamed.value["solution"]["problem"], own["problem"]);
    EXPECT_EQ(renamed.value["solution"]["methodologies"], own["methodologies"]);
    EXPECT_EQ(renamed.value["solution"]["created_at"], own["created_at"]);
    EXPECT_EQ(renamed.value["solution"]["updated_by"], first.principal.user_id);
    EXPECT_TRUE(renamed.value["solution"]["updated_at"].is_string());
    EXPECT_EQ(get(0, id).value["solution"]["name"], "Late payers");
    const auto cleared =
        t.call(first,
               "update_solution",
               in(first, 0, {{"id", id}, {"methodologies", json::array()}, {"notes", ""}}));
    ASSERT_TRUE(cleared.ok) << cleared.error.message;
    EXPECT_TRUE(cleared.value["solution"]["methodologies"].empty());
    EXPECT_TRUE(cleared.value["solution"]["notes"].is_null());
    EXPECT_EQ(t.call(first, "update_solution", in(first, 0, {{"id", id}})).error.code,
              "invalid_input");
    EXPECT_EQ(
        t.call(first, "update_solution", in(first, 0, {{"id", id}, {"problem", " "}})).error.path,
        "/problem");
    EXPECT_EQ(
        t.call(first, "update_solution", in(first, 1, {{"id", id}, {"name", "x"}})).error.code,
        "not_found")
        << "another workspace of the space cannot edit it";

    // Orama's are read-only: the server refuses, with the reason and what to do instead.
    const auto edit_orama =
        t.call(first, "update_solution", in(first, 0, {{"id", "S-1"}, {"name", "Mine now"}}));
    EXPECT_EQ(edit_orama.error.code, "forbidden");
    EXPECT_EQ(edit_orama.error.path, "/id");
    EXPECT_NE(edit_orama.error.message.find("S-1 (Churn & Retention) is one of Orama's catalogue "
                                            "solutions, which are read-only and cannot be edited"),
              std::string::npos)
        << edit_orama.error.message;
    EXPECT_NE(edit_orama.error.message.find("duplicate_solution"), std::string::npos);
    const auto delete_orama = t.call(first, "delete_solution", in(first, 0, {{"id", "S-1"}}));
    EXPECT_EQ(delete_orama.error.code, "forbidden");
    EXPECT_NE(delete_orama.error.message.find("cannot be deleted"), std::string::npos);
    EXPECT_EQ(lake::api::rest_status(delete_orama), 403);
    EXPECT_EQ(t.call(first, "delete_solution", in(first, 0, {{"id", "S-99"}})).error.code,
              "not_found");
    EXPECT_EQ(get(0, "S-1").value["solution"]["name"], "Churn & Retention") << "unchanged";

    // Duplicate Orama's into an editable one of your own.
    const auto from_orama = t.call(first, "duplicate_solution", in(first, 0, {{"id", "S-1"}}));
    ASSERT_TRUE(from_orama.ok) << from_orama.error.message;
    const json copy = from_orama.value["solution"];
    EXPECT_EQ(copy["source"], "workspace");
    EXPECT_EQ(copy["editable"], true);
    EXPECT_EQ(copy["name"], "Churn & Retention");
    EXPECT_EQ(copy["problem"], churn["use_case"]);
    EXPECT_EQ(copy["decision"], churn["promise"]);
    EXPECT_EQ(copy["measurement"]["metric"], "retained revenue (USD)");
    EXPECT_EQ(copy["methodologies"],
              (json::array(
                  {{{"id", "M-1"}, {"minimum_level", 1}}, {{"id", "M-5"}, {"minimum_level", 2}}})));
    EXPECT_EQ(copy["pipeline"], "Methodologies in order: M-2, M-1, M-5");
    EXPECT_EQ(copy["duplicated_from"], "S-1");
    EXPECT_NE(copy["notes"].get<std::string>().find("Duplicated from Orama's S-1"),
              std::string::npos);
    const auto edited_copy =
        t.call(first, "update_solution", in(first, 0, {{"id", copy["id"]}, {"name", "Our churn"}}));
    ASSERT_TRUE(edited_copy.ok) << edited_copy.error.message;
    EXPECT_EQ(edited_copy.value["solution"]["name"], "Our churn");

    // Duplicate another of yours, named.
    const auto from_own = t.call(
        first, "duplicate_solution", in(first, 0, {{"id", id}, {"name", "Late payers, EMEA"}}));
    ASSERT_TRUE(from_own.ok) << from_own.error.message;
    EXPECT_NE(from_own.value["solution"]["id"], id);
    EXPECT_EQ(from_own.value["solution"]["name"], "Late payers, EMEA");
    EXPECT_EQ(from_own.value["solution"]["duplicated_from"], id);
    EXPECT_EQ(from_own.value["solution"]["measurement"]["metric"], "DSO");
    EXPECT_TRUE(from_own.value["solution"]["updated_at"].is_null());
    const auto unnamed = t.call(first, "duplicate_solution", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(unnamed.ok) << unnamed.error.message;
    EXPECT_EQ(unnamed.value["solution"]["name"], "Late payers (copy)");
    EXPECT_EQ(t.call(first, "duplicate_solution", in(first, 0, {{"id", "S-99"}})).error.code,
              "not_found");
    EXPECT_EQ(t.call(first, "duplicate_solution", in(first, 1, {{"id", id}})).error.code,
              "not_found")
        << "another workspace cannot copy it";
    EXPECT_EQ(list(0, {{"source", "workspace"}}).value["solutions"].size(), 4U);

    // Delete yours, in its own workspace only.
    EXPECT_EQ(t.call(first, "delete_solution", in(first, 1, {{"id", id}})).error.code, "not_found");
    const auto deleted = t.call(first, "delete_solution", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value, (json{{"deleted", true}}));
    EXPECT_EQ(get(0, id).error.code, "not_found");
    EXPECT_EQ(t.call(first, "delete_solution", in(first, 0, {{"id", id}})).error.code, "not_found");
    EXPECT_EQ(list(0, {{"source", "workspace"}}).value["solutions"].size(), 3U);

    // An engine that does not list Orama's: yours are still listed, and the answer says why.
    catalogue_listed = false;
    const auto without = list(0, json::object());
    ASSERT_TRUE(without.ok) << without.error.message;
    EXPECT_EQ(without.value["solutions"].size(), 3U);
    EXPECT_EQ(without.value["catalogue"]["available"], false);
    EXPECT_NE(without.value["catalogue"]["message"].get<std::string>().find("does not list"),
              std::string::npos);

    // Another space is refused before any workspace is read.
    const std::string kept = copy["id"];
    for (const std::string capability : {"list_solutions",
                                         "get_solution",
                                         "create_solution",
                                         "update_solution",
                                         "delete_solution",
                                         "duplicate_solution"}) {
        json input = in(first, 0, {{"id", kept}});
        if (capability == "list_solutions") {
            input.erase("id");
        }
        if (capability == "create_solution") {
            input = in(first, 0, late);
        }
        if (capability == "update_solution") {
            input["name"] = "x";
        }
        EXPECT_EQ(t.call(second, capability, input).error.code, "unauthorized") << capability;
    }
    EXPECT_EQ(get(0, kept).value["solution"]["name"], "Our churn") << "unchanged";
}

TEST(WorkspaceRecords, AnOpportunityStatusChangeRereadsAfterAConcurrentWrite) {
    DirectRecords direct;
    auto records = direct.open();
    lake::control::Opportunity row{.customer_id = "customer-1",
                                   .task_id = lake::control::new_uuid_v4(),
                                   .rank = 1,
                                   .intent = "Predict churn",
                                   .status = "new"};
    const auto written = run(lake::control::record_opportunities(records, {row, [&] {
        auto declined = row;
        declined.rank = 2;
        declined.intent = "Forecast";
        declined.status = "not_yet";
        return declined;
    }()}));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, 2U);
    EXPECT_EQ(*run(lake::control::record_opportunities(records, {row})), 0U)
        << "a scan already saved is not written again";
    const std::string id = lake::control::opportunity_id(row.task_id, 1);
    EXPECT_EQ(id, lake::control::opportunity_id(row.task_id, 1)) << "the key is derived";
    EXPECT_NE(id, lake::control::opportunity_id(row.task_id, 2));
    EXPECT_EQ(id[14], '8') << "a version 8 UUID";

    // A write that lands between the read and the write is reread, not overwritten blindly.
    auto& stored = direct.tables->rows("opportunities").at(id);
    const auto before = stored["revision"].get<std::int64_t>();
    bool raced = false;
    auto racing = WorkspaceRecords::open(
        [&, tables = direct.tables](std::string capability, bool, json input)
            -> liquid::runtime::task<lake::control::ZettaControlResult<json>> {
            if (capability == "put_record" && !raced) {
                raced = true;
                auto& live = tables->rows("opportunities").at(id);
                live["revision"] = live["revision"].get<std::int64_t>() + 1000;
            }
            co_return tables->run(capability, input);
        },
        direct.workspace_id, kek);
    ASSERT_TRUE(racing);
    const auto changed = run(lake::control::set_opportunity_status(*racing, "customer-1", id,
                                                                   "project", "churn-1a2b"));
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_TRUE(raced);
    EXPECT_EQ(changed->status, "project");
    EXPECT_EQ(changed->project, "churn-1a2b");
    EXPECT_EQ(changed->lake_revision, 2);
    EXPECT_NE(direct.tables->rows("opportunities").at(id)["revision"].get<std::int64_t>(), before);
    EXPECT_EQ(run(lake::control::set_opportunity_status(records, "customer-2", id, "dismissed", ""))
                  .error()
                  .code,
              "not_found")
        << "another space's id never opens it";
    EXPECT_EQ(run(lake::control::set_opportunity_status(records, "customer-1", id, "gone", ""))
                  .error()
                  .code,
              "invalid_input");
    const auto listed = run(lake::control::list_opportunities(records, "customer-1"));
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 2U);
    EXPECT_EQ(listed->at(0).status, "project");
    EXPECT_EQ(listed->at(1).status, "not_yet");
}

// lake#315 part 2 (decision 54, S10): a repository's description is one sealed settings record in
// the workspace Zetta of its workspace, keyed by the repository; the routing row holds none of it.
TEST_P(WorkspaceRecordStores, RepositoryDescriptionsLiveInTheirWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto url = [](const std::string& workspace) { return "http://" + workspace + ".test"; };
    struct Placed {
        const TwoSpaces::Space* space;
        std::string workspace, repository, description;
    };
    std::vector<Placed> placed; // first/home, first/lab, second/home, second/lab
    for (const auto& space : t.spaces) {
        for (std::size_t index = 0; index < space.workspaces.size(); ++index) {
            const std::string description = "About " + space.workspaces[index];
            const auto created = t.call(space, "create_repository",
                                        {{"customer_id", space.customer_id},
                                         {"workspace_id", space.workspaces[index]},
                                         {"name", "described-" + std::to_string(index)},
                                         {"description", description}});
            ASSERT_TRUE(created.ok) << created.error.message;
            EXPECT_EQ(created.value["description"], description) << "the create answers it";
            placed.push_back({&space, space.workspaces[index], created.value["id"], description});
        }
    }
    const auto records_in = [&](const std::string& workspace) {
        return t.zetta->records[url(workspace)].rows("repositories").size();
    };

    // One sealed record per repository, only at its own workspace's endpoint; the routing row
    // holds no description.
    for (const auto& where : placed) {
        const auto& rows = t.zetta->records[url(where.workspace)].rows("repositories");
        ASSERT_EQ(rows.size(), 1U) << where.workspace;
        EXPECT_EQ(rows.begin()->first, where.repository);
        EXPECT_EQ(rows.begin()->second.dump().find(where.description), std::string::npos)
            << "the body is sealed";
        const auto routing = t.store->get_repository(where.repository);
        ASSERT_TRUE(routing);
        EXPECT_TRUE(routing->description.empty());
    }
    if (GetParam()) {
        // The routing row has no description column at all (migration 0070 dropped it).
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT 1 FROM information_schema.columns WHERE table_name = "
                            "'repositories' AND column_name = 'description'")
                        .empty());
    }
    // The store refuses a description: it belongs to the workspace.
    const auto refused_row = t.store->create_repository(
        {first.customer_id, "column", "in the row", "private", first.workspaces[0]});
    ASSERT_FALSE(refused_row);
    EXPECT_EQ(refused_row.error().code, "invalid_input");

    // Reads fill the description from the workspace; a listing reads each workspace once.
    for (const auto& where : placed) {
        const auto got = t.call(*where.space, "get_repository", {{"id", where.repository}});
        ASSERT_TRUE(got.ok) << got.error.message;
        EXPECT_EQ(got.value["description"], where.description);
        EXPECT_FALSE(got.value.contains("description_unavailable"));
        EXPECT_EQ(got.value["workspace_id"], where.workspace);
    }
    t.zetta->calls.clear();
    const auto listed = t.call(first, "list_repositories", {{"customer_id", first.customer_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 2U);
    for (const json& item : listed.value["items"]) {
        EXPECT_EQ(item["description"], "About " + item["workspace_id"].get<std::string>());
        EXPECT_FALSE(item.contains("description_unavailable"));
    }
    EXPECT_EQ(std::ranges::count_if(t.zetta->calls,
                                    [](const auto& call) { return call.first == "list_records"; }),
              2)
        << "one list_records per workspace";
    EXPECT_EQ(t.zetta->calls.size(), 2U);
    // The synchronous call (lake's own page routing) answers the routing row alone.
    auto routing_context = t.context(first);
    const auto routed = t.registry.call("get_repository", {{"id", placed[0].repository}},
                                        routing_context);
    ASSERT_TRUE(routed.ok) << routed.error.message;
    EXPECT_EQ(routed.value["description"], "");

    // A repository created without a description has no record and reads an empty one.
    const auto plain = t.call(first, "create_repository",
                              {{"customer_id", first.customer_id},
                               {"workspace_id", first.workspaces[0]},
                               {"name", "plain"}});
    ASSERT_TRUE(plain.ok) << plain.error.message;
    EXPECT_EQ(records_in(first.workspaces[0]), 1U);
    const auto plain_read = t.call(first, "get_repository", {{"id", plain.value["id"]}});
    EXPECT_EQ(plain_read.value["description"], "");
    EXPECT_FALSE(plain_read.value.contains("description_unavailable"));

    // Missing, malformed, unknown and foreign workspace ids are refused at /workspace_id before
    // anything is written; a taken name leaves no record behind.
    const auto create = [&](const TwoSpaces::Space& space, json workspace, std::string name) {
        json input{{"customer_id", space.customer_id},
                   {"name", std::move(name)},
                   {"description", "Refused"}};
        if (!workspace.is_null()) {
            input["workspace_id"] = std::move(workspace);
        }
        return t.call(space, "create_repository", input);
    };
    for (const json& wrong : {json(nullptr), json("not-a-workspace"),
                              json(lake::control::new_uuid_v4()), json(second.workspaces[0])}) {
        const auto refused = create(first, wrong, "refused");
        EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
        // The registry names an unknown workspace by the space it does not belong to.
        EXPECT_TRUE(refused.error.path == "/workspace_id" || refused.error.path == "/customer_id")
            << wrong << ' ' << refused.error.path;
    }
    const auto taken = create(first, first.workspaces[0], "described-0");
    EXPECT_EQ(taken.error.code, "conflict");
    EXPECT_EQ(taken.error.path, "/name");
    for (const auto& where : placed) {
        EXPECT_EQ(records_in(where.workspace), 1U) << "a refused create leaves nothing";
    }
    // The synchronous call cannot write a workspace, so it refuses a description.
    auto sync_context = t.context(first);
    const auto sync_created = t.registry.call("create_repository",
                                              {{"customer_id", first.customer_id},
                                               {"workspace_id", first.workspaces[0]},
                                               {"name", "synchronous"},
                                               {"description", "Refused"}},
                                              sync_context);
    EXPECT_EQ(sync_created.error.code, "invalid_input");
    EXPECT_EQ(sync_created.error.path, "/description");

    // Least privilege: another space reads neither the repository nor its description, and
    // cannot create in this space.
    EXPECT_EQ(t.call(second, "get_repository", {{"id", placed[0].repository}}).error.code,
              "unauthorized");
    const auto their_list =
        t.call(second, "list_repositories", {{"customer_id", first.customer_id}});
    EXPECT_TRUE(!their_list.ok || their_list.value["items"].empty()) << their_list.value;
    auto trespass = t.context(second);
    const auto trespassed = liquid::runtime::block_on(t.registry.call_async(
        "create_repository",
        {{"customer_id", first.customer_id},
         {"workspace_id", first.workspaces[0]},
         {"name", "trespass"},
         {"description", "Refused"}},
        trespass));
    ASSERT_TRUE(trespassed);
    EXPECT_EQ(trespassed->error.code, "unauthorized");

    // A change is written at the revision read; a raced change is reread and neither is lost.
    auto context = t.context(first);
    auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[0]);
    ASSERT_TRUE(records) << records.error().message;
    int changes = 0;
    const auto raced = run(lake::control::update_repository_settings(
            *records, placed[0].repository, first.customer_id,
            [&](lake::control::RepositorySettings& settings) {
                if (++changes == 1) {
                    // Another lake process changes it between this read and this write.
                    const auto other = run(lake::control::update_repository_settings(
                        *records, placed[0].repository, first.customer_id,
                        [](lake::control::RepositorySettings& value) {
                            value.description += " (edited)";
                        }));
                    EXPECT_TRUE(other);
                }
                settings.description += " again";
            }));
    ASSERT_TRUE(raced) << raced.error().message;
    EXPECT_EQ(changes, 2) << "the conflicting write was reread";
    EXPECT_EQ(raced->description, placed[0].description + " (edited) again");
    EXPECT_EQ(raced->lake_revision, 3);
    // A stale revision is refused.
    const auto stored = run(records->get(lake::control::RepositorySettingsCodec::collection,
                                         placed[0].repository));
    ASSERT_TRUE(stored && *stored);
    const auto stale = run(records->put(
        lake::control::RepositorySettingsCodec::collection, placed[0].repository,
        lake::control::repository_settings_lookup(first.customer_id), (*stored)->body,
        lake::control::RecordCondition{.if_revision = (*stored)->revision - 1}, 4,
        (*stored)->at_us));
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "conflict");
    // The plain repository gets its first record when one is set.
    const auto first_set = run(lake::control::update_repository_settings(
        *records, plain.value["id"], first.customer_id,
        [](lake::control::RepositorySettings& value) { value.description = "Set later"; }));
    ASSERT_TRUE(first_set) << first_set.error().message;
    EXPECT_EQ(first_set->lake_revision, 1);
    EXPECT_EQ(t.call(first, "get_repository", {{"id", plain.value["id"]}}).value["description"],
              "Set later");

    // A record copied under another repository's key, or into another workspace, does not open
    // there: ownership cannot change.
    auto& home_rows = t.zetta->records[url(first.workspaces[0])].rows("repositories");
    auto& lab_rows = t.zetta->records[url(first.workspaces[1])].rows("repositories");
    const json kept = lab_rows.at(placed[1].repository);
    lab_rows[placed[1].repository]["value"] = home_rows.at(placed[0].repository)["value"];
    EXPECT_EQ(t.call(first, "get_repository", {{"id", placed[1].repository}}).value["description"],
              "");
    lab_rows[placed[1].repository] = kept;

    // A workspace that is not ready: reads name the reason instead of falling back, and a create
    // with a description is refused with no row left behind.
    const auto lab = t.store->get_workspace(first.workspaces[1]);
    t.status(lab->zetta_service_id, "Provisioning", url(first.workspaces[1]));
    const auto not_ready = t.call(first, "get_repository", {{"id", placed[1].repository}});
    ASSERT_TRUE(not_ready.ok) << not_ready.error.message;
    EXPECT_EQ(not_ready.value["description"], "");
    EXPECT_NE(not_ready.value["description_unavailable"].get<std::string>().find(
                  "workspace_not_ready"),
              std::string::npos)
        << not_ready.value;
    const auto partial = t.call(first, "list_repositories", {{"customer_id", first.customer_id}});
    ASSERT_TRUE(partial.ok) << partial.error.message;
    for (const json& item : partial.value["items"]) {
        EXPECT_EQ(item.contains("description_unavailable"),
                  item["workspace_id"] == first.workspaces[1])
            << item;
    }
    const auto waiting = create(first, first.workspaces[1], "waiting");
    EXPECT_EQ(waiting.error.code, "workspace_not_ready");
    const auto rows = t.store->list_repositories(first.customer_id, 100, "");
    ASSERT_TRUE(rows);
    EXPECT_FALSE(std::ranges::any_of(
        *rows, [](const lake::control::Repository& row) { return row.name == "waiting"; }));
    t.ready(first.workspaces[1]);
    EXPECT_EQ(t.call(first, "get_repository", {{"id", placed[1].repository}}).value["description"],
              placed[1].description);

    // Workspace deletion: once the repository and its workspace are deleted, the description goes
    // with the workspace, and the other workspaces keep theirs.
    ASSERT_TRUE(t.call(first, "delete_repository", {{"id", placed[1].repository}}).ok);
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    const auto gone = t.call(first, "get_repository", {{"id", placed[1].repository}});
    EXPECT_TRUE(!gone.ok || gone.value.contains("description_unavailable")) << gone.value;
    EXPECT_EQ(t.call(first, "get_repository", {{"id", placed[0].repository}}).value["description"],
              placed[0].description + " (edited) again");
    EXPECT_EQ(t.call(second, "get_repository", {{"id", placed[2].repository}}).value["description"],
              placed[2].description);
}

// lake#317, decision 52: an event about a workspace's data goes to that workspace's own audit,
// the chained, sealed collection lake_audit_events of its Zetta, queued on the request path and
// written by the AuditRecorder's flush; control-plane events, refusals for want of authority and
// calls that name no workspace stay in the central log. The readers show both.
TEST_P(WorkspaceRecordStores, AuditEventsAboutAWorkspaceLiveInItsOwnChain) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string& home = first.workspaces[0];
    const std::string& lab = first.workspaces[1];
    lake::api::AuditRecorder recorder(t.store.get(), t.service.get(), std::string(kek), "");
    const auto url = [](const std::string& workspace) { return "http://" + workspace + ".test"; };
    const auto rows = [&](const std::string& workspace) -> auto& {
        return t.zetta->records[url(workspace)].rows("audit_events");
    };
    const auto call_with = [&](lake::api::ApiContext context, lake::api::AuditRecorder* writer,
                               const std::string& capability, const json& input,
                               const std::string& request_id) {
        context.audit_recorder = writer;
        context.request_id = request_id;
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const auto call = [&](const TwoSpaces::Space& space, const std::string& capability,
                          const json& input, const std::string& request_id) {
        return call_with(t.context(space), &recorder, capability, input, request_id);
    };
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    const auto as_operator = [&](const std::string& capability, const json& input) {
        return call_with(operator_context, &recorder, capability, input,
                         lake::control::new_uuid_v4());
    };
    const auto central = [&](const std::string& request_id) {
        const auto events = lake::test::audit_events(*t.store, {.request_id = request_id});
        EXPECT_TRUE(events);
        return events ? *events : std::vector<lake::audit::Event>{};
    };
    const auto ask = [&](const TwoSpaces::Space& space, const std::string& customer_id,
                         const json& workspace_id) {
        const std::string request_id = lake::control::new_uuid_v4();
        call(space, "get_ask_conversation",
             {{"customer_id", customer_id},
              {"workspace_id", workspace_id},
              {"conversation_id", lake::control::new_uuid_v4()}},
             request_id);
        return request_id;
    };

    // A call about a workspace's data only queues its event: nothing central, nothing written.
    const std::string home_read = ask(first, first.customer_id, home);
    EXPECT_TRUE(central(home_read).empty());
    EXPECT_EQ(recorder.pending(), 1U);
    EXPECT_TRUE(rows(home).empty()) << "no workspace round trip on the request path";
    // The person sees it at once, as queued, and the operator too.
    auto mine = call(first, "my_audit", {{"include_reads", true}}, lake::control::new_uuid_v4());
    ASSERT_TRUE(mine.ok) << mine.error.message;
    const auto find = [](const json& items, const std::string& request_id) {
        for (const json& item : items) {
            if (item.value("request_id", "") == request_id) {
                return item;
            }
        }
        return json();
    };
    json queued = find(mine.value["items"], home_read);
    ASSERT_FALSE(queued.is_null());
    EXPECT_EQ(queued["log"], "workspace");
    EXPECT_EQ(queued["workspace_id"], home);
    EXPECT_TRUE(queued.value("pending", false));

    // Control-plane calls, refusals for want of authority and calls naming no workspace stay
    // central.
    const std::string listing = lake::control::new_uuid_v4();
    call(first, "list_workspaces", {{"customer_id", first.customer_id}}, listing);
    ASSERT_EQ(central(listing).size(), 1U);
    const std::string foreign = ask(first, second.customer_id, second.workspaces[0]);
    ASSERT_EQ(central(foreign).size(), 1U);
    EXPECT_EQ(central(foreign)[0].draft.outcome, "denied");
    const std::string malformed = ask(first, first.customer_id, "not-a-workspace");
    ASSERT_EQ(central(malformed).size(), 1U);
    EXPECT_EQ(central(malformed)[0].draft.details["error_code"], "invalid_input");
    const std::string unknown = ask(first, first.customer_id, lake::control::new_uuid_v4());
    ASSERT_EQ(central(unknown).size(), 1U);
    const std::string missing = lake::control::new_uuid_v4();
    call(first, "get_ask_conversation", {{"customer_id", first.customer_id}}, missing);
    ASSERT_EQ(central(missing).size(), 1U);
    EXPECT_EQ(recorder.pending(), 1U);

    // The flush writes the chain, sealed, in that workspace only.
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().written, 1U);
    ASSERT_EQ(rows(home).size(), 1U);
    EXPECT_TRUE(rows(lab).empty());
    EXPECT_TRUE(rows(second.workspaces[0]).empty());
    for (const auto& [key, row] : rows(home)) {
        EXPECT_EQ(row.dump().find(first.principal.taap_id), std::string::npos);
        EXPECT_EQ(row.dump().find("get_ask_conversation"), std::string::npos);
    }

    // More events in both of the first space's workspaces, and a second lake replica appending
    // to the same chain: it never forks.
    lake::api::AuditRecorder replica(t.store.get(), t.service.get(), std::string(kek), "");
    std::vector<std::string> home_reads{home_read};
    for (int index = 0; index < 3; ++index) {
        home_reads.push_back(ask(first, first.customer_id, home));
    }
    const std::string lab_read = ask(first, first.customer_id, lab);
    const std::string replica_read = lake::control::new_uuid_v4();
    call_with(t.context(first), &replica, "get_ask_conversation",
              {{"customer_id", first.customer_id},
               {"workspace_id", home},
               {"conversation_id", lake::control::new_uuid_v4()}},
              replica_read);
    home_reads.push_back(replica_read);
    liquid::runtime::block_on(replica.flush());
    liquid::runtime::block_on(recorder.flush());
    EXPECT_GT(recorder.totals().conflicts, 0U) << "the replica took the place this one knew";
    EXPECT_EQ(rows(home).size(), home_reads.size());
    EXPECT_EQ(rows(lab).size(), 1U);
    auto verified = as_operator("verify_audit", {{"workspace_id", home}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_EQ(verified.value["log"], "workspace");
    EXPECT_EQ(verified.value["chained"], home_reads.size());
    EXPECT_EQ(verified.value["last_sequence"], home_reads.size());
    EXPECT_TRUE(as_operator("verify_audit", json::object()).ok)
        << "the central chain still verifies";

    // The readers: the operator's audit shows both logs; one workspace's alone; each person's
    // activity reads only their own spaces' workspaces.
    auto everything = as_operator("audit", {{"limit", 1000}, {"include_reads", true}});
    ASSERT_TRUE(everything.ok) << everything.error.message;
    for (const std::string& request_id : home_reads) {
        const json item = find(everything.value["items"], request_id);
        ASSERT_FALSE(item.is_null()) << request_id;
        EXPECT_EQ(item["log"], "workspace");
        EXPECT_GT(item.value("sequence", 0), 0);
        EXPECT_FALSE(item.value("signature", "").empty());
    }
    EXPECT_EQ(find(everything.value["items"], listing)["log"], "central");
    auto lab_only = as_operator("audit", {{"workspace_id", lab}, {"include_reads", true}});
    ASSERT_TRUE(lab_only.ok) << lab_only.error.message;
    ASSERT_EQ(lab_only.value["items"].size(), 1U);
    EXPECT_EQ(lab_only.value["items"][0]["request_id"], lab_read);
    auto central_only = as_operator("audit", {{"log", "central"}, {"limit", 1000}});
    EXPECT_TRUE(find(central_only.value["items"], lab_read).is_null());
    mine = call(first, "my_audit", {{"include_reads", true}, {"limit", 1000}},
                lake::control::new_uuid_v4());
    ASSERT_TRUE(mine.ok);
    EXPECT_FALSE(find(mine.value["items"], lab_read).is_null());
    EXPECT_FALSE(find(mine.value["items"], listing).is_null());
    const auto theirs = call(second, "my_audit", {{"include_reads", true}, {"limit", 1000}},
                             lake::control::new_uuid_v4());
    ASSERT_TRUE(theirs.ok);
    EXPECT_TRUE(find(theirs.value["items"], lab_read).is_null());
    EXPECT_EQ(call(second, "my_audit", {{"workspace_id", lab}}, lake::control::new_uuid_v4())
                  .error.code,
              "unauthorized")
        << "another space's workspace";
    // Reads are left out of a person's activity by default, wherever they are recorded.
    mine = call(first, "my_audit", {{"limit", 1000}}, lake::control::new_uuid_v4());
    EXPECT_TRUE(find(mine.value["items"], lab_read).is_null());

    // Export: one workspace's whole chain.
    const auto exported = as_operator("export_audit", {{"workspace_id", home}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    const std::string jsonl = exported.value["jsonl"];
    EXPECT_EQ(std::ranges::count(jsonl, '\n'), static_cast<long>(home_reads.size()));
    EXPECT_NE(jsonl.find("\"log\":\"workspace\""), std::string::npos);

    // Retention: the central setting stays; a workspace's audit is kept with the workspace.
    const auto retention =
        as_operator("set_audit_retention", {{"customer_id", first.customer_id}, {"days", 500}});
    ASSERT_TRUE(retention.ok) << retention.error.message;
    EXPECT_EQ(retention.value["workspace_audit_retention"], "kept until the workspace is deleted");

    // A workspace that is not ready keeps its events for the next flush.
    const auto lab_row = t.store->get_workspace(lab);
    ASSERT_TRUE(lab_row);
    t.status(lab_row->zetta_service_id, "Pending", url(lab));
    ask(first, first.customer_id, lab);
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(recorder.pending(), 1U);
    EXPECT_GT(recorder.totals().flush_errors, 0U);
    auto unread = as_operator("audit", {{"customer_id", first.customer_id}});
    ASSERT_TRUE(unread.ok);
    ASSERT_EQ(unread.value["unread_workspaces"].size(), 1U);
    EXPECT_EQ(unread.value["unread_workspaces"][0]["workspace_id"], lab);
    t.ready(lab);
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(rows(lab).size(), 2U);

    // Tampering is found: a removed event breaks the chain, and a value moved from another
    // workspace does not open there.
    auto context = t.context(first);
    auto home_records = lake::api::workspace_records(context, first.customer_id, home);
    ASSERT_TRUE(home_records);
    const auto collection = lake::control::WorkspaceAuditCodec::collection;
    const std::string second_place = home_records->index_of(collection, "sequence:2");
    const json removed = rows(home).at(second_place);
    rows(home).erase(second_place);
    auto broken = as_operator("verify_audit", {{"workspace_id", home}});
    EXPECT_FALSE(broken.ok);
    EXPECT_EQ(broken.error.code, "invalid_chain");
    rows(home)[second_place] = removed;
    EXPECT_TRUE(as_operator("verify_audit", {{"workspace_id", home}}).ok);
    rows(home)[second_place]["value"] = rows(lab).begin()->second["value"];
    EXPECT_EQ(as_operator("verify_audit", {{"workspace_id", home}}).error.code, "invalid_chain");
    rows(home)[second_place] = removed;

    // A deleted workspace's queued events go with it.
    const std::string doomed = second.workspaces[1];
    ask(second, second.customer_id, doomed);
    EXPECT_EQ(recorder.pending(), 1U);
    ASSERT_TRUE(t.store->update_workspace(doomed, {.desired = "deleted"}));
    const auto doomed_row = t.store->get_workspace(doomed);
    t.status(doomed_row->zetta_service_id, "Deleted", url(doomed));
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().dropped_gone, 1U);
    const std::string after_delete = ask(second, second.customer_id, doomed);
    EXPECT_EQ(recorder.pending(), 0U) << "a call naming a deleted workspace is refused centrally";
    EXPECT_EQ(central(after_delete).size(), 1U);

    // Bounded: rejection is explicit and signs only delivery failure evidence centrally.
    lake::api::AuditRecorder bounded(t.store.get(), t.service.get(), std::string(kek), "", 1);
    const std::string kept = lake::control::new_uuid_v4();
    const std::string dropped = lake::control::new_uuid_v4();
    for (const auto& request_id : {kept, dropped}) {
        const auto result = call_with(t.context(first),
                                      &bounded,
                                      "get_ask_conversation",
                                      {{"customer_id", first.customer_id},
                                       {"workspace_id", home},
                                       {"conversation_id", lake::control::new_uuid_v4()}},
                                      request_id);
        if (request_id == dropped) {
            EXPECT_EQ(result.error.code, "audit_queue_full");
        }
    }
    EXPECT_EQ(bounded.pending(), 1U);
    EXPECT_EQ(bounded.totals().dropped, 1U);
    ASSERT_EQ(central(dropped).size(), 1U);
    EXPECT_EQ(central(dropped).front().draft.action, "audit_delivery_failed");

    // Without a workspace audit (no AuditRecorder: a lake with no workspace Zettas), every
    // event is central.
    const std::string single_store = lake::control::new_uuid_v4();
    call_with(t.context(first), nullptr, "get_ask_conversation",
              {{"customer_id", first.customer_id},
               {"workspace_id", home},
               {"conversation_id", lake::control::new_uuid_v4()}},
              single_store);
    EXPECT_EQ(central(single_store).size(), 1U);
}

TEST(AuditRecorder, OverflowReportsWhenDeliveryEvidenceCannotBeRecorded) {
    lake::api::AuditRecorder recorder(nullptr, nullptr, "", "", 2);
    ASSERT_TRUE(recorder.add("customer", "workspace", lake::audit::pending({.action = "kept"})));
    lake::api::ApiContext context;
    context.audit_recorder = &recorder;
    const auto rejected =
        lake::api::record_audit(context,
                                {.action = "workspace_query"},
                                {lake::api::AuditLog::Workspace, "customer", "workspace"});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "audit_queue_full");
    EXPECT_NE(rejected.error().message.find("evidence could not be recorded"), std::string::npos);
    EXPECT_TRUE(recorder.add(
        "other-customer", "other-workspace", lake::audit::pending({.action = "retained"})));
}

// LSR-098: an unavailable customer's workspaces cannot consume another customer's share,
// including while a flush has detached the queues and new requests arrive.
TEST_P(WorkspaceRecordStores, AuditQueueIsolatesUnavailableCustomersDuringFlush) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    lake::api::AuditRecorder recorder(t.store.get(), t.service.get(), std::string(kek), "", 8);
    const auto event = [] {
        return lake::audit::pending({.kind = "capability",
                                     .principal = "test",
                                     .action = "workspace_query",
                                     .outcome = "failure"});
    };
    for (const auto& workspace : first.workspaces) {
        const auto row = t.store->get_workspace(workspace);
        ASSERT_TRUE(row);
        t.status(row->zetta_service_id, "Pending", "http://" + workspace + ".test");
        EXPECT_TRUE(recorder.add(first.customer_id, workspace, event()));
        EXPECT_TRUE(recorder.add(first.customer_id, workspace, event()));
        EXPECT_FALSE(recorder.add(first.customer_id, workspace, event()));
    }
    EXPECT_FALSE(recorder.add(first.customer_id, lake::control::new_uuid_v4(), event()))
        << "more workspaces do not bypass the customer allocation";
    const auto workspace = second.workspaces[0];
    ASSERT_TRUE(recorder.add(second.customer_id, workspace, event()));
    auto context = t.context(second);
    auto records = lake::api::workspace_records(context, second.customer_id, workspace);
    ASSERT_TRUE(records);
    auto& tables = t.zetta->records["http://" + workspace + ".test"];
    const auto key =
        records->index_of(lake::control::WorkspaceAuditCodec::collection, "sequence:1");
    bool during_flush = false;
    tables.before_put[key] = [&](lake::test::RecordTables&) {
        during_flush = true;
        EXPECT_EQ(recorder.pending(), 5U);
        EXPECT_FALSE(recorder.add(first.customer_id, first.workspaces[0], event()));
        EXPECT_TRUE(recorder.add(second.customer_id, workspace, event()));
        EXPECT_FALSE(recorder.add(second.customer_id, workspace, event()));
    };
    liquid::runtime::block_on(recorder.flush());
    EXPECT_TRUE(during_flush);
    EXPECT_EQ(tables.rows("audit_events").size(), 1U);
    EXPECT_EQ(recorder.pending(), 5U);
    EXPECT_EQ(recorder.totals().dropped, 5U);
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(tables.rows("audit_events").size(), 2U);
    EXPECT_EQ(recorder.pending(), 4U);
    EXPECT_EQ(recorder.totals().dropped, 5U) << "accepted retries are never evicted";
    for (const auto& stopped : first.workspaces) {
        t.ready(stopped);
    }
    liquid::runtime::block_on(recorder.flush());
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().written, 6U);
    EXPECT_TRUE(recorder.add(first.customer_id, first.workspaces[0], event()))
        << "successful writes release the customer's allocation";
}

TEST_P(WorkspaceRecordStores, AuditQueueOverflowPreservesResultsAndSignedDeliveryEvidence) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto workspace = first.workspaces[0];
    lake::api::AuditRecorder recorder(t.store.get(), t.service.get(), std::string(kek), "", 2);
    ASSERT_TRUE(
        recorder.add(first.customer_id, workspace, lake::audit::pending({.action = "kept"})));
    auto context = t.context(first);
    context.audit_recorder = &recorder;
    context.request_id = lake::control::new_uuid_v4();
    const auto completed =
        liquid::runtime::block_on(t.registry.call_async("create_transcript",
                                                        {{"customer_id", first.customer_id},
                                                         {"workspace_id", workspace},
                                                         {"title", "Private title"},
                                                         {"source", "text"},
                                                         {"text", "Private workspace content"}},
                                                        context));
    ASSERT_TRUE(completed);
    EXPECT_FALSE(completed->ok);
    EXPECT_EQ(completed->error.code, "audit_queue_full");
    const auto envelope = completed->envelope();
    ASSERT_TRUE(envelope.contains("operation_result"));
    EXPECT_EQ(envelope["operation_result"]["ok"], true);
    const auto id = envelope["operation_result"]["value"]["id"];
    const auto read =
        t.call(first,
               "get_transcript",
               {{"customer_id", first.customer_id}, {"workspace_id", workspace}, {"id", id}});
    EXPECT_TRUE(read.ok) << read.error.message;
    const auto evidence = lake::test::audit_events(*t.store, {.request_id = context.request_id});
    ASSERT_TRUE(evidence);
    ASSERT_EQ(evidence->size(), 1U);
    const auto& entry = evidence->front();
    EXPECT_EQ(entry.draft.action, "audit_delivery_failed");
    EXPECT_EQ(entry.draft.details["operation_outcome"], "success");
    EXPECT_EQ(entry.draft.details["workspace_id"], workspace);
    EXPECT_FALSE(entry.draft.details["event_id"].get<std::string>().empty());
    EXPECT_EQ(entry.draft.details.size(), 4U);
    EXPECT_EQ(lake::audit::to_json(entry).dump().find("Private"), std::string::npos);
    EXPECT_TRUE(lake::audit::verify_event(*t.store, entry));

    const auto row = t.store->get_workspace(workspace);
    t.status(row->zetta_service_id, "Pending", "http://" + workspace + ".test");
    context.request_id = lake::control::new_uuid_v4();
    const auto failed = liquid::runtime::block_on(t.registry.call_async(
        "get_transcript",
        {{"customer_id", first.customer_id}, {"workspace_id", workspace}, {"id", id}},
        context));
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->error.code, "audit_queue_full");
    EXPECT_EQ(failed->envelope()["operation_result"]["error"]["code"], "workspace_not_ready");

    // Direct push/reference callers use the same record_audit path and receive the failure too.
    const auto direct =
        lake::api::record_audit(context,
                                {.customer_id = first.customer_id,
                                 .principal = context.acting_identity,
                                 .object = "private-reference",
                                 .action = "reference_update",
                                 .outcome = "success",
                                 .request_id = lake::control::new_uuid_v4(),
                                 .details = {{"private", "payload"}}},
                                lake::api::workspace_audit_route(context, workspace));
    EXPECT_FALSE(direct);
    EXPECT_EQ(direct.error().code, "audit_queue_full");
    EXPECT_EQ(recorder.pending(), 1U);
}

// lake#317, migration 0063: the central events recorded before the split that are about a
// workspace's data are copied to that workspace's audit as they are; events of a deleted
// workspace, refusals and control-plane events stay central.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesWorkspaceAuditEvents) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto repository = t.store->create_repository(
        {first.customer_id, "audited", "", "private", first.workspaces[0]});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto doomed = t.store->create_repository(
        {second.customer_id, "doomed", "", "private", second.workspaces[1]});
    ASSERT_TRUE(doomed) << doomed.error().message;
    const lake::audit::IdentityOptions options{.instance_kek = std::string(kek)};
    // What an earlier lake recorded centrally.
    const auto legacy = [&](lake::audit::EventDraft draft) {
        draft.request_id = lake::control::new_uuid_v4();
        if (draft.kind.empty()) {
            draft.kind = "capability";
        }
        if (draft.principal.empty()) {
            draft.principal = first.principal.taap_id;
        }
        const auto recorded = lake::audit::record(*t.store, draft, options);
        EXPECT_TRUE(recorded) << recorded.error().message;
        return recorded->id;
    };
    const std::string renamed = legacy({.object = "repository/" + repository->id,
                                        .action = "rename_repository",
                                        .outcome = "success"});
    const std::string queried = legacy({.customer_id = first.customer_id,
                                        .object = "capability/workspace_query",
                                        .action = "workspace_query",
                                        .outcome = "success",
                                        .details = {{"workspace_id", first.workspaces[1]}}});
    const std::string refused = legacy({.object = "repository/" + repository->id,
                                        .action = "rename_repository",
                                        .outcome = "denied"});
    const std::string token = legacy({.object = "capability/create_token",
                                      .action = "create_token",
                                      .outcome = "success"});
    const std::string unnamed = legacy({.object = "capability/workspace_act",
                                        .action = "workspace_act",
                                        .outcome = "success"});
    const std::string gone = legacy({.object = "repository/" + doomed->id,
                                     .action = "get_repository",
                                     .outcome = "success"});
    ASSERT_TRUE(t.store->update_workspace(second.workspaces[1], {.desired = "deleted"}));

    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    const auto run_capability = [&](const std::string& capability, const json& input) {
        operator_context.request_id = lake::control::new_uuid_v4();
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(capability, input, operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const auto copy = [&](const std::string& mode) {
        return run_capability("copy_workspace_records",
                              {{"collection", "audit_events"}, {"mode", mode}});
    };
    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 3) << plan.value.dump();
    EXPECT_EQ(plan.value["assigned"], 2);
    ASSERT_EQ(plan.value["unresolved"].size(), 1U);
    EXPECT_EQ(plan.value["unresolved"][0]["id"], gone);
    EXPECT_EQ(plan.value["unresolved"][0]["outcome"], "workspace_deleted");
    EXPECT_FALSE(copy("verify").value["complete"]);
    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 2);
    EXPECT_EQ(copy("copy").value["already_present"], 2) << "a rerun copies nothing twice";
    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 2);
    EXPECT_TRUE(verified.value["complete"]) << verified.value.dump();

    // Each copy is in its workspace, as it was, and the readers show it once.
    auto audit = run_capability("audit", {{"workspace_id", first.workspaces[0]}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    ASSERT_EQ(audit.value["items"].size(), 1U);
    EXPECT_EQ(audit.value["items"][0]["id"], renamed);
    EXPECT_FALSE(audit.value["items"][0].contains("sequence")) << "a copy has no place";
    audit = run_capability("audit", {{"workspace_id", first.workspaces[1]}});
    ASSERT_EQ(audit.value["items"].size(), 1U);
    EXPECT_EQ(audit.value["items"][0]["id"], queried);
    const auto both = run_capability("audit", {{"limit", 1000}});
    ASSERT_TRUE(both.ok);
    EXPECT_EQ(std::ranges::count_if(both.value["items"],
                                    [&](const json& item) { return item["id"] == renamed; }),
              1);
    for (const std::string& id : {refused, token, unnamed, gone}) {
        EXPECT_EQ(std::ranges::count_if(both.value["items"],
                                        [&](const json& item) {
                                            return item["id"] == id && item["log"] == "central";
                                        }),
                  1)
            << id;
    }
    const auto workspace_verified =
        run_capability("verify_audit", {{"workspace_id", first.workspaces[0]}});
    ASSERT_TRUE(workspace_verified.ok) << workspace_verified.error.message;
    EXPECT_EQ(workspace_verified.value["copied"], 1);
    const auto central_verified = run_capability("verify_audit", json::object());
    EXPECT_TRUE(central_verified.ok)
        << central_verified.error.code << ": " << central_verified.error.message;

    // A changed copy is reported, never accepted.
    auto& home_rows =
        t.zetta->records["http://" + first.workspaces[0] + ".test"].rows("audit_events");
    auto& lab_rows =
        t.zetta->records["http://" + first.workspaces[1] + ".test"].rows("audit_events");
    home_rows.begin()->second["value"] = lab_rows.begin()->second["value"];
    const auto tampered = copy("verify");
    EXPECT_EQ(tampered.value["missing"].size(), 1U);
    EXPECT_FALSE(tampered.value["complete"]);

    // Operator only.
    operator_context.operator_credential = false;
    EXPECT_EQ(copy("plan").error.code, "unauthorized");
}

// lake#348, lake#349: repository transfer and build records live in the workspace Zetta of the
// repository's workspace. A sync request only counts in memory; the recorder's flush writes each
// workspace's hourly record and each build record there, and nowhere else.
TEST_P(WorkspaceRecordStores, RepositoryUsageStaysInEachRepositorysWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto repository = [](const TwoSpaces::Space& space, const std::string& workspace) {
        lake::control::Repository row;
        row.id = lake::control::new_uuid_v4();
        row.customer_id = space.customer_id;
        row.workspace_id = workspace;
        return row;
    };
    const auto first_home = repository(first, first.workspaces[0]);
    const auto first_lab = repository(first, first.workspaces[1]);
    const auto second_home = repository(second, second.workspaces[0]);
    const std::string hour = "2026-09-04T10:00:00Z";
    lake::api::MeteringRecorder recorder(t.store.get(), t.service.get(), std::string(kek), 6);
    recorder.add_transfer(first_home, 100, 10, hour);
    recorder.add_transfer(first_home, 1, 2, hour); // merges into the same counter
    recorder.add_transfer(first_lab, 5, 0, hour);
    recorder.add_transfer(second_home, 7, 7, hour);
    // A repository migration 0051 left without a workspace is counted, never recorded.
    recorder.add_transfer(repository(first, ""), 3, 3, hour);
    EXPECT_EQ(recorder.totals().unattributed_bytes, 6U);
    EXPECT_EQ(recorder.pending(), 3U);
    const auto url = [](const std::string& workspace) { return "http://" + workspace + ".test"; };
    EXPECT_TRUE(t.zetta->records[url(first.workspaces[0])].collections.empty())
        << "counting writes nothing";
    const auto pending = recorder.pending_transfers(first.customer_id);
    EXPECT_EQ(pending.size(), 2U) << "a space sees only its own pending transfer";

    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().flushed_transfers, 3U);
    const auto transfer = [&](const TwoSpaces::Space& space,
                              const std::string& workspace,
                              const std::string& customer_id) {
        auto context = t.context(space);
        auto records = lake::api::workspace_records(context, space.customer_id, workspace);
        EXPECT_TRUE(records) << records.error().message;
        auto listed = run(lake::control::list_metered_transfer(*records, customer_id));
        EXPECT_TRUE(listed) << listed.error().message;
        return *listed;
    };
    auto home = transfer(first, first.workspaces[0], first.customer_id);
    ASSERT_EQ(home.size(), 1U);
    EXPECT_EQ(home[0].hour, hour);
    EXPECT_EQ(home[0].transfer_in_bytes, 101U);
    EXPECT_EQ(home[0].transfer_out_bytes, 12U);
    ASSERT_EQ(transfer(first, first.workspaces[1], first.customer_id).size(), 1U);
    EXPECT_EQ(transfer(second, second.workspaces[0], second.customer_id)[0].transfer_in_bytes, 7U);
    EXPECT_TRUE(transfer(second, second.workspaces[1], second.customer_id).empty());
    EXPECT_TRUE(transfer(second, second.workspaces[0], first.customer_id).empty())
        << "another space's records are not in this workspace";
    // The workspace holds sealed values and blind indexes only.
    for (const auto& [key, row] :
         t.zetta->records[url(first.workspaces[0])].rows("metering_usage")) {
        EXPECT_EQ(row.dump().find(first.customer_id), std::string::npos);
        EXPECT_EQ(row.dump().find(hour), std::string::npos);
    }

    // A second lake replica adds to the same hour: a conditional write on the revision it read.
    lake::api::MeteringRecorder replica(t.store.get(), t.service.get(), std::string(kek));
    replica.add_transfer(first_home, 9, 0, hour);
    ASSERT_TRUE(liquid::runtime::block_on(replica.flush()));
    home = transfer(first, first.workspaces[0], first.customer_id);
    ASSERT_EQ(home.size(), 1U);
    EXPECT_EQ(home[0].transfer_in_bytes, 110U);

    // A workspace that is not ready keeps its counter for the next flush.
    const auto lab = t.store->get_workspace(first.workspaces[1]);
    ASSERT_TRUE(lab);
    t.status(lab->zetta_service_id, "Pending", url(first.workspaces[1]));
    recorder.add_transfer(first_lab, 4, 0, hour);
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.pending(), 1U);
    EXPECT_GT(recorder.totals().flush_errors, 0U);
    t.ready(first.workspaces[1]);
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(transfer(first, first.workspaces[1], first.customer_id)[0].transfer_in_bytes, 9U);

    // A deleted workspace's counter is dropped and counted: its records went with it.
    recorder.add_transfer(repository(second, second.workspaces[1]), 50, 0, hour);
    ASSERT_TRUE(t.store->update_workspace(second.workspaces[1], {.desired = "deleted"}));
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().dropped_bytes, 50U);

    // An unknown workspace id is dropped the same way, not retried forever: both stores answer it
    // with invalid_input rather than not_found. Transfer and a build record alike.
    const std::string unknown = lake::control::new_uuid_v4();
    recorder.add_transfer(repository(second, unknown), 20, 0, hour);
    recorder.add_build(unknown, {.customer_id = second.customer_id,
                                 .result_id = "result-unknown",
                                 .hour = hour,
                                 .compute_seconds = 1});
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.pending(), 0U) << "an unknown workspace is never retried";
    EXPECT_EQ(recorder.totals().dropped_bytes, 70U);
    EXPECT_EQ(recorder.totals().dropped_builds, 1U);

    // Bounded: a new counter beyond the bound is dropped and counted, an existing one merges.
    lake::api::MeteringRecorder bounded(t.store.get(), t.service.get(), std::string(kek), 2);
    bounded.add_transfer(first_home, 1, 0, "2026-09-04T11:00:00Z");
    bounded.add_transfer(first_home, 1, 0, "2026-09-04T12:00:00Z");
    bounded.add_transfer(first_home, 8, 0, "2026-09-04T13:00:00Z");
    bounded.add_transfer(first_home, 1, 0, "2026-09-04T11:00:00Z");
    EXPECT_EQ(bounded.pending(), 2U);
    EXPECT_EQ(bounded.totals().dropped_bytes, 8U);

    // Build records: once per result, in the repository's workspace, however often queued.
    lake::control::MeteredBuild build{.customer_id = first.customer_id,
                                      .result_id = "result-1",
                                      .hour = hour,
                                      .compute_seconds = 3,
                                      .repository_id = first_home.id};
    recorder.add_build(first.workspaces[0], build);
    recorder.add_build(first.workspaces[0], build);
    EXPECT_EQ(recorder.pending(), 1U);
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    recorder.add_build(first.workspaces[0], build);
    ASSERT_TRUE(liquid::runtime::block_on(recorder.flush()));
    EXPECT_EQ(recorder.totals().flushed_builds, 1U) << "the second write found it recorded";
    auto context = t.context(first);
    auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[0]);
    ASSERT_TRUE(records);
    const auto builds = run(lake::control::list_metered_builds(*records, first.customer_id));
    ASSERT_TRUE(builds);
    ASSERT_EQ(builds->size(), 1U);
    EXPECT_EQ(builds->front().compute_seconds, 3U);
    EXPECT_EQ(builds->front().repository_id, first_home.id);
    auto other = lake::api::workspace_records(context, first.customer_id, first.workspaces[1]);
    ASSERT_TRUE(other);
    EXPECT_TRUE(run(lake::control::list_metered_builds(*other, first.customer_id))->empty());
}

TEST(WorkspaceRecords, NoWorkspaceSurfaceReachesTheRecordStore) {
    // Record capabilities are not in lake's workspace allow-list: lake's collections are reached
    // only through WorkspaceRecords, never by a person or an agent through workspace_query or
    // workspace_act.
    TwoSpaces t(std::make_shared<lake::control::InMemoryStore>(), "");
    const auto& space = t.spaces[0];
    const auto call = [&](const std::string& name, json input) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[0];
        auto context = t.context(space);
        return *liquid::runtime::block_on(t.registry.call_async(name, input, context));
    };
    for (const std::string capability : {"list_records", "get_record"}) {
        const auto read = call("workspace_query", {{"capability", capability},
                                                   {"input", {{"collection", "lake_ask_messages"}}}});
        EXPECT_EQ(read.error.code, "invalid_input") << capability;
        EXPECT_EQ(read.error.path, "/capability") << capability;
    }
    for (const std::string capability :
         {"put_record", "delete_record", "create_collection"}) {
        const auto act = call("workspace_act", {{"capability", capability},
                                                {"input", {{"collection", "lake_ask_messages"}}}});
        EXPECT_EQ(act.error.code, "invalid_input") << capability;
        EXPECT_EQ(act.error.path, "/capability") << capability;
    }
}

// lake#341: a repository's forge records live in the workspace Zetta of the repository's
// workspace, and nowhere else.
TEST_P(WorkspaceRecordStores, ForgeRecordsLiveInTheirRepositorysWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    struct Placed {
        const TwoSpaces::Space* space;
        std::string workspace, repository;
    };
    std::vector<Placed> placed; // first/home, first/lab, second/home, second/lab
    for (const auto& space : t.spaces) {
        for (std::size_t index = 0; index < space.workspaces.size(); ++index) {
            const auto repository = t.store->create_repository(
                {space.customer_id, "forge-" + std::to_string(index), "", "private",
                 space.workspaces[index]});
            ASSERT_TRUE(repository) << repository.error().message;
            placed.push_back({&space, space.workspaces[index], repository->id});
        }
    }
    std::vector<std::string> issues;
    for (const auto& where : placed) {
        const auto created = t.call(*where.space, "create_issue",
                                    {{"repository_id", where.repository},
                                     {"title", "issue in " + where.workspace}});
        ASSERT_TRUE(created.ok) << created.error.message;
        EXPECT_EQ(created.value["repository_id"], where.repository);
        issues.push_back(created.value["id"]);
    }
    // Each record is stored, sealed, only at its own workspace's endpoint.
    for (const auto& where : placed) {
        const auto& rows =
            t.zetta->records["http://" + where.workspace + ".test"].rows("forge_records");
        EXPECT_EQ(rows.size(), 1U) << where.workspace;
        for (const auto& [key, row] : rows) {
            EXPECT_EQ(row.dump().find("issue in"), std::string::npos) << "the body is sealed";
        }
    }
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const auto listed = t.call(*placed[index].space, "list_issues",
                                   {{"repository_id", placed[index].repository}});
        ASSERT_TRUE(listed.ok) << listed.error.message;
        ASSERT_EQ(listed.value["items"].size(), 1U);
        EXPECT_EQ(listed.value["items"][0]["id"], issues[index]);
    }

    // An issue of another workspace of the same space is not this repository's.
    const auto crossed = t.call(first, "update_issue",
                                {{"repository_id", placed[0].repository},
                                 {"issue_id", issues[1]},
                                 {"state", "closed"}});
    EXPECT_EQ(crossed.error.code, "not_found");
    EXPECT_EQ(crossed.error.path, "/issue_id");
    // A record copied into another workspace does not open there: ownership cannot change.
    auto& home_rows =
        t.zetta->records["http://" + placed[0].workspace + ".test"].rows("forge_records");
    auto& lab_rows =
        t.zetta->records["http://" + placed[1].workspace + ".test"].rows("forge_records");
    lab_rows[issues[0]] = home_rows.at(issues[0]);
    const auto moved = t.call(first, "update_issue",
                              {{"repository_id", placed[1].repository},
                               {"issue_id", issues[0]},
                               {"state", "closed"}});
    EXPECT_EQ(moved.error.code, "not_found");
    lab_rows.erase(issues[0]);

    // Updates write at the revision they read and keep each other's changes.
    ASSERT_TRUE(t.call(first, "update_issue",
                       {{"repository_id", placed[0].repository},
                        {"issue_id", issues[0]},
                        {"labels", json::array({"bug"})}})
                    .ok);
    const auto closed = t.call(first, "update_issue",
                               {{"repository_id", placed[0].repository},
                                {"issue_id", issues[0]},
                                {"state", "closed"}});
    ASSERT_TRUE(closed.ok) << closed.error.message;
    EXPECT_EQ(closed.value["value"]["labels"], json::array({"bug"}));
    EXPECT_EQ(closed.value["value"]["state"], "closed");
    auto context = t.context(first);
    {
        auto records = lake::api::workspace_records(context, first.customer_id,
                                                    placed[0].workspace);
        ASSERT_TRUE(records);
        const auto stored = run(records->get(lake::control::ForgeRecordCodec::collection,
                                             issues[0]));
        ASSERT_TRUE(stored && *stored);
        EXPECT_EQ((*stored)->lake_revision, 3);
        // A write at a revision someone has since replaced is refused.
        const auto stale = run(records->put(
            lake::control::ForgeRecordCodec::collection, issues[0],
            lake::control::forge_record_lookup(placed[0].repository, "issue"), (*stored)->body,
            lake::control::RecordCondition{.if_revision = (*stored)->revision - 1}, 4,
            (*stored)->at_us));
        ASSERT_FALSE(stale);
        EXPECT_EQ(stale.error().code, "conflict");
    }

    // Least privilege: another space can neither read nor write; ids name the field.
    const auto foreign = t.call(second, "list_issues", {{"repository_id", placed[0].repository}});
    EXPECT_EQ(foreign.error.code, "unauthorized");
    const auto foreign_write = t.call(second, "create_issue",
                                      {{"repository_id", placed[0].repository},
                                       {"title", "trespass"}});
    EXPECT_EQ(foreign_write.error.code, "unauthorized");
    const auto missing = t.call(first, "list_issues", json::object());
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.path, "/repository_id");
    for (const std::string& bad :
         std::vector<std::string>{"not-a-repository", lake::control::new_uuid_v4()}) {
        const auto refused = t.call(first, "list_issues", {{"repository_id", bad}});
        EXPECT_FALSE(refused.ok) << bad;
        EXPECT_EQ(refused.error.path, "/repository_id") << bad;
    }

    // Branch protection reads the rules of its own repository's workspace.
    ASSERT_TRUE(t.call(first, "set_branch_protection",
                       {{"repository_id", placed[0].repository},
                        {"ref", "main"},
                        {"required_review", true}})
                    .ok);
    const auto rules = [&](const lake::control::Repository& repository) {
        auto loaded =
            liquid::runtime::block_on(lake::api::load_branch_protection(context, repository));
        EXPECT_TRUE(loaded);
        return std::move(*loaded);
    };
    const auto home_repository = *t.store->get_repository(placed[0].repository);
    const auto lab_repository = *t.store->get_repository(placed[1].repository);
    const auto home_rules = rules(home_repository);
    ASSERT_TRUE(home_rules) << home_rules.error().message;
    EXPECT_EQ(home_rules->rules.size(), 1U);
    EXPECT_TRUE(home_rules->releases.empty());
    const auto lab_rules = rules(lab_repository);
    ASSERT_TRUE(lab_rules) << lab_rules.error().message;
    EXPECT_TRUE(lab_rules->rules.empty());

    // A repository without a workspace has nowhere to keep forge records, and its pushes cannot
    // read rules: both refuse instead of falling back.
    auto unassigned = home_repository;
    unassigned.workspace_id.clear();
    const auto opened = lake::api::RepositoryForge::open(context, unassigned);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, "conflict");
    EXPECT_EQ(opened.error().path, "/repository_id");
    const auto unassigned_rules = rules(unassigned);
    ASSERT_FALSE(unassigned_rules);
    EXPECT_EQ(unassigned_rules.error().code, "conflict");

    // A workspace that is not ready refuses reads and a push's rules.
    const auto lab_workspace = t.store->get_workspace(placed[1].workspace);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + placed[1].workspace + ".test");
    EXPECT_EQ(t.call(first, "list_issues", {{"repository_id", placed[1].repository}}).error.code,
              "workspace_not_ready");
    const auto not_ready = rules(lab_repository);
    ASSERT_FALSE(not_ready);
    EXPECT_EQ(not_ready.error().code, "workspace_not_ready");

    // Workspace deletion: refused while the repository lives; once it is deleted, the workspace
    // and its records go, and the other workspace keeps its own.
    ASSERT_TRUE(t.call(first, "delete_repository", {{"id", placed[1].repository}}).ok);
    ASSERT_TRUE(t.store->update_workspace(placed[1].workspace, {.desired = "deleted"}));
    EXPECT_FALSE(t.call(first, "list_issues", {{"repository_id", placed[1].repository}}).ok);
    EXPECT_EQ(t.call(first, "list_issues", {{"repository_id", placed[0].repository}})
                  .value["items"]
                  .size(),
              1U);
}

// lake#598: the issue pages name who wrote an issue or a comment, and offer the repository's people
// to assign; the page's writes are the capabilities' own, permission-checked the same way.
TEST_P(WorkspaceRecordStores, IssuesNameTheirAuthorAndOfferTheRepositorysPeople) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto repository = t.store->create_repository(
        {first.customer_id, "issues", "", "private", first.workspaces[0]});
    ASSERT_TRUE(repository) << repository.error().message;
    const std::string id = repository->id;

    // Lake records the author; the input cannot name one.
    const auto created = t.call(first,
                                "create_issue",
                                {{"repository_id", id},
                                 {"title", "Import fails on an empty CSV"},
                                 {"body", "Steps:\n\n1. Upload `empty.csv`"},
                                 {"labels", json::array({"bug"})},
                                 {"assignees", json::array({first.principal.taap_id})}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["value"]["author"], first.principal.taap_id);
    EXPECT_EQ(created.value["value"]["state"], "open");
    const std::string issue = created.value["id"];
    const auto forged = t.call(first,
                               "create_issue",
                               {{"repository_id", id},
                                {"title", "Someone else wrote this"},
                                {"author", second.principal.taap_id}});
    EXPECT_EQ(forged.error.code, "invalid_input");

    const auto comment = t.call(
        first,
        "create_issue_comment",
        {{"repository_id", id}, {"issue_id", issue}, {"body", "Fixed by the empty-file check."}});
    ASSERT_TRUE(comment.ok) << comment.error.message;
    EXPECT_EQ(comment.value["value"]["author"], first.principal.taap_id);
    EXPECT_EQ(comment.value["value"]["issue_id"], issue);

    // Edit, relabel, unassign, close and reopen: each keeps the author and the other fields.
    const auto edited = t.call(first,
                               "update_issue",
                               {{"repository_id", id},
                                {"issue_id", issue},
                                {"title", "Import fails on an empty file"},
                                {"body", "Any empty file."},
                                {"labels", json::array({"bug", "import"})},
                                {"assignees", json::array()}});
    ASSERT_TRUE(edited.ok) << edited.error.message;
    EXPECT_EQ(edited.value["value"]["title"], "Import fails on an empty file");
    EXPECT_EQ(edited.value["value"]["labels"], json::array({"bug", "import"}));
    EXPECT_EQ(edited.value["value"]["assignees"], json::array());
    EXPECT_EQ(edited.value["value"]["author"], first.principal.taap_id);
    for (const std::string state : {"closed", "open"}) {
        const auto moved = t.call(
            first, "update_issue", {{"repository_id", id}, {"issue_id", issue}, {"state", state}});
        ASSERT_TRUE(moved.ok) << moved.error.message;
        EXPECT_EQ(moved.value["value"]["state"], state);
        EXPECT_EQ(moved.value["value"]["title"], "Import fails on an empty file");
    }
    EXPECT_EQ(t.call(first,
                     "update_issue",
                     {{"repository_id", id}, {"issue_id", issue}, {"state", "done"}})
                  .error.code,
              "invalid_input");

    // The people to assign: a personal space's owner, with the identity `assignees` holds.
    const auto people = t.call(first, "issue_assignees", {{"repository_id", id}});
    ASSERT_TRUE(people.ok) << people.error.message;
    ASSERT_EQ(people.value["people"].size(), 1U);
    EXPECT_EQ(people.value["people"][0]["taap_id"], first.principal.taap_id);
    EXPECT_EQ(people.value["people"][0]["kind"], "user");
    EXPECT_FALSE(people.value["people"][0]["handle"].get<std::string>().empty());

    // Someone without `write` on the repository can neither list its people nor write its issues.
    EXPECT_EQ(t.call(second, "issue_assignees", {{"repository_id", id}}).error.code,
              "unauthorized");
    EXPECT_EQ(t.call(second,
                     "update_issue",
                     {{"repository_id", id}, {"issue_id", issue}, {"state", "closed"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(t.call(second,
                     "create_issue_comment",
                     {{"repository_id", id}, {"issue_id", issue}, {"body", "trespass"}})
                  .error.code,
              "unauthorized");
    const auto missing = t.call(first, "issue_assignees", json::object());
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.path, "/repository_id");
}

// The operator's deployment control plane as lake reads it (docs/api/platform.md): three tables of
// rows, filtered by nothing (every row is this lake's).
struct PlatformControl final : lake::control::ZettaControl {
    std::vector<json> deployments, environment_env, domains;

    liquid::runtime::task<lake::control::ZettaControlResult<lake::control::ZettaRows>>
    sql(std::string statement, std::string) override {
        using Rows = lake::control::ZettaControlResult<lake::control::ZettaRows>;
        if (statement.find("environment_env") != std::string::npos) {
            co_return Rows(environment_env);
        }
        if (statement.find("domains") != std::string::npos) {
            co_return Rows(domains);
        }
        co_return Rows(deployments);
    }
    liquid::runtime::task<lake::control::ZettaControlResult<void>> append(std::string table,
                                                                        json row) override {
        (table == lake::control::environment_env_table ? environment_env
         : table == lake::control::domains_table       ? domains
                                                       : deployments)
            .push_back(std::move(row));
        co_return lake::control::ZettaControlResult<void>{};
    }
};

json platform_deployment(const std::string& repository, const std::string& environment,
                         const std::string& phase, const std::string& updated_at) {
    return {{"operator", "operator-a"}, {"lake", "lake.example"},
            {"repository", repository},  {"environment", environment},
            {"kind", "application"},     {"change", "change-" + repository.substr(0, 8)},
            {"image", "registry.example/app@sha256:0123"},
            {"deployment_id", "deployment-" + repository},
            {"requested_by", "admin"},   {"requested_at", "2026-10-01T00:00:00Z"},
            {"phase", phase},            {"namespace", "ns-" + repository.substr(0, 8)},
            {"url", "https://" + repository.substr(0, 8) + ".example"},
            {"message", ""},             {"updated_at", updated_at},
            {"ordering_token", ""}};
}

// The sync's opener: the workspace caller, as the server's owner loop hands it.
lake::control::PlatformRecordsOpener platform_opener(
    std::shared_ptr<lake::control::WorkspaceService> service) {
    return [service](const std::string& customer_id, const std::string& workspace_id) {
        return WorkspaceRecords::open(
            [service, customer_id, workspace_id](std::string capability, bool read, json input) {
                return service->call(customer_id, workspace_id, std::move(capability), read,
                                     std::move(input));
            },
            workspace_id, kek);
    };
}

// lake#358, lake#359: a repository's environments and the operator rows lake mirrors for it live
// only in the workspace Zetta of the repository's workspace.
TEST_P(WorkspaceRecordStores, PlatformEnvironmentsLiveInTheirRepositorysWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    auto control = std::make_shared<PlatformControl>();
    lake::control::PlatformService platform(
        t.store, control, nullptr,
        lake::control::PlatformConfig{.lake = "lake.example", .instance_kek = std::string(kek)});
    struct Placed {
        const TwoSpaces::Space* space;
        std::string workspace, repository;
    };
    std::vector<Placed> placed; // first/home, first/lab, second/home, second/lab
    for (const auto& space : t.spaces) {
        for (std::size_t index = 0; index < space.workspaces.size(); ++index) {
            const auto repository = t.store->create_repository(
                {space.customer_id, "deploy-" + std::to_string(index), "", "private",
                 space.workspaces[index]});
            ASSERT_TRUE(repository) << repository.error().message;
            placed.push_back({&space, space.workspaces[index], repository->id});
            control->deployments.push_back(platform_deployment(
                repository->id, "production", "deploying", "2026-10-01T00:01:00Z"));
            control->environment_env.push_back(
                {{"lake", "lake.example"}, {"repository", repository->id},
                 {"environment", "production"}, {"key", "COLOR"}, {"value", "blue-secretless"},
                 {"deleted", false}, {"set_by", "admin"},
                 {"updated_at", "2026-10-01T00:01:00Z"}, {"ordering_token", ""}});
        }
    }
    const auto sync = [&] {
        const auto completed = liquid::runtime::block_on(platform.sync(platform_opener(t.service)));
        EXPECT_TRUE(completed);
        return *completed;
    };
    ASSERT_TRUE(sync()) << "every workspace is ready";
    const auto call = [&](const TwoSpaces::Space& space, const std::string& capability,
                          json input) {
        auto context = t.context(space);
        context.platform = &platform;
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    // Each repository's environment and rows are stored, sealed, only at its own workspace's
    // endpoint; nothing of them is in the control store but the environment's policy object.
    for (const auto& where : placed) {
        auto& tables = t.zetta->records["http://" + where.workspace + ".test"];
        EXPECT_EQ(tables.rows("platform_environments").size(), 1U) << where.workspace;
        EXPECT_EQ(tables.rows("platform_mirror").size(), 2U) << where.workspace;
        for (const std::string collection : {"platform_environments", "platform_mirror"}) {
            for (const auto& [key, row] : tables.rows(collection)) {
                EXPECT_EQ(row.dump().find("production"), std::string::npos) << "sealed";
                EXPECT_EQ(row.dump().find("blue-secretless"), std::string::npos) << "sealed";
            }
        }
        const auto object = t.store->find_policy_object(
            "environment", lake::control::platform_environment_id(where.repository, "production"));
        ASSERT_TRUE(object) << object.error().message;
        const auto read = call(*where.space, "get_environment",
                               {{"repository_id", where.repository}, {"environment", "production"}});
        ASSERT_TRUE(read.ok) << read.error.message;
        EXPECT_EQ(read.value["object_id"], object->id);
        EXPECT_EQ(read.value["deployment"]["phase"], "deploying");
        EXPECT_EQ(read.value["env"][0]["value"], "blue-secretless");
        const auto listed = call(*where.space, "list_environments",
                                 {{"repository_id", where.repository}});
        ASSERT_TRUE(listed.ok) << listed.error.message;
        ASSERT_EQ(listed.value["items"].size(), 1U);
        EXPECT_EQ(listed.value["items"][0]["id"],
                  lake::control::platform_environment_id(where.repository, "production"));
    }
    // A sync that sees nothing new writes nothing, and a newer row replaces the older one.
    const auto writes = [&](const std::string& workspace) {
        std::size_t count = 0;
        for (const auto& call : t.zetta->records["http://" + workspace + ".test"].calls) {
            count += call == "put_record" ? 1 : 0;
        }
        return count;
    };
    const std::size_t before = writes(placed[0].workspace);
    ASSERT_TRUE(sync());
    EXPECT_EQ(writes(placed[0].workspace), before);
    control->deployments.push_back(
        platform_deployment(placed[0].repository, "production", "ready", "2026-10-01T00:02:00Z"));
    ASSERT_TRUE(sync());
    // The newer row, and its phase appended to the workspace's audit (decision 52).
    EXPECT_EQ(writes(placed[0].workspace), before + 2);
    EXPECT_EQ(call(first, "get_environment",
                   {{"repository_id", placed[0].repository}, {"environment", "production"}})
                  .value["deployment"]["phase"],
              "ready");

    // Mutable rows: an older row never replaces a newer one, and a write at a revision someone
    // has since replaced is refused.
    auto context = t.context(first);
    {
        auto records = lake::api::workspace_records(context, first.customer_id,
                                                    placed[0].workspace);
        ASSERT_TRUE(records);
        const auto older = run(lake::control::put_platform_mirror_record(
            *records, placed[0].repository,
            {"deployment", "deployment-" + placed[0].repository,
             platform_deployment(placed[0].repository, "production", "pending",
                                 "2026-10-01T00:00:30Z"),
             "now"}));
        ASSERT_TRUE(older);
        EXPECT_FALSE(*older);
        const std::string key =
            lake::control::platform_mirror_id("deployment", "deployment-" + placed[0].repository);
        const auto stored = run(records->get(lake::control::PlatformMirrorCodec::collection, key));
        ASSERT_TRUE(stored && *stored);
        const auto stale = run(records->put(
            lake::control::PlatformMirrorCodec::collection, key,
            lake::control::platform_record_lookup(placed[0].repository), (*stored)->body,
            lake::control::RecordCondition{.if_revision = (*stored)->revision - 1}, 9,
            (*stored)->at_us));
        ASSERT_FALSE(stale);
        EXPECT_EQ(stale.error().code, "conflict");
        // Two writers that create one environment at once create it once.
        lake::control::PlatformEnvironment twin{"", placed[0].repository, "production",
                                                lake::control::new_uuid_v4(), "", 0};
        const auto again = run(lake::control::put_platform_environment_record(*records, twin));
        ASSERT_TRUE(again) << again.error().message;
        EXPECT_NE(again->object_id, twin.object_id) << "the first writer's environment answers";
    }
    // A record copied into another workspace does not open there: ownership cannot change.
    auto& home = t.zetta->records["http://" + placed[0].workspace + ".test"];
    auto& lab = t.zetta->records["http://" + placed[1].workspace + ".test"];
    const std::string moved =
        lake::control::platform_environment_id(placed[0].repository, "staging-moved");
    lab.rows("platform_environments")[moved] =
        home.rows("platform_environments").begin()->second;
    EXPECT_EQ(call(first, "list_environments", {{"repository_id", placed[1].repository}})
                  .value["items"]
                  .size(),
              1U);
    lab.rows("platform_environments").erase(moved);

    // Least privilege: another space sees none of the environments and its listing never reaches
    // the workspace; a single environment is refused; ids name their field.
    const std::size_t calls_before = home.calls.size();
    const auto foreign = call(second, "list_environments", {{"repository_id", placed[0].repository}});
    ASSERT_TRUE(foreign.ok) << foreign.error.message;
    EXPECT_TRUE(foreign.value["items"].empty());
    EXPECT_TRUE(call(second, "list_deployments", {{"repository_id", placed[0].repository}})
                    .value["items"]
                    .empty());
    EXPECT_EQ(home.calls.size(), calls_before) << "no workspace call for a caller who may not read";
    EXPECT_EQ(call(second, "get_environment",
                   {{"repository_id", placed[0].repository}, {"environment", "production"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(call(second, "set_environment_env",
                   {{"repository_id", placed[0].repository}, {"environment", "production"},
                    {"key", "COLOR"}, {"value", "red"}})
                  .error.code,
              "unauthorized");
    const auto missing = call(first, "get_environment", {{"repository_id", placed[0].repository}});
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.path, "/environment");
    EXPECT_EQ(call(first, "list_environments", json::object()).error.path, "/repository_id");
    for (const std::string& bad :
         std::vector<std::string>{"not-a-repository", lake::control::new_uuid_v4()}) {
        const auto refused = call(first, "get_environment",
                                  {{"repository_id", bad}, {"environment", "production"}});
        EXPECT_FALSE(refused.ok) << bad;
        EXPECT_EQ(refused.error.path, "/repository_id") << bad;
        EXPECT_EQ(call(first, "list_environments", {{"repository_id", bad}}).error.path,
                  "/repository_id")
            << bad;
    }
    const auto unknown = call(first, "get_environment",
                              {{"repository_id", placed[0].repository}, {"environment", "nope"}});
    EXPECT_EQ(unknown.error.code, "not_found");
    EXPECT_EQ(unknown.error.path, "/environment");

    // A workspace that is not ready: its repository's reads are refused, the sync marks only that
    // repository stale, and the service stays healthy.
    const auto lab_workspace = t.store->get_workspace(placed[1].workspace);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + placed[1].workspace + ".test");
    EXPECT_EQ(call(first, "get_environment",
                   {{"repository_id", placed[1].repository}, {"environment", "production"}})
                  .error.code,
              "workspace_not_ready");
    control->deployments.push_back(
        platform_deployment(placed[1].repository, "production", "ready", "2026-10-01T00:03:00Z"));
    const auto partial = sync();
    ASSERT_FALSE(partial);
    EXPECT_EQ(partial.error().code, "workspace_not_ready");
    EXPECT_FALSE(platform.degraded());
    EXPECT_TRUE(platform.stale_since(placed[1].repository));
    EXPECT_FALSE(platform.stale_since(placed[0].repository));
    t.ready(placed[1].workspace);
    EXPECT_TRUE(call(first, "get_environment",
                     {{"repository_id", placed[1].repository}, {"environment", "production"}})
                    .value.contains("stale_since"));
    ASSERT_TRUE(sync());
    const auto refreshed = call(first, "get_environment",
                                {{"repository_id", placed[1].repository},
                                 {"environment", "production"}});
    EXPECT_FALSE(refreshed.value.contains("stale_since"));
    EXPECT_EQ(refreshed.value["deployment"]["phase"], "ready");

    if (GetParam()) {
        // A repository without a workspace has nowhere to keep environments: refused, never a
        // fallback, and the sync reports it.
        const auto orphan = t.store->create_repository(
            {first.customer_id, "orphan", "", "private", first.workspaces[0]});
        ASSERT_TRUE(orphan);
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = NULL WHERE id = $1::uuid",
                       orphan->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
        control->deployments.push_back(
            platform_deployment(orphan->id, "production", "deploying", "2026-10-01T00:04:00Z"));
        const auto unassigned = sync();
        ASSERT_FALSE(unassigned);
        EXPECT_NE(unassigned.error().message.find("no workspace"), std::string::npos);
        const auto listed = call(first, "list_deployments", {{"repository_id", orphan->id}});
        EXPECT_TRUE(listed.ok) << "no environment object yet, so nothing to read";
        const auto parent = t.store->find_policy_object("repository", orphan->id);
        ASSERT_TRUE(t.store->create_policy_object(
            {"environment", lake::control::platform_environment_id(orphan->id, "production"),
             parent->id, "admin", "orphan-environment", true}));
        const auto refused = call(first, "list_environments", {{"repository_id", orphan->id}});
        EXPECT_EQ(refused.error.code, "conflict");
        EXPECT_EQ(refused.error.path, "/repository_id");
        control->deployments.pop_back();
    }

    // Workspace deletion: once the repository and its workspace are deleted, the records go with
    // the workspace, and the other workspace keeps its own.
    ASSERT_TRUE(call(first, "delete_repository", {{"id", placed[1].repository}}).ok);
    ASSERT_TRUE(t.store->update_workspace(placed[1].workspace, {.desired = "deleted"}));
    EXPECT_FALSE(call(first, "get_environment",
                      {{"repository_id", placed[1].repository}, {"environment", "production"}})
                     .ok);
    EXPECT_TRUE(call(first, "get_environment",
                     {{"repository_id", placed[0].repository}, {"environment", "production"}})
                    .ok);
}

// Secrets (lake#328-#332): an organization space with its own organization and workspaces, and
// the operator context that administers it.
struct SecretSpace {
    std::string organization_id, customer_id;
    std::vector<std::string> workspaces;
};

SecretSpace secret_space(TwoSpaces& t, const std::string& name, int workspaces) {
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization = t.store->create_organization(
        {"taap:1:ed25519:secrets-" + name + "-" + suffix, name, ""});
    EXPECT_TRUE(organization) << organization.error().message;
    const auto customer = t.store->create_customer(
        {"organization", "", organization->id, "secrets-" + name + "-" + suffix});
    EXPECT_TRUE(customer) << customer.error().message;
    SecretSpace space{organization->id, customer->id, {}};
    for (int index = 0; index < workspaces; ++index) {
        const auto workspace =
            t.store->create_workspace({customer->id, name + std::to_string(index), "dev", ""});
        EXPECT_TRUE(workspace) << workspace.error().message;
        t.ready(workspace->id);
        space.workspaces.push_back(workspace->id);
    }
    return space;
}

lake::api::ApiContext secret_operator(TwoSpaces& t) {
    lake::api::ApiContext context;
    context.store = t.store.get();
    context.workspaces = t.service.get();
    context.instance_kek = std::string(kek);
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.request_id = lake::control::new_uuid_v4();
    return context;
}

lake::api::ApiResult secret_call(TwoSpaces& t, lake::api::ApiContext context,
                                 const std::string& capability, json input) {
    const auto completed =
        liquid::runtime::block_on(t.registry.call_async(capability, input, context));
    EXPECT_TRUE(completed);
    return completed ? *completed : lake::api::ApiResult::failure("store_error", "incomplete");
}

TEST_P(WorkspaceRecordStores, SecretsLiveInTheirMountsWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto admin = [&](const std::string& capability, json input) {
        return secret_call(t, secret_operator(t), capability, std::move(input));
    };
    std::vector<SecretSpace> spaces{secret_space(t, "first", 2), secret_space(t, "second", 2)};
    struct Placed {
        std::size_t space, workspace;
        std::string mount;
    };
    std::vector<Placed> placed;
    for (std::size_t s = 0; s < spaces.size(); ++s) {
        for (std::size_t w = 0; w < spaces[s].workspaces.size(); ++w) {
            // The same mount name in each workspace: names are unique within a workspace.
            const auto mount = admin("create_secret_mount",
                                     {{"customer_id", spaces[s].customer_id},
                                      {"workspace_id", spaces[s].workspaces[w]},
                                      {"organization_id", spaces[s].organization_id},
                                      {"name", "payments-vault"}});
            ASSERT_TRUE(mount.ok) << mount.error.message;
            EXPECT_EQ(mount.value["workspace_id"], spaces[s].workspaces[w]);
            placed.push_back({s, w, mount.value["id"]});
            const auto put =
                admin("put_secret", {{"mount_id", placed.back().mount},
                                     {"name", "database"},
                                     {"value", {{"password", "plaintext-password-" +
                                                                 std::to_string(s) + "-" +
                                                                 std::to_string(w)}}}});
            ASSERT_TRUE(put.ok) << put.error.message;
            ASSERT_TRUE(admin("create_encryption_key",
                              {{"mount_id", placed.back().mount}, {"name", "orders"}})
                            .ok);
        }
    }
    // Each mount's records are stored, sealed, only at its own workspace's endpoint, and
    // PostgreSQL holds nothing of them but the route.
    for (const auto& where : placed) {
        auto& tables =
            t.zetta->records["http://" + spaces[where.space].workspaces[where.workspace] + ".test"];
        EXPECT_EQ(tables.rows("secrets_mounts").size(), 2U) << "the mount and its name claim";
        EXPECT_EQ(tables.rows("secrets_entries").size(), 1U);
        EXPECT_EQ(tables.rows("secrets_versions").size(), 1U);
        EXPECT_EQ(tables.rows("secrets_encryption_keys").size(), 1U);
        EXPECT_EQ(tables.rows("secrets_data_keys").size(), 2U);
        for (const std::string collection :
             {"secrets_mounts", "secrets_entries", "secrets_versions", "secrets_encryption_keys",
              "secrets_data_keys"}) {
            for (const auto& [key, row] : tables.rows(collection)) {
                const std::string dumped = row.dump();
                EXPECT_EQ(dumped.find("payments-vault"), std::string::npos) << collection;
                EXPECT_EQ(dumped.find("database"), std::string::npos) << collection;
                // A marker long enough that sealed bytes never spell it: a sealed record is
                // base64url, and the three characters "pw-" turned up in one on Linux CI
                // (lake#547), failing a record that held no plaintext.
                EXPECT_EQ(dumped.find("plaintext-password-"), std::string::npos) << collection;
            }
        }
        const auto read = admin("read_secret", {{"mount_id", where.mount}, {"name", "database"}});
        ASSERT_TRUE(read.ok) << read.error.message;
        EXPECT_EQ(read.value["value"]["password"],
                  "plaintext-password-" + std::to_string(where.space) + "-" +
                      std::to_string(where.workspace));
        const auto route = t.store->get_secret_mount_route(where.mount);
        ASSERT_TRUE(route);
        EXPECT_EQ(route->workspace_id, spaces[where.space].workspaces[where.workspace]);
    }
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const std::string table : {"secrets_mounts", "secrets_entries", "secrets_versions",
                                        "secrets_encryption_keys", "secrets_data_keys"}) {
            EXPECT_TRUE(tx.exec("SELECT to_regclass('" + table + "')")[0][0].is_null()) << table;
        }
    }
    const auto& first = spaces[0];
    const auto listed = admin("list_secret_mounts",
                              {{"customer_id", first.customer_id},
                               {"workspace_id", first.workspaces[0]}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1U);
    EXPECT_EQ(listed.value["items"][0]["id"], placed[0].mount);

    // A second mount of the same name in the same workspace is refused.
    const auto duplicate = admin("create_secret_mount",
                                 {{"customer_id", first.customer_id},
                                  {"workspace_id", first.workspaces[0]},
                                  {"organization_id", first.organization_id},
                                  {"name", "payments-vault"}});
    EXPECT_EQ(duplicate.error.code, "conflict");
    EXPECT_EQ(duplicate.error.path, "/name");

    // workspace_id: required, and only a workspace of the mount's own space.
    const auto create = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["organization_id"] = first.organization_id;
        input["name"] = "elsewhere";
        return admin("create_secret_mount", std::move(input));
    };
    const auto omitted = create(json::object());
    EXPECT_EQ(omitted.error.code, "invalid_input");
    EXPECT_EQ(omitted.error.path, "/workspace_id");
    for (const std::string bad : {std::string("not-a-workspace"), lake::control::new_uuid_v4()}) {
        const auto refused = create({{"workspace_id", bad}});
        EXPECT_EQ(refused.error.code, "invalid_input") << bad;
        EXPECT_EQ(refused.error.path, "/workspace_id") << bad;
    }
    const auto foreign_workspace = create({{"workspace_id", spaces[1].workspaces[0]}});
    EXPECT_EQ(foreign_workspace.error.code, "invalid_input");
    // An organization of one space with another space and its workspace writes nothing there.
    auto& second_home = t.zetta->records["http://" + spaces[1].workspaces[0] + ".test"];
    const auto before = second_home.rows("secrets_mounts").size();
    const auto crossed = admin("create_secret_mount",
                               {{"customer_id", spaces[1].customer_id},
                                {"workspace_id", spaces[1].workspaces[0]},
                                {"organization_id", first.organization_id},
                                {"name", "trespass"}});
    EXPECT_EQ(crossed.error.code, "invalid_input");
    EXPECT_EQ(crossed.error.path, "/organization_id");
    EXPECT_EQ(second_home.rows("secrets_mounts").size(), before);
    const auto unlisted = admin("list_secret_mounts", {{"customer_id", first.customer_id}});
    EXPECT_EQ(unlisted.error.code, "invalid_input");
    EXPECT_EQ(unlisted.error.path, "/workspace_id");
    for (const std::string bad : {std::string("not-a-mount"), lake::control::new_uuid_v4()}) {
        const auto missing = admin("read_secret", {{"mount_id", bad}, {"name", "database"}});
        EXPECT_EQ(missing.error.code, "not_found") << bad;
        EXPECT_EQ(missing.error.path, "/mount_id") << bad;
    }

    // Credential boundaries: a person of another space reads nothing, lists nothing.
    const auto person = t.context(t.spaces[1]);
    const auto denied =
        secret_call(t, person, "read_secret", {{"mount_id", placed[0].mount}, {"name", "database"}});
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto denied_write = secret_call(
        t, person, "put_secret",
        {{"mount_id", placed[0].mount}, {"name", "database"}, {"value", {{"password", "x"}}}});
    EXPECT_EQ(denied_write.error.code, "unauthorized");

    // A record copied into another workspace does not open there: a mount cannot change owner.
    auto context = secret_operator(t);
    {
        auto& home = t.zetta->records["http://" + first.workspaces[0] + ".test"];
        auto& lab = t.zetta->records["http://" + first.workspaces[1] + ".test"];
        lab.rows("secrets_mounts")[placed[0].mount] = home.rows("secrets_mounts").at(placed[0].mount);
        auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[1]);
        ASSERT_TRUE(records);
        const auto moved =
            run(records->get(lake::control::SecretMountCodec::collection, placed[0].mount));
        ASSERT_TRUE(moved);
        EXPECT_FALSE(*moved);
        lab.rows("secrets_mounts").erase(placed[0].mount);
    }

    // A workspace that is not ready refuses its mounts; the other workspace serves on.
    const auto lab_workspace = t.store->get_workspace(first.workspaces[1]);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + first.workspaces[1] + ".test");
    EXPECT_EQ(admin("read_secret", {{"mount_id", placed[1].mount}, {"name", "database"}}).error.code,
              "workspace_not_ready");
    EXPECT_TRUE(admin("read_secret", {{"mount_id", placed[0].mount}, {"name", "database"}}).ok);
    // A customer KEK rotation cannot finish while a workspace holding data keys is not ready; the
    // replaced KEK is kept, and the next rotation finishes the job.
    EXPECT_EQ(admin("rotate_customer_kek", {{"customer_id", first.customer_id}}).error.code,
              "workspace_not_ready");
    EXPECT_NE(t.store->customer_kek_state(first.customer_id)->previous_version, 0U);
    t.ready(first.workspaces[1]);
    const auto rotated = admin("rotate_customer_kek", {{"customer_id", first.customer_id}});
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(t.store->customer_kek_state(first.customer_id)->previous_version, 0U);
    for (std::size_t index : {0U, 1U}) {
        const auto read =
            admin("read_secret", {{"mount_id", placed[index].mount}, {"name", "database"}});
        ASSERT_TRUE(read.ok) << read.error.message;
    }
    // The other space's KEK and keys are untouched.
    EXPECT_EQ(t.store->customer_kek_state(spaces[1].customer_id)->version, 1U);

    // Workspace deletion: its mount goes with it, the space's other workspace keeps its own, and
    // a later rotation skips the deleted workspace.
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    EXPECT_FALSE(admin("read_secret", {{"mount_id", placed[1].mount}, {"name", "database"}}).ok);
    EXPECT_TRUE(admin("read_secret", {{"mount_id", placed[0].mount}, {"name", "database"}}).ok);
    EXPECT_TRUE(admin("rotate_customer_kek", {{"customer_id", first.customer_id}}).ok);
    EXPECT_TRUE(admin("read_secret", {{"mount_id", placed[0].mount}, {"name", "database"}}).ok);
}

// lake#561: a secret mount's owner is the account that owns the workspace (an individual's
// personal space here, with no organization at all) or, as before, an organization inside the
// space. An account's mount hangs under the customer's policy object, and its name is claimed for
// the account, so an account mount and an organization mount of the same name do not collide.
TEST_P(WorkspaceRecordStores, SecretMountsBelongToAnAccountOrAnOrganization) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& own = t.spaces[0];
    const auto& other = t.spaces[1];
    const auto as = [&](const TwoSpaces::Space& space, const std::string& capability, json input) {
        return secret_call(t, t.context(space), capability, std::move(input));
    };
    const std::string workspace = own.workspaces[0];

    // The account creates a mount without naming an organization: it needs admin on its customer.
    const auto mount = as(own,
                          "create_secret_mount",
                          {{"customer_id", own.customer_id},
                           {"workspace_id", workspace},
                           {"name", "vault"},
                           {"description", "Personal credentials"}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    EXPECT_EQ(mount.value["owner_type"], "customer");
    EXPECT_TRUE(mount.value["organization_id"].is_null());
    EXPECT_EQ(mount.value["workspace_id"], workspace);

    // Its route carries no organization, and its policy object hangs under the customer's.
    const auto route = t.store->get_secret_mount_route(mount_id);
    ASSERT_TRUE(route) << route.error().message;
    EXPECT_TRUE(route->organization_id.empty());
    EXPECT_EQ(route->workspace_id, workspace);
    const auto mount_object = t.store->find_policy_object("secret_mount", mount_id);
    ASSERT_TRUE(mount_object);
    const auto environment = t.store->get_policy_object(mount_object->parent_id);
    ASSERT_TRUE(environment);
    EXPECT_EQ(environment->object_type, "environment");
    EXPECT_EQ(environment->external_id, "secrets/" + own.customer_id);
    const auto customer_object = t.store->find_policy_object("customer", own.customer_id);
    ASSERT_TRUE(customer_object);
    EXPECT_EQ(environment->parent_id, customer_object->id);

    // Secrets: put, read, list, and the mount among the account's readable mounts.
    const auto put = as(own,
                        "put_secret",
                        {{"mount_id", mount_id},
                         {"name", "database"},
                         {"value", {{"password", "account-owned-password"}}}});
    ASSERT_TRUE(put.ok) << put.error.message;
    EXPECT_EQ(put.value["version"], 1);
    const auto read = as(own, "read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["value"]["password"], "account-owned-password");
    const auto secrets = as(own, "list_secrets", {{"mount_id", mount_id}});
    ASSERT_TRUE(secrets.ok) << secrets.error.message;
    ASSERT_EQ(secrets.value["items"].size(), 1U);
    EXPECT_EQ(secrets.value["items"][0]["name"], "database");
    const auto mounts = as(
        own, "list_secret_mounts", {{"customer_id", own.customer_id}, {"workspace_id", workspace}});
    ASSERT_TRUE(mounts.ok) << mounts.error.message;
    ASSERT_EQ(mounts.value["items"].size(), 1U);
    EXPECT_EQ(mounts.value["items"][0]["id"], mount_id);
    EXPECT_EQ(mounts.value["items"][0]["owner_type"], "customer");

    // Keys: create one and use it.
    const auto key = as(own, "create_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}});
    ASSERT_TRUE(key.ok) << key.error.message;
    const auto sealed =
        as(own, "encrypt", {{"mount_id", mount_id}, {"name", "orders"}, {"plaintext", "aGVsbG8"}});
    ASSERT_TRUE(sealed.ok) << sealed.error.message;
    const auto opened =
        as(own,
           "decrypt",
           {{"mount_id", mount_id}, {"name", "orders"}, {"ciphertext", sealed.value["value"]}});
    ASSERT_TRUE(opened.ok) << opened.error.message;
    EXPECT_EQ(opened.value["value"], "aGVsbG8");
    const auto keys = as(own, "list_encryption_keys", {{"mount_id", mount_id}});
    ASSERT_TRUE(keys.ok) << keys.error.message;
    EXPECT_EQ(keys.value["items"].size(), 1U);

    // The records are in the workspace, sealed: no name or value in the clear.
    auto& tables = t.zetta->records["http://" + workspace + ".test"];
    for (const std::string collection : {"secrets_mounts", "secrets_entries", "secrets_versions"}) {
        for (const auto& [record_key, row] : tables.rows(collection)) {
            EXPECT_EQ(row.dump().find("account-owned-password"), std::string::npos) << collection;
            EXPECT_EQ(row.dump().find("Personal credentials"), std::string::npos) << collection;
        }
    }

    // Another person, with no grant on this account, is refused everywhere and lists nothing.
    const auto foreign_create =
        as(other,
           "create_secret_mount",
           {{"customer_id", own.customer_id}, {"workspace_id", workspace}, {"name", "trespass"}});
    EXPECT_EQ(foreign_create.error.code, "unauthorized");
    EXPECT_EQ(as(other, "read_secret", {{"mount_id", mount_id}, {"name", "database"}}).error.code,
              "unauthorized");
    EXPECT_EQ(as(other,
                 "put_secret",
                 {{"mount_id", mount_id}, {"name", "database"}, {"value", {{"password", "x"}}}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(as(other, "list_secrets", {{"mount_id", mount_id}}).error.code, "unauthorized");
    EXPECT_EQ(as(other, "get_secret_mount", {{"mount_id", mount_id}}).error.code, "unauthorized");
    EXPECT_EQ(
        as(other, "encrypt", {{"mount_id", mount_id}, {"name", "orders"}, {"plaintext", "aGk"}})
            .error.code,
        "unauthorized");
    EXPECT_EQ(
        as(other, "create_encryption_key", {{"mount_id", mount_id}, {"name", "mine"}}).error.code,
        "unauthorized");
    const auto foreign_list = as(other,
                                 "list_secret_mounts",
                                 {{"customer_id", own.customer_id}, {"workspace_id", workspace}});
    EXPECT_TRUE(foreign_list.ok ? foreign_list.value["items"].empty()
                                : foreign_list.error.code == "unauthorized")
        << foreign_list.value.dump();
    // Nor can the other person make their own account the owner of a mount in this workspace.
    const auto crossed =
        as(other,
           "create_secret_mount",
           {{"customer_id", other.customer_id}, {"workspace_id", workspace}, {"name", "crossed"}});
    EXPECT_FALSE(crossed.ok);
    EXPECT_EQ(tables.rows("secrets_mounts").size(), 2U) << "the mount and its name claim";

    // An organization inside the account's space owns a mount of the same name in the same
    // workspace: the claims are per owner, so neither collides with the other.
    const auto organization =
        as(own,
           "create_organization",
           {{"display_name", "Side project"}, {"customer_id", own.customer_id}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const std::string organization_id = organization.value["id"];
    const auto organization_mount = as(own,
                                       "create_secret_mount",
                                       {{"customer_id", own.customer_id},
                                        {"workspace_id", workspace},
                                        {"organization_id", organization_id},
                                        {"name", "vault"}});
    ASSERT_TRUE(organization_mount.ok) << organization_mount.error.message;
    EXPECT_EQ(organization_mount.value["owner_type"], "organization");
    EXPECT_EQ(organization_mount.value["organization_id"], organization_id);
    const std::string organization_mount_id = organization_mount.value["id"];
    EXPECT_NE(organization_mount_id, mount_id);
    // The organization's mount is as it always was: its route names the organization, and its
    // policy object hangs under the organization's environment.
    const auto organization_route = t.store->get_secret_mount_route(organization_mount_id);
    ASSERT_TRUE(organization_route);
    EXPECT_EQ(organization_route->organization_id, organization_id);
    const auto organization_environment = t.store->get_policy_object(
        t.store->find_policy_object("secret_mount", organization_mount_id)->parent_id);
    ASSERT_TRUE(organization_environment);
    EXPECT_EQ(organization_environment->external_id,
              "secrets/" + own.customer_id + "/" + organization_id);
    EXPECT_EQ(organization_environment->parent_id,
              t.store->find_policy_object("organization", organization_id)->id);
    // Each owner still claims its name once.
    for (const json& owner : {json::object(), json{{"organization_id", organization_id}}}) {
        json input = {
            {"customer_id", own.customer_id}, {"workspace_id", workspace}, {"name", "vault"}};
        input.update(owner);
        const auto again = as(own, "create_secret_mount", input);
        EXPECT_EQ(again.error.code, "conflict") << owner.dump();
        EXPECT_EQ(again.error.path, "/name") << owner.dump();
    }
    // Both mounts list, each with its own owner, and keep their own secrets.
    const auto both = as(
        own, "list_secret_mounts", {{"customer_id", own.customer_id}, {"workspace_id", workspace}});
    ASSERT_TRUE(both.ok) << both.error.message;
    ASSERT_EQ(both.value["items"].size(), 2U);
    std::set<std::string> owners;
    for (const auto& item : both.value["items"]) {
        owners.insert(item["owner_type"].get<std::string>());
    }
    EXPECT_EQ(owners, (std::set<std::string>{"customer", "organization"}));
    const auto only_organization = as(own,
                                      "list_secret_mounts",
                                      {{"customer_id", own.customer_id},
                                       {"workspace_id", workspace},
                                       {"organization_id", organization_id}});
    ASSERT_TRUE(only_organization.ok) << only_organization.error.message;
    ASSERT_EQ(only_organization.value["items"].size(), 1U);
    EXPECT_EQ(only_organization.value["items"][0]["id"], organization_mount_id);
    EXPECT_EQ(as(own, "read_secret", {{"mount_id", organization_mount_id}, {"name", "database"}})
                  .error.code,
              "not_found");
    EXPECT_TRUE(as(own, "read_secret", {{"mount_id", mount_id}, {"name", "database"}}).ok);

    // The other person's organization is not inside this space, and cannot own a mount here.
    const auto foreign_organization =
        as(other,
           "create_organization",
           {{"display_name", "Elsewhere"}, {"customer_id", other.customer_id}});
    ASSERT_TRUE(foreign_organization.ok) << foreign_organization.error.message;
    const auto outside = secret_call(t,
                                     secret_operator(t),
                                     "create_secret_mount",
                                     {{"customer_id", own.customer_id},
                                      {"workspace_id", workspace},
                                      {"organization_id", foreign_organization.value["id"]},
                                      {"name", "outside"}});
    EXPECT_EQ(outside.error.code, "invalid_input");
    EXPECT_EQ(outside.error.path, "/organization_id");

    // A customer KEK rotation rewraps the account's data keys too; its secrets stay readable.
    const auto rotated = as(own, "rotate_customer_kek", {{"customer_id", own.customer_id}});
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    const auto after = as(own, "read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value["value"]["password"], "account-owned-password");

    if (GetParam()) {
        // Migration 0071: the route's organization is nullable, and the account's route holds NULL.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        EXPECT_EQ(tx.exec("SELECT is_nullable FROM information_schema.columns WHERE table_name = "
                          "'secret_mount_routes' AND column_name = 'organization_id'")[0][0]
                      .as<std::string>(),
                  "YES");
        EXPECT_TRUE(tx.exec_params("SELECT organization_id FROM secret_mount_routes WHERE "
                                   "mount_id = $1::uuid",
                                   mount_id)[0][0]
                        .is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('secret_mount_owners_guard')")[0][0].is_null());
    }
}

std::int64_t lease_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// lake#362: dynamic leases are records of their mount's workspace among two spaces with two
// workspaces each; the control store keeps only each lease's content-free route.
TEST_P(WorkspaceRecordStores, DynamicLeasesLiveInTheirMountsWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto admin = [&](const std::string& capability, json input) {
        return secret_call(t, secret_operator(t), capability, std::move(input));
    };
    std::vector<SecretSpace> spaces{secret_space(t, "first", 2), secret_space(t, "second", 2)};
    struct Placed {
        std::size_t space, workspace;
        std::string mount, lease, password;
    };
    std::vector<Placed> placed;
    for (std::size_t s = 0; s < spaces.size(); ++s) {
        for (std::size_t w = 0; w < spaces[s].workspaces.size(); ++w) {
            const auto mount = admin("create_secret_mount",
                                     {{"customer_id", spaces[s].customer_id},
                                      {"workspace_id", spaces[s].workspaces[w]},
                                      {"organization_id", spaces[s].organization_id},
                                      {"name", "leases"}});
            ASSERT_TRUE(mount.ok) << mount.error.message;
            const auto leased = admin("lease_dynamic_secret",
                                      {{"mount_id", mount.value["id"]},
                                       {"role", "role-" + std::to_string(s) + std::to_string(w)},
                                       {"lifetime_seconds", 600}});
            ASSERT_TRUE(leased.ok) << leased.error.message;
            EXPECT_EQ(leased.value["lease"]["workspace_id"], spaces[s].workspaces[w]);
            placed.push_back({s, w, mount.value["id"], leased.value["lease"]["id"],
                              leased.value["value"]["password"]});
        }
    }
    // Each lease is stored, sealed, only at its own workspace's endpoint.
    for (const auto& where : placed) {
        const std::string workspace = spaces[where.space].workspaces[where.workspace];
        auto& rows = t.zetta->records["http://" + workspace + ".test"].rows("secrets_dynamic_leases");
        ASSERT_EQ(rows.size(), 1U) << workspace;
        EXPECT_TRUE(rows.contains(where.lease));
        const std::string dumped = rows.at(where.lease).dump();
        EXPECT_EQ(dumped.find(where.password), std::string::npos);
        EXPECT_EQ(dumped.find("role-"), std::string::npos);
        const auto route = t.store->get_secret_lease_route(where.lease);
        ASSERT_TRUE(route) << route.error().message;
        EXPECT_EQ(route->workspace_id, workspace);
        EXPECT_EQ(route->customer_id, spaces[where.space].customer_id);
        EXPECT_EQ(route->mount_id, where.mount);
        EXPECT_GT(route->due_us, 0);
    }
    if (GetParam()) {
        // PostgreSQL holds no lease, and the route holds no content.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT to_regclass('secrets_dynamic_leases')")[0][0].is_null());
        EXPECT_EQ(tx.exec("SELECT count(*) FROM secret_lease_routes")[0][0].as<int>(), 4);
        const auto columns =
            tx.exec("SELECT string_agg(column_name, ',' ORDER BY column_name) FROM "
                    "information_schema.columns WHERE table_name = 'secret_lease_routes' AND "
                    "table_schema = current_schema()");
        EXPECT_EQ(columns[0][0].as<std::string>(),
                  "created_at,customer_id,due_at,lease_id,mount_id,workspace_id");
        // The route's workspace never changes.
        EXPECT_THROW(tx.exec_params("UPDATE secret_lease_routes SET workspace_id = $2::uuid "
                                    "WHERE lease_id = $1::uuid",
                                    placed[0].lease, spaces[0].workspaces[1]),
                     pqxx::check_violation);
    }

    // A record copied into another workspace does not open there.
    {
        auto context = secret_operator(t);
        const auto& first = spaces[0];
        auto& home = t.zetta->records["http://" + first.workspaces[0] + ".test"];
        auto& lab = t.zetta->records["http://" + first.workspaces[1] + ".test"];
        lab.rows("secrets_dynamic_leases")[placed[0].lease] =
            home.rows("secrets_dynamic_leases").at(placed[0].lease);
        auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[1]);
        ASSERT_TRUE(records);
        const auto moved = run(
            records->get(lake::control::SecretLeaseCodec::collection, placed[0].lease));
        ASSERT_TRUE(moved);
        EXPECT_FALSE(*moved);
        lab.rows("secrets_dynamic_leases").erase(placed[0].lease);
    }

    // Least privilege: a person of another space can neither renew nor revoke, and reaches no
    // workspace doing so.
    const auto person = t.context(t.spaces[1]);
    auto& home = t.zetta->records["http://" + spaces[0].workspaces[0] + ".test"];
    home.calls.clear();
    for (const std::string capability : {"renew_dynamic_secret", "revoke_dynamic_secret"}) {
        const auto denied = secret_call(t, person, capability, {{"lease_id", placed[0].lease}});
        EXPECT_EQ(denied.error.code, "unauthorized") << capability;
    }
    EXPECT_TRUE(home.calls.empty()) << "a refused caller reaches no workspace";
    const auto denied_lease = secret_call(t, person, "lease_dynamic_secret",
                                          {{"mount_id", placed[0].mount}, {"role", "x"}});
    EXPECT_EQ(denied_lease.error.code, "unauthorized");
    for (const std::string bad : {std::string("not-a-lease"), lake::control::new_uuid_v4()}) {
        const auto missing = admin("renew_dynamic_secret", {{"lease_id", bad}});
        EXPECT_EQ(missing.error.code, "not_found") << bad;
        EXPECT_EQ(missing.error.path, "/lease_id") << bad;
    }

    // A workspace that is not ready refuses its leases; the other workspace serves on, and the
    // expiry loop leaves the lease for later.
    const auto& first = spaces[0];
    const auto lab_workspace = t.store->get_workspace(first.workspaces[1]);
    t.status(lab_workspace->zetta_service_id, "Provisioning",
             "http://" + first.workspaces[1] + ".test");
    EXPECT_EQ(admin("renew_dynamic_secret", {{"lease_id", placed[1].lease}}).error.code,
              "workspace_not_ready");
    EXPECT_TRUE(admin("renew_dynamic_secret", {{"lease_id", placed[0].lease}}).ok);
    t.ready(first.workspaces[1]);

    // Due leases are settled in their own workspace only.
    ASSERT_TRUE(t.store->reschedule_secret_lease(placed[1].lease, 1));
    for (const auto& where : placed) {
        if (where.lease != placed[1].lease) {
            ASSERT_TRUE(admin("revoke_dynamic_secret", {{"lease_id", where.lease}}).ok);
        }
    }
    auto context = secret_operator(t);
    auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[1]);
    ASSERT_TRUE(records);
    {
        auto got = run(records->get(lake::control::SecretLeaseCodec::collection, placed[1].lease));
        ASSERT_TRUE(got && *got);
        auto lease = lake::control::SecretLeaseCodec::decode(**got);
        ASSERT_TRUE(lease);
        lease->expires_us = lease_now_us() - 1;
        ASSERT_TRUE(run(records->put(lake::control::SecretLeaseCodec::collection, placed[1].lease,
                                     lake::control::SecretLeaseCodec::lookup(placed[1].mount),
                                     lake::control::SecretLeaseCodec::encode(*lease),
                                     lake::control::RecordCondition{.if_revision = lease->revision},
                                     lease->lake_revision + 1, lease->at_us)));
    }
    const auto swept = liquid::runtime::block_on(lake::api::expire_secret_leases(context));
    ASSERT_TRUE(swept);
    EXPECT_EQ(swept->claimed, 1U) << "only the due lease is claimed";
    EXPECT_EQ(swept->expired, 1U);
    {
        auto got = run(records->get(lake::control::SecretLeaseCodec::collection, placed[1].lease));
        ASSERT_TRUE(got && *got);
        const auto lease = lake::control::SecretLeaseCodec::decode(**got);
        ASSERT_TRUE(lease);
        EXPECT_FALSE(lease->expired_at.empty());
        EXPECT_FALSE(lease->ciphertext);
    }
    EXPECT_EQ(admin("renew_dynamic_secret", {{"lease_id", placed[1].lease}}).error.code, "conflict");

    if (GetParam()) {
        // A route whose record was never written (a create that stopped between the two) is
        // removed once it is old enough that no write can still be on its way.
        const std::string abandoned = lake::control::new_uuid_v4();
        ASSERT_TRUE(t.store->record_secret_lease_route(
            {abandoned, placed[0].mount, first.customer_id, first.workspaces[0], 1}));
        {
            pqxx::connection connection(database_uri_);
            pqxx::work tx(connection);
            tx.exec_params("UPDATE secret_lease_routes SET created_at = now() - interval '10 "
                           "minutes' WHERE lease_id = $1::uuid",
                           abandoned);
            tx.commit();
        }
        const auto removed = liquid::runtime::block_on(lake::api::expire_secret_leases(context));
        ASSERT_TRUE(removed);
        EXPECT_EQ(removed->removed, 1U);
        EXPECT_EQ(t.store->get_secret_lease_route(abandoned).error().code, "not_found");
    }

    // Workspace deletion: its leases go with it; the space's other workspace keeps its own.
    const auto fresh = admin("lease_dynamic_secret",
                             {{"mount_id", placed[1].mount}, {"role", "late"}});
    ASSERT_TRUE(fresh.ok) << fresh.error.message;
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    EXPECT_FALSE(admin("revoke_dynamic_secret", {{"lease_id", fresh.value["lease"]["id"]}}).ok);
    EXPECT_TRUE(admin("revoke_dynamic_secret", {{"lease_id", placed[0].lease}}).ok);
    ASSERT_TRUE(t.store->reschedule_secret_lease(fresh.value["lease"]["id"], 1));
    const auto after_delete = liquid::runtime::block_on(lake::api::expire_secret_leases(context));
    ASSERT_TRUE(after_delete);
    EXPECT_EQ(after_delete->settled, 1U);
    EXPECT_EQ(t.store->get_secret_lease_route(fresh.value["lease"]["id"])->due_us, 0);
}

// lake#382 (decision 54, S7): the instance transparency log stays with the instance and keeps the
// leaf a relying party expects for each dynamic-secret lease, renewal and revocation (the
// identity-exit scenario), even though the lease itself is a record of its mount's workspace and a
// workspace audit route is available. The three capabilities stay in the central chain, so
// finish_call appends only the recorded central event's digest commitment. Complete events and
// routing identifiers stay in private audit (lake#953, LSR-087).
TEST_P(WorkspaceRecordStores, DynamicSecretsInAWorkspaceKeepTheirInstanceLogLeaf) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    for (const std::string_view capability :
         {"lease_dynamic_secret", "renew_dynamic_secret", "revoke_dynamic_secret"}) {
        EXPECT_EQ(lake::api::audit_log_of(capability), lake::api::AuditLog::Central) << capability;
    }
    TwoSpaces t(store, database_uri_);
    lake::api::AuditRecorder recorder(t.store.get(), t.service.get(), std::string(kek), "");
    lake::transparency::TiledLog log(t.dir / "store", "lake");
    const auto admin = [&](const std::string& capability, json input) {
        auto context = secret_operator(t);
        context.audit_recorder = &recorder;
        context.transparency_log = &log;
        return secret_call(t, std::move(context), capability, std::move(input));
    };
    const SecretSpace space = secret_space(t, "leaf", 1);
    const std::string& workspace = space.workspaces[0];
    const std::string mount_name = "leaf-mount-" + lake::control::new_uuid_v4().substr(0, 8);
    const std::string role = "leaf-role-" + lake::control::new_uuid_v4().substr(0, 8);
    const auto mount = admin("create_secret_mount", {{"customer_id", space.customer_id},
                                                     {"workspace_id", workspace},
                                                     {"organization_id", space.organization_id},
                                                     {"name", mount_name}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::size_t queued = recorder.pending();
    const auto before = log.state();
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before->tree_size, 0U) << "only dynamic-secret events reach this log";

    const auto leased = admin("lease_dynamic_secret", {{"mount_id", mount.value["id"]},
                                                       {"role", role},
                                                       {"lifetime_seconds", 600}});
    ASSERT_TRUE(leased.ok) << leased.error.message;
    EXPECT_EQ(leased.value["lease"]["workspace_id"], workspace) << "the lease is workspace data";
    const std::string lease_id = leased.value["lease"]["id"];
    const std::string username = leased.value["value"]["username"];
    const std::string password = leased.value["value"]["password"];
    ASSERT_FALSE(password.empty());
    const auto after_lease = log.state();
    ASSERT_TRUE(after_lease) << after_lease.error().message;
    EXPECT_EQ(after_lease->tree_size, 1U) << "leasing in a workspace appends the instance leaf";
    ASSERT_TRUE(admin("renew_dynamic_secret", {{"lease_id", lease_id}}).ok);
    ASSERT_TRUE(admin("revoke_dynamic_secret", {{"lease_id", lease_id}}).ok);
    EXPECT_EQ(recorder.pending(), queued) << "no dynamic-secret event is queued for the workspace";

    // Each leaf commits to the private central event, in order: lease, renew, revoke.
    const auto events =
        lake::test::audit_events(*t.store, {.object = "dynamic_secret/" + lease_id, .limit = 10});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_EQ(events->size(), 3U);
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    ASSERT_EQ(state->tree_size, 3U);
    const auto tile = log.tile(0, 3);
    ASSERT_TRUE(tile) << tile.error().message;
    std::string encoded = tile->encoded;
    std::ranges::replace(encoded, '-', '+');
    std::ranges::replace(encoded, '_', '/');
    while (encoded.size() % 4 != 0) {
        encoded.push_back('=');
    }
    std::string leaves((encoded.size() / 4) * 3, '\0');
    const int decoded_size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(leaves.data()),
                                             reinterpret_cast<const unsigned char*>(encoded.data()),
                                             static_cast<int>(encoded.size()));
    ASSERT_GE(decoded_size, 0);
    leaves.resize(static_cast<std::size_t>(decoded_size) - (encoded.ends_with("==")  ? 2U
                                                            : encoded.ends_with('=') ? 1U
                                                                                     : 0U));
    constexpr std::string_view tile_header = "LAKE-TILE-1\n";
    ASSERT_TRUE(leaves.starts_with(tile_header));
    std::size_t offset = tile_header.size();
    std::set<std::string> operations;
    for (const auto& event : *events) {
        EXPECT_EQ(event.draft.kind, "dynamic_secret");
        EXPECT_EQ(event.draft.customer_id, space.customer_id);
        EXPECT_EQ(event.draft.object, "dynamic_secret/" + lease_id);
        EXPECT_EQ(event.draft.details.at("workspace_id"), workspace);
        operations.insert(event.draft.details.value("operation", ""));
        ASSERT_GE(leaves.size() - offset, 4U);
        std::size_t size = 0;
        for (int part = 0; part < 4; ++part) {
            size = (size << 8) | static_cast<unsigned char>(leaves[offset++]);
        }
        ASSERT_GE(leaves.size() - offset, size);
        const std::string_view entry(leaves.data() + offset, size);
        constexpr std::string_view header = "LAKE-AUDIT-COMMITMENT-1\n";
        ASSERT_TRUE(entry.starts_with(header));
        EXPECT_EQ(json::from_cbor(entry.begin() + header.size(), entry.end()),
                  (json{{"digest", event.digest}}));
        offset += size;
    }
    EXPECT_EQ(offset, leaves.size());
    EXPECT_EQ(operations, (std::set<std::string>{"lease", "renew", "revoke"}));
    // Neither credentials nor tenant and routing identifiers reach the public log.
    for (const std::string& private_value : {username,
                                             password,
                                             role,
                                             mount_name,
                                             lease_id,
                                             space.customer_id,
                                             workspace,
                                             mount.value.at("id").get<std::string>()}) {
        EXPECT_EQ(leaves.find(private_value), std::string::npos);
    }
}

// MCP connections (decision 60, lake#652, docs/api/mcp-connections.md): a connection is a sealed
// record in its workspace; its token is a workspace secret Lake reads server-side and never
// answers; a tool runs only once a person allowed it; a change is a person's or an auto-approved
// agent's; every call is audited in the workspace and metered.
TEST_P(WorkspaceRecordStores, McpConnectionsLiveInTheirWorkspaceAndKeepTheirTokenSecret) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.required_bearer = "the-plaintext-bearer-token";
    t.mcp_http = lake::api::make_mcp_http(true);
    lake::api::UsageRecorder usage(t.store.get(), t.service.get());
    lake::api::AuditRecorder audit(t.store.get(), t.service.get(), std::string(kek), "");
    t.usage_recorder = &usage;
    t.audit_recorder = &audit;
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string home = first.workspaces[0];
    const auto in = [&](std::size_t workspace, json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = first.workspaces[workspace];
        return input;
    };

    // The token, in a secret of the workspace.
    const auto mount = t.call(first, "create_secret_mount", in(0, {{"name", "mcp-tokens"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    ASSERT_TRUE(t.call(first,
                       "put_secret",
                       {{"mount_id", mount_id},
                        {"name", "ticket-desk"},
                        {"value", {{"token", "the-plaintext-bearer-token"}}}})
                    .ok);
    const json secret{{"mount_id", mount_id}, {"name", "ticket-desk"}, {"field", "token"}};

    // Refusals before anything is kept.
    EXPECT_EQ(t.call(first,
                     "create_mcp_connection",
                     in(0, {{"name", "x"}, {"url", "http://mcp.example.com/mcp"}}))
                  .error.path,
              "/url");
    EXPECT_EQ(
        t.call(first,
               "create_mcp_connection",
               in(0,
                  {{"name", "x"},
                   {"url", server.url()},
                   {"auth",
                    {{"kind", "bearer"},
                     {"secret",
                      {{"mount_id", mount_id}, {"name", "ticket-desk"}, {"field", "nope"}}}}}}))
            .error.path,
        "/auth/secret/field");
    EXPECT_EQ(t.call(first,
                     "create_mcp_connection",
                     in(1,
                        {{"name", "x"},
                         {"url", server.url()},
                         {"auth", {{"kind", "bearer"}, {"secret", secret}}}}))
                  .error.path,
              "/auth/secret/mount_id")
        << "a secret of another workspace";

    // Created, and discovered at once; nothing allowed yet.
    const auto created = t.call(first,
                                "create_mcp_connection",
                                in(0,
                                   {{"name", "Ticket Desk"},
                                    {"url", server.url()},
                                    {"auth", {{"kind", "bearer"}, {"secret", secret}}}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value["id"];
    EXPECT_EQ(created.value["prefix"], "ticket_desk");
    EXPECT_EQ(created.value["status"], "connected") << created.value.dump();
    EXPECT_EQ(created.value["tool_count"], 4);
    EXPECT_EQ(created.value["allowed_count"], 0);
    EXPECT_EQ(created.value["auth"]["secret"], secret) << "the reference, never the value";
    EXPECT_EQ(created.value.dump().find("the-plaintext-bearer-token"), std::string::npos);
    for (const json& tool : created.value["tools"]) {
        EXPECT_FALSE(tool["allowed"].get<bool>());
        EXPECT_EQ(tool["change"], tool["name"] != "echo" && tool["name"] != "fail") << tool["name"];
    }
    EXPECT_EQ(t.call(first,
                     "create_mcp_connection",
                     in(0, {{"name", "ticket desk"}, {"url", server.url()}}))
                  .error.code,
              "conflict");

    // Sealed at rest, and the token is nowhere in the workspace's records.
    for (const auto& [key, row] :
         t.zetta->records["http://" + home + ".test"].rows("mcp_connections")) {
        EXPECT_EQ(row.dump().find("Ticket Desk"), std::string::npos);
        EXPECT_EQ(row.dump().find("the-plaintext-bearer-token"), std::string::npos);
    }
    EXPECT_TRUE(t.zetta->records["http://" + first.workspaces[1] + ".test"]
                    .rows("mcp_connections")
                    .empty());

    // Listed in its workspace only; another space is refused.
    auto listed = t.call(first, "list_mcp_connections", in(0, json::object()));
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["connections"].size(), 1U);
    EXPECT_TRUE(
        t.call(first, "list_mcp_connections", in(1, json::object())).value["connections"].empty());
    EXPECT_EQ(t.call(second, "get_mcp_connection", in(0, {{"id", id}})).error.code, "unauthorized");

    // Not allowed: refused before the server hears of it.
    const std::size_t heard = server.calls("tools/call").size();
    EXPECT_EQ(
        t.call(first,
               "read_mcp_tool",
               in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "x"}}}}))
            .error.path,
        "/tool");
    EXPECT_EQ(server.calls("tools/call").size(), heard);

    // A person allows two tools; echo reads, create_ticket changes.
    const auto decided = t.call(first,
                                "update_mcp_connection",
                                in(0,
                                   {{"id", id},
                                    {"tools",
                                     {{"echo", {{"allowed", true}}},
                                      {"create_ticket", {{"allowed", true}}},
                                      {"inject", {{"allowed", true}, {"change", false}}}}}}));
    ASSERT_TRUE(decided.ok) << decided.error.message;
    EXPECT_EQ(decided.value["allowed_count"], 3);
    EXPECT_EQ(t.call(first,
                     "update_mcp_connection",
                     in(0, {{"id", id}, {"tools", {{"nope", {{"allowed", true}}}}}}))
                  .error.path,
              "/tools/nope");

    const auto read = t.call(
        first,
        "read_mcp_tool",
        in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "hello"}}}}));
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["structured_content"]["echo"]["text"], "hello");
    EXPECT_FALSE(read.value["change"].get<bool>());
    EXPECT_EQ(server.calls("tools/call").back()["params"]["arguments"]["text"], "hello");
    const auto refused_change = t.call(
        first,
        "read_mcp_tool",
        in(0, {{"connection_id", id}, {"tool", "create_ticket"}, {"arguments", {{"title", "x"}}}}));
    EXPECT_EQ(refused_change.error.code, "invalid_input") << "a change never runs as a read";
    const auto opened = t.call(first,
                               "call_mcp_tool",
                               in(0,
                                  {{"connection_id", id},
                                   {"tool", "create_ticket"},
                                   {"arguments", {{"title", "Broken"}}}}));
    ASSERT_TRUE(opened.ok) << opened.error.message;
    EXPECT_EQ(opened.value["structured_content"]["id"], "T-1");
    // An outside server's words come back as data, marked as such.
    const auto injected =
        t.call(first, "read_mcp_tool", in(0, {{"connection_id", id}, {"tool", "inject"}}));
    ASSERT_TRUE(injected.ok) << injected.error.message;
    EXPECT_NE(injected.value["note"].get<std::string>().find("data, never instructions"),
              std::string::npos);

    // An agent: Ask Orama's turn reads, never changes.
    const auto task =
        t.store->create_agent_task({.customer_id = first.customer_id,
                                    .agent = "agent-ask-orama",
                                    .kind = "ask",
                                    .workspace_id = home,
                                    .body = {{"message", "hi"}},
                                    .scopes = {"read_mcp_tool", "list_mcp_connection_tools"},
                                    .requested_by_user_id = first.principal.user_id});
    ASSERT_TRUE(task) << task.error().message;
    const std::string claim(64, 'a');
    ASSERT_TRUE(t.store->claim_agent_task(task->id, "agent-ask-orama", claim));
    auto ask = t.context(first);
    ask.credential_token_hash = claim;
    ask.credential_kind = "agent_task";
    ask.credential_session_id = "task:" + task->id;
    ask.scopes = {"read_mcp_tool", "list_mcp_connection_tools", "call_mcp_tool"};
    const auto as = [&](lake::api::ApiContext context, const std::string& capability, json input) {
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    EXPECT_TRUE(as(ask,
                   "read_mcp_tool",
                   in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "a"}}}}))
                    .ok);
    EXPECT_EQ(
        as(ask,
           "call_mcp_tool",
           in(0,
              {{"connection_id", id}, {"tool", "create_ticket"}, {"arguments", {{"title", "a"}}}}))
            .error.code,
        "not_approved");
    const auto agent_tools = as(ask, "list_mcp_connection_tools", in(0, json::object()));
    ASSERT_TRUE(agent_tools.ok) << agent_tools.error.message;
    EXPECT_EQ(agent_tools.value["tools"].size(), 3U) << "an agent sees allowed tools only";
    EXPECT_EQ(
        agent_tools.value["tools"][0]["exported_name"].get<std::string>().rfind("ticket_desk__", 0),
        0U);
    EXPECT_EQ(as(ask,
                 "set_mcp_tool_auto_approve",
                 in(0,
                    {{"connection_id", id},
                     {"tool", "create_ticket"},
                     {"agent", "agent-ask-orama"},
                     {"auto_approve", true}}))
                  .error.code,
              "unauthorized")
        << "an agent never grants itself";

    // A user-built agent: a change runs only with the person's grant, and only with its skill.
    const auto launch = t.store->create_agent_launch({.customer_id = first.customer_id,
                                                      .agent_id = "agent-7",
                                                      .agent_name = "Triage",
                                                      .mode = "job",
                                                      .trigger = "manual",
                                                      .scopes = lake::control::agent_launch_scopes,
                                                      .created_by_user_id = first.principal.user_id,
                                                      .created_by_taap_id = first.principal.taap_id,
                                                      .workspace_id = home});
    ASSERT_TRUE(launch) << launch.error().message;
    auto agent = t.context(first);
    agent.session_credential = false;
    agent.credential_kind = "agent_launch";
    agent.credential_session_id = "launch:" + launch->id;
    agent.scopes = lake::control::agent_launch_scopes;
    t.zetta->answers["get_agent"] = {
        {"id", "agent-7"}, {"name", "Triage"}, {"skills", json::array({"search"})}};
    EXPECT_EQ(as(agent,
                 "read_mcp_tool",
                 in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "a"}}}}))
                  .error.code,
              "unauthorized")
        << "no mcp:ticket_desk skill";
    t.zetta->answers["get_agent"] = {
        {"id", "agent-7"}, {"name", "Triage"}, {"skills", json::array({"mcp:ticket_desk"})}};
    EXPECT_TRUE(as(agent,
                   "read_mcp_tool",
                   in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "a"}}}}))
                    .ok);
    // agent-runtime sends no workspace_id: the launch names it.
    json without = in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "a"}}}});
    without.erase("workspace_id");
    EXPECT_TRUE(as(agent, "read_mcp_tool", without).ok) << "the launch's workspace";
    const json change_input = in(
        0, {{"connection_id", id}, {"tool", "create_ticket"}, {"arguments", {{"title", "auto"}}}});
    EXPECT_EQ(as(agent, "call_mcp_tool", change_input).error.code, "not_approved");
    EXPECT_EQ(t.call(first,
                     "set_mcp_tool_auto_approve",
                     in(0,
                        {{"connection_id", id},
                         {"tool", "echo"},
                         {"agent", "agent-7"},
                         {"auto_approve", true}}))
                  .error.path,
              "/tool")
        << "a read needs no approval";
    const auto granted = t.call(first,
                                "set_mcp_tool_auto_approve",
                                in(0,
                                   {{"connection_id", id},
                                    {"tool", "create_ticket"},
                                    {"agent", "agent-7"},
                                    {"auto_approve", true}}));
    ASSERT_TRUE(granted.ok) << granted.error.message;
    const auto automatic = as(agent, "call_mcp_tool", change_input);
    ASSERT_TRUE(automatic.ok) << automatic.error.message;
    ASSERT_TRUE(t.call(first,
                       "set_mcp_tool_auto_approve",
                       in(0,
                          {{"connection_id", id},
                           {"tool", "create_ticket"},
                           {"agent", "agent-7"},
                           {"auto_approve", false}}))
                    .ok);
    EXPECT_EQ(as(agent, "call_mcp_tool", change_input).error.code, "not_approved") << "taken back";

    // Every call is in the workspace's audit, with who, which tool and how; never the arguments.
    const auto calls = t.call(first, "list_mcp_connection_calls", in(0, {{"id", id}}));
    ASSERT_TRUE(calls.ok) << calls.error.message;
    bool saw_auto = false;
    for (const json& row : calls.value["calls"]) {
        EXPECT_EQ(row.dump().find("Broken"), std::string::npos) << "no arguments in the audit";
        if (row["action"] == "call_mcp_tool" && row["auto_approved"] == true) {
            saw_auto = true;
            EXPECT_EQ(row["agent"], "Triage");
            EXPECT_EQ(row["via"], "agent");
        }
    }
    EXPECT_TRUE(saw_auto) << calls.value.dump();

    // Metered in the workspace: calls and bytes each way.
    liquid::runtime::block_on(usage.flush());
    auto& tables = t.zetta->records["http://" + home + ".test"];
    EXPECT_GE(
        tables.metric_total(
            "lake_usage", {{"dimension", "calls"}, {"kind", "mcp_connection"}, {"object_id", id}}),
        6.0);
    EXPECT_GT(tables.metric_total("lake_usage", {{"dimension", "transfer"}, {"kind", "mcp_in"}}),
              0.0);
    EXPECT_GT(tables.metric_total("lake_usage", {{"dimension", "transfer"}, {"kind", "mcp_out"}}),
              0.0);

    // Re-export: only once a person turns it on, under the prefix, through the workspace's MCP.
    const auto list_tools = [&](lake::api::ApiContext context) {
        context.path_workspace_id = home;
        context.registry = &t.registry;
        const auto answered = liquid::runtime::block_on(
            lake::api::mcp_dispatch_async(t.registry,
                                          {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                          context,
                                          "test"));
        EXPECT_TRUE(answered && *answered);
        std::set<std::string> names;
        for (const json& tool : json::parse(**answered)["result"]["tools"]) {
            names.insert(tool["name"].get<std::string>());
        }
        return names;
    };
    EXPECT_FALSE(list_tools(t.context(first)).contains("ticket_desk__echo"));
    ASSERT_TRUE(t.call(first, "update_mcp_connection", in(0, {{"id", id}, {"exported", true}})).ok);
    auto names = list_tools(t.context(first));
    EXPECT_TRUE(names.contains("ticket_desk__echo"));
    EXPECT_TRUE(names.contains("ticket_desk__create_ticket"));
    EXPECT_FALSE(names.contains("ticket_desk__fail")) << "not allowed, not exported";
    EXPECT_TRUE(names.contains("list_mcp_connections")) << "Lake's own tools stay";
    EXPECT_FALSE(list_tools(t.context(second)).contains("ticket_desk__echo")) << "another space";
    auto mcp_context = t.context(first);
    mcp_context.path_workspace_id = home;
    const auto echoed = liquid::runtime::block_on(lake::api::mcp_dispatch_async(
        t.registry,
        {{"jsonrpc", "2.0"},
         {"id", 2},
         {"method", "tools/call"},
         {"params", {{"name", "ticket_desk__echo"}, {"arguments", {{"text", "through lake"}}}}}},
        mcp_context,
        "test"));
    ASSERT_TRUE(echoed && *echoed);
    const json echo = json::parse(**echoed)["result"];
    EXPECT_FALSE(echo["isError"].get<bool>()) << echo.dump();
    EXPECT_EQ(echo["structuredContent"]["echo"]["text"], "through lake")
        << "the server's own answer";
    const auto missing = liquid::runtime::block_on(
        lake::api::mcp_dispatch_async(t.registry,
                                      {{"jsonrpc", "2.0"},
                                       {"id", 3},
                                       {"method", "tools/call"},
                                       {"params", {{"name", "ticket_desk__fail"}}}},
                                      mcp_context,
                                      "test"));
    EXPECT_TRUE(json::parse(**missing)["result"]["isError"].get<bool>());
    // LSR-187 (lake#993): no credential, or a person of another space, reaches nothing.
    {
        auto anonymous = t.context(first);
        anonymous.authorized = false;
        anonymous.acting_identity.clear();
        auto other_space = t.context(second);
        for (auto* caller : {&anonymous, &other_space}) {
            caller->path_workspace_id = home;
            const auto refused = liquid::runtime::block_on(lake::api::mcp_dispatch_async(
                t.registry,
                {{"jsonrpc", "2.0"},
                 {"id", 4},
                 {"method", "tools/call"},
                 {"params", {{"name", "ticket_desk__echo"}, {"arguments", {{"text", "x"}}}}}},
                *caller,
                "test"));
            ASSERT_TRUE(refused && *refused);
            EXPECT_TRUE(json::parse(**refused)["result"]["isError"].get<bool>())
                << (caller == &anonymous ? "anonymous" : "another space");
        }
    }

    // A refused token marks the connection as needing sign-in.
    server.required_bearer = "rotated-elsewhere";
    const auto expired =
        t.call(first,
               "read_mcp_tool",
               in(0, {{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "a"}}}}));
    EXPECT_EQ(expired.error.code, "sign_in_required");
    EXPECT_EQ(t.call(first, "get_mcp_connection", in(0, {{"id", id}})).value["status"],
              "needs_sign_in");

    // Deleted, with its secret left in its mount.
    ASSERT_TRUE(t.call(first, "delete_mcp_connection", in(0, {{"id", id}})).ok);
    EXPECT_EQ(t.call(first, "get_mcp_connection", in(0, {{"id", id}})).error.code, "not_found");
    EXPECT_TRUE(t.call(first, "read_secret", {{"mount_id", mount_id}, {"name", "ticket-desk"}}).ok);
}

// OAuth 2.1 (lake#652): discovery, dynamic client registration, PKCE S256, the refresh token as
// a workspace secret written as the person, a state taken once, and refresh with rotation.
TEST_P(WorkspaceRecordStores, McpOAuthRejectsDeepMetadataRegistrationAndTokens) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    class DeepOAuthHttp : public lake::api::McpHttp {
    public:
        std::string path;
        std::shared_ptr<lake::api::McpHttp> transport = lake::api::make_mcp_http(true);
        lake::api::McpHttpResponse exchange(const lake::api::McpHttpRequest& request,
                                            const lake::api::McpEnough& enough) override {
            auto forwarded = request;
            if (path == "/.well-known/openid-configuration") {
                if (request.url.ends_with("/.well-known/oauth-authorization-server")) {
                    lake::api::McpHttpResponse missing;
                    missing.status = 404;
                    return missing;
                }
                if (request.url.ends_with(path)) {
                    forwarded.url = request.url.substr(0, request.url.size() - path.size()) +
                                    "/.well-known/oauth-authorization-server";
                }
            }
            auto response = transport->exchange(forwarded, enough);
            if (!path.empty() && request.url.ends_with(path) && !response.body.empty() &&
                response.body.back() == '}') {
                response.body.pop_back();
                response.body +=
                    ",\"ignored\":" + std::string(128, '[') + "0" + std::string(128, ']') + "}";
            }
            return response;
        }
        std::unique_ptr<lake::api::McpEventStream>
        open(const lake::api::McpHttpRequest& request) override {
            return transport->open(request);
        }
    };
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.required_bearer = "not-signed-in";
    auto http = std::make_shared<DeepOAuthHttp>();
    t.mcp_http = http;
    const auto& first = t.spaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = first.workspaces[0];
        return input;
    };
    const auto mount = t.call(first, "create_secret_mount", in({{"name", "depth-oauth"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const auto created =
        t.call(first,
               "create_mcp_connection",
               in({{"name", "Depth"}, {"url", server.url()}, {"auth", {{"kind", "oauth"}}}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const auto begin = [&] {
        return t.call(first,
                      "begin_mcp_connection_sign_in",
                      in({{"id", id}, {"mount_id", mount.value.at("id")}}));
    };
    for (const std::string path : {"/.well-known/oauth-protected-resource/mcp",
                                   "/.well-known/oauth-authorization-server",
                                   "/.well-known/openid-configuration",
                                   "/register"}) {
        SCOPED_TRACE(path);
        http->path = path;
        EXPECT_FALSE(begin().ok) << "unused fields must also obey the depth limit";
    }
    http->path.clear();
    auto begun = begin();
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const auto complete = [&](const lake::api::ApiResult& pending) {
        const std::string url = pending.value.at("authorization_url");
        const auto query = lake::test::McpTestServer::form(url.substr(url.find('?') + 1));
        const auto code = server.issue_code(query.at("client_id"),
                                            query.at("redirect_uri"),
                                            query.at("code_challenge"),
                                            query.at("resource"));
        return t.call(first,
                      "complete_mcp_connection_sign_in",
                      in({{"state", pending.value.at("state")}, {"code", code}}));
    };
    http->path = "/token";
    EXPECT_FALSE(complete(begun).ok);
    http->path.clear();
    begun = begin();
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const auto done = complete(begun);
    ASSERT_TRUE(done.ok) << done.error.message;
    EXPECT_EQ(done.value.at("status"), "connected") << "a later sign-in still works";

    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in({{"id", id}, {"tools", {{"echo", {{"allowed", true}}}}}}))
                    .ok);
    server.required_bearer = "revoked";
    http->path = "/token";
    EXPECT_FALSE(
        t.call(first,
               "read_mcp_tool",
               in({{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "refresh"}}}}))
            .ok);
    EXPECT_EQ(server.token_grants().back(), "refresh_token");
}

TEST_P(WorkspaceRecordStores, McpConnectionsSignInWithOAuthAndKeepOnlyARefreshToken) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.required_bearer = "nobody-has-this-yet";
    t.mcp_http = lake::api::make_mcp_http(true);
    const auto& first = t.spaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = first.workspaces[0];
        return input;
    };
    const auto mount = t.call(first, "create_secret_mount", in({{"name", "oauth"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    const auto created =
        t.call(first,
               "create_mcp_connection",
               in({{"name", "Docs"}, {"url", server.url()}, {"auth", {{"kind", "oauth"}}}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["status"], "needs_sign_in");
    const std::string id = created.value["id"];

    const auto begun =
        t.call(first, "begin_mcp_connection_sign_in", in({{"id", id}, {"mount_id", mount_id}}));
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const std::string url = begun.value["authorization_url"];
    EXPECT_EQ(url.rfind(server.base() + "/authorize?", 0), 0U) << url;
    EXPECT_EQ(begun.value["redirect_uri"], "https://lake.test/mcp-connections/oauth/callback");
    const auto query = lake::test::McpTestServer::form(url.substr(url.find('?') + 1));
    EXPECT_EQ(query.at("code_challenge_method"), "S256");
    EXPECT_EQ(query.at("resource"), server.base() + "/mcp");
    EXPECT_EQ(query.at("state"), begun.value["state"].get<std::string>());
    ASSERT_EQ(server.clients().size(), 1U) << "registered itself";
    EXPECT_EQ(server.clients()[0].redirect_uris,
              std::vector<std::string>{"https://lake.test/mcp-connections/oauth/callback"});
    EXPECT_EQ(query.at("client_id"), server.clients()[0].id);

    // The person consents at the server, which sends the code back.
    const std::string code = server.issue_code(query.at("client_id"),
                                               query.at("redirect_uri"),
                                               query.at("code_challenge"),
                                               query.at("resource"));
    EXPECT_EQ(t.call(t.spaces[1],
                     "complete_mcp_connection_sign_in",
                     in({{"state", begun.value["state"]}, {"code", code}}))
                  .error.code,
              "unauthorized");
    const auto done = t.call(first,
                             "complete_mcp_connection_sign_in",
                             in({{"state", begun.value["state"]}, {"code", code}}));
    ASSERT_TRUE(done.ok) << done.error.message;
    EXPECT_EQ(done.value["status"], "connected");
    EXPECT_EQ(done.value["tool_count"], 4);
    EXPECT_EQ(done.value["auth"]["secret"]["field"], "refresh_token");
    EXPECT_EQ(t.call(first,
                     "complete_mcp_connection_sign_in",
                     in({{"state", begun.value["state"]}, {"code", code}}))
                  .error.code,
              "not_found")
        << "a state is taken once";
    const std::string first_refresh = server.refresh_token();
    auto kept = t.call(first, "read_secret", {{"mount_id", mount_id}, {"name", "mcp-docs"}});
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_EQ(kept.value["value"]["refresh_token"], first_refresh);
    EXPECT_FALSE(kept.value["value"].contains("access_token")) << "only the refresh token is kept";
    EXPECT_EQ(done.value.dump().find(first_refresh), std::string::npos) << "never answered";

    // The access token the server stops taking is renewed with the refresh token, which rotates.
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in({{"id", id}, {"tools", {{"echo", {{"allowed", true}}}}}}))
                    .ok);
    server.required_bearer = "revoked";
    const auto called =
        t.call(first,
               "read_mcp_tool",
               in({{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "after"}}}}));
    ASSERT_TRUE(called.ok) << called.error.message;
    EXPECT_EQ(server.token_grants(),
              (std::vector<std::string>{"authorization_code", "refresh_token"}));
    kept = t.call(first, "read_secret", {{"mount_id", mount_id}, {"name", "mcp-docs"}});
    EXPECT_EQ(kept.value["value"]["refresh_token"], server.refresh_token());
    EXPECT_NE(server.refresh_token(), first_refresh) << "the rotated token is the one kept";
}

// A sign-in's token reaches only the server it was asked for (lake#927): resource metadata naming
// another resource, or authorization server metadata naming another issuer, stops the sign-in
// before Lake registers or redirects anyone; a connection moved to another server between
// beginning and completing a sign-in, or after it, sends that server neither the code's token nor
// the kept one.
TEST_P(WorkspaceRecordStores, McpConnectionSignInIsBoundToTheServerItIsFor) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.required_bearer = "nobody-has-this-yet";
    lake::test::McpTestServer other; // a server the token is not for
    other.required_bearer = "nobody-has-this-either";
    t.mcp_http = lake::api::make_mcp_http(true);
    const auto& first = t.spaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = first.workspaces[0];
        return input;
    };
    const auto mount = t.call(first, "create_secret_mount", in({{"name", "oauth"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    const auto created =
        t.call(first,
               "create_mcp_connection",
               in({{"name", "Docs"}, {"url", server.url()}, {"auth", {{"kind", "oauth"}}}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value["id"];
    const auto begin = [&] {
        return t.call(
            first, "begin_mcp_connection_sign_in", in({{"id", id}, {"mount_id", mount_id}}));
    };
    const auto complete = [&](const json& state, const std::string& code) {
        return t.call(
            first, "complete_mcp_connection_sign_in", in({{"state", state}, {"code", code}}));
    };
    const auto query_of = [](const json& begun) {
        const std::string url = begun["authorization_url"];
        return lake::test::McpTestServer::form(url.substr(url.find('?') + 1));
    };
    const auto move_to = [&](const std::string& url) {
        const auto moved = t.call(first, "update_mcp_connection", in({{"id", id}, {"url", url}}));
        ASSERT_TRUE(moved.ok) << moved.error.message;
    };
    const auto other_saw_a_bearer = [&] {
        return std::ranges::any_of(other.seen(), [](const auto& request) {
            return request.headers.contains("authorization");
        });
    };

    // Metadata naming another API, another port of this host, another path or no resource at all.
    for (const std::string& named : {std::string("https://api.example.com/"),
                                     other.base() + "/mcp",
                                     server.base() + "/admin",
                                     server.base() + "/mcp/admin",
                                     server.base() + "/MCP",
                                     server.base() + "/mcp?audience=admin",
                                     std::string()}) {
        server.metadata_resource = named;
        const auto refused = begin();
        EXPECT_FALSE(refused.ok) << named;
        EXPECT_NE(refused.error.message.find("resource"), std::string::npos)
            << named << ": " << refused.error.message;
    }
    // An issuer whose metadata names another issuer, or none.
    server.metadata_resource.reset();
    for (const std::string& issuer : {std::string("https://issuer.example.com"),
                                      other.base(),
                                      server.base() + "/tenant",
                                      std::string()}) {
        server.metadata_issuer = issuer;
        const auto refused = begin();
        EXPECT_FALSE(refused.ok) << issuer;
        EXPECT_NE(refused.error.message.find("issuer"), std::string::npos)
            << issuer << ": " << refused.error.message;
    }
    server.metadata_issuer.reset();
    EXPECT_TRUE(server.clients().empty()) << "nothing registered for a refused discovery";

    // The server's own URL, with or without a trailing slash, or its origin, is its resource.
    for (const std::string& named : {server.base() + "/mcp/", server.base() + "/"}) {
        server.metadata_resource = named;
        const auto begun = begin();
        ASSERT_TRUE(begun.ok) << named << ": " << begun.error.message;
        EXPECT_EQ(query_of(begun.value).at("resource"), named);
    }
    server.metadata_resource.reset();

    // The connection moves to another server between the sign-in's start and its end: the code is
    // never exchanged, and the other server is never sent a token.
    auto begun = begin();
    ASSERT_TRUE(begun.ok) << begun.error.message;
    auto query = query_of(begun.value);
    auto code = server.issue_code(query.at("client_id"),
                                  query.at("redirect_uri"),
                                  query.at("code_challenge"),
                                  query.at("resource"));
    move_to(other.url());
    EXPECT_FALSE(complete(begun.value["state"], code).ok);
    EXPECT_TRUE(server.token_grants().empty()) << "the code was exchanged for another server";
    EXPECT_FALSE(other_saw_a_bearer());

    // Signed in at its own server, then moved: the kept token is not sent to the new address.
    move_to(server.url());
    begun = begin();
    ASSERT_TRUE(begun.ok) << begun.error.message;
    query = query_of(begun.value);
    code = server.issue_code(query.at("client_id"),
                             query.at("redirect_uri"),
                             query.at("code_challenge"),
                             query.at("resource"));
    const auto done = complete(begun.value["state"], code);
    ASSERT_TRUE(done.ok) << done.error.message;
    EXPECT_EQ(done.value["status"], "connected");
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in({{"id", id}, {"tools", {{"echo", {{"allowed", true}}}}}}))
                    .ok);
    move_to(other.url());
    const auto moved_call =
        t.call(first,
               "read_mcp_tool",
               in({{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "hi"}}}}));
    EXPECT_FALSE(moved_call.ok);
    EXPECT_EQ(moved_call.error.code, "sign_in_required");
    EXPECT_FALSE(other_saw_a_bearer()) << "the token signed in for one server went to another";
    // Back at its own server, the move has already dropped the sign-in (lake#928).
    move_to(server.url());
    const auto back =
        t.call(first,
               "read_mcp_tool",
               in({{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "hi"}}}}));
    EXPECT_EQ(back.error.code, "sign_in_required");
}

// A connection moved to another address keeps no credential it had (lake#928): a writer who moves
// it to a server they run gets no bearer, refresh token, cached access token or pending sign-in's
// code sent there, and moving it back restores none of them. A bearer connection moves only when
// the same update names its secret again, as someone who may read it.
TEST_P(WorkspaceRecordStores, McpConnectionMovedToAnotherAddressKeepsNoCredential) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.required_bearer = "the-plaintext-bearer-token";
    lake::test::McpTestServer other;   // a server the writer runs
    lake::test::McpTestServer granted; // a server the secret's reader chose
    t.mcp_http = lake::api::make_mcp_http(true);
    const auto& first = t.spaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = first.workspaces[0];
        return input;
    };
    const auto mount = t.call(first, "create_secret_mount", in({{"name", "mcp-tokens"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    ASSERT_TRUE(t.call(first,
                       "put_secret",
                       {{"mount_id", mount_id},
                        {"name", "desk"},
                        {"value", {{"token", "the-plaintext-bearer-token"}}}})
                    .ok);
    const json secret{{"mount_id", mount_id}, {"name", "desk"}, {"field", "token"}};
    const auto other_saw_a_credential = [&] {
        return std::ranges::any_of(other.seen(), [](const auto& request) {
            return request.headers.contains("authorization") ||
                   request.body.find("grant_type=") != std::string::npos ||
                   request.body.find("code_verifier=") != std::string::npos;
        });
    };
    const auto echo = [&](const std::string& id) {
        return t.call(
            first,
            "read_mcp_tool",
            in({{"connection_id", id}, {"tool", "echo"}, {"arguments", {{"text", "hi"}}}}));
    };
    const auto update = [&](json change) {
        return t.call(first, "update_mcp_connection", in(change));
    };

    // A bearer connection, working at its own server.
    const auto created = t.call(first,
                                "create_mcp_connection",
                                in({{"name", "Desk"},
                                    {"url", server.url()},
                                    {"auth", {{"kind", "bearer"}, {"secret", secret}}}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string bearer_id = created.value["id"];
    ASSERT_TRUE(update({{"id", bearer_id}, {"tools", {{"echo", {{"allowed", true}}}}}}).ok);
    ASSERT_TRUE(echo(bearer_id).ok);

    // Moved without naming the secret: refused before anything changes or reaches the new address.
    const auto moved = update({{"id", bearer_id}, {"url", other.url()}});
    EXPECT_FALSE(moved.ok);
    EXPECT_EQ(moved.error.path, "/auth") << moved.error.message;
    EXPECT_EQ(t.call(first, "get_mcp_connection", in({{"id", bearer_id}})).value["url"],
              server.url());
    EXPECT_TRUE(echo(bearer_id).ok);
    EXPECT_TRUE(other.seen().empty()) << "the move reached the new address";
    // The same address again is not a move.
    EXPECT_TRUE(update({{"id", bearer_id}, {"name", "Desk 2"}, {"url", server.url()}}).ok);
    EXPECT_TRUE(echo(bearer_id).ok);
    // Moved while naming the secret again, by someone who may read it, carries it over.
    const auto carried = update({{"id", bearer_id},
                                 {"url", granted.url()},
                                 {"auth", {{"kind", "bearer"}, {"secret", secret}}}});
    ASSERT_TRUE(carried.ok) << carried.error.message;
    EXPECT_EQ(carried.value["auth"]["secret"], secret);
    EXPECT_TRUE(std::ranges::any_of(granted.seen(), [](const auto& request) {
        return request.headers.contains("authorization");
    }));

    // An OAuth connection signed in at its own server, with a pending sign-in of someone else.
    server.required_bearer = "nobody-has-this-yet";
    const auto oauth = t.call(first,
                              "create_mcp_connection",
                              in({{"name", "Docs"},
                                  {"url", server.url()},
                                  {"auth", {{"kind", "oauth"}}}}));
    ASSERT_TRUE(oauth.ok) << oauth.error.message;
    const std::string id = oauth.value["id"];
    const auto begin = [&] {
        return t.call(
            first, "begin_mcp_connection_sign_in", in({{"id", id}, {"mount_id", mount_id}}));
    };
    const auto code_for = [&](const json& begun) {
        const std::string url = begun["authorization_url"];
        const auto query = lake::test::McpTestServer::form(url.substr(url.find('?') + 1));
        return server.issue_code(query.at("client_id"),
                                 query.at("redirect_uri"),
                                 query.at("code_challenge"),
                                 query.at("resource"));
    };
    const auto complete = [&](const json& begun, const std::string& code) {
        return t.call(first,
                      "complete_mcp_connection_sign_in",
                      in({{"state", begun["state"]}, {"code", code}}));
    };
    auto begun = begin();
    ASSERT_TRUE(begun.ok) << begun.error.message;
    ASSERT_TRUE(complete(begun.value, code_for(begun.value)).ok);
    ASSERT_TRUE(update({{"id", id}, {"tools", {{"echo", {{"allowed", true}}}}}}).ok);
    ASSERT_TRUE(echo(id).ok);
    const auto pending = begin();
    ASSERT_TRUE(pending.ok) << pending.error.message;
    const std::string pending_code = code_for(pending.value);
    const std::size_t grants = server.token_grants().size();

    // Moved to the writer's server, which they then begin a sign-in at, so its metadata names
    // their token endpoint: nothing the first sign-in or the pending one holds goes there.
    const auto moved_oauth = update({{"id", id}, {"url", other.url()}});
    ASSERT_TRUE(moved_oauth.ok) << moved_oauth.error.message;
    EXPECT_EQ(moved_oauth.value["status"], "needs_sign_in");
    EXPECT_TRUE(moved_oauth.value["auth"]["secret"].is_null()) << moved_oauth.value.dump();
    ASSERT_TRUE(begin().ok);
    EXPECT_EQ(echo(id).error.code, "sign_in_required");
    EXPECT_FALSE(complete(pending.value, pending_code).ok);
    EXPECT_FALSE(other_saw_a_credential()) << "a token or code went to the new address";
    // Moved back: the first sign-in and the pending one are both gone.
    ASSERT_TRUE(update({{"id", id}, {"url", server.url()}}).ok);
    EXPECT_EQ(echo(id).error.code, "sign_in_required");
    EXPECT_FALSE(complete(pending.value, pending_code).ok);
    EXPECT_EQ(server.token_grants().size(), grants) << "nothing exchanged after the move";
}

// A connection's streams (lake#652): the store root's owner mirrors the server's notifications to a
// workspace topic, starts a job of the connection's agent for each new input event as the person
// who set the streams, and publishes each finished job's result as agent-events carries it.
TEST_P(WorkspaceRecordStores, McpConnectionStreamsMirrorNotificationsAndDriveTheAgent) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    server.get_stream_events = 2;
    t.mcp_http = lake::api::make_mcp_http(true);
    const auto& first = t.spaces[0];
    const std::string home = first.workspaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = home;
        return input;
    };
    const auto created =
        t.call(first, "create_mcp_connection", in({{"name", "Inbox"}, {"url", server.url()}}));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value["id"];
    t.zetta->answers["get_agent"] = {
        {"id", "agent-9"},
        {"name", "Inbox Agent"},
        {"runtime", "python"},
        {"skills", json::array({"mcp:inbox"})},
        {"definition", {{"source", "zetta"}, {"instructions", "x"}, {"files", json::array()}}}};
    const auto streams = t.call(first,
                                "update_mcp_connection",
                                in({{"id", id},
                                    {"agent_id", "agent-9"},
                                    {"input_topic", "inbox-in"},
                                    {"output_topic", "inbox-out"},
                                    {"notifications_topic", "inbox-notices"},
                                    {"subscribe_resources", json::array({"memo://inbox"})}}));
    ASSERT_TRUE(streams.ok) << streams.error.message;
    EXPECT_EQ(streams.value["streams"]["input_topic"], "inbox-in");

    // One event waits on the input stream.
    {
        auto context = t.context(first);
        lake::api::WorkspaceApiBus bus(context, first.customer_id, home);
        ASSERT_TRUE(liquid::runtime::block_on(bus.create_topic("inbox-in")));
        ASSERT_TRUE(liquid::runtime::block_on(bus.publish("inbox-in", {{"ticket", "T-5"}}, "e-1")));
    }
    lake::api::McpStreamWorker worker;
    lake::api::ApiContext owner;
    owner.store = t.store.get();
    owner.workspaces = t.service.get();
    owner.instance_kek = std::string(kek);
    owner.registry = &t.registry;
    owner.mcp_http = t.mcp_http;
    owner.mcp_allow_private_addresses = true;
    std::size_t mirrored = 0;
    std::size_t turns = 0;
    for (int pass = 0; pass < 10 && (mirrored < 2 || turns < 1); ++pass) {
        const auto done = worker.pass(owner);
        mirrored += done.notifications;
        turns += done.turns;
        for (const auto& problem : done.problems) {
            ADD_FAILURE() << problem;
        }
    }
    EXPECT_EQ(server.subscribed().count("memo://inbox"), 1U);
    EXPECT_EQ(mirrored, 2U);
    auto& topics = t.zetta->topics;
    ASSERT_TRUE(topics.contains("inbox-notices"));
    EXPECT_EQ(topics["inbox-notices"].events.size(), 2U);
    EXPECT_NE(topics["inbox-notices"].events[0].dump().find("notifications/resources/updated"),
              std::string::npos);
    EXPECT_EQ(turns, 1U) << "one job for the one event";
    const auto again = worker.pass(owner);
    EXPECT_EQ(again.turns, 0U) << "an event starts one turn only";

    // The job, as the person; its result goes to the output stream once it finishes.
    const auto launches = t.store->list_agent_launches(first.customer_id, "agent-9", 10, home);
    ASSERT_TRUE(launches);
    ASSERT_EQ(launches->size(), 1U);
    EXPECT_EQ((*launches)[0].created_by_user_id, first.principal.user_id);
    EXPECT_EQ((*launches)[0].input["event"]["ticket"], "T-5");
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(t.store.get());
    if (memory == nullptr) {
        return; // the operator's status is written directly only in the memory store
    }
    ASSERT_TRUE(memory->set_agent_launch_status((*launches)[0].id, "Succeeded", "triaged", "", ""));
    const auto finished = worker.pass(owner);
    EXPECT_EQ(finished.results, 1U);
    ASSERT_TRUE(topics.contains("inbox-out"));
    ASSERT_EQ(topics["inbox-out"].events.size(), 1U);
    EXPECT_NE(topics["inbox-out"].events[0].dump().find("task_id"), std::string::npos);
    EXPECT_NE(topics["inbox-out"].events[0].dump().find("triaged"), std::string::npos);
    EXPECT_EQ(worker.pass(owner).results, 0U) << "published once";
}

// lake#677: an agent started by a workspace stream. The streams are sealed records in the
// workspace; each message that arrives after one is set starts one job of the agent with the
// message as its input, as the person who set it; deleting the agent removes them, and a trigger
// whose agent is gone is removed by the worker.
TEST_P(WorkspaceRecordStores, AStreamStartsItsAgentOncePerMessageAsThePersonWhoSetIt) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string home = first.workspaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = home;
        return input;
    };
    json agent{{"id", "agent-7"},
               {"name", "Order Watcher"},
               {"runtime", "python"},
               {"skills", json::array()},
               {"definition", {{"source", "zetta"}, {"instructions", "x"}, {"files", json::array()}}}};
    bool agent_deleted = false;
    t.zetta->responder = [&](const std::string& capability,
                             const json&) -> std::optional<lake::control::ZettaControlResult<json>> {
        if (capability == "get_agent") {
            if (agent_deleted) {
                return lake::control::ZettaControlResult<json>(std::unexpected(
                    lake::control::ZettaControlError{"not_found", "no such agent"}));
            }
            return agent;
        }
        if (capability == "delete_agent") {
            agent_deleted = true;
            return json{{"deleted", true}};
        }
        return std::nullopt;
    };
    // A message already on the stream when it is set starts nothing.
    {
        auto context = t.context(first);
        lake::api::WorkspaceApiBus bus(context, first.customer_id, home);
        ASSERT_TRUE(liquid::runtime::block_on(bus.create_topic("orders")));
        ASSERT_TRUE(liquid::runtime::block_on(bus.publish("orders", {{"order", "O-0"}}, "o-0")));
    }

    // Refusals: a system stream, a stream the workspace does not have, a native agent.
    const auto system = t.call(first,
                               "set_agent_streams",
                               in({{"agent_id", "agent-7"}, {"streams", {"agent-events"}}}));
    ASSERT_FALSE(system.ok);
    EXPECT_EQ(system.error.code, "invalid_input");
    EXPECT_EQ(system.error.path, "/streams/0");
    const auto missing = t.call(first,
                                "set_agent_streams",
                                in({{"agent_id", "agent-7"}, {"streams", {"orders", "nowhere"}}}));
    ASSERT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_EQ(missing.error.path, "/streams/1");
    agent["runtime"] = "native";
    const auto native = t.call(
        first, "set_agent_streams", in({{"agent_id", "agent-7"}, {"streams", {"orders"}}}));
    ASSERT_FALSE(native.ok);
    EXPECT_EQ(native.error.code, "invalid_input");
    agent["runtime"] = "python";
    const auto none =
        t.call(first, "list_agent_streams", in({{"agent_id", "agent-7"}}));
    ASSERT_TRUE(none.ok) << none.error.message;
    EXPECT_TRUE(none.value["streams"].empty()) << "a refused call stores nothing";

    const auto set = t.call(
        first, "set_agent_streams", in({{"agent_id", "agent-7"}, {"streams", {"topic:orders"}}}));
    ASSERT_TRUE(set.ok) << set.error.message;
    ASSERT_EQ(set.value["streams"].size(), 1U);
    EXPECT_EQ(set.value["streams"][0]["stream"], "orders");
    EXPECT_EQ(set.value["streams"][0]["cursor"], 1) << "it starts after the message already there";
    EXPECT_EQ(set.value["streams"][0]["set_by"], first.principal.user_id);
    const std::string trigger_id = set.value["streams"][0]["id"];
    // Setting the same set again keeps the trigger (and whose it is).
    const auto again = t.call(
        first, "set_agent_streams", in({{"agent_id", "agent-7"}, {"streams", {"orders"}}}));
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["streams"][0]["id"], trigger_id);
    // Sealed in the workspace: the stream's name is not on the record at rest.
    bool stored = false;
    for (auto& [url, tables] : t.zetta->records) {
        if (const auto found = tables.collections.find("lake_agent_stream_triggers");
            found != tables.collections.end()) {
            for (const auto& [key, row] : found->second.records) {
                stored = true;
                EXPECT_EQ(row.dump().find("orders"), std::string::npos);
            }
        }
    }
    EXPECT_TRUE(stored) << "the trigger is a record in the workspace Zetta";
    // Another account's person cannot read them.
    auto foreign = in({{"agent_id", "agent-7"}});
    EXPECT_FALSE(t.call(second, "list_agent_streams", foreign).ok);

    // Two new messages: two jobs, each with its message, as the person who set the stream.
    {
        auto context = t.context(first);
        lake::api::WorkspaceApiBus bus(context, first.customer_id, home);
        ASSERT_TRUE(liquid::runtime::block_on(bus.publish("orders", {{"order", "O-1"}}, "o-1")));
        ASSERT_TRUE(liquid::runtime::block_on(bus.publish("orders", {{"order", "O-2"}}, "o-2")));
    }
    lake::api::AgentStreamWorker worker;
    lake::api::ApiContext owner;
    owner.store = t.store.get();
    owner.workspaces = t.service.get();
    owner.instance_kek = std::string(kek);
    owner.registry = &t.registry;
    const auto done = worker.pass(owner);
    for (const auto& problem : done.problems) {
        ADD_FAILURE() << problem;
    }
    EXPECT_EQ(done.runs, 2U);
    EXPECT_EQ(worker.pass(owner).runs, 0U) << "a message starts one run only";
    const auto launches = t.store->list_agent_launches(first.customer_id, "agent-7", 10, home);
    ASSERT_TRUE(launches);
    ASSERT_EQ(launches->size(), 2U);
    std::set<std::string> orders;
    for (const auto& launch : *launches) {
        EXPECT_EQ(launch.created_by_user_id, first.principal.user_id);
        EXPECT_EQ(launch.mode, "job");
        EXPECT_EQ(launch.trigger, "stream");
        EXPECT_EQ(launch.input["stream"], "orders");
        EXPECT_EQ(launch.input["trigger_id"], trigger_id);
        orders.insert(launch.input["message"]["order"].get<std::string>());
    }
    EXPECT_EQ(orders, (std::set<std::string>{"O-1", "O-2"}));
    const auto listed = t.call(first, "list_agent_streams", in({{"agent_id", "agent-7"}}));
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value["streams"][0]["cursor"], 3) << "its place on the stream is kept";

    // Deleting the agent removes its streams.
    const auto deleted = t.call(first, "delete_agent", in({{"id", "agent-7"}}));
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["streams_removed"], 1);
    EXPECT_TRUE(t.call(first, "list_agent_streams", in(json::object())).value["streams"].empty());

    // A trigger whose agent was deleted elsewhere is removed at its next message.
    agent_deleted = false;
    ASSERT_TRUE(t.call(first,
                       "set_agent_streams",
                       in({{"agent_id", "agent-7"}, {"streams", {"orders"}}}))
                    .ok);
    agent_deleted = true;
    {
        auto context = t.context(first);
        lake::api::WorkspaceApiBus bus(context, first.customer_id, home);
        ASSERT_TRUE(liquid::runtime::block_on(bus.publish("orders", {{"order", "O-3"}}, "o-3")));
    }
    const auto orphaned = worker.pass(owner);
    EXPECT_EQ(orphaned.runs, 0U);
    EXPECT_EQ(orphaned.removed, 1U);
    EXPECT_TRUE(t.call(first, "list_agent_streams", in(json::object())).value["streams"].empty());
}

// lake#524: deleting an agent withdraws its present launches (with a stop_reason that names the
// deletion) and its cron schedules, so the operator removes their pods instead of restarting them
// against an agent that is gone. Another agent's, another workspace's and a finished launch are
// left as they are. A launch whose agent was deleted outside lake is withdrawn the first time it
// reads its agent and the workspace answers not_found.
// LSR-086: disabling a person ends their delegated authority, including a context already
// reconstructed by a stream worker. Another person's automation keeps running.
void WorkspaceRecordStores::check_delegation_offboarding(bool suspended) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    auto linked = person("offboard");
    linked.email = store->get_user(first.principal.user_id)->email;
    ASSERT_TRUE(store->sign_in_clerk(linked));
    const lake::control::NewAgentLaunch input{.customer_id = first.customer_id,
                                              .agent_id = "agent-7",
                                              .mode = "service",
                                              .scopes = lake::control::agent_launch_scopes,
                                              .created_by_user_id = first.principal.user_id,
                                              .created_by_taap_id = first.principal.taap_id,
                                              .workspace_id = first.workspaces[0]};
    const lake::control::NewAgentCronSchedule cron{.customer_id = first.customer_id,
                                                   .agent_id = "agent-7",
                                                   .cron = "0 9 * * 1",
                                                   .scopes = input.scopes,
                                                   .created_by_user_id = input.created_by_user_id,
                                                   .created_by_taap_id = input.created_by_taap_id,
                                                   .workspace_id = input.workspace_id};
    const lake::control::AgentTask task_input{.customer_id = first.customer_id,
                                              .agent = "agent-ask-orama",
                                              .workspace_id = first.workspaces[0],
                                              .scopes = {"list_datasets"},
                                              .requested_by_user_id = first.principal.user_id,
                                              .requested_by_taap_id = first.principal.taap_id};
    const auto launch = store->create_agent_launch(input);
    const auto schedule = store->create_agent_cron_schedule(cron);
    const auto task = store->create_agent_task(task_input);
    const auto waiting = store->create_agent_task(task_input);
    ASSERT_TRUE(launch);
    ASSERT_TRUE(schedule);
    ASSERT_TRUE(task);
    ASSERT_TRUE(waiting);
    auto other_input = input;
    other_input.created_by_user_id = second.principal.user_id;
    other_input.created_by_taap_id = second.principal.taap_id;
    const auto bystander = store->create_agent_launch(other_input);
    ASSERT_TRUE(bystander);
    const std::string launch_key = "lal." + launch->id + ".0123456789abcdef0123456789abcdef";
    const std::string schedule_key = "las." + schedule->id + ".0123456789abcdef0123456789abcdef";
    const std::string task_key = "lat." + task->id + ".0123456789abcdef0123456789abcdef";
    const auto digest = lake::control::identity_crypto::sha256_hex;
    if (auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store.get())) {
        ASSERT_TRUE(
            memory->set_agent_launch_status(launch->id, "Running", "", "", digest(launch_key)));
        ASSERT_TRUE(memory->set_agent_cron_schedule_status(
            schedule->id, "Ready", "", digest(schedule_key)));
    } else {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_launch_status (launch_id, phase, credential_sha256) "
                       "VALUES ($1::uuid, 'Running', $2)",
                       launch->id,
                       digest(launch_key));
        tx.exec_params(
            "INSERT INTO agent_cron_schedule_status (schedule_id, phase, credential_sha256) "
            "VALUES ($1::uuid, 'Ready', $2)",
            schedule->id,
            digest(schedule_key));
        tx.commit();
    }
    ASSERT_TRUE(store->claim_agent_task(task->id, task_input.agent, digest(task_key)));
    ASSERT_TRUE(store->renew_agent_task_lease(task->id, digest(task_key)));
    for (const auto& key : {launch_key, schedule_key, task_key}) {
        ASSERT_TRUE(store->context_for(key));
    }
    // Already-disabled rows from before this fix must fail even without workload withdrawal.
    if (!database_uri_.empty()) {
        const auto set_disabled = [&](bool disabled) {
            pqxx::connection connection(database_uri_);
            pqxx::work tx(connection);
            tx.exec_params("UPDATE users SET disabled_at = CASE WHEN $2 THEN now() ELSE NULL END "
                           "WHERE id = $1::uuid",
                           first.principal.user_id,
                           disabled);
            tx.commit();
        };
        set_disabled(true);
        for (const auto& key : {launch_key, schedule_key, task_key}) {
            EXPECT_FALSE(store->context_for(key));
        }
        EXPECT_FALSE(store->renew_agent_task_lease(task->id, digest(task_key)));
        EXPECT_FALSE(store->claim_agent_task(waiting->id, task_input.agent, "late-claim"));
        EXPECT_FALSE(store->create_agent_launch(input));
        EXPECT_FALSE(store->create_agent_cron_schedule(cron));
        EXPECT_FALSE(store->create_agent_task(task_input));
        set_disabled(false);
    }
    if (suspended) {
        ASSERT_TRUE(store->apply_clerk_user_state({linked.subject, 2000, true, false}));
        EXPECT_FALSE(store->get_user(first.principal.user_id)->may_act());
    } else {
        ASSERT_TRUE(store->disable_clerk_user(linked.subject));
    }
    for (const auto& key : {launch_key, schedule_key, task_key}) {
        const auto principal = store->context_for(key);
        EXPECT_FALSE(principal) << "disabled user retained delegated access";
        if (!principal) {
            EXPECT_TRUE(principal.error().code == "not_found" ||
                        principal.error().code == "unauthorized");
        }
    }
    EXPECT_EQ(store->get_agent_launch(launch->id)->desired, "absent");
    EXPECT_FALSE(store->get_agent_launch(launch->id)->stop_reason.empty());
    EXPECT_EQ(store->get_agent_cron_schedule(schedule->id)->desired, "absent");
    EXPECT_EQ(store->get_agent_task(task->id)->desired, "absent");
    EXPECT_EQ(store->get_agent_task(task->id)->phase, "Failed");
    EXPECT_EQ(store->get_agent_task(waiting->id)->phase, "Failed");
    EXPECT_TRUE(store->get_agent_task(task->id)->credential_sha256.empty());
    EXPECT_FALSE(store->renew_agent_task_lease(task->id, digest(task_key))) << "cached context";
    EXPECT_FALSE(store->claim_agent_task(waiting->id, task_input.agent, "late-claim"));
    EXPECT_FALSE(store->create_agent_launch(input))
        << "stream context reconstructed before disable";
    EXPECT_FALSE(store->create_agent_cron_schedule(cron));
    EXPECT_FALSE(store->create_agent_task(task_input));
    EXPECT_EQ(store->get_agent_launch(bystander->id)->desired, "present");
    const auto generation = store->get_agent_launch(launch->id)->generation;
    if (suspended) {
        ASSERT_TRUE(store->apply_clerk_user_state({linked.subject, 2000, true, false}));
        EXPECT_FALSE(store->get_user(first.principal.user_id)->may_act());
    } else {
        ASSERT_TRUE(store->disable_clerk_user(linked.subject));
    }
    EXPECT_EQ(store->get_agent_launch(launch->id)->generation, generation) << "idempotent disable";
    if (suspended) {
        ASSERT_TRUE(store->apply_clerk_user_state({linked.subject, 3000, false, false}));
        EXPECT_TRUE(store->get_user(first.principal.user_id)->may_act());
        for (const auto& key : {launch_key, schedule_key, task_key})
            EXPECT_FALSE(store->context_for(key)) << "unban must not restore old delegation";
        EXPECT_TRUE(store->create_agent_launch(input));
    }
}

void WorkspaceRecordStores::check_stream_offboarding(bool suspended) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const std::string home = first.workspaces[0];
    auto linked = person("stream-offboard");
    linked.email = store->get_user(first.principal.user_id)->email;
    ASSERT_TRUE(store->sign_in_clerk(linked));
    auto context = t.context(first);
    auto records = lake::api::workspace_records(context, first.customer_id, home);
    ASSERT_TRUE(records);
    lake::api::WorkspaceApiBus bus(context, first.customer_id, home);
    ASSERT_TRUE(run(bus.create_topic("offboard-in")));
    t.zetta->answers["get_agent"] = {{"id", "agent-7"}, {"runtime", "python"}};
    const auto trigger = run(
        lake::control::record_agent_stream_trigger(*records,
                                                   {.customer_id = first.customer_id,
                                                    .agent_id = "agent-7",
                                                    .topic = "offboard-in",
                                                    .set_by = first.principal.user_id,
                                                    .set_by_identity = first.principal.taap_id}));
    ASSERT_TRUE(trigger);
    const auto connection =
        run(lake::control::record_mcp_connection(*records,
                                                 {.customer_id = first.customer_id,
                                                  .name = "Offboarding",
                                                  .prefix = "offboarding",
                                                  .url = "https://mcp.example.test",
                                                  .agent_id = "agent-7",
                                                  .input_topic = "offboard-in",
                                                  .output_topic = "offboard-out",
                                                  .notifications_topic = "offboard-notices",
                                                  .streams_set_by = first.principal.user_id}));
    ASSERT_TRUE(connection);
    const auto dispatch_error =
        lake::api::record_mcp_connection_dispatch(context, home, *connection);
    ASSERT_FALSE(dispatch_error) << dispatch_error.value_or("");
    lake::api::ApiContext owner;
    owner.store = store.get();
    owner.workspaces = t.service.get();
    owner.registry = &t.registry;
    owner.instance_kek = std::string(kek);
    auto cached = lake::api::stream_person(owner, first.principal.user_id, "agent_stream_trigger");
    ASSERT_TRUE(cached);
    ASSERT_TRUE(run(bus.publish("offboard-in", {{"ticket", "before"}}, "before")));
    lake::api::AgentStreamWorker agent_worker;
    lake::api::McpStreamWorker mcp_worker;
    EXPECT_EQ(agent_worker.pass(owner).runs, 1U);
    EXPECT_EQ(mcp_worker.pass(owner).turns, 1U);
    const auto before_disable =
        run(lake::control::list_mcp_connections(*records, first.customer_id));
    ASSERT_TRUE(before_disable);
    ASSERT_EQ(before_disable->size(), 1U);
    ASSERT_EQ(before_disable->front().pending_launches.size(), 1U);
    const auto pending = before_disable->front().pending_launches.front();
    if (auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store.get())) {
        ASSERT_TRUE(memory->set_agent_launch_status(pending, "Succeeded", "done", "", ""));
    } else {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO agent_launch_status (launch_id, phase) VALUES ($1::uuid, 'Succeeded')",
            pending);
        tx.commit();
    }
    if (suspended) {
        ASSERT_TRUE(store->apply_clerk_user_state({linked.subject, 2000, true, false}));
        EXPECT_FALSE(store->get_user(first.principal.user_id)->may_act());
    } else {
        ASSERT_TRUE(store->disable_clerk_user(linked.subject));
    }
    EXPECT_FALSE(lake::api::stream_person(owner, first.principal.user_id, "agent_stream_trigger"));
    EXPECT_FALSE(
        lake::api::stream_person(owner, lake::control::new_uuid_v4(), "mcp_connection_stream"));
    ASSERT_TRUE(run(bus.publish("offboard-in", {{"ticket", "after"}}, "after")));
    // The records still exist before cleanup, but even an already-reconstructed context cannot
    // launch from the new event. The records themselves durably retain pending cleanup.
    const auto gap = lake::api::start_stream_runs(
        owner,
        *cached,
        bus,
        first.customer_id,
        home,
        "agent-7",
        "offboard-in",
        1,
        20,
        [](const lake::control::BusEvent& event) { return event.payload; });
    EXPECT_TRUE(gap.launched.empty());
    const auto retained =
        run(lake::control::list_agent_stream_triggers(*records, first.customer_id));
    ASSERT_TRUE(retained);
    ASSERT_EQ(retained->size(), 1U);
    EXPECT_EQ(store->mcp_stream_dispatch(100)->size(), 1U);
    lake::api::AgentStreamWorker restarted_agent_worker;
    lake::api::McpStreamWorker restarted_mcp_worker;
    const auto stopped = restarted_agent_worker.pass(owner);
    EXPECT_EQ(stopped.runs, 0U);
    EXPECT_EQ(stopped.removed, 1U);
    const auto mcp_stopped = restarted_mcp_worker.pass(owner);
    EXPECT_EQ(mcp_stopped.turns, 0U);
    EXPECT_EQ(mcp_stopped.notifications, 0U);
    EXPECT_EQ(mcp_stopped.results, 0U);
    ASSERT_TRUE(t.zetta->topics.contains("offboard-out"));
    EXPECT_TRUE(t.zetta->topics.at("offboard-out").events.empty()) << "no late result publication";
    const auto triggers =
        run(lake::control::list_agent_stream_triggers(*records, first.customer_id));
    ASSERT_TRUE(triggers);
    EXPECT_TRUE(triggers->empty());
    const auto connections = run(lake::control::list_mcp_connections(*records, first.customer_id));
    ASSERT_TRUE(connections);
    ASSERT_EQ(connections->size(), 1U);
    EXPECT_TRUE(connections->front().streams_set_by.empty());
    EXPECT_TRUE(connections->front().input_topic.empty());
    EXPECT_TRUE(connections->front().pending_launches.empty());
    const auto dispatch = store->mcp_stream_dispatch(100);
    ASSERT_TRUE(dispatch);
    EXPECT_TRUE(dispatch->empty());
    EXPECT_EQ(restarted_agent_worker.pass(owner).removed, 0U);
    EXPECT_EQ(restarted_mcp_worker.pass(owner).connections, 0U);
}

TEST_P(WorkspaceRecordStores, DisablingAUserRevokesAgentDelegation) {
    check_delegation_offboarding(false);
}
TEST_P(WorkspaceRecordStores, SuspendedUsersLoseAgentDelegation) {
    check_delegation_offboarding(true);
}
TEST_P(WorkspaceRecordStores, DisabledUsersCannotDriveAgentOrMcpStreams) {
    check_stream_offboarding(false);
}
TEST_P(WorkspaceRecordStores, SuspendedUsersCannotDriveAgentOrMcpStreams) {
    check_stream_offboarding(true);
}

TEST_P(WorkspaceRecordStores, DeletingAnAgentStopsItsLaunchesAndSchedules) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const std::string home = first.workspaces[0];
    const std::string other = first.workspaces.size() > 1 ? first.workspaces[1] : std::string();
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = home;
        return input;
    };
    std::set<std::string> deleted;
    t.zetta->responder =
        [&](const std::string& capability,
            const json& input) -> std::optional<lake::control::ZettaControlResult<json>> {
        const std::string id = input.value("id", std::string());
        if (capability == "get_agent") {
            if (deleted.contains(id)) {
                return lake::control::ZettaControlResult<json>(std::unexpected(
                    lake::control::ZettaControlError{"not_found", "no agent '" + id + "'"}));
            }
            return json{{"id", id}, {"name", "Briefer"}, {"runtime", "python"}};
        }
        if (capability == "delete_agent") {
            deleted.insert(id);
            return json{{"deleted", true}};
        }
        return std::nullopt;
    };
    const auto launch =
        [&](const std::string& agent, const std::string& mode, const std::string& workspace) {
            const auto made =
                t.store->create_agent_launch({.customer_id = first.customer_id,
                                              .agent_id = agent,
                                              .agent_name = "Briefer",
                                              .mode = mode,
                                              .trigger = "manual",
                                              .scopes = lake::control::agent_launch_scopes,
                                              .created_by_user_id = first.principal.user_id,
                                              .created_by_taap_id = first.principal.taap_id,
                                              .workspace_id = workspace});
            EXPECT_TRUE(made) << made.error().message;
            return made ? made->id : std::string();
        };
    const std::string service = launch("agent-9", "service", home);
    const std::string job = launch("agent-9", "job", home);
    const std::string finished = launch("agent-9", "job", home);
    ASSERT_TRUE(t.store->withdraw_agent_launch(finished));
    const std::string bystander = launch("agent-8", "service", home);
    const std::string elsewhere =
        other.empty() ? std::string() : launch("agent-9", "service", other);
    const auto schedule =
        t.store->create_agent_cron_schedule({.customer_id = first.customer_id,
                                             .agent_id = "agent-9",
                                             .agent_name = "Briefer",
                                             .cron = "0 9 * * 1",
                                             .scopes = lake::control::agent_launch_scopes,
                                             .created_by_user_id = first.principal.user_id,
                                             .created_by_taap_id = first.principal.taap_id,
                                             .workspace_id = home});
    ASSERT_TRUE(schedule) << schedule.error().message;
    const auto generation_of = [&](const std::string& id) {
        return t.store->get_agent_launch(id)->generation;
    };
    const std::int64_t finished_generation = generation_of(finished);

    const auto removed = t.call(first, "delete_agent", in({{"id", "agent-9"}}));
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_EQ(removed.value["launches_stopped"], 2);
    EXPECT_EQ(removed.value["schedules_removed"], 1);
    for (const std::string& id : {service, job}) {
        const auto stopped = t.store->get_agent_launch(id);
        ASSERT_TRUE(stopped);
        EXPECT_EQ(stopped->desired, "absent") << "the operator removes its Deployment or Job";
        EXPECT_NE(stopped->stop_reason.find("agent-9 was deleted"), std::string::npos)
            << stopped->stop_reason;
    }
    EXPECT_EQ(generation_of(finished), finished_generation) << "an absent launch is left alone";
    EXPECT_EQ(t.store->get_agent_launch(bystander)->desired, "present") << "another agent's";
    if (!elsewhere.empty()) {
        EXPECT_EQ(t.store->get_agent_launch(elsewhere)->desired, "present")
            << "another workspace's agent of the same id";
    }
    EXPECT_EQ(t.store->get_agent_cron_schedule(schedule->id)->desired, "absent")
        << "the operator deletes its CronJob";
    const auto again = t.call(first, "delete_agent", in({{"id", "agent-9"}}));
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["launches_stopped"], 0) << "nothing is left to stop";

    // An agent deleted outside lake: its service launch reads it, is told not_found, and stops.
    const std::string orphan = launch("agent-7", "service", home);
    const std::string watching = launch("agent-6", "service", home);
    deleted.insert("agent-7");
    auto runtime = t.context(first);
    runtime.session_credential = false;
    runtime.credential_kind = "agent_launch";
    runtime.credential_session_id = "launch:" + orphan;
    runtime.scopes = lake::control::agent_launch_scopes;
    const auto read_as = [&](lake::api::ApiContext context, const std::string& agent) {
        const auto completed = liquid::runtime::block_on(
            t.registry.call_async("get_agent", in({{"id", agent}}), context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    // Reading another agent that is gone changes nothing: only the launch's own agent counts.
    deleted.insert("agent-6");
    EXPECT_FALSE(read_as(runtime, "agent-6").ok);
    EXPECT_EQ(t.store->get_agent_launch(watching)->desired, "present");
    deleted.erase("agent-6");
    const auto missing = read_as(runtime, "agent-7");
    EXPECT_FALSE(missing.ok);
    const auto withdrawn = t.store->get_agent_launch(orphan);
    ASSERT_TRUE(withdrawn);
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_NE(withdrawn->stop_reason.find("no longer exists"), std::string::npos)
        << withdrawn->stop_reason;
    // A person reading an agent that is gone stops nothing.
    const std::string kept = launch("agent-5", "service", home);
    deleted.insert("agent-5");
    EXPECT_FALSE(t.call(first, "get_agent", in({{"id", "agent-5"}})).ok);
    EXPECT_EQ(t.store->get_agent_launch(kept)->desired, "present");
}

// lake#684, lake#685: Lake keeps a dispatch row for each connection that streams or re-exports
// tools. A re-export, or a change of what it re-exports, changes the workspace endpoint's tool-list
// digest, so a session that began before is told; the stream worker calls only the workspaces
// with a streaming connection, never one without.
TEST_P(WorkspaceRecordStores, McpConnectionDispatchNamesExportsAndStreamingWorkspacesOnly) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::McpTestServer server;
    t.mcp_http = lake::api::make_mcp_http(true);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string home = first.workspaces[0];
    const std::string lab = first.workspaces[1];
    const auto in = [&](const std::string& workspace, json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = workspace;
        return input;
    };
    const auto exported_digests = [&](const std::string& workspace) {
        const auto rows = t.store->mcp_exported_dispatch(workspace);
        EXPECT_TRUE(rows) << rows.error().message;
        std::vector<lake::api::McpExportedDigest> out;
        for (const auto& row : rows ? *rows : std::vector<lake::control::McpConnectionDispatch>{}) {
            EXPECT_EQ(row.exported_digest.size(), 64U);
            out.push_back({row.connection_id, row.exported_digest});
        }
        return out;
    };
    const std::string lake_digest = lake::api::mcp_tool_list_digest(t.registry);
    const auto endpoint_digest = [&](const std::string& workspace) {
        return lake::api::mcp_workspace_tool_list_digest(lake_digest, exported_digests(workspace));
    };

    // One connection in each of home and lab, and one of the other account: none streams or
    // re-exports, so none has a row.
    const auto at_home =
        t.call(first, "create_mcp_connection", in(home, {{"name", "Desk"}, {"url", server.url()}}));
    ASSERT_TRUE(at_home.ok) << at_home.error.message;
    const std::string id = at_home.value["id"];
    ASSERT_TRUE(
        t.call(first, "create_mcp_connection", in(lab, {{"name", "Lab"}, {"url", server.url()}}))
            .ok);
    ASSERT_TRUE(t.call(second,
                       "create_mcp_connection",
                       {{"customer_id", second.customer_id},
                        {"workspace_id", second.workspaces[0]},
                        {"name", "Other"},
                        {"url", server.url()}})
                    .ok);
    EXPECT_TRUE(t.store->mcp_stream_dispatch(100)->empty());
    EXPECT_TRUE(exported_digests(home).empty());
    EXPECT_EQ(endpoint_digest(home), lake_digest) << "nothing exported: Lake's own list";

    // A session begins on the workspace endpoint.
    const std::string session = lake::api::mcp_session_id(endpoint_digest(home));
    EXPECT_FALSE(lake::api::mcp_session_tool_list_changed(session, endpoint_digest(home)));

    // Exported with one allowed tool: the endpoint's list changed, and the session is told.
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in(home, {{"id", id}, {"tools", {{"echo", {{"allowed", true}}}}}}))
                    .ok);
    EXPECT_TRUE(exported_digests(home).empty()) << "allowed, not exported";
    ASSERT_TRUE(
        t.call(first, "update_mcp_connection", in(home, {{"id", id}, {"exported", true}})).ok);
    const auto exported = exported_digests(home);
    ASSERT_EQ(exported.size(), 1U);
    EXPECT_EQ(exported[0].connection_id, id);
    const std::string after_export = endpoint_digest(home);
    EXPECT_NE(after_export, lake_digest);
    EXPECT_TRUE(lake::api::mcp_session_tool_list_changed(session, after_export));
    EXPECT_EQ(endpoint_digest(lab), lake_digest) << "another workspace's list is unchanged";

    // The session lists again; a change of the allow-list tells it once more.
    lake::api::McpSessionLists lists;
    lists.listed(session, after_export);
    EXPECT_FALSE(lists.changed(session, endpoint_digest(home)));
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in(home, {{"id", id}, {"tools", {{"create_ticket", {{"allowed", true}}}}}}))
                    .ok);
    const std::string after_allow = endpoint_digest(home);
    EXPECT_NE(after_allow, after_export);
    EXPECT_TRUE(lists.changed(session, after_allow));
    ASSERT_TRUE(t.call(first, "refresh_mcp_connection", in(home, {{"id", id}})).ok);
    EXPECT_EQ(endpoint_digest(home), after_allow) << "a refresh that changes nothing tells no one";

    // Not exported any more: Lake's own list again.
    ASSERT_TRUE(
        t.call(first, "update_mcp_connection", in(home, {{"id", id}, {"exported", false}})).ok);
    EXPECT_TRUE(exported_digests(home).empty());
    EXPECT_EQ(endpoint_digest(home), lake_digest);

    // The worker: with no streaming connection, it calls no workspace at all.
    lake::api::McpStreamWorker worker;
    lake::api::ApiContext owner;
    owner.store = t.store.get();
    owner.workspaces = t.service.get();
    owner.instance_kek = std::string(kek);
    owner.registry = &t.registry;
    owner.mcp_http = t.mcp_http;
    owner.mcp_allow_private_addresses = true;
    t.zetta->calls.clear();
    t.zetta->call_urls.clear();
    const auto idle = worker.pass(owner);
    EXPECT_EQ(idle.workspaces, 0U);
    EXPECT_TRUE(t.zetta->calls.empty()) << "no workspace is called";

    // Notifications mirrored at home: only home's Zetta is called.
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in(home, {{"id", id}, {"notifications_topic", "desk-notices"}}))
                    .ok);
    const auto streaming = t.store->mcp_stream_dispatch(100);
    ASSERT_TRUE(streaming);
    ASSERT_EQ(streaming->size(), 1U);
    EXPECT_EQ((*streaming)[0].workspace_id, home);
    EXPECT_EQ((*streaming)[0].connection_id, id);
    t.zetta->calls.clear();
    t.zetta->call_urls.clear();
    const auto busy = worker.pass(owner);
    EXPECT_EQ(busy.workspaces, 1U);
    EXPECT_EQ(busy.connections, 1U);
    ASSERT_FALSE(t.zetta->call_urls.empty());
    const std::set<std::string> called(t.zetta->call_urls.begin(), t.zetta->call_urls.end());
    EXPECT_EQ(called.size(), 1U) << "one workspace, home, is called";

    // The stream taken away: its row goes, and the worker calls nothing again.
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in(home, {{"id", id}, {"notifications_topic", nullptr}}))
                    .ok);
    EXPECT_TRUE(t.store->mcp_stream_dispatch(100)->empty());
    t.zetta->calls.clear();
    EXPECT_EQ(worker.pass(owner).workspaces, 0U);
    EXPECT_TRUE(t.zetta->calls.empty());

    // A deleted connection leaves no row.
    ASSERT_TRUE(t.call(first,
                       "update_mcp_connection",
                       in(home, {{"id", id}, {"exported", true}, {"agent_id", "agent-1"},
                                 {"input_topic", "desk-in"}}))
                    .ok);
    EXPECT_EQ(t.store->mcp_stream_dispatch(100)->size(), 1U);
    EXPECT_EQ(exported_digests(home).size(), 1U);
    ASSERT_TRUE(t.call(first, "delete_mcp_connection", in(home, {{"id", id}})).ok);
    EXPECT_TRUE(t.store->mcp_stream_dispatch(100)->empty());
    EXPECT_TRUE(exported_digests(home).empty());
}

// lake#659: a server that speaks only stdio runs as an App of its workspace. Lake writes the
// runner's token as a new secret in the person's mount, creates the App with the runner's image,
// the package's command line and its environment, and reaches it at the App's cluster Service with
// that token; nobody changes that address. Until the App is ready the connection is `starting`;
// deleting it deletes the App.
TEST_P(WorkspaceRecordStores, McpStdioConnectionsRunAsAWorkspaceApp) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    t.mcp_http = lake::api::make_mcp_http(true);
    t.apps_wildcard = "apps.lake.test";
    const auto& first = t.spaces[0];
    const std::string home = first.workspaces[0];
    const auto in = [&](json input) {
        input["customer_id"] = first.customer_id;
        input["workspace_id"] = home;
        return input;
    };
    const auto mount = t.call(first, "create_secret_mount", in({{"name", "mcp-servers"}}));
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    ASSERT_TRUE(t.call(first,
                       "put_secret",
                       {{"mount_id", mount_id}, {"name", "github"}, {"value", {{"token", "ghp-x"}}}})
                    .ok);
    const json stdio{{"runtime", "node"},
                     {"package", "@modelcontextprotocol/server-everything@2026.8.31"},
                     {"args", json::array({"stdio"})},
                     {"mount_id", mount_id},
                     {"env",
                      {{"LOG_LEVEL", "info"},
                       {"GITHUB_TOKEN",
                        {{"secret",
                          {{"mount_id", mount_id}, {"name", "github"}, {"field", "token"}}}}}}}};

    // A Lake with no runner image runs no stdio server.
    EXPECT_EQ(t.call(first, "create_mcp_connection", in({{"name", "Everything"}, {"stdio", stdio}}))
                  .error.code,
              "not_supported");
    // Nor does one whose runner image is a tag: the operator runs only a digest-pinned image
    // (lake#783), so nothing is started that could only fail.
    t.mcp_runner_image = "oramasearch/lake-mcp-runner:0.1.0";
    const auto tagged =
        t.call(first, "create_mcp_connection", in({{"name", "Everything"}, {"stdio", stdio}}));
    EXPECT_EQ(tagged.error.code, "not_supported");
    EXPECT_TRUE(tagged.error.message.contains("pinned by digest")) << tagged.error.message;
    const std::string runner_image =
        "oramasearch/lake-mcp-runner:0.1.0@sha256:" + std::string(64, 'c');
    t.mcp_runner_image = runner_image;

    // Refusals before anything starts.
    const auto refused = [&](json changed, const std::string& path) {
        json input = stdio;
        input.update(changed);
        const auto answer =
            t.call(first, "create_mcp_connection", in({{"name", "Everything"}, {"stdio", input}}));
        EXPECT_FALSE(answer.ok) << path;
        EXPECT_EQ(answer.error.path, path) << answer.error.message;
    };
    refused({{"runtime", "ruby"}}, "/stdio/runtime");
    refused({{"package", "--eval=1"}}, "/stdio/package");
    refused({{"package", "left pad"}}, "/stdio/package");
    refused({{"command", "-x"}}, "/stdio/command");
    refused({{"env", {{"MCP_RUNNER_TOKEN", "mine"}}}}, "/stdio/env/MCP_RUNNER_TOKEN");
    refused({{"env", {{"1BAD", "x"}}}}, "/stdio/env/1BAD");
    EXPECT_EQ(t.call(first,
                     "create_mcp_connection",
                     in({{"name", "Everything"}, {"stdio", stdio}, {"url", "https://x.example"}}))
                  .error.path,
              "/url");
    const auto apps = [&] {
        const auto listed = t.store->list_web_apps(first.customer_id);
        EXPECT_TRUE(listed);
        std::vector<lake::control::WebApp> live;
        for (const auto& app : listed ? *listed : std::vector<lake::control::WebApp>{}) {
            if (app.desired != "deleted") {
                live.push_back(app);
            }
        }
        return live;
    };
    EXPECT_TRUE(apps().empty()) << "nothing started for a refused connection";

    // Added: the App, its token in the mount, and the connection that reaches it.
    const auto created =
        t.call(first, "create_mcp_connection", in({{"name", "Everything"}, {"stdio", stdio}}));
    ASSERT_TRUE(created.ok) << created.error.message << " at " << created.error.path;
    const std::string id = created.value["id"];
    EXPECT_EQ(created.value["kind"], "stdio");
    EXPECT_EQ(created.value["status"], "starting");
    const std::string app_id = created.value["app_id"];
    std::string hex = app_id;
    std::erase(hex, '-');
    EXPECT_EQ(created.value["url"],
              "http://app-" + hex.substr(0, 12) + ".dev-ws-" + home + ".svc.cluster.local/mcp");
    EXPECT_EQ(created.value["auth"]["kind"], "bearer");
    EXPECT_EQ(created.value["auth"]["secret"],
              json({{"mount_id", mount_id}, {"name", "mcp-everything-runner"}, {"field", "token"}}));
    EXPECT_EQ(created.value["stdio"]["env_keys"], json::array({"LOG_LEVEL"}));
    EXPECT_EQ(created.value["stdio"]["secret_env_keys"], json::array({"GITHUB_TOKEN"}));
    const auto running = apps();
    ASSERT_EQ(running.size(), 1U);
    const auto& app = running[0];
    EXPECT_EQ(app.id, app_id);
    EXPECT_EQ(app.workspace_id, home);
    EXPECT_EQ(app.mode, "image");
    EXPECT_EQ(app.image, runner_image);
    EXPECT_EQ(app.framework, "mcp");
    EXPECT_EQ(app.port, 8080) << "the port the workspace admits Lake to";
    EXPECT_TRUE(app.name.starts_with("mcp-everything-")) << app.name;
    EXPECT_EQ(created.value["stdio"]["app_name"], app.name);
    EXPECT_EQ(app.secret_env_refs.at("MCP_STDIO_COMMAND"),
              "lake-secret://" + mount_id + "/mcp-everything-runner#command");
    EXPECT_EQ(app.env.at("LOG_LEVEL"), "info");
    EXPECT_EQ(app.secret_env_refs.at("MCP_RUNNER_TOKEN"),
              "lake-secret://" + mount_id + "/mcp-everything-runner#token");
    EXPECT_EQ(app.secret_env_refs.at("GITHUB_TOKEN"), "lake-secret://" + mount_id + "/github#token");
    const auto token = t.call(first,
                              "read_secret",
                              {{"mount_id", mount_id}, {"name", "mcp-everything-runner"}});
    ASSERT_TRUE(token.ok) << token.error.message;
    const std::string runner_token = token.value["value"]["token"];
    EXPECT_GE(runner_token.size(), 32U);
    EXPECT_EQ(json::parse(token.value["value"]["command"].get<std::string>()),
              json::array({"npx",
                           "--yes",
                           "@modelcontextprotocol/server-everything@2026.8.31",
                           "stdio"}));
    const auto got = t.call(first, "get_mcp_connection", in({{"id", id}}));
    ASSERT_TRUE(got.ok);
    EXPECT_EQ(got.value.dump().find(runner_token), std::string::npos) << "never answered";
    EXPECT_EQ(got.value["app"]["phase"], "Pending");
    EXPECT_EQ(got.value["app"]["ready"], false);
    {
        auto context = t.context(first);
        auto records = lake::api::workspace_records(context, first.customer_id, home);
        ASSERT_TRUE(records);
        const auto kept = run(lake::control::get_mcp_connection(*records, first.customer_id, id));
        ASSERT_TRUE(kept && *kept);
        EXPECT_TRUE(lake::api::mcp_stdio_app_address(**kept));
        auto moved = **kept;
        moved.url = "http://lake-prod-postgres.lake.svc.cluster.local/mcp";
        EXPECT_FALSE(lake::api::mcp_stdio_app_address(moved)) << "only the App's own Service";
    }

    // Its address and token are the App's: a person cannot point it elsewhere.
    for (const auto& [field, value] :
         std::vector<std::pair<std::string, json>>{{"url", "https://mcp.example.com/mcp"},
                                                   {"transport", "sse"},
                                                   {"auth", {{"kind", "none"}}}}) {
        const auto changed = t.call(first, "update_mcp_connection", in({{"id", id}, {field, value}}));
        EXPECT_FALSE(changed.ok) << field;
        EXPECT_EQ(changed.error.path, "/" + field);
    }
    EXPECT_TRUE(t.call(first, "update_mcp_connection", in({{"id", id}, {"name", "Everything 2"}})).ok);

    // A refresh before the App is ready says so and calls nothing.
    const auto early = t.call(first, "refresh_mcp_connection", in({{"id", id}}));
    ASSERT_TRUE(early.ok) << early.error.message;
    EXPECT_EQ(early.value["status"], "starting");
    EXPECT_NE(early.value["last_error"].get<std::string>().find("its App is Pending"),
              std::string::npos)
        << early.value["last_error"];

    // Deleted: the App goes with it, and the secret stays in its mount.
    ASSERT_TRUE(t.call(first, "delete_mcp_connection", in({{"id", id}})).ok);
    EXPECT_TRUE(apps().empty()) << "the App is being deleted";
    EXPECT_TRUE(t.call(first,
                       "read_secret",
                       {{"mount_id", mount_id}, {"name", "mcp-everything-runner"}})
                    .ok);
    EXPECT_EQ(t.call(first, "get_mcp_connection", in({{"id", id}})).error.code, "not_found");
}

// lake#1230: a workspace's settings are one sealed record in its Zetta. `agent_model` unset means
// gpt-6.1-sol; only a served chat model can be chosen; whoever may change the workspace's account
// sets it, a member who may only read it reads it, and another account's person neither.
TEST_P(WorkspaceRecordStores, AWorkspaceChoosesItsAgentModelAmongTheServedChatModels) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const std::string home = first.workspaces[0];
    const json at{{"customer_id", first.customer_id}, {"workspace_id", home}};
    const auto with = [&](json more) {
        more.update(at);
        return more;
    };

    const auto unset = t.call(first, "get_workspace_settings", at);
    ASSERT_TRUE(unset.ok) << unset.error.message;
    EXPECT_EQ(unset.value["agent_model"], "gpt-6.1-sol");
    EXPECT_TRUE(unset.value["agent_model_chosen"].is_null());
    EXPECT_EQ(unset.value["default_agent_model"], "gpt-6.1-sol");
    std::vector<std::string> offered;
    for (const json& model : unset.value["agent_models"]) {
        offered.push_back(model["model"]);
        EXPECT_EQ(model["default"], model["model"] == "gpt-6.1-sol");
        EXPECT_TRUE(model["input_micros_per_million"].is_null()) << "billing is off here";
    }
    EXPECT_EQ(offered,
              (std::vector<std::string>{"gpt-6.1-sol",
                                        "gpt-6.1-terra",
                                        "gpt-6-astra",
                                        "gpt-6-luna",
                                        "gpt-5.5",
                                        "gpt-5",
                                        "gpt-4o"}))
        << "the served chat models, never an embedding model";

    for (const std::string refused : {"text-embedding-3-small", "gpt-7", "GPT-6.1-SOL"}) {
        const auto set = t.call(first, "set_workspace_settings", with({{"agent_model", refused}}));
        EXPECT_FALSE(set.ok) << refused;
        EXPECT_EQ(set.error.code, "invalid_input") << refused;
        EXPECT_EQ(set.error.path, "/agent_model") << refused;
    }
    const auto chosen =
        t.call(first, "set_workspace_settings", with({{"agent_model", "gpt-6.1-terra"}}));
    ASSERT_TRUE(chosen.ok) << chosen.error.message;
    EXPECT_EQ(chosen.value["agent_model"], "gpt-6.1-terra");
    EXPECT_EQ(chosen.value["agent_model_chosen"], "gpt-6.1-terra");
    EXPECT_EQ(chosen.value["set_by"], first.principal.user_id);
    EXPECT_TRUE(chosen.value["updated_at"].is_string());
    // Left out, unchanged.
    const auto kept = t.call(first, "set_workspace_settings", at);
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_EQ(kept.value["agent_model"], "gpt-6.1-terra");
    // The other workspace of the account keeps its own.
    const auto lab =
        t.call(first,
               "get_workspace_settings",
               {{"customer_id", first.customer_id}, {"workspace_id", first.workspaces[1]}});
    ASSERT_TRUE(lab.ok) << lab.error.message;
    EXPECT_EQ(lab.value["agent_model"], "gpt-6.1-sol");
    // What the gateway reads.
    {
        auto context = t.context(first);
        const auto model = lake::control::settled(liquid::runtime::block_on(
            lake::api::workspace_agent_model(context, first.customer_id, home)));
        ASSERT_TRUE(model) << model.error().message;
        EXPECT_EQ(*model, "gpt-6.1-terra");
    }

    // Sealed in the workspace Zetta: the model's name is not on the record at rest.
    bool stored = false;
    for (auto& [url, tables] : t.zetta->records) {
        if (const auto found = tables.collections.find("lake_workspace_settings");
            found != tables.collections.end()) {
            for (const auto& [key, row] : found->second.records) {
                stored = true;
                EXPECT_EQ(row.dump().find("gpt-6.1-terra"), std::string::npos);
            }
        }
    }
    EXPECT_TRUE(stored) << "the settings are a record in the workspace Zetta";

    // Another account's person can neither read nor change them.
    EXPECT_FALSE(t.call(second, "get_workspace_settings", at).ok);
    EXPECT_FALSE(
        t.call(second, "set_workspace_settings", with({{"agent_model", "gpt-6-luna"}})).ok);
    // A member who may read the account (an organization member's grant) reads them and cannot
    // change them.
    const auto object = store->find_policy_object("customer", first.customer_id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(store->create_grant(
        {second.principal.taap_id,
         object->id,
         "read",
         lake::control::identity_crypto::sha256_hex(second.principal.taap_id + object->id),
         first.principal.taap_id,
         lake::control::new_uuid_v4()}));
    const auto read = t.call(second, "get_workspace_settings", at);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["agent_model"], "gpt-6.1-terra");
    const auto denied =
        t.call(second, "set_workspace_settings", with({{"agent_model", "gpt-6-luna"}}));
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");

    // Null goes back to the default.
    const auto reset = t.call(first, "set_workspace_settings", with({{"agent_model", nullptr}}));
    ASSERT_TRUE(reset.ok) << reset.error.message;
    EXPECT_EQ(reset.value["agent_model"], "gpt-6.1-sol");
    EXPECT_TRUE(reset.value["agent_model_chosen"].is_null());
}

INSTANTIATE_TEST_SUITE_P(Stores, WorkspaceRecordStores, testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Postgres" : "Memory"; });

// lake#374, lake#375: what was written for an agent task is one record in its workspace. The first
// finished answer stays; a message or answer raced by another write is reread, never written over
// blindly; a record under another task or space does not open; a record an event wrote before the
// request was copied gets the request and keeps what the event wrote.
TEST(WorkspaceRecords, AnAgentTaskKeepsItsFirstAnswerAndRereadsARacedEvent) {
    DirectRecords direct;
    auto records = direct.open();
    const std::string task = lake::control::new_uuid_v4();
    using lake::control::AgentTaskContent;
    const auto event = [&](std::string phase, std::string message, json result) {
        return AgentTaskContent{.task_id = task,
                                .customer_id = "customer-1",
                                .agent = "agent-project",
                                .message = std::move(message),
                                .result = std::move(result),
                                .phase = std::move(phase)};
    };
    const auto created = run(lake::control::create_agent_task_content(
        records, {.task_id = task,
                  .customer_id = "customer-1",
                  .agent = "agent-project",
                  .request = {{"message", "Predict churn"}, {"lake_token", "{{credential}}"}}}));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->lake_revision, 1);
    EXPECT_EQ(run(lake::control::create_agent_task_content(
                      records, {.task_id = task, .customer_id = "customer-1", .request = json::object()}))
                  .error()
                  .code,
              "conflict")
        << "a request is written once";
    for (const auto& [key, row] : direct.tables->rows("agent_tasks")) {
        EXPECT_EQ(row.dump().find("Predict churn"), std::string::npos) << "sealed at rest";
    }

    // A progress message whose write is raced by another writer is reread and applied again.
    bool raced = false;
    auto racing = WorkspaceRecords::open(
        [&, tables = direct.tables](std::string capability, bool, json input)
            -> liquid::runtime::task<lake::control::ZettaControlResult<json>> {
            if (capability == "put_record" && !raced) {
                raced = true;
                auto& live = tables->rows("agent_tasks").at(task);
                live["revision"] = live["revision"].get<std::int64_t>() + 1000;
            }
            co_return tables->run(capability, input);
        },
        direct.workspace_id, kek);
    ASSERT_TRUE(racing);
    const auto progressed =
        run(lake::control::record_agent_task_content(*racing, event("", "halfway", nullptr)));
    ASSERT_TRUE(progressed) << progressed.error().message;
    EXPECT_TRUE(raced);
    EXPECT_EQ(progressed->message, "halfway");
    EXPECT_EQ(progressed->request["message"], "Predict churn") << "the request is kept";
    EXPECT_EQ(progressed->lake_revision, 2);

    // The first answer stays: a later answer or message changes nothing.
    const auto answered = run(lake::control::record_agent_task_content(
        records, event("Succeeded", "", {{"reply", "first"}})));
    ASSERT_TRUE(answered) << answered.error().message;
    EXPECT_EQ(answered->phase, "Succeeded");
    EXPECT_EQ(answered->result["reply"], "first");
    for (const auto& later : {event("Failed", "too late", nullptr),
                              event("", "a late progress message", nullptr),
                              event("Succeeded", "", {{"reply", "second"}})}) {
        const auto kept = run(lake::control::record_agent_task_content(records, later));
        ASSERT_TRUE(kept) << kept.error().message;
        EXPECT_EQ(kept->result["reply"], "first");
        EXPECT_EQ(kept->phase, "Succeeded");
        EXPECT_EQ(kept->message, "");
    }
    EXPECT_EQ(run(lake::control::get_agent_task_content(records, task, "customer-1")).value()->lake_revision,
              3);

    // Another space's or another key's reading finds nothing; another space cannot write it.
    EXPECT_FALSE(*run(lake::control::get_agent_task_content(records, task, "customer-2")));
    EXPECT_FALSE(*run(
        lake::control::get_agent_task_content(records, lake::control::new_uuid_v4(), "customer-1")));
    auto foreign = event("", "not theirs", nullptr);
    foreign.customer_id = "customer-2";
    EXPECT_EQ(run(lake::control::record_agent_task_content(records, foreign)).error().code,
              "conflict");
    auto& rows = direct.tables->rows("agent_tasks");
    const std::string moved = lake::control::new_uuid_v4();
    rows[moved] = rows.at(task);
    rows[moved]["key"] = moved;
    EXPECT_FALSE(*run(lake::control::get_agent_task_content(records, moved, "customer-1")))
        << "a record copied under another key does not open";

    // An event of a task without a record (one created before the move) writes a record without a
    // request and keeps the answer.
    const std::string early = lake::control::new_uuid_v4();
    auto early_event = event("Succeeded", "", {{"reply", "done before the copy"}});
    early_event.task_id = early;
    const auto recorded = run(lake::control::record_agent_task_content(records, early_event));
    ASSERT_TRUE(recorded) << recorded.error().message;
    EXPECT_TRUE(recorded->request.is_null());
    EXPECT_EQ(recorded->result["reply"], "done before the copy");
}

namespace {

class NoAgentCalls final : public lake::control::AgentCaller {
public:
    liquid::runtime::task<lake::control::ZettaControlResult<json>>
    turn(std::string, std::string, json) override {
        co_return lake::control::ZettaControlResult<json>(std::unexpected(
            lake::control::ZettaControlError{"store_unavailable", "library agents use the bus"}));
    }
};

// Two spaces with two workspaces each, as TwoSpaces makes them, with the Project Agent on lake's
// agent bus (docs/api/agent-bus.md).
struct AgentTaskSpaces {
    TwoSpaces t;
    std::shared_ptr<lake::control::WorkspaceService> service;
    std::shared_ptr<lake::control::MemoryAgentBus> dispatch =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus;
    NoAgentCalls agents;

    AgentTaskSpaces(std::shared_ptr<lake::control::Store> store, std::string uri)
        : t(std::move(store), std::move(uri)) {
        lake::control::WorkspaceConfig config{t.dir.string(), 0};
        config.agent_endpoints = {{"agent-project", "http://project:8080"},
                                  {"agent-application", "http://application:8080"}};
        config.internal_url = "http://lake.internal";
        service = std::make_shared<lake::control::WorkspaceService>(config, t.store, t.zetta);
        bus = std::make_shared<lake::control::AgentTaskBus>(
            t.store, dispatch, std::vector<std::string>{"agent-project", "agent-application"},
            std::chrono::seconds(0));
        EXPECT_TRUE(lake::control::settled(liquid::runtime::block_on(bus->start())));
    }

    lake::api::ApiResult call(lake::api::ApiContext context, const std::string& name, json input) {
        context.workspaces = service.get();
        context.agent_bus = bus.get();
        context.agent_caller = &agents;
        context.instance_kek = std::string(kek);
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : lake::api::ApiResult::failure("store_error", "incomplete");
    }
    lake::api::ApiResult person(const TwoSpaces::Space& space, const std::string& name,
                                json input) {
        return call(t.context(space), name, std::move(input));
    }
    lake::api::ApiResult agent(const std::string& name, json input,
                               const std::string& as = "agent-project") {
        lake::api::ApiContext context;
        context.store = t.store.get();
        context.authorized = true;
        context.acting_identity = "agent-service:" + as;
        context.credential_kind = "agent_service";
        context.agent_service = as;
        context.request_id = lake::control::new_uuid_v4();
        input["agent"] = as;
        return call(context, name, std::move(input));
    }
    lake::api::ApiResult ask(const TwoSpaces::Space& space, const std::string& workspace,
                             const std::string& message) {
        return person(space, "agent_turn", {{"customer_id", space.customer_id},
                                            {"workspace_id", workspace},
                                            {"agent", "agent-project"},
                                            {"message", message}});
    }
    lake::api::ApiResult follow(const TwoSpaces::Space& space, const std::string& workspace,
                                const std::string& task) {
        return person(space, "get_agent_task", {{"customer_id", space.customer_id},
                                                {"workspace_id", workspace},
                                                {"task_id", task}});
    }
    std::map<std::string, json>& rows(const std::string& workspace) {
        return t.zetta->records["http://" + workspace + ".test"].rows("agent_tasks");
    }
};

} // namespace

TEST_P(WorkspaceRecordStores, AgentTasksKeepWhatWasWrittenInTheirOwnWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    AgentTaskSpaces a(store, database_uri_);
    const auto& first = a.t.spaces[0];
    const auto& second = a.t.spaces[1];
    // A turn in each workspace of each space; each request is a record of its own workspace only.
    std::map<std::string, std::string> tasks; // workspace -> task
    for (const auto* space : {&first, &second}) {
        for (const std::string& workspace : space->workspaces) {
            const auto asked = a.ask(*space, workspace, "private request in " + workspace);
            ASSERT_TRUE(asked.ok) << asked.error.message;
            EXPECT_EQ(asked.value["status"], "pending");
            tasks[workspace] = asked.value["task_id"];
            ASSERT_EQ(a.rows(workspace).size(), 1U) << workspace;
            EXPECT_EQ(a.rows(workspace).begin()->first, tasks[workspace]);
            EXPECT_EQ(a.rows(workspace).begin()->second.dump().find("private request"),
                      std::string::npos)
                << "sealed at rest";
        }
    }
    const std::string home = first.workspaces[0], lab = first.workspaces[1];
    // The control store keeps dispatch only: no request, message or answer.
    for (const auto& [workspace, task] : tasks) {
        const auto row = a.t.store->get_agent_task(task);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->workspace_id, workspace);
        EXPECT_EQ(row->body, json::object());
        EXPECT_EQ(row->message, "");
        EXPECT_TRUE(row->result.is_null());
    }
    // The bus carries the task id only.
    for (const auto& event : a.dispatch->events("agent-tasks.agent-project")) {
        EXPECT_EQ(event.payload.dump().find("private request"), std::string::npos);
    }

    // The claim answers the request from the task's workspace, with the credential in it.
    const auto claimed = a.agent("claim_agent_task", {{"task_id", tasks[home]}});
    ASSERT_TRUE(claimed.ok) << claimed.error.message;
    EXPECT_EQ(claimed.value["body"]["message"], "private request in " + home);
    EXPECT_EQ(claimed.value["body"]["workspace_id"], home);
    EXPECT_TRUE(claimed.value["body"]["lake_token"].get<std::string>().starts_with("lat."));
    EXPECT_EQ(a.agent("claim_agent_task", {{"task_id", tasks[home]}}).error.code, "conflict")
        << "exactly one claim";
    EXPECT_EQ(a.agent("claim_agent_task", {{"task_id", tasks[lab]}}, "agent-application").error.code,
              "conflict")
        << "another agent's bus credential cannot claim it";
    EXPECT_EQ(a.person(first, "claim_agent_task", {{"task_id", tasks[lab]}, {"agent", "agent-project"}})
                  .error.code,
              "unauthorized")
        << "a person is not the agent";

    // Progress and the answer are written to the workspace; the control store keeps the phase.
    ASSERT_TRUE(a.agent("publish_agent_event",
                        {{"task_id", tasks[home]}, {"type", "progress"}, {"message", "halfway"}})
                    .ok);
    auto followed = a.follow(first, home, tasks[home]);
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["status"], "running");
    EXPECT_EQ(followed.value["message"], "halfway");
    EXPECT_EQ(a.t.store->get_agent_task(tasks[home])->message, "");
    const auto answered = a.agent("publish_agent_event",
                                  {{"task_id", tasks[home]},
                                   {"type", "result"},
                                   {"result", {{"ok", true}, {"reply", "the private answer"}}}});
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(a.agent("publish_agent_event",
                      {{"task_id", tasks[home]}, {"type", "result"}, {"result", {{"reply", "late"}}}})
                  .error.code,
              "conflict");
    followed = a.follow(first, home, tasks[home]);
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["status"], "succeeded");
    EXPECT_EQ(followed.value["answer"]["reply"], "the private answer");
    EXPECT_TRUE(a.t.store->get_agent_task(tasks[home])->result.is_null());
    EXPECT_EQ(a.rows(home).begin()->second.dump().find("private answer"), std::string::npos);
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        // The dispatch rows have no content columns at all (migration 0070 dropped them).
        const auto content = tx.exec(
            "SELECT count(*) FROM information_schema.columns WHERE table_schema = "
            "current_schema() AND ((table_name = 'agent_tasks' AND column_name = 'body') OR "
            "(table_name = 'agent_task_status' AND column_name IN ('message', 'result')))");
        EXPECT_EQ(content[0][0].as<int>(), 0) << "no content in PostgreSQL";
    }

    // Another workspace of the space, or another space, never reads it.
    EXPECT_EQ(a.follow(first, lab, tasks[home]).error.code, "not_found");
    EXPECT_EQ(a.follow(second, second.workspaces[0], tasks[home]).error.code, "not_found");
    const auto trespass = a.person(second, "get_agent_task", {{"customer_id", first.customer_id},
                                                              {"workspace_id", home},
                                                              {"task_id", tasks[home]}});
    EXPECT_EQ(trespass.error.code, "unauthorized");

    // Missing, malformed and unknown workspace ids name the field, for asking and following.
    for (const std::string capability : {"agent_turn", "get_agent_task"}) {
        json input{{"customer_id", first.customer_id}};
        if (capability == "agent_turn") {
            input["agent"] = "agent-project";
            input["message"] = "hello";
        } else {
            input["task_id"] = tasks[home];
        }
        const auto missing = a.person(first, capability, input);
        EXPECT_EQ(missing.error.code, "invalid_input") << capability;
        EXPECT_EQ(missing.error.path, "/workspace_id") << capability;
        for (const std::string& bad :
             std::vector<std::string>{"not-a-workspace", lake::control::new_uuid_v4()}) {
            input["workspace_id"] = bad;
            const auto refused = a.person(first, capability, input);
            EXPECT_EQ(refused.error.code, "invalid_input") << capability << " " << bad;
            EXPECT_EQ(refused.error.path, "/workspace_id") << capability << " " << bad;
        }
        // Another space's workspace is not this space's.
        input["workspace_id"] = second.workspaces[0];
        const auto foreign = a.person(first, capability, input);
        EXPECT_EQ(foreign.error.code, "invalid_input") << capability;
        EXPECT_EQ(foreign.error.path, "/customer_id") << capability;
    }

    // A workspace that is not ready refuses instead of falling back: a new turn is not
    // recorded, the task is not read, and the claim leaves it waiting for a later claim.
    const auto lab_workspace = a.t.store->get_workspace(lab);
    a.t.status(lab_workspace->zetta_service_id, "Provisioning", "http://" + lab + ".test");
    EXPECT_EQ(a.ask(first, lab, "not now").error.code, "workspace_not_ready");
    EXPECT_EQ(a.follow(first, lab, tasks[lab]).error.code, "workspace_not_ready");
    EXPECT_EQ(a.agent("claim_agent_task", {{"task_id", tasks[lab]}}).error.code,
              "workspace_not_ready");
    EXPECT_TRUE(a.t.store->get_agent_task(tasks[lab])->phase.empty()) << "still waiting";
    a.t.ready(lab);
    const auto later = a.agent("claim_agent_task", {{"task_id", tasks[lab]}});
    ASSERT_TRUE(later.ok) << later.error.message;
    EXPECT_EQ(later.value["body"]["message"], "private request in " + lab);
    EXPECT_EQ(a.rows(lab).size(), 1U) << "the refused turn left nothing behind";

    // A task whose request is not in its workspace is never claimed with an empty request.
    auto& their_rows = a.rows(second.workspaces[1]);
    their_rows.clear();
    EXPECT_EQ(a.agent("claim_agent_task", {{"task_id", tasks[second.workspaces[1]]}}).error.code,
              "not_found");

    // Deleting a workspace takes its tasks' records with it; the other workspace keeps its own.
    ASSERT_TRUE(a.t.store->update_workspace(second.workspaces[0], {.desired = "deleted"}));
    const auto deleted = a.follow(second, second.workspaces[0], tasks[second.workspaces[0]]);
    EXPECT_EQ(deleted.error.code, "invalid_input");
    EXPECT_EQ(deleted.error.path, "/workspace_id");
    followed = a.follow(first, home, tasks[home]);
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["answer"]["reply"], "the private answer");
}

// Against a real Zetta when LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN name one (for example a
// local `zetta --headless` built from zetta dev with #1233): the record store's real contract.
TEST(WorkspaceRecordsZetta, TheEngineStoresAndReturnsSealedRecords) {
    const char* url = std::getenv("LAKE_TEST_ZETTA_URL");
    const char* token = std::getenv("LAKE_TEST_ZETTA_TOKEN");
    if (url == nullptr || token == nullptr || *url == '\0') {
        GTEST_SKIP() << "LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN are not set";
    }
    lake::control::install_zetta_control_transport();
    struct Shutdown {
        ~Shutdown() {
            lake::control::shutdown_zetta_control_transport();
        }
    } shutdown;
    auto caller = std::make_shared<lake::control::ZettaWorkspaceCaller>();
    const std::string endpoint = url;
    const std::string bearer = token;
    auto opened = WorkspaceRecords::open(
        [caller, endpoint, bearer](std::string capability, bool read, json input) {
            return caller->call(endpoint, bearer, std::move(capability), read, std::move(input));
        },
        lake::control::new_uuid_v4(), kek);
    ASSERT_TRUE(opened) << opened.error().message;
    AskMessages messages(*opened);
    const std::string conversation = lake::control::new_uuid_v4();
    std::string long_answer;
    while (long_answer.size() < 20'000) {
        long_answer += lake::control::new_uuid_v4();
    }
    const auto hello = run(messages.append(message(conversation, "u", "user", "hello")));
    ASSERT_TRUE(hello) << hello.error().code << ": " << hello.error().message;
    const auto big = run(messages.append(message(conversation, "u", "assistant", long_answer,
                                                 {{"reads", json::array({"x"})}})));
    ASSERT_TRUE(big) << big.error().message;
    const auto read = run(messages.list(lake::control::ask_conversation_lookup(conversation, "u")));
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read->size(), 2U);
    EXPECT_EQ(read->at(0).content, "hello");
    EXPECT_EQ(read->at(1).content, long_answer);
    EXPECT_EQ(read->at(1).payload["reads"][0], "x");
    const auto duplicate = run(messages.append(message(conversation, "u", "user", "again"),
                                               lake::control::RecordWrite{hello->id, 0}));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "conflict") << "if_absent is decided by the engine";
    EXPECT_TRUE(duplicate.error().details.contains("revision"));

    // A mutable record: conditional writes against the engine's revisions.
    const lake::control::RecordCollection counters{"test_counters", 1};
    for (int index = 0; index < 3; ++index) {
        const auto updated = run(opened->update(
            counters, "c-1", "all", [](const std::optional<lake::control::StoredRecord>& current) {
                return std::optional<json>(json{{"n", current ? current->body.value("n", 0) + 1 : 1}});
            }));
        ASSERT_TRUE(updated && *updated) << (updated ? "" : updated.error().message);
    }
    const auto counted = run(opened->get(counters, "c-1"));
    ASSERT_TRUE(counted && *counted);
    EXPECT_EQ((**counted).body["n"], 3);
    EXPECT_EQ((**counted).lake_revision, 3);
    EXPECT_EQ(run(opened->put(counters, "c-1", "all", {{"n", 0}}, {.if_revision = 1})).error().code,
              "conflict");
    ASSERT_TRUE(run(opened->remove(counters, "c-1", (**counted).revision)));
    EXPECT_FALSE(*run(opened->get(counters, "c-1")));

    // A workspace audit chain (lake#317): places claimed if_absent by the engine, found again
    // by the tail search, and verified.
    lake::control::InMemoryStore identity_store;
    const lake::audit::IdentityOptions options{.instance_kek = std::string(kek)};
    const auto signer = lake::audit::identity(identity_store, options);
    ASSERT_TRUE(signer) << signer.error().message;
    std::optional<lake::control::WorkspaceAuditTail> tail;
    for (int index = 0; index < 3; ++index) {
        const auto appended = run(lake::control::append_workspace_audit_event(
            *opened, identity_store, *signer,
            lake::audit::pending({.kind = "capability",
                                  .principal = "admin",
                                  .object = "capability/workspace_query",
                                  .action = "workspace_query",
                                  .outcome = "success",
                                  .request_id = lake::control::new_uuid_v4()}),
            options, index == 0 ? std::nullopt : tail));
        ASSERT_TRUE(appended) << appended.error().message;
        tail = *appended;
    }
    EXPECT_EQ(tail->sequence, 3);
    const auto found = run(lake::control::find_workspace_audit_tail(*opened));
    ASSERT_TRUE(found) << found.error().message;
    EXPECT_EQ(found->sequence, 3);
    const auto stale = run(lake::control::append_workspace_audit_event(
        *opened, identity_store, *signer,
        lake::audit::pending({.kind = "capability",
                              .principal = "admin",
                              .object = "capability/workspace_sql",
                              .action = "workspace_sql",
                              .outcome = "success",
                              .request_id = lake::control::new_uuid_v4()}),
        options, lake::control::WorkspaceAuditTail{1, ""}));
    ASSERT_TRUE(stale) << stale.error().message;
    EXPECT_EQ(stale->sequence, 4) << "a taken place is found and passed";
    const auto verified = run(lake::control::verify_workspace_audit(*opened, identity_store));
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->error_message;
    EXPECT_EQ(verified->chained, 4U);
}

// Tenant image records (lake#383 phase A): a manifest is written once and found by digest, a
// second write of the same bytes is answered as present, a tag moved by another push between the
// read and the write is reread rather than overwritten, a tag listing reads one repository only,
// and another space's or repository's keys never open.
TEST(WorkspaceRecords, ImageTagsMoveAtTheRevisionReadAndManifestsAreWrittenOnce) {
    DirectRecords direct;
    auto records = direct.open();
    const auto manifest = [](const std::string& text) {
        return lake::control::OciManifest{
            .customer_id = "customer-1",
            .digest = "sha256:" + lake::control::identity_crypto::sha256_hex(text),
            .content_type = "application/vnd.oci.image.manifest.v1+json",
            .document = text,
            .references = {{"sha256:" + std::string(64, 'a'), 12}}};
    };
    const auto first = manifest(R"({"schemaVersion":2,"layers":[]})");
    const auto stored = run(lake::control::put_oci_manifest(records, first));
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(stored->document, first.document);
    const auto again = run(lake::control::put_oci_manifest(records, first));
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(direct.tables->rows("oci_manifests").size(), 1U) << "content-addressed, once";
    EXPECT_FALSE(*run(lake::control::get_oci_manifest(records, "customer-2", first.digest)))
        << "another space's key";

    lake::control::OciTag tag{.customer_id = "customer-1",
                              .repository = "web",
                              .tag = "v1",
                              .digest = first.digest,
                              .content_type = first.content_type,
                              .manifest_size = first.document.size(),
                              .references = first.references};
    const auto created = run(lake::control::set_oci_tag(records, tag));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->lake_revision, 1);
    EXPECT_EQ(run(lake::control::create_oci_tag(records, tag)).error().code, "conflict")
        << "a copy never overwrites a tag";

    // Another push moves the tag between this push's read and its write: reread and moved again.
    const auto second = manifest(R"({"schemaVersion":2,"layers":[],"n":2})");
    ASSERT_TRUE(run(lake::control::put_oci_manifest(records, second)));
    const std::string key = direct.tables->rows("oci_tags").begin()->first;
    bool raced = false;
    auto racing = WorkspaceRecords::open(
        [&, tables = direct.tables](std::string capability, bool, json input)
            -> liquid::runtime::task<lake::control::ZettaControlResult<json>> {
            if (capability == "put_record" && !raced) {
                raced = true;
                auto& live = tables->rows("oci_tags").at(key);
                live["revision"] = live["revision"].get<std::int64_t>() + 1000;
            }
            co_return tables->run(capability, input);
        },
        direct.workspace_id, kek);
    ASSERT_TRUE(racing);
    tag.digest = second.digest;
    const auto moved = run(lake::control::set_oci_tag(*racing, tag));
    ASSERT_TRUE(moved) << moved.error().message;
    EXPECT_TRUE(raced);
    EXPECT_EQ(moved->digest, second.digest);
    EXPECT_EQ(moved->lake_revision, 2);

    lake::control::OciTag other = tag;
    other.repository = "cache/web";
    other.tag = "buildcache";
    ASSERT_TRUE(run(lake::control::set_oci_tag(records, other)));
    const auto listed = run(lake::control::list_oci_tags(records, "customer-1", "web"));
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U) << "one repository's tags";
    EXPECT_EQ(listed->front().digest, second.digest);
    EXPECT_TRUE(run(lake::control::list_oci_tags(records, "customer-2", "web"))->empty());
    EXPECT_FALSE(*run(lake::control::get_oci_tag(records, "customer-1", "web", "v2")));
}

// lake#655 (docs/api/methodologies.md): a workspace creates its own methodologies from Python
// code, kept in one of its repositories and registered by its engine next to Orama's, which stay
// read-only. Validate checks the code before anything runs, then runs the interface in the
// sandbox; a run's data goes in and its result comes out only through the run's own credential,
// which reaches nothing else; a training run's model is registered by the engine.
TEST_P(WorkspaceRecordStores, MethodologiesAreTheWorkspacesOwnAndOramasStayReadOnly) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    lake::test::MethodologyRepositories repositories;
    t.sync_store = &repositories;
    t.methodology_runtime_image = "registry.test/oramasearch/methodology-runtime:1";
    t.internal_url = "http://lake.test";
    t.public_host = "lake.example.test";
    const auto& first = t.spaces[0];
    const auto& second = t.spaces[1];
    const auto in = [&](const auto& space, std::size_t workspace, json input) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[workspace];
        return input;
    };

    // The engine's registry: Orama's catalogue, read-only, and the workspace's own (zetta#1261).
    const json m1{{"id", "M-1"},
                  {"name", "Tabular supervised learning"},
                  {"family", "The tabular decision core"},
                  {"model_kinds", {"gbdt"}},
                  {"inputs", json::array({{{"role", "label"}, {"required", true}}})},
                  {"outputs", {"model"}},
                  {"metrics", {"held_out_auc"}},
                  {"runnable", true},
                  {"source", "orama"},
                  {"editable", false}};
    std::map<std::string, json> own;
    std::vector<json> recorded_runs;
    json study = json::object();
    std::vector<json> study_calls;
    int next_id = 0;
    // lake#690: what the engine staged of a large artifact, and the model Ship → Models serves.
    std::string staged_artifact;
    json served_model = json::object();
    const std::string served_weights(20000, 'w');
    t.zetta->responder =
        [&](const std::string& capability,
            const json& input) -> std::optional<lake::control::ZettaControlResult<json>> {
        using Result = lake::control::ZettaControlResult<json>;
        const auto missing = [] {
            return Result(std::unexpected(
                lake::control::ZettaControlError{"not_found", "no such methodology"}));
        };
        if (capability == "get_methodology") {
            const std::string id = input.value("id", std::string());
            if (id == "M-1") {
                return Result(json{{"methodology", m1}});
            }
            const auto found = own.find(id);
            return found == own.end() ? missing() : Result(json{{"methodology", found->second}});
        }
        if (capability == "list_methodologies") {
            json all = json::array({m1});
            for (const auto& [id, entry] : own) {
                all.push_back(entry);
            }
            return Result(json{{"methodologies", all}});
        }
        if (capability == "register_methodology") {
            char id[16];
            std::snprintf(id, sizeof(id), "W-%08x", ++next_id);
            json entry = input;
            entry["id"] = id;
            entry["source"] = "workspace";
            entry["editable"] = true;
            entry["validation"] = {{"status", "not_validated"}, {"findings", json::array()}};
            own[id] = entry;
            return Result(json{{"methodology", entry}});
        }
        if (capability == "update_methodology") {
            const std::string id = input.value("id", std::string());
            const auto found = own.find(id);
            if (found == own.end()) {
                return missing();
            }
            const std::string before = found->second["code"].value("revision", std::string());
            for (const auto& [key, value] : input.items()) {
                found->second[key] = value;
            }
            if (found->second["code"].value("revision", std::string()) != before &&
                !input.contains("validation")) {
                found->second["validation"] = {{"status", "not_validated"}};
            }
            return Result(json{{"methodology", found->second}});
        }
        if (capability == "delete_methodology") {
            own.erase(input.value("id", std::string()));
            return Result(json{{"removed", true}, {"id", input.value("id", std::string())}});
        }
        if (capability == "create_study" || capability == "get_study" ||
            capability == "stop_trial" || capability == "stop_study" ||
            (capability == "record_methodology_run" && input.contains("study"))) {
            // A workspace methodology's experiment (zetta lake#689): trials wait for Lake's runner.
            const auto runner = [&] {
                json waiting = json::array();
                if (study["status"] != "running") {
                    return waiting;
                }
                for (const json& trial : study["trials"]) {
                    if (trial["state"] == "running") {
                        waiting.push_back({{"study", "study-1"},
                                           {"study_name", study["spec"]["name"]},
                                           {"trial", trial["run"]},
                                           {"number", trial["number"]},
                                           {"methodology", study["spec"]["methodology"]},
                                           {"params", {{"epochs", 10 + trial["number"].get<int>()}}},
                                           {"seed", trial["number"]},
                                           {"dataset_path", study["spec"]["dataset"]},
                                           {"target", study["spec"]["target"]},
                                           {"features", json::array()},
                                           {"holdout_fraction", 0.2}});
                    }
                }
                return waiting;
            };
            const auto admit = [&] {
                std::size_t running = 0;
                for (const json& trial : study["trials"]) {
                    running += trial["state"] == "running" ? 1 : 0;
                }
                while (study["trials"].size() < 3 && running < 2) {
                    const std::size_t number = study["trials"].size();
                    study["trials"].push_back({{"number", number},
                                               {"run", "trial-run-" + std::to_string(number)},
                                               {"state", "running"}});
                    ++running;
                }
                if (running == 0) {
                    study["status"] = "complete";
                }
            };
            const auto answer = [&] {
                json out = study;
                out["runner"] = runner();
                return out;
            };
            const auto end = [&](const std::string& run, const std::string& state) {
                for (json& trial : study["trials"]) {
                    if (trial["run"] == run) {
                        trial["state"] = state;
                    }
                }
                admit();
            };
            if (capability == "create_study") {
                study_calls.push_back(input);
                study = {{"id", "study-1"},
                         {"spec", input},
                         {"status", "running"},
                         {"created", "2026-10-11T10:00:00Z"},
                         {"updated", "2026-10-11T10:00:00Z"},
                         {"trials", json::array()},
                         {"spent_cost", 0.0},
                         {"stop_reason", ""},
                         {"promoted_model", ""}};
                admit();
                return Result(answer());
            }
            if (capability == "get_study") {
                return Result(answer());
            }
            if (capability == "stop_trial") {
                study_calls.push_back(input);
                end(input.value("run", std::string()), "failed");
                return Result(json{{"study", "study-1"},
                                   {"run", input.value("run", std::string())},
                                   {"state", "failed"},
                                   {"status", study["status"]},
                                   {"runner", runner()}});
            }
            if (capability == "stop_study") {
                study["status"] = "stopped";
                return Result(json{{"study", "study-1"}, {"status", "stopped"}, {"reason", "stopped"}});
            }
            recorded_runs.push_back(input);
            end(input.value("trial", std::string()), "complete");
            return Result(json{{"model_id", "model-" + std::to_string(recorded_runs.size())},
                               {"run_id", input.value("trial", std::string())},
                               {"study",
                                {{"id", "study-1"},
                                 {"trial_state", "complete"},
                                 {"objective", 0.8},
                                 {"status", study["status"]},
                                 {"runner", runner()}}}});
        }
        if (capability == "record_methodology_run") {
            recorded_runs.push_back(input);
            return Result(json{{"model_id", "model-" + std::to_string(recorded_runs.size())}});
        }
        if (capability == "append_methodology_artifact") {
            // lake#690: the engine stages a large artifact in order.
            const std::size_t offset = input.value("offset", std::size_t{0});
            if (offset > staged_artifact.size()) {
                return Result(std::unexpected(lake::control::ZettaControlError{
                    "conflict", "expected offset " + std::to_string(staged_artifact.size())}));
            }
            staged_artifact.resize(offset);
            staged_artifact += lake::test_base64_decode(input.value("chunk_base64", std::string()));
            return Result(json{{"received", staged_artifact.size()}});
        }
        if (capability == "get_model") {
            if (input.value("model_id", std::string()) != served_model.value("id", std::string())) {
                return Result(std::unexpected(
                    lake::control::ZettaControlError{"not_found", "no such model"}));
            }
            return Result(served_model);
        }
        if (capability == "expose_model" || capability == "unexpose_model") {
            served_model["exposed"] = capability == "expose_model";
            // The engine's expose_model takes `model` and answers its id (zetta#648).
            return Result(json{{"model_id", input.value("model", std::string())},
                               {"exposed", served_model["exposed"]}});
        }
        if (capability == "read_model_weights") {
            const std::size_t offset = input.value("offset", std::size_t{0});
            const std::size_t limit = input.value("limit", std::size_t{8} << 20);
            const std::string chunk = served_weights.substr(offset, limit);
            const std::size_t end = offset + chunk.size();
            return Result(json{{"model_id", input.value("model_id", std::string())},
                               {"offset", offset},
                               {"chunk_base64", lake::test_base64_encode(chunk)},
                               {"next_offset", end < served_weights.size() ? json(end) : json(nullptr)},
                               {"total_bytes", served_weights.size()},
                               {"sha256", lake::control::identity_crypto::sha256_hex(served_weights)}});
        }
        if (capability == "run") {
            // A pipeline whose step is a workspace methodology: the engine hands it to Lake's
            // runner (zetta#1261).
            return Result(std::unexpected(lake::control::ZettaControlError{
                "runner_required",
                "this pipeline's methodology runs in Lake's methodology runner",
                "",
                {{"methodology", input.value("methodology_under_test", std::string())},
                 {"dataset_path", "sales.payers"},
                 {"params", {{"epochs", 20}}},
                 {"target", "late"},
                 {"holdout_fraction", 0.25},
                 {"project", "p-1"},
                 {"project_name", "collections"},
                 {"graph", "g-1"},
                 {"graph_name", "late payers"}}}));
        }
        if (capability == "sql") {
            const std::string statement = input.value("statement", std::string());
            if (statement.starts_with("SELECT COUNT(*) FROM sales.payers")) {
                return Result(json{{"columns", {"count"}}, {"rows", {{{"count", 3}}}}});
            }
            if (statement.starts_with("SELECT * FROM sales.payers")) {
                return Result(json{{"columns", {"days", "amount", "late"}},
                                   {"rows",
                                    {{{"days", 30}, {"amount", 10.5}, {"late", 1}},
                                     {{"days", 3}, {"amount", 2.0}, {"late", 0}},
                                     {{"days", 45}, {"amount", 99.0}, {"late", 1}}}}});
            }
        }
        return std::nullopt;
    };

    // Create one from a description: a private repository with a drafted manifest, Python and lock.
    const auto created = t.call(
        first,
        "create_methodology",
        in(first, 0, {{"name", "Late payers"}, {"description", "Which customers will pay late?"}}));
    ASSERT_TRUE(created.ok) << created.error.code << ": " << created.error.message;
    const json entry = created.value["methodology"];
    const std::string id = entry["id"];
    EXPECT_TRUE(lake::control::workspace_methodology_id(id));
    EXPECT_EQ(entry["source"], "workspace");
    EXPECT_EQ(entry["editable"], true);
    EXPECT_EQ(entry["runnable"], false) << "not validated yet";
    EXPECT_EQ(created.value["ready_to_validate"], true);
    const std::string repository_id = entry["code"]["repository_id"];
    const auto repository = t.store->get_repository(repository_id);
    ASSERT_TRUE(repository);
    EXPECT_EQ(repository->name, "methodology-late-payers");
    EXPECT_EQ(repository->workspace_id, first.workspaces[0]);
    EXPECT_EQ(entry["code"]["revision"], repositories.repositories[repository_id].back().id);
    EXPECT_TRUE(repositories.repositories[repository_id].back().files.contains("methodology.py"));
    EXPECT_EQ(entry["target"]["kind"], "binary");

    // Read it with its source; Orama's reads read-only.
    const auto read =
        t.call(first, "get_methodology", in(first, 0, {{"id", id}, {"include_source", true}}));
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["source"]["files"].size(), 3U);
    EXPECT_EQ(read.value["source"]["available"], true);
    const auto orama = t.call(first, "get_methodology", in(first, 0, {{"id", "M-1"}}));
    ASSERT_TRUE(orama.ok);
    EXPECT_EQ(orama.value["methodology"]["source"], "orama");
    EXPECT_EQ(orama.value["methodology"]["editable"], false);

    // Orama's are read-only, refused by the server with the reason.
    for (const std::string capability : {"update_methodology", "delete_methodology"}) {
        json change = in(first, 0, {{"id", "M-1"}});
        if (capability == "update_methodology") {
            change["name"] = "Mine now";
        }
        const auto refused = t.call(first, capability, change);
        EXPECT_EQ(refused.error.code, "forbidden") << capability;
        EXPECT_NE(refused.error.message.find("duplicate_methodology"), std::string::npos);
    }

    // Duplicate Orama's: its description, with a template, in a new repository of the workspace.
    const auto copied = t.call(first, "duplicate_methodology", in(first, 0, {{"id", "M-1"}}));
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["methodology"]["duplicated_from"], "M-1");
    EXPECT_NE(copied.value["methodology"]["description"].get<std::string>().find(
                  "Duplicated from Orama's M-1"),
              std::string::npos);
    // Duplicate one of the workspace's own: its Python source is copied.
    const auto again = t.call(
        first, "duplicate_methodology", in(first, 0, {{"id", id}, {"name", "Late payers, EMEA"}}));
    ASSERT_TRUE(again.ok) << again.error.message;
    const std::string copy_repository = again.value["methodology"]["code"]["repository_id"];
    EXPECT_NE(copy_repository, repository_id);
    EXPECT_EQ(repositories.repositories[copy_repository].back().files.at("methodology.py"),
              repositories.repositories[repository_id].back().files.at("methodology.py"));
    EXPECT_EQ(again.value["methodology"]["name"], "Late payers, EMEA");

    // Not usable before Validate; training is refused with the reason.
    const auto early =
        t.call(first,
               "run_methodology",
               in(first, 0, {{"id", id}, {"dataset", "sales.payers"}, {"target", "late"}}));
    EXPECT_EQ(early.error.code, "conflict");

    // Validate a broken methodology: what is missing is answered at once, and nothing runs.
    const auto broken = t.call(
        first,
        "create_methodology",
        in(first,
           0,
           {{"name", "Broken"},
            {"files",
             json::array(
                 {{{"path", "methodology.json"},
                   {"contents",
                    repositories.repositories[repository_id].back().files.at("methodology.json")}},
                  {{"path", "methodology.py"},
                   {"contents", "def fit(data, params):\n    return 1\n"}},
                  {{"path", "requirements.lock"}, {"contents", "numpy==2.1.3\n"}}})}}));
    ASSERT_TRUE(broken.ok) << broken.error.message;
    EXPECT_EQ(broken.value["ready_to_validate"], false);
    const std::string broken_id = broken.value["methodology"]["id"];
    const auto refused = t.call(first, "validate_methodology", in(first, 0, {{"id", broken_id}}));
    ASSERT_TRUE(refused.ok) << refused.error.message;
    EXPECT_EQ(refused.value["ready"], false);
    EXPECT_TRUE(refused.value["run"].is_null());
    std::set<std::string> failing;
    for (const json& finding : refused.value["findings"]) {
        if (!finding["ok"].get<bool>()) {
            failing.insert(finding["check"].get<std::string>());
        }
    }
    EXPECT_EQ(failing,
              (std::set<std::string>{
                  "entrypoint.explain", "entrypoint.predict", "entrypoint.score", "packages"}));
    EXPECT_EQ(own[broken_id]["validation"]["status"], "failed");
    EXPECT_TRUE(
        t.store->list_methodology_runs(first.customer_id, first.workspaces[0], broken_id, 10)
            ->empty());

    // Validate a good one: its image is built by the build path, and a validate run is queued.
    const auto validating = t.call(first, "validate_methodology", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(validating.ok) << validating.error.code << ": " << validating.error.message;
    EXPECT_EQ(validating.value["ready"], true);
    const std::string validate_run = validating.value["run"]["id"];
    const auto row = t.store->get_methodology_run(validate_run);
    ASSERT_TRUE(row);
    EXPECT_EQ(row->mode, "validate");
    EXPECT_EQ(row->runtime_image, "registry.test/oramasearch/methodology-runtime:1");
    EXPECT_FALSE(row->network) << "no network unless granted";
    EXPECT_FALSE(row->build_id.empty());
    const auto build = t.store->get_build(row->build_id);
    ASSERT_TRUE(build);
    EXPECT_EQ(build->revision, entry["code"]["revision"].get<std::string>());
    EXPECT_TRUE(build->dockerfile.starts_with("inline:ARG METHODOLOGY_RUNTIME_IMAGE"));
    EXPECT_EQ(build->build_args.value("METHODOLOGY_RUNTIME_IMAGE", std::string()),
              "registry.test/oramasearch/methodology-runtime:1");
    EXPECT_EQ(own[id]["validation"]["status"], "running");

    // The operator reports the run with the credential it generated for the stage and collect pods.
    const std::string secret = "lmr." + validate_run + "." + std::string(48, 'a');
    const auto report =
        [&](const std::string& run, const std::string& phase, const std::string& credential) {
            lake::control::MethodologyRunStatus status;
            status.phase = phase;
            status.credential_sha256 = credential.empty()
                                           ? std::string()
                                           : lake::control::identity_crypto::sha256_hex(credential);
            if (phase == "Collecting" || phase == "Succeeded") {
                status.started_at = "2026-10-11T10:00:00Z";
                status.finished_at = "2026-10-11T10:01:30Z";
                status.exit_code = 0;
            }
            ASSERT_TRUE(t.store->put_methodology_run_status(run, status));
        };
    report(validate_run, "Staging", secret);
    const auto runner = [&](const std::string& credential) {
        lake::api::ApiContext context;
        context.store = t.store.get();
        context.workspaces = t.service.get();
        context.instance_kek = std::string(kek);
        context.sync_store = &repositories;
        const auto principal = t.store->context_for(credential);
        if (principal) {
            context.authorized = true;
            context.session_credential = true;
            context.acting_user_id = principal->user_id;
            context.acting_identity = principal->taap_id;
            context.credential_session_id = principal->session_id;
            context.credential_kind = principal->kind;
            context.credential_workload_id = principal->workload_id;
            context.credential_customer_id = principal->workload_customer_id;
            context.credential_token_hash = principal->workload_token_hash;
            context.scopes = principal->scopes;
        }
        context.request_id = lake::control::new_uuid_v4();
        return context;
    };
    const auto as_runner =
        [&](const std::string& credential, const std::string& capability, json input) {
            auto context = runner(credential);
            const auto completed =
                liquid::runtime::block_on(t.registry.call_async(capability, input, context));
            EXPECT_TRUE(completed);
            return *completed;
        };
    ASSERT_TRUE(t.store->context_for(secret)) << "the credential the operator reported resolves";
    EXPECT_FALSE(t.store->context_for("lmr." + validate_run + "." + std::string(48, 'b')))
        << "a wrong secret is no credential";

    // Data in: the spec and the validation sample, a page at a time.
    const auto page =
        as_runner(secret, "methodology_run_input", {{"run_id", validate_run}, {"limit", 150}});
    ASSERT_TRUE(page.ok) << page.error.code << ": " << page.error.message;
    EXPECT_EQ(page.value["spec"]["mode"], "validate");
    EXPECT_EQ(page.value["spec"]["target"], "target");
    EXPECT_EQ(page.value["spec"]["network"], false);
    EXPECT_EQ(page.value["rows"].size(), 150U);
    EXPECT_EQ(page.value["total_rows"], 200);
    EXPECT_EQ(page.value["next_offset"], 150);
    const auto rest =
        as_runner(secret, "methodology_run_input", {{"run_id", validate_run}, {"offset", 150}});
    EXPECT_EQ(rest.value["rows"].size(), 50U);
    EXPECT_TRUE(rest.value["next_offset"].is_null());

    // The run's credential reaches nothing else: no workspace, no other run, no repository.
    for (const auto& [capability, input] : std::vector<std::pair<std::string, json>>{
             {"workspace_query",
              in(first, 0, {{"capability", "list_datasets"}, {"input", json::object()}})},
             {"get_methodology", in(first, 0, {{"id", id}})},
             {"list_methodology_runs", in(first, 0, json::object())},
             {"repository_file",
              {{"id", repository_id}, {"ref", "main"}, {"path", "methodology.py"}}},
             {"methodology_run_input", {{"run_id", lake::control::new_uuid_v4()}}}}) {
        const auto denied = as_runner(secret, capability, input);
        EXPECT_FALSE(denied.ok) << capability;
        EXPECT_EQ(denied.error.code, "unauthorized") << capability;
    }
    // And a person cannot call the runner's capabilities.
    const auto person = t.call(first, "methodology_run_input", {{"run_id", validate_run}});
    EXPECT_FALSE(person.ok);

    // Result out, once: Validate passes and the methodology becomes usable.
    report(validate_run, "Collecting", secret);
    const json passed{{"status", "ok"},
                      {"findings",
                       json::array({{{"check", "interface"},
                                     {"ok", true},
                                     {"message", "fit, predict and score answer"}}})},
                      {"metrics", {{"in_sample_auc", 0.97}, {"held_out_auc", 0.91}}},
                      {"rows", {{"train", 160}, {"held_out", 40}}}};
    const auto out = as_runner(secret,
                               "methodology_run_output",
                               {{"run_id", validate_run}, {"result", passed}, {"log", "fitted\n"}});
    ASSERT_TRUE(out.ok) << out.error.code << ": " << out.error.message;
    EXPECT_TRUE(out.value["model_id"].is_null()) << "Validate keeps no model";
    EXPECT_EQ(own[id]["validation"]["status"], "passed");
    EXPECT_EQ(own[id]["validation"]["run_id"], validate_run);
    const auto twice =
        as_runner(secret, "methodology_run_output", {{"run_id", validate_run}, {"result", passed}});
    EXPECT_EQ(twice.error.code, "conflict");
    EXPECT_NE(twice.error.message.find("already recorded"), std::string::npos);
    report(validate_run, "Succeeded", "");
    EXPECT_FALSE(t.store->context_for(secret)) << "a finished run's credential is gone";

    const auto listed =
        t.call(first, "list_methodology_runs", in(first, 0, {{"methodology_id", id}}));
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["runs"].size(), 1U);
    EXPECT_EQ(listed.value["runs"][0]["phase"], "Succeeded");
    EXPECT_EQ(listed.value["runs"][0]["outcome"], "passed");

    // Train: parameters are checked, the dataset is read through the engine, the model registered.
    const auto bad_params = t.call(first,
                                   "run_methodology",
                                   in(first,
                                      0,
                                      {{"id", id},
                                       {"dataset", "sales.payers"},
                                       {"target", "late"},
                                       {"params", {{"epochs", "many"}}}}));
    EXPECT_EQ(bad_params.error.code, "invalid_input");
    EXPECT_EQ(bad_params.error.path, "/params/epochs");
    const auto no_target =
        t.call(first, "run_methodology", in(first, 0, {{"id", id}, {"dataset", "sales.payers"}}));
    EXPECT_EQ(no_target.error.path, "/target");
    const auto training = t.call(first,
                                 "run_methodology",
                                 in(first,
                                    0,
                                    {{"id", id},
                                     {"dataset", "sales.payers"},
                                     {"target", "late"},
                                     {"params", {{"epochs", 50}}}}));
    ASSERT_TRUE(training.ok) << training.error.code << ": " << training.error.message;
    const std::string train_run = training.value["run"]["id"];
    EXPECT_EQ(t.store->get_methodology_run(train_run)->build_id, row->build_id)
        << "the validated revision's image is used again";
    const std::string train_secret = "lmr." + train_run + "." + std::string(48, 'c');
    report(train_run, "Staging", train_secret);
    const auto data = as_runner(train_secret, "methodology_run_input", {{"run_id", train_run}});
    ASSERT_TRUE(data.ok) << data.error.message;
    EXPECT_EQ(data.value["total_rows"], 3);
    EXPECT_EQ(data.value["spec"]["features"], (json{"days", "amount"}));
    EXPECT_EQ(data.value["spec"]["params"]["epochs"], 50);
    EXPECT_EQ(data.value["rows"][0]["days"], 30);
    EXPECT_FALSE(as_runner(secret, "methodology_run_input", {{"run_id", train_run}}).ok)
        << "one run's credential never reads another's data";
    report(train_run, "Collecting", train_secret);
    const auto trained =
        as_runner(train_secret,
                  "methodology_run_output",
                  {{"run_id", train_run},
                   {"result",
                    {{"status", "ok"},
                     {"metrics", {{"in_sample_auc", 0.99}, {"held_out_auc", 0.88}, {"note", "x"}}},
                     {"rows", {{"train", 2}, {"held_out", 1}}},
                     {"artifact_format", "pickle"}}},
                   {"artifact_base64", "aGVsbG8="}});
    ASSERT_TRUE(trained.ok) << trained.error.code << ": " << trained.error.message;
    EXPECT_EQ(trained.value["model_id"], "model-1");
    ASSERT_EQ(recorded_runs.size(), 1U);
    EXPECT_EQ(recorded_runs[0]["methodology"], id);
    EXPECT_EQ(recorded_runs[0]["artifact_base64"], "aGVsbG8=");
    EXPECT_EQ(recorded_runs[0]["metrics"], (json{{"in_sample_auc", 0.99}, {"held_out_auc", 0.88}}))
        << "only numbers named in_sample_ or held_out_ are metrics";
    EXPECT_EQ(recorded_runs[0]["dataset_path"], "sales.payers");
    const auto got = t.call(first, "get_methodology_run", in(first, 0, {{"run_id", train_run}}));
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value["run"]["model_id"], "model-1");
    EXPECT_EQ(got.value["run"]["result"]["dropped_metrics"], (json{"note"}));

    // An artifact over 24 MiB moves in chunks before the output (lake#690): Lake forwards each to
    // the engine, which stages it; the output names its size and SHA-256.
    const auto large = t.call(first,
                              "run_methodology",
                              in(first, 0, {{"id", id}, {"dataset", "sales.payers"}, {"target", "late"}}));
    ASSERT_TRUE(large.ok) << large.error.code << ": " << large.error.message;
    const std::string large_run = large.value["run"]["id"];
    const std::string large_secret = "lmr." + large_run + "." + std::string(48, 'e');
    report(large_run, "Collecting", large_secret);
    const std::string artifact = "0123456789abcdef0123";
    const auto chunk = [&](std::size_t offset, std::size_t length) {
        return as_runner(large_secret,
                         "methodology_run_artifact",
                         {{"run_id", large_run},
                          {"offset", offset},
                          {"chunk_base64", lake::test_base64_encode(artifact.substr(offset, length))}});
    };
    const auto first_chunk = chunk(0, 12);
    ASSERT_TRUE(first_chunk.ok) << first_chunk.error.code << ": " << first_chunk.error.message;
    EXPECT_EQ(first_chunk.value["received"], 12);
    EXPECT_EQ(chunk(15, 5).error.code, "conflict") << "a gap names where to resume";
    EXPECT_EQ(chunk(12, 8).value["received"], 20);
    EXPECT_EQ(staged_artifact, artifact);
    EXPECT_EQ(as_runner(train_secret,
                        "methodology_run_artifact",
                        {{"run_id", large_run}, {"offset", 0}, {"chunk_base64", "AAAA"}})
                  .error.code,
              "unauthorized")
        << "one run's credential never sends another's artifact";
    const json large_result{{"status", "ok"},
                            {"metrics", {{"held_out_auc", 0.8}}},
                            {"artifact_format", "pickle"}};
    const std::string digest = lake::control::identity_crypto::sha256_hex(artifact);
    EXPECT_EQ(as_runner(large_secret,
                        "methodology_run_output",
                        {{"run_id", large_run},
                         {"result", large_result},
                         {"artifact_base64", "aGVsbG8="},
                         {"artifact", {{"bytes", 20}, {"sha256", digest}}}})
                  .error.code,
              "invalid_input")
        << "inline or in chunks, not both";
    EXPECT_EQ(as_runner(large_secret,
                        "methodology_run_output",
                        {{"run_id", large_run},
                         {"result", large_result},
                         {"artifact", {{"bytes", 20}, {"sha256", "not-hex"}}}})
                  .error.path,
              "/artifact/sha256");
    const auto large_out = as_runner(large_secret,
                                     "methodology_run_output",
                                     {{"run_id", large_run},
                                      {"result", large_result},
                                      {"artifact", {{"bytes", 20}, {"sha256", digest}}}});
    ASSERT_TRUE(large_out.ok) << large_out.error.code << ": " << large_out.error.message;
    ASSERT_EQ(recorded_runs.size(), 2U);
    EXPECT_EQ(recorded_runs[1]["artifact_staged"], (json{{"bytes", 20}, {"sha256", digest}}));
    EXPECT_FALSE(recorded_runs[1].contains("artifact_base64"));
    EXPECT_EQ(chunk(0, 4).error.code, "conflict") << "no chunk after the result is recorded";

    // Serving its model from Ship → Models (lake#690): publishing starts a deployment the
    // operator runs; predict goes to the serve pod through Lake.
    served_model = {{"id", "model-1"},
                    {"kind", "python:" + id},
                    {"exposed", false},
                    {"feature_schema",
                     {{"methodology", id},
                      {"lake_run_id", train_run},
                      {"features", {"days", "amount"}},
                      {"artifact_format", "pickle"}}}};
    std::vector<lake::api::MethodologyServeRequest> served_requests;
    t.methodology_server = [&](const lake::api::MethodologyServeRequest& request) {
        served_requests.push_back(request);
        const json rows = json::parse(request.body).at("rows");
        json predictions = json::array();
        for (std::size_t index = 0; index < rows.size(); ++index) {
            predictions.push_back(0.25 * static_cast<double>(index + 1));
        }
        return lake::api::MethodologyServeResponse{
            200, json{{"predictions", predictions}, {"model_id", "model-1"}}.dump(), ""};
    };
    const json predict_input = in(
        first,
        0,
        {{"model_id", "model-1"},
         {"rows", json::array({{{"entity", "c-1"}, {"days", 30}, {"amount", 1.0}}, {{"days", 2}}})}});
    const auto published = t.call(
        first,
        "workspace_act",
        in(first, 0, {{"capability", "expose_model"}, {"input", {{"model_id", "model-1"}}}}));
    ASSERT_TRUE(published.ok) << published.error.code << ": " << published.error.message;
    EXPECT_EQ(published.value["deployment"]["phase"], "Pending");
    const std::string deployment_id = published.value["deployment"]["id"];
    const auto deployment = t.store->get_methodology_deployment(deployment_id);
    ASSERT_TRUE(deployment);
    EXPECT_EQ(deployment->model_id, "model-1");
    EXPECT_EQ(deployment->methodology_id, id);
    EXPECT_EQ(deployment->run_id, train_run);
    EXPECT_EQ(deployment->build_id, row->build_id) << "the image that trained it serves it";
    EXPECT_EQ(deployment->runtime_image, "registry.test/oramasearch/methodology-runtime:1");
    const auto again_published = t.call(
        first,
        "workspace_act",
        in(first, 0, {{"capability", "expose_model"}, {"input", {{"model_id", "model-1"}}}}));
    ASSERT_TRUE(again_published.ok) << again_published.error.code << ": " << again_published.error.message;
    EXPECT_EQ(again_published.value["deployment"]["id"], deployment_id) << "one per model";
    const auto not_yet = t.call(first, "predict", predict_input);
    EXPECT_EQ(not_yet.error.code, "conflict");
    EXPECT_NE(not_yet.error.message.find("not serving yet"), std::string::npos);
    EXPECT_TRUE(served_requests.empty());

    // The operator stages it with the deployment's credential, which reaches only its input.
    const std::string deployment_secret = "lmd." + deployment_id + "." + std::string(48, 'd');
    lake::control::MethodologyDeploymentStatus staging;
    staging.phase = "Staging";
    staging.credential_sha256 = lake::control::identity_crypto::sha256_hex(deployment_secret);
    ASSERT_TRUE(t.store->put_methodology_deployment_status(deployment_id, staging));
    const auto model_page = as_runner(deployment_secret,
                                      "methodology_deployment_input",
                                      {{"deployment_id", deployment_id}, {"limit", 15000}});
    ASSERT_TRUE(model_page.ok) << model_page.error.code << ": " << model_page.error.message;
    EXPECT_EQ(model_page.value["spec"]["model_id"], "model-1");
    EXPECT_EQ(model_page.value["spec"]["methodology_id"], id);
    EXPECT_EQ(model_page.value["spec"]["features"], (json{"days", "amount"}));
    EXPECT_EQ(model_page.value["spec"]["artifact_format"], "pickle");
    EXPECT_FALSE(model_page.value["spec"]["manifest"].empty());
    EXPECT_EQ(model_page.value["total_bytes"], 20000);
    EXPECT_EQ(model_page.value["next_offset"], 15000);
    const auto model_rest = as_runner(deployment_secret,
                                      "methodology_deployment_input",
                                      {{"deployment_id", deployment_id}, {"offset", 15000}});
    ASSERT_TRUE(model_rest.ok) << model_rest.error.code << ": " << model_rest.error.message;
    EXPECT_TRUE(model_rest.value["next_offset"].is_null());
    EXPECT_EQ(lake::test_base64_decode(model_page.value["chunk_base64"].get<std::string>()) +
                  lake::test_base64_decode(model_rest.value["chunk_base64"].get<std::string>()),
              served_weights);
    for (const auto& [capability, input] : std::vector<std::pair<std::string, json>>{
             {"methodology_run_input", {{"run_id", train_run}}},
             {"get_methodology", in(first, 0, {{"id", id}})},
             {"methodology_deployment_input", {{"deployment_id", lake::control::new_uuid_v4()}}}}) {
        EXPECT_EQ(as_runner(deployment_secret, capability, input).error.code, "unauthorized")
            << capability;
    }
    EXPECT_EQ(as_runner(train_secret,
                        "methodology_deployment_input",
                        {{"deployment_id", deployment_id}})
                  .error.code,
              "unauthorized")
        << "a run's credential never reads a model";
    EXPECT_FALSE(t.call(first, "methodology_deployment_input", {{"deployment_id", deployment_id}}).ok)
        << "nor does a person";

    // The operator's endpoint is a serve Service, nothing else.
    lake::control::MethodologyDeploymentStatus ready = staging;
    ready.phase = "Ready";
    ready.endpoint = "http://169.254.169.254";
    EXPECT_FALSE(t.store->put_methodology_deployment_status(deployment_id, ready));
    ready.endpoint = "http://" + deployment->slug + ".lake-prod-methodology-runs.svc.cluster.local";
    ASSERT_TRUE(t.store->put_methodology_deployment_status(deployment_id, ready));
    const auto served = t.call(first, "get_methodology_deployment", in(first, 0, {{"model_id", "model-1"}}));
    ASSERT_TRUE(served.ok) << served.error.message;
    EXPECT_EQ(served.value["deployment"]["phase"], "Ready");
    EXPECT_EQ(served.value["deployment"]["ready"], true);
    EXPECT_FALSE(served.value["deployment"].contains("endpoint")) << "where it runs is the operator's";

    const auto predicted = t.call(first, "predict", predict_input);
    ASSERT_TRUE(predicted.ok) << predicted.error.code << ": " << predicted.error.message;
    EXPECT_EQ(predicted.value["runner"], "methodology");
    ASSERT_EQ(predicted.value["predictions"].size(), 2U);
    EXPECT_EQ(predicted.value["predictions"][0]["entity"], "c-1");
    EXPECT_EQ(predicted.value["predictions"][0]["prediction"], 0.25);
    EXPECT_EQ(predicted.value["predictions"][1]["row"], 1);
    ASSERT_EQ(served_requests.size(), 1U);
    EXPECT_EQ(served_requests[0].endpoint, ready.endpoint);
    EXPECT_EQ(served_requests[0].path, "/predict");
    t.methodology_server = [](const lake::api::MethodologyServeRequest&) {
        return lake::api::MethodologyServeResponse{
            422,
            json{{"error", {{"code", "methodology_error"}, {"message", "predict raised KeyError"}}}}
                .dump(),
            ""};
    };
    const auto raised = t.call(first, "predict", predict_input);
    EXPECT_EQ(raised.error.code, "invalid_input");
    EXPECT_EQ(raised.error.message, "predict raised KeyError");
    EXPECT_EQ(t.call(second, "predict", predict_input).error.code.empty(), false)
        << "another account cannot call it";

    // Unpublishing stops it.
    const auto unpublished = t.call(
        first,
        "workspace_act",
        in(first, 0, {{"capability", "unexpose_model"}, {"input", {{"model_id", "model-1"}}}}));
    ASSERT_TRUE(unpublished.ok) << unpublished.error.code << ": " << unpublished.error.message;
    EXPECT_EQ(unpublished.value["deployment"]["phase"], "Stopped");
    EXPECT_EQ(t.store->get_methodology_deployment(deployment_id)->desired, "absent");
    const auto listed_deployments =
        t.call(first, "list_methodology_deployments", in(first, 0, json::object()));
    ASSERT_TRUE(listed_deployments.ok) << listed_deployments.error.message;
    EXPECT_EQ(listed_deployments.value["deployments"].size(), 1U);
    EXPECT_FALSE(t.store->context_for(deployment_secret)) << "a stopped deployment's credential is gone";

    // A pipeline using it as a step: the engine's run hands it to the runner, as a training run
    // of that pipeline.
    const auto step = t.call(first,
                             "workspace_act",
                             in(first,
                                0,
                                {{"capability", "run"},
                                 {"input",
                                  {{"project", "collections"},
                                   {"graph", "late payers"},
                                   {"methodology_under_test", id}}}}));
    ASSERT_TRUE(step.ok) << step.error.code << ": " << step.error.message;
    EXPECT_EQ(step.value["runner"], "lake");
    const std::string step_run = step.value["methodology_run"]["id"];
    const auto step_record =
        t.call(first, "get_methodology_run", in(first, 0, {{"run_id", step_run}}));
    ASSERT_TRUE(step_record.ok) << step_record.error.message;
    EXPECT_EQ(step_record.value["run"]["project"], "collections");
    EXPECT_EQ(step_record.value["run"]["graph"], "late payers");
    EXPECT_EQ(step_record.value["run"]["params"]["epochs"], 20);
    EXPECT_EQ(step_record.value["run"]["holdout_fraction"], 0.25);
    ASSERT_TRUE(t.store->withdraw_methodology_run(step_run, "test"));

    // An experiment over its parameters (lake#689): the engine admits trials that wait for Lake's
    // runner; Lake runs each as a training run, once, and reports the result back.
    const auto started = t.call(first,
                                "start_experiment",
                                in(first,
                                   0,
                                   {{"name", "late payers tuning"},
                                    {"methodology", id},
                                    {"dataset", "sales.payers"},
                                    {"target", "late"},
                                    {"from_defaults", true},
                                    {"confirm", true}}));
    ASSERT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
    EXPECT_EQ(started.value["spec"]["target"], "late");
    ASSERT_EQ(started.value["runner"].size(), 2U);
    ASSERT_EQ(started.value["methodology_runs"].size(), 2U);
    EXPECT_EQ(started.value["runner"][0]["experiment_id"], "study-1");
    ASSERT_TRUE(started.value["runner"][0]["methodology_run"]["run_id"].is_string())
        << started.value.dump();
    const std::string trial_run = started.value["runner"][0]["methodology_run"]["run_id"];
    ASSERT_TRUE(started.value["runner"][1]["methodology_run"]["run_id"].is_string());
    const std::string failing_run = started.value["runner"][1]["methodology_run"]["run_id"];
    const auto trial_record =
        t.call(first, "get_methodology_run", in(first, 0, {{"run_id", trial_run}}));
    ASSERT_TRUE(trial_record.ok) << trial_record.error.message;
    EXPECT_EQ(trial_record.value["run"]["experiment_id"], "study-1");
    EXPECT_EQ(trial_record.value["run"]["trial"], "trial-run-0");
    EXPECT_EQ(trial_record.value["run"]["params"]["epochs"], 10);
    EXPECT_EQ(trial_record.value["run"]["mode"], "train");
    EXPECT_EQ(t.store->get_methodology_run(trial_run)->build_id, row->build_id)
        << "a trial runs the validated revision's image";
    // Reading it again dispatches nothing twice.
    const auto reread =
        t.call(first, "get_experiment", in(first, 0, {{"experiment_id", "study-1"}}));
    ASSERT_TRUE(reread.ok) << reread.error.code << ": " << reread.error.message;
    EXPECT_EQ(reread.value["methodology_runs"].size(), 2U);
    EXPECT_EQ(reread.value["runner"][0]["methodology_run"]["run_id"], trial_run);

    // Its result completes the trial; the engine admits the third, which Lake queues at once, for
    // the person who started the experiment.
    const std::string trial_secret = "lmr." + trial_run + "." + std::string(48, 'd');
    report(trial_run, "Staging", trial_secret);
    report(trial_run, "Collecting", trial_secret);
    const auto trial_out =
        as_runner(trial_secret,
                  "methodology_run_output",
                  {{"run_id", trial_run},
                   {"result",
                    {{"status", "ok"},
                     {"metrics", {{"in_sample_auc", 0.9}, {"held_out_auc", 0.8}}},
                     {"rows", {{"train", 2}, {"held_out", 1}}}}},
                   {"artifact_base64", "aGVsbG8="}});
    ASSERT_TRUE(trial_out.ok) << trial_out.error.code << ": " << trial_out.error.message;
    const json& trial_recorded = recorded_runs.back();
    EXPECT_EQ(trial_recorded["study"], "study-1");
    EXPECT_EQ(trial_recorded["trial"], "trial-run-0");
    EXPECT_EQ(trial_recorded["duration_seconds"], 90);
    const auto after =
        t.call(first, "get_experiment", in(first, 0, {{"experiment_id", "study-1"}}));
    ASSERT_TRUE(after.ok) << after.error.message;
    ASSERT_EQ(after.value["methodology_runs"].size(), 3U);
    ASSERT_TRUE(after.value["methodology_runs"][2]["run_id"].is_string()) << after.value.dump();
    const std::string third_run = after.value["methodology_runs"][2]["run_id"];
    EXPECT_EQ(t.store->get_methodology_run(third_run)->created_by_user_id,
              t.store->get_methodology_run(trial_run)->created_by_user_id);

    // A run that ends without a result ends its trial with the reason.
    lake::control::MethodologyRunStatus crashed;
    crashed.phase = "Failed";
    crashed.message = "the methodology's code exited with 1";
    ASSERT_TRUE(t.store->put_methodology_run_status(failing_run, crashed));
    const auto reconciled =
        t.call(first, "get_experiment", in(first, 0, {{"experiment_id", "study-1"}}));
    ASSERT_TRUE(reconciled.ok) << reconciled.error.message;
    ASSERT_FALSE(study_calls.empty());
    EXPECT_EQ(study_calls.back()["run"], "trial-run-1");
    EXPECT_EQ(study_calls.back()["reason"], "the methodology's code exited with 1");

    // Stopping the experiment stops its trials' runs.
    const auto stopped = t.call(
        first, "stop_experiment", in(first, 0, {{"experiment_id", "study-1"}, {"confirm", true}}));
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(t.store->get_methodology_run(third_run)->desired, "absent");

    // The run's record is sealed in the workspace Zetta; the dispatch row holds no content.
    for (const auto& [key, record] :
         t.zetta->records["http://" + first.workspaces[0] + ".test"].rows("methodology_runs")) {
        EXPECT_EQ(record.dump().find("sales.payers"), std::string::npos) << "sealed at rest";
        EXPECT_EQ(record.dump().find("lmr."), std::string::npos);
    }

    // Another workspace and another space see none of it.
    EXPECT_EQ(
        t.call(first, "get_methodology_run", in(first, 1, {{"run_id", train_run}})).error.code,
        "not_found");
    EXPECT_FALSE(t.call(second, "get_methodology_run", in(first, 0, {{"run_id", train_run}})).ok);
    EXPECT_FALSE(
        t.call(second, "update_methodology", in(first, 0, {{"id", id}, {"name", "x"}})).ok);

    // Edit: the code is committed, a new revision must be validated again.
    const auto edited =
        t.call(first,
               "update_methodology",
               in(first,
                  0,
                  {{"id", id},
                   {"name", "Late payers v2"},
                   {"files", json::array({{{"path", "notes.md"}, {"contents", "# Notes\n"}}})}}));
    ASSERT_TRUE(edited.ok) << edited.error.code << ": " << edited.error.message;
    EXPECT_EQ(edited.value["methodology"]["name"], "Late payers v2");
    EXPECT_EQ(edited.value["methodology"]["runnable"], false) << "a new revision needs Validate";
    EXPECT_TRUE(repositories.repositories[repository_id].back().files.contains("notes.md"));
    EXPECT_NE(repositories.repositories[repository_id]
                  .back()
                  .files.at("methodology.json")
                  .find("Late payers v2"),
              std::string::npos)
        << "the name is the manifest's";

    // Delete: its runs stop, its repository stays.
    const auto pending = t.call(first, "validate_methodology", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(pending.ok) << pending.error.message;
    const auto deleted = t.call(first, "delete_methodology", in(first, 0, {{"id", id}}));
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["repository_id"], repository_id);
    EXPECT_EQ(t.store->get_methodology_run(pending.value["run"]["id"])->desired, "absent");
    EXPECT_TRUE(t.store->get_repository(repository_id));
    EXPECT_FALSE(own.contains(id));
}
