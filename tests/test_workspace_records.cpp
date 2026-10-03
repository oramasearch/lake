// Lake's records in workspace Zettas (docs/architecture/workspace-records.md, lake#370), with Ask
// Orama conversations as the first collection and Design → Opportunities as the first mutable one
// (lake#376): sealing, structural isolation between workspaces and spaces, conditional writes,
// workspace deletion, and the operator's one-shot copy.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "api/forge_records_api.hpp"
#include "api/secret_records_api.hpp"
#include "control/secrets_crypto.hpp"
#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/metering_recorder.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/secret_records.hpp"
#include "lake/control/stream_owners.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/control/zetta_workspace.hpp"
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
    return {subject, subject + "@example.test", "", "browser", identity->taap_id,
            identity->public_key, identity->wrapped_seed};
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

    // A Discover scan Auto-Suggest finished in a workspace: one suggestion and one decline.
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
        EXPECT_TRUE(store->record_agent_task_event(
            task->id, "agent-autosuggest", "Succeeded", "",
            {{"ok", true}, {"result", {{"project_profile", profile}}}}));
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
        std::string schema = "workspace_records_" + lake::control::new_uuid_v4();
        std::erase(schema, '-');
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("SET client_min_messages TO warning");
        tx.exec("CREATE SCHEMA " + tx.quote_name(schema));
        tx.exec("SET search_path TO " + tx.quote_name(schema));
        std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
        tx.exec(std::string((std::istreambuf_iterator<char>(file)), {}));
        tx.commit();
        database_uri_ = std::string(uri) + " options='-csearch_path=" + schema + "'";
        uri_ = uri;
        schema_ = schema;
        return lake::control::make_postgres_store(database_uri_, std::string(kek));
    }
    void TearDown() override {
        if (schema_.empty()) {
            return;
        }
        pqxx::connection connection(uri_);
        pqxx::work tx(connection);
        tx.exec("DROP SCHEMA IF EXISTS " + tx.quote_name(schema_) + " CASCADE");
        tx.commit();
    }
    std::string database_uri_, uri_, schema_;
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
    EXPECT_TRUE(t.opportunities(first, 1, home_scan).empty())
        << "a scan of another workspace is not saved here, even when named";
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

TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesEachOpportunityToItsScansWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    const auto task = [&](const TwoSpaces::Space& space, const std::string& workspace_id) {
        const auto made = t.store->create_agent_task({.customer_id = space.customer_id,
                                                      .agent = "agent-autosuggest",
                                                      .kind = "discover",
                                                      .workspace_id = workspace_id,
                                                      .scopes = {"list_datasets"}});
        EXPECT_TRUE(made) << made.error().message;
        return made->id;
    };
    const std::string home_scan = task(first, first.workspaces[0]);
    const std::string lab_scan = task(first, first.workspaces[1]);
    // The second space has two workspaces; its scan's own workspace still owns its rows.
    const std::string theirs_scan = task(second, second.workspaces[1]);
    // The first space's lab is deleted: its scan's rows went with it and are only reported.
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    const auto legacy_row = [](std::string customer, std::string scan, int rank, std::string status,
                               std::int64_t at_us) {
        lake::control::Opportunity row;
        row.id = lake::control::new_uuid_v4();
        row.customer_id = std::move(customer);
        row.task_id = std::move(scan);
        row.rank = rank;
        row.intent = "intent " + std::to_string(rank);
        row.solution = "S-" + std::to_string(rank);
        row.methodology = "gbdt";
        row.why = "a label exists";
        row.payload = {{"intent", row.intent}, {"required_first", {"entity resolution"}}};
        row.status = std::move(status);
        row.project = row.status == "project" ? "churn-1a2b" : "";
        row.at_us = at_us;
        return row;
    };
    std::vector<lake::control::Opportunity> legacy = {
        legacy_row(first.customer_id, home_scan, 1, "project", 1'790'000'000'000'001),
        legacy_row(first.customer_id, home_scan, 2, "not_yet", 1'790'000'000'000'001),
        legacy_row(first.customer_id, lab_scan, 1, "new", 1'790'000'000'000'002),
        legacy_row(second.customer_id, theirs_scan, 1, "dismissed", 1'790'000'000'000'003)};
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const auto& row : legacy) {
            tx.exec_params("INSERT INTO opportunities (id, customer_id, task_id, rank, intent, "
                           "solution, methodology, why, payload, status, project, created_at) "
                           "VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6, $7, $8, $9::jsonb, "
                           "$10, $11, to_timestamp($12::double precision / 1000000))",
                           row.id, row.customer_id, row.task_id, row.rank, row.intent,
                           row.solution, row.methodology, row.why, row.payload.dump(), row.status,
                           row.project, row.at_us);
        }
        tx.commit();
    } else {
        for (const auto& row : legacy) {
            static_cast<lake::control::InMemoryStore&>(*t.store).seed_legacy_opportunity(row);
        }
    }
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy = [&](std::string mode) {
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "opportunities"}, {"mode", mode}},
            operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };

    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 4);
    EXPECT_EQ(plan.value["assigned"], 3);
    ASSERT_EQ(plan.value["unresolved"].size(), 1U);
    EXPECT_EQ(plan.value["unresolved"][0]["id"], legacy[2].id);
    EXPECT_EQ(plan.value["unresolved"][0]["outcome"], "deleted_workspace");
    // ask_messages left the list when migration 0050 dropped its table (lake#483).
    const auto retired = liquid::runtime::block_on(t.registry.call_async(
        "copy_workspace_records", {{"collection", "ask_messages"}, {"mode", "plan"}},
        operator_context));
    ASSERT_TRUE(retired);
    EXPECT_FALSE(retired->ok);
    EXPECT_EQ(retired->error.code, "invalid_input");
    const std::string home_url = "http://" + first.workspaces[0] + ".test";
    const std::string theirs_url = "http://" + second.workspaces[1] + ".test";
    EXPECT_TRUE(t.zetta->records[home_url].collections.empty()) << "a plan writes nothing";

    const auto before = copy("verify");
    ASSERT_TRUE(before.ok);
    EXPECT_EQ(before.value["missing"].size(), 3U);
    EXPECT_FALSE(before.value["complete"]);

    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 3);
    EXPECT_EQ(t.zetta->records[home_url].rows("opportunities").size(), 2U);
    EXPECT_EQ(t.zetta->records[theirs_url].rows("opportunities").size(), 1U);
    EXPECT_TRUE(t.zetta->records["http://" + second.workspaces[0] + ".test"]
                    .rows("opportunities")
                    .empty())
        << "the space's other workspace gets none of the scan's rows";
    const auto rerun = copy("copy");
    EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
    EXPECT_EQ(rerun.value["already_present"], 3);

    const auto verified = copy("verify");
    ASSERT_TRUE(verified.ok);
    EXPECT_EQ(verified.value["verified"], 3);
    EXPECT_TRUE(verified.value["missing"].empty());
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_FALSE(verified.value["complete"]) << "the deleted workspace's row is still reported";

    // The person reads the copies back in the owning workspace, keyed by scan and rank, with
    // their decisions; a settle of the same scan afterwards writes nothing.
    auto home = t.opportunities(first, 0);
    ASSERT_EQ(home.size(), 2U);
    EXPECT_EQ(home[0]["id"], lake::control::opportunity_id(home_scan, 1));
    EXPECT_EQ(home[0]["status"], "project");
    EXPECT_EQ(home[0]["project"], "churn-1a2b");
    EXPECT_EQ(home[0]["detail"]["required_first"][0], "entity resolution");
    EXPECT_EQ(home[1]["status"], "not_yet");
    auto theirs = t.opportunities(second, 1);
    ASSERT_EQ(theirs.size(), 1U);
    EXPECT_EQ(theirs[0]["status"], "dismissed");

    // A status the person changes after the copy is theirs, not a difference; a changed scan row
    // is reported.
    const auto dismissed = t.call(first, "dismiss_opportunity",
                                  {{"customer_id", first.customer_id},
                                   {"workspace_id", first.workspaces[0]},
                                   {"opportunity_id", home[1]["id"]}});
    ASSERT_TRUE(dismissed.ok) << dismissed.error.message;
    EXPECT_TRUE(copy("verify").value["mismatched"].empty());
    auto& copied_rows = t.zetta->records[home_url].rows("opportunities");
    copied_rows.at(lake::control::opportunity_id(home_scan, 2))["value"] =
        copied_rows.at(lake::control::opportunity_id(home_scan, 1))["value"];
    EXPECT_EQ(copy("verify").value["missing"].size(), 1U) << "a row that no longer opens is missing";

    const auto refused = [&] {
        auto context = t.context(first);
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "opportunities"}, {"mode", "plan"}},
            context));
        return *completed;
    }();
    EXPECT_FALSE(refused.ok) << "only the operator copies";
}

TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesEachTopicCreatorIntoItsWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    using lake::control::StreamOwner;
    const std::vector<StreamOwner> legacy = {
        // Owned: each names a live workspace of its own space (two in the first space).
        {first.customer_id, first.workspaces[0], "orders", "taap:alice"},
        {first.customer_id, first.workspaces[0], "invoices", "taap:bob"},
        {first.customer_id, first.workspaces[1], "orders", "taap:carol"},
        {second.customer_id, second.workspaces[0], "orders", "taap:dave"},
        // Not copied: a workspace of another space, and a workspace id that never existed.
        {second.customer_id, first.workspaces[0], "stray", "taap:eve"},
        {second.customer_id, "workspace-one", "orders", "taap:eve"},
        // Its workspace is deleted below: the topic went with that incarnation.
        {second.customer_id, second.workspaces[1], "gone", "taap:dave"}};
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const StreamOwner& row : legacy) {
            tx.exec_params("INSERT INTO stream_owners (customer_id, workspace_id, topic, created_by) "
                           "VALUES ($1::uuid, $2, $3, $4)",
                           row.customer_id, row.workspace_id, row.topic, row.created_by);
        }
        tx.commit();
    } else {
        for (const StreamOwner& row : legacy) {
            static_cast<lake::control::InMemoryStore&>(*t.store).seed_legacy_stream_owner(row);
        }
    }
    // The second space's lab workspace was deleted after the rows were written: its topic went
    // with it, so its row is reported but does not hold the drop back.
    ASSERT_TRUE(t.store->update_workspace(second.workspaces[1], {.desired = "deleted"}));
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy = [&](std::string mode, std::string after = {}, int limit = 1000) {
        json input{{"collection", "stream_owners"}, {"mode", mode}, {"limit", limit}};
        if (!after.empty()) {
            input["after"] = after;
        }
        const auto completed = liquid::runtime::block_on(
            t.registry.call_async("copy_workspace_records", input, operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };

    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 7);
    EXPECT_EQ(plan.value["assigned"], 4);
    std::multiset<std::string> outcomes;
    for (const auto& row : plan.value["unresolved"]) {
        outcomes.insert(row["outcome"].get<std::string>());
    }
    EXPECT_EQ(outcomes, (std::multiset<std::string>{"unknown_workspace", "unknown_workspace",
                                                    "workspace_deleted"}));
    const std::string home_url = "http://" + first.workspaces[0] + ".test";
    EXPECT_TRUE(t.zetta->records[home_url].collections.empty()) << "a plan writes nothing";

    const auto before = copy("verify");
    EXPECT_EQ(before.value["missing"].size(), 4U);
    EXPECT_FALSE(before.value["complete"]);

    // Paged: two rows a page, following next_after.
    std::size_t copied = 0;
    std::string after;
    for (int page = 0; page < 10; ++page) {
        const auto result = copy("copy", after, 2);
        ASSERT_TRUE(result.ok) << result.error.message;
        copied += result.value["copied"].get<std::size_t>();
        if (result.value["next_after"].is_null()) {
            break;
        }
        after = result.value["next_after"];
    }
    EXPECT_EQ(copied, 4U);
    EXPECT_EQ(t.zetta->records[home_url].rows("stream_owners").size(), 2U);
    EXPECT_EQ(t.zetta->records["http://" + first.workspaces[1] + ".test"].rows("stream_owners").size(), 1U);
    EXPECT_EQ(t.zetta->records["http://" + second.workspaces[0] + ".test"].rows("stream_owners").size(), 1U);
    const auto rerun = copy("copy");
    EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
    EXPECT_EQ(rerun.value["already_present"], 4);

    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 4);
    EXPECT_TRUE(verified.value["missing"].empty());
    EXPECT_FALSE(verified.value["complete"]) << "rows naming an unknown workspace need review";

    // Each creator reads back in its own workspace only.
    const auto creator = [&](const auto& space, const std::string& workspace,
                             const std::string& topic) {
        auto context = t.context(space);
        auto records = lake::api::workspace_records(context, space.customer_id, workspace);
        EXPECT_TRUE(records);
        return *liquid::runtime::block_on(
                    lake::control::stream_creator(*records, space.customer_id, topic))
                    .value();
    };
    EXPECT_EQ(creator(first, first.workspaces[0], "orders"), "taap:alice");
    EXPECT_EQ(creator(first, first.workspaces[1], "orders"), "taap:carol");
    EXPECT_EQ(creator(second, second.workspaces[0], "orders"), "taap:dave");
    EXPECT_EQ(creator(first, first.workspaces[0], "stray"), "");

    // A record moved under another topic's key does not open there, so verify reports it.
    auto& rows = t.zetta->records[home_url].rows("stream_owners");
    ASSERT_EQ(rows.size(), 2U);
    const auto one = rows.begin();
    std::next(one)->second["value"] = one->second["value"];
    EXPECT_EQ(copy("verify").value["missing"].size(), 1U);

    // The rows naming an unknown workspace are the only blockers: once reviewed (removed here, as
    // dev data may be), the remaining pages verify complete, deleted-workspace rows included.
    rows.erase(std::next(one));
    ASSERT_TRUE(copy("copy").ok);
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM stream_owners WHERE customer_id = $1::uuid AND topic IN "
                       "('stray', 'orders') AND workspace_id <> $2",
                       second.customer_id, second.workspaces[0]);
        tx.commit();
        const auto done = copy("verify");
        EXPECT_EQ(done.value["unresolved"].size(), 1U);
        EXPECT_EQ(done.value["unresolved"][0]["outcome"], "workspace_deleted");
        EXPECT_TRUE(done.value["complete"]) << done.value.dump();
    }

    const auto refused = [&] {
        auto context = t.context(first);
        return *liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "stream_owners"}, {"mode", "plan"}}, context));
    }();
    EXPECT_FALSE(refused.ok) << "only the operator copies";
}

// lake#350: the retired drive_share_links rows move into the workspace that owns each one, keep
// their ids (so the `share-link:<id>` grant still names them) and become reachable from a
// pre-move token through the content-free route.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesDriveShareLinksAndRoutesTheirTokens) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    std::vector<lake::control::DriveShareLink> legacy;
    std::int64_t at = 1'790'000'000'000'000;
    for (auto* space : {&first, &second}) {
        const auto repository =
            t.store->create_repository({space->customer_id, "drive", "", "private",
                                        lake::test::repository_workspace(
                                            *t.store, space->customer_id)});
        ASSERT_TRUE(repository) << repository.error().message;
        const auto parent = t.store->find_policy_object("repository", repository->id);
        ASSERT_TRUE(parent);
        const std::string node = "22222222222222222222222222222222";
        const auto folder = t.store->create_policy_object({"path", repository->id + ":" + node,
                                                           parent->id, space->principal.taap_id,
                                                           lake::control::new_uuid_v4(), true});
        ASSERT_TRUE(folder) << folder.error().message;
        lake::control::DriveShareLink link;
        link.id = lake::control::new_uuid_v4();
        link.repository_id = repository->id;
        link.object_id = folder->id;
        link.node_id = node;
        link.path = "shared";
        link.role = "read";
        link.token_hash = "digest-" + link.id;
        link.expires_at = "2999-01-01T00:00:00.000000Z";
        link.created_by_taap_id = space->principal.taap_id;
        link.at_us = at += 1'000'000;
        legacy.push_back(link);
    }
    legacy[1].revoked_at = "2026-09-01T00:00:00.000000Z";
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const auto& row : legacy) {
            tx.exec_params("INSERT INTO drive_share_links (id, repository_id, object_id, node_id, "
                           "path, role, token_hash, expires_at, revoked_at, created_by_taap_id, "
                           "created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6, $7, "
                           "$8::timestamptz, NULLIF($9, '')::timestamptz, $10, "
                           "to_timestamp($11::double precision / 1000000))",
                           row.id, row.repository_id, row.object_id, row.node_id, row.path,
                           row.role, row.token_hash, row.expires_at, row.revoked_at,
                           row.created_by_taap_id, row.at_us);
        }
        tx.commit();
    } else {
        for (const auto& row : legacy) {
            static_cast<lake::control::InMemoryStore&>(*t.store).seed_legacy_drive_share_link(row);
        }
    }
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy = [&](std::string mode) {
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "drive_share_links"}, {"mode", mode}},
            operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["collection"], "drive_share_links");
    EXPECT_EQ(plan.value["rows"], 2);
    EXPECT_EQ(plan.value["assigned"], 1);
    ASSERT_EQ(plan.value["unresolved"].size(), 1U);
    EXPECT_EQ(plan.value["unresolved"][0]["id"], legacy[1].id);
    EXPECT_EQ(plan.value["unresolved"][0]["outcome"], "needs_selection");
    const std::string home = first.workspaces[0];
    EXPECT_TRUE(t.zetta->records["http://" + home + ".test"].collections.empty())
        << "a plan writes nothing";
    // The assigned link's pre-move token is routed; the unassigned one is not.
    const auto route = t.store->drive_share_link_route(legacy[0].token_hash);
    ASSERT_TRUE(route) << route.error().message;
    EXPECT_EQ(route->workspace_id, home);
    EXPECT_EQ(route->link_id, legacy[0].id);
    EXPECT_FALSE(t.store->drive_share_link_route(legacy[1].token_hash));

    EXPECT_EQ(copy("verify").value["missing"].size(), 1U);
    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 1);
    EXPECT_EQ(copy("copy").value["already_present"], 1) << "a rerun copies nothing twice";
    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 1);
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_FALSE(verified.value["complete"]) << "the unresolved link keeps the table";

    // The copy is the same link, under its own id, in its own workspace only.
    auto context = t.context(first);
    auto records = lake::api::workspace_records(context, first.customer_id, home);
    ASSERT_TRUE(records);
    const auto stored = lake::control::settled(liquid::runtime::block_on(
        records->get(lake::control::DriveShareLinkCodec::collection, legacy[0].id)));
    ASSERT_TRUE(stored && *stored);
    const auto link = lake::control::DriveShareLinkCodec::decode(**stored);
    ASSERT_TRUE(link);
    EXPECT_EQ(link->token_hash, legacy[0].token_hash);
    EXPECT_EQ(link->path, "shared");
    EXPECT_EQ(link->at_us, legacy[0].at_us);
    for (const auto& space : t.spaces) {
        for (const auto& workspace : space.workspaces) {
            EXPECT_EQ(t.zetta->records["http://" + workspace + ".test"]
                          .rows("drive_share_links")
                          .size(),
                      workspace == home ? 1U : 0U)
                << workspace;
        }
    }
    // A record whose sealed value was swapped is reported, never accepted.
    auto& rows = t.zetta->records["http://" + home + ".test"].rows("drive_share_links");
    rows.at(legacy[0].id)["value"] = "AAAA";
    EXPECT_EQ(copy("verify").value["missing"].size(), 1U);
}

// lake#356 and lake#357: the retired application event and delivery tables move into the
// workspace of each event's application, deliveries with their event, and read back through the
// capability with their original ids, attempts and outcomes.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesApplicationEventsAndTheirDeliveries) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    const auto application = [&](const std::string& customer, const std::string& workspace,
                                 const std::string& name) {
        const std::string id = lake::control::new_uuid_v4();
        auto created = t.store->create_application({.id = id,
                                                    .customer_id = customer,
                                                    .pool_id = "app:" + id,
                                                    .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        return id;
    };
    // The first space's application is in its lab workspace; the second space's in its first.
    const std::string lab_app = application(first.customer_id, first.workspaces[1], "lab app");
    const std::string their_app = application(second.customer_id, second.workspaces[0], "theirs");
    const std::string webhook_id = lake::control::new_uuid_v4();
    struct Legacy {
        lake::control::ApplicationWebhookEvent event;
        std::vector<lake::control::ApplicationWebhookDelivery> deliveries;
    };
    const auto legacy = [&](const std::string& application_id, std::int64_t at_us,
                            int attempts) {
        Legacy row;
        row.event = {lake::control::new_uuid_v4(), application_id, "user.created",
                     json{{"email", "person@example.test"}}, "", at_us};
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            lake::control::ApplicationWebhookDelivery delivery;
            delivery.id = lake::control::new_uuid_v4();
            delivery.event_id = row.event.id;
            delivery.webhook_id = webhook_id;
            delivery.attempt = attempt;
            delivery.status = attempt == attempts ? 204 : 503;
            delivery.signature = "sig-" + std::to_string(attempt);
            delivery.error = attempt == attempts ? "" : "unavailable";
            delivery.application_id = application_id;
            delivery.at_us = at_us + attempt;
            row.deliveries.push_back(delivery);
        }
        return row;
    };
    std::vector<Legacy> rows = {legacy(lab_app, 1'790'000'000'000'000, 2),
                                legacy(their_app, 1'790'000'000'100'000, 1)};
    std::string legacy_app; // PostgreSQL only: an application from before migration 0049
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        // An application registered before 0049 in a space with two workspaces has none.
        legacy_app = lake::control::new_uuid_v4();
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'legacy', "
                       "'[\"https://legacy.example.test\"]'::jsonb)",
                       legacy_app, second.customer_id, "app:" + legacy_app);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        rows.push_back(legacy(legacy_app, 1'790'000'000'200'000, 1));
        for (const std::string& application_id : {lab_app, their_app, legacy_app}) {
            tx.exec_params("INSERT INTO auth_app_webhooks (id, application_id, url, events, "
                           "encrypted_secret) VALUES ($1::uuid, $2::uuid, 'https://h.test', "
                           "ARRAY['user.created'], 'x')",
                           application_id == lab_app ? webhook_id : lake::control::new_uuid_v4(),
                           application_id);
        }
        for (Legacy& row : rows) {
            if (row.event.application_id != lab_app) {
                const auto hook = tx.exec_params("SELECT id::text FROM auth_app_webhooks WHERE "
                                                 "application_id = $1::uuid",
                                                 row.event.application_id);
                for (auto& delivery : row.deliveries) {
                    delivery.webhook_id = hook[0][0].as<std::string>();
                }
            }
            tx.exec_params("INSERT INTO auth_app_webhook_events (id, application_id, event, "
                           "payload, created_at) VALUES ($1::uuid, $2::uuid, $3, $4::jsonb, "
                           "to_timestamp($5::double precision / 1000000))",
                           row.event.id, row.event.application_id, row.event.event,
                           row.event.payload.dump(), row.event.at_us);
            for (const auto& delivery : row.deliveries) {
                tx.exec_params("INSERT INTO auth_app_webhook_deliveries (id, event_id, webhook_id, "
                               "attempt, status, signature, error, created_at) VALUES ($1::uuid, "
                               "$2::uuid, $3::uuid, $4, $5, $6, $7, "
                               "to_timestamp($8::double precision / 1000000))",
                               delivery.id, delivery.event_id, delivery.webhook_id,
                               delivery.attempt, delivery.status, delivery.signature,
                               delivery.error, delivery.at_us);
            }
        }
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        for (const Legacy& row : rows) {
            memory.seed_legacy_application_webhook_event(row.event);
            for (const auto& delivery : row.deliveries) {
                memory.seed_legacy_application_webhook_delivery(delivery);
            }
        }
    }
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy = [&](std::string collection, std::string mode) {
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", collection}, {"mode", mode}},
            operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const std::size_t unresolved = GetParam() ? 1U : 0U;
    for (const std::string collection : {"auth_app_webhook_events", "auth_app_webhook_deliveries"}) {
        const bool events = collection == "auth_app_webhook_events";
        const std::size_t total = events ? rows.size() : (GetParam() ? 4U : 3U);
        const std::size_t owned = events ? 2U : 3U;
        const auto plan = copy(collection, "plan");
        ASSERT_TRUE(plan.ok) << plan.error.message;
        EXPECT_EQ(plan.value["rows"], total) << collection;
        EXPECT_EQ(plan.value["unresolved"].size(), unresolved) << collection;
        const auto before = copy(collection, "verify");
        EXPECT_EQ(before.value["missing"].size(), owned) << collection;
        EXPECT_FALSE(before.value["complete"]);
        const auto copied = copy(collection, "copy");
        ASSERT_TRUE(copied.ok) << copied.error.message;
        EXPECT_EQ(copied.value["copied"], owned) << collection;
        const auto rerun = copy(collection, "copy");
        EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
        EXPECT_EQ(rerun.value["already_present"], owned);
        const auto verified = copy(collection, "verify");
        EXPECT_EQ(verified.value["verified"], owned) << collection;
        EXPECT_TRUE(verified.value["missing"].empty());
        EXPECT_TRUE(verified.value["mismatched"].empty());
        EXPECT_EQ(verified.value["complete"], unresolved == 0) << collection;
    }
    // Each row is only in its own application's workspace.
    const std::string lab_url = "http://" + first.workspaces[1] + ".test";
    const std::string their_url = "http://" + second.workspaces[0] + ".test";
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_webhook_events").size(), 1U);
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_webhook_deliveries").size(), 2U);
    EXPECT_EQ(t.zetta->records[their_url].rows("auth_app_webhook_events").size(), 1U);
    EXPECT_TRUE(t.zetta->records["http://" + first.workspaces[0] + ".test"].collections.empty());

    // The application's event log reads the copy back with the original ids and outcomes.
    const auto read = liquid::runtime::block_on(t.registry.call_async(
        "list_application_webhook_events",
        {{"application_id", lab_app}, {"workspace_id", first.workspaces[1]}}, operator_context));
    ASSERT_TRUE(read && read->ok) << (read ? read->error.message : "incomplete");
    ASSERT_EQ(read->value["items"].size(), 1U);
    const json& event = read->value["items"][0];
    EXPECT_EQ(event["id"], rows[0].event.id);
    EXPECT_EQ(event["payload"]["email"], "person@example.test");
    ASSERT_EQ(event["deliveries"].size(), 2U);
    EXPECT_EQ(event["deliveries"][0]["id"], rows[0].deliveries[0].id);
    EXPECT_EQ(event["deliveries"][0]["status"], 503);
    EXPECT_EQ(event["deliveries"][1]["status"], 204);

    if (GetParam()) {
        // Assigning the legacy application its workspace resolves its rows on the next run.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       legacy_app, second.workspaces[1]);
        tx.commit();
        const auto resolved = copy("auth_app_webhook_events", "copy");
        EXPECT_EQ(resolved.value["copied"], 1);
        EXPECT_TRUE(resolved.value["unresolved"].empty());
        EXPECT_EQ(copy("auth_app_webhook_deliveries", "copy").value["copied"], 1);
        EXPECT_TRUE(copy("auth_app_webhook_events", "verify").value["complete"]);
        EXPECT_TRUE(copy("auth_app_webhook_deliveries", "verify").value["complete"]);
    }

    // A changed copy is reported, never silently accepted.
    auto& copied_rows = t.zetta->records[lab_url].rows("auth_app_webhook_deliveries");
    auto first_row = copied_rows.begin();
    auto second_row = std::next(first_row);
    first_row->second["value"] = second_row->second["value"];
    const auto tampered = copy("auth_app_webhook_deliveries", "verify");
    EXPECT_EQ(tampered.value["missing"].size(), 1U) << "a row that no longer opens is missing";
}

// lake#353, lake#354: the one-shot copy moves each retired application user and organization row
// into its application's workspace, with the claims that keep a pool user's email and an
// organization's TAAP identity and name unique there. A row whose application has no workspace is
// unresolved until it is assigned (PostgreSQL), one whose workspace was deleted does not block.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesApplicationUsersAndOrganizations) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    const auto application = [&](const std::string& customer, const std::string& workspace) {
        const std::string id = lake::control::new_uuid_v4();
        auto created = t.store->create_application({.id = id,
                                                    .customer_id = customer,
                                                    .pool_id = "app:" + id,
                                                    .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        return id;
    };
    const std::string lab_app = application(first.customer_id, first.workspaces[1]);
    const std::string their_app = application(second.customer_id, second.workspaces[0]);
    std::vector<lake::control::ApplicationUser> users;
    std::vector<lake::control::ApplicationOrganization> organizations;
    const auto user = [&](const std::string& application_id,
                          const std::string& email,
                          const std::string& status,
                          std::int64_t at_us) {
        lake::control::ApplicationUser row;
        row.application_id = application_id;
        row.user_id = lake::control::new_uuid_v4();
        row.taap_id = "taap:copy-" + row.user_id;
        row.email = email;
        row.display_name = "Copied " + email;
        row.status = status;
        row.own_pool = true;
        row.at_us = at_us;
        row.created_at = lake::control::record_instant(at_us);
        row.updated_at = row.created_at;
        users.push_back(row);
    };
    const auto organization =
        [&](const std::string& application_id, const std::string& name, std::int64_t at_us) {
            lake::control::ApplicationOrganization row;
            row.id = lake::control::new_uuid_v4();
            row.application_id = application_id;
            row.taap_id = "taap:copy-org-" + name;
            row.name = name;
            row.metadata = {{"plan", "copied"}};
            row.at_us = at_us;
            row.created_at = lake::control::record_instant(at_us);
            row.updated_at = row.created_at;
            organizations.push_back(row);
        };
    user(lab_app, "one@example.test", "active", 1'790'000'000'000'000);
    user(lab_app, "two@example.test", "suspended", 1'790'000'000'100'000);
    user(their_app, "one@example.test", "active", 1'790'000'000'200'000);
    organization(lab_app, "Lab organization", 1'790'000'000'300'000);
    organization(their_app, "Their organization", 1'790'000'000'400'000);
    std::string legacy_app; // PostgreSQL only: an application without a workspace yet
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        legacy_app = lake::control::new_uuid_v4();
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'legacy', "
                       "'[\"https://legacy.example.test\"]'::jsonb)",
                       legacy_app,
                       second.customer_id,
                       "app:" + legacy_app);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        user(legacy_app, "legacy@example.test", "active", 1'790'000'000'500'000);
        for (const auto& row : users) {
            tx.exec_params("INSERT INTO users (id, taap_id, email, display_name, pool_id) VALUES "
                           "($1::uuid, $2, $3, $4, $5)",
                           row.user_id,
                           row.taap_id,
                           row.email,
                           row.display_name,
                           "app:" + row.application_id);
            tx.exec_params("INSERT INTO auth_app_users (application_id, user_id, status, "
                           "created_at, updated_at) VALUES ($1::uuid, $2::uuid, $3, "
                           "to_timestamp($4::double precision / 1000000), "
                           "to_timestamp($4::double precision / 1000000))",
                           row.application_id,
                           row.user_id,
                           row.status,
                           row.at_us);
        }
        for (const auto& row : organizations) {
            tx.exec_params("INSERT INTO auth_app_organizations (id, application_id, taap_id, name, "
                           "metadata, created_at, updated_at) VALUES ($1::uuid, $2::uuid, $3, $4, "
                           "$5::jsonb, to_timestamp($6::double precision / 1000000), "
                           "to_timestamp($6::double precision / 1000000))",
                           row.id,
                           row.application_id,
                           row.taap_id,
                           row.name,
                           row.metadata.dump(),
                           row.at_us);
        }
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        for (const auto& row : users) {
            memory.seed_legacy_application_user(row);
        }
        for (const auto& row : organizations) {
            memory.seed_legacy_application_organization(row);
        }
    }
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto call = [&](const std::string& name, const json& input) {
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(name, input, operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const auto copy = [&](std::string collection, std::string mode) {
        return call("copy_workspace_records", {{"collection", collection}, {"mode", mode}});
    };
    const std::size_t unresolved = GetParam() ? 1U : 0U;
    for (const std::string collection : {"auth_app_users", "auth_app_organizations"}) {
        const bool user_rows = collection == "auth_app_users";
        const std::size_t owned = user_rows ? 3U : 2U;
        const std::size_t total = owned + (user_rows ? unresolved : 0U);
        const auto plan = copy(collection, "plan");
        ASSERT_TRUE(plan.ok) << plan.error.message;
        EXPECT_EQ(plan.value["rows"], total) << collection;
        EXPECT_EQ(plan.value["unresolved"].size(), user_rows ? unresolved : 0U) << collection;
        const auto before = copy(collection, "verify");
        EXPECT_EQ(before.value["missing"].size(), owned) << collection;
        EXPECT_FALSE(before.value["complete"]);
        const auto copied = copy(collection, "copy");
        ASSERT_TRUE(copied.ok) << copied.error.message;
        EXPECT_EQ(copied.value["copied"], owned) << collection << copied.value.dump();
        const auto rerun = copy(collection, "copy");
        EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
        EXPECT_EQ(rerun.value["already_present"], owned);
        const auto verified = copy(collection, "verify");
        EXPECT_EQ(verified.value["verified"], owned) << collection;
        EXPECT_TRUE(verified.value["missing"].empty());
        EXPECT_TRUE(verified.value["mismatched"].empty());
        EXPECT_EQ(verified.value["complete"], !user_rows || unresolved == 0) << collection;
    }
    // Each row and its claims are only in its own application's workspace: two users and two
    // email claims in the lab, one of each in the other space's workspace.
    const std::string lab_url = "http://" + first.workspaces[1] + ".test";
    const std::string their_url = "http://" + second.workspaces[0] + ".test";
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_users").size(), 4U);
    EXPECT_EQ(t.zetta->records[their_url].rows("auth_app_users").size(), 2U);
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_organizations").size(), 3U)
        << "the organization and its TAAP and name claims";
    EXPECT_TRUE(t.zetta->records["http://" + first.workspaces[0] + ".test"].collections.empty());

    // The capabilities read the copies back, with their original ids, instants and status.
    const auto listed = call("list_application_users",
                             {{"application_id", lab_app}, {"workspace_id", first.workspaces[1]}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 2U);
    EXPECT_EQ(listed.value["items"][0]["user_id"], users[1].user_id) << "newest first";
    EXPECT_EQ(listed.value["items"][0]["status"], "suspended");
    EXPECT_EQ(listed.value["items"][1]["created_at"], users[0].created_at);
    const auto orgs = call("list_application_organizations",
                           {{"application_id", lab_app}, {"workspace_id", first.workspaces[1]}});
    ASSERT_TRUE(orgs.ok) << orgs.error.message;
    ASSERT_EQ(orgs.value["items"].size(), 1U);
    EXPECT_EQ(orgs.value["items"][0]["id"], organizations[0].id);
    // The copied claims hold: the organization's name is taken in its application.
    const auto duplicate = call("create_application_organization",
                                {{"application_id", lab_app},
                                 {"workspace_id", first.workspaces[1]},
                                 {"taap_id", "taap:copy-org-new"},
                                 {"name", "Lab organization"}});
    EXPECT_EQ(duplicate.error.code, "conflict");

    // A status changed in the workspace after the copy is the workspace's, not a difference.
    ASSERT_TRUE(call("suspend_application_user",
                     {{"application_id", lab_app},
                      {"workspace_id", first.workspaces[1]},
                      {"user_id", users[0].user_id}})
                    .ok);
    EXPECT_TRUE(copy("auth_app_users", "verify").value["mismatched"].empty());

    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       legacy_app,
                       second.workspaces[1]);
        tx.commit();
        const auto resolved = copy("auth_app_users", "copy");
        EXPECT_EQ(resolved.value["copied"], 1);
        EXPECT_TRUE(resolved.value["unresolved"].empty());
        EXPECT_TRUE(copy("auth_app_users", "verify").value["complete"]);
    }

    // A changed copy is reported, never silently accepted.
    for (auto& [key, row] : t.zetta->records[their_url].rows("auth_app_organizations")) {
        row["value"] = "tampered";
    }
    EXPECT_EQ(copy("auth_app_organizations", "verify").value["missing"].size(), 1U);
    // Operator only.
    operator_context.operator_credential = false;
    operator_context.acting_identity = first.principal.taap_id;
    EXPECT_EQ(copy("auth_app_users", "plan").error.code, "unauthorized");
}

// lake#352 and lake#355: the one-shot copy moves each application's signing keys (one key ring)
// and each webhook into the application's workspace, with the private keys and secrets as the
// envelopes the tables held. After the copy the keys sign and the secrets sign deliveries.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesApplicationKeysAndWebhooks) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    const auto application = [&](const std::string& customer, const std::string& workspace,
                                 const std::string& name) {
        const std::string id = lake::control::new_uuid_v4();
        auto created = t.store->create_application({.id = id,
                                                    .customer_id = customer,
                                                    .pool_id = "app:" + id,
                                                    .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        return id;
    };
    // The first space's application is in its lab workspace, the second space's in its first; one
    // more application's workspace is deleted before the copy.
    const std::string lab_app = application(first.customer_id, first.workspaces[1], "lab app");
    const std::string their_app = application(second.customer_id, second.workspaces[0], "theirs");
    const std::string gone_app = application(first.customer_id, first.workspaces[0], "gone");
    struct Legacy {
        std::vector<lake::control::ApplicationSigningKey> keys;
        lake::control::ApplicationWebhook webhook;
        std::string secret;
    };
    std::map<std::string, Legacy> legacy;
    const auto keys_of = [&](const std::string& application_id, int versions) {
        Legacy& row = legacy[application_id];
        for (int version = 1; version <= versions; ++version) {
            auto key = lake::control::apps::generate_signing_key(application_id, version);
            EXPECT_TRUE(key);
            key->current = version == versions;
            key->created_at = "2026-09-0" + std::to_string(version) + "T10:00:00Z";
            key->retired_at = key->current ? "" : "2026-09-0" + std::to_string(version + 1) + "T10:00:00Z";
            row.keys.push_back(*key);
        }
        row.secret = "secret-" + application_id;
        row.webhook = {.id = lake::control::new_uuid_v4(),
                       .application_id = application_id,
                       .url = "https://hooks.example.test/" + application_id,
                       .events = {"organization.created"},
                       .secret = row.secret,
                       .enabled = true,
                       .at_us = 1'790'000'000'000'000};
    };
    keys_of(lab_app, 2);
    keys_of(their_app, 1);
    keys_of(gone_app, 1);
    std::string legacy_app; // PostgreSQL only: an application from before migration 0049
    const auto insert = [&](pqxx::work& tx, const Legacy& row) {
        for (const auto& key : row.keys) {
            const auto envelope = t.store->encrypt_application_credential(
                "application.jwt." + key.application_id, key.seed);
            ASSERT_TRUE(envelope) << envelope.error().message;
            tx.exec_params("INSERT INTO auth_app_signing_keys (application_id, kid, version, "
                           "encrypted_seed, public_key, current, created_at, retired_at) VALUES "
                           "($1::uuid, $2, $3, $4, $5, $6, $7::timestamptz, "
                           "NULLIF($8, '')::timestamptz)",
                           key.application_id, key.kid, key.version, *envelope, key.public_key,
                           key.current, key.created_at, key.retired_at);
        }
        const auto envelope = t.store->encrypt_application_credential(
            "application.webhook." + row.webhook.id, row.secret);
        ASSERT_TRUE(envelope) << envelope.error().message;
        tx.exec_params("INSERT INTO auth_app_webhooks (id, application_id, url, events, "
                       "encrypted_secret, created_at, updated_at) VALUES ($1::uuid, $2::uuid, $3, "
                       "$4, $5, to_timestamp($6::double precision / 1000000), "
                       "to_timestamp($6::double precision / 1000000))",
                       row.webhook.id, row.webhook.application_id, row.webhook.url,
                       row.webhook.events, *envelope, row.webhook.at_us);
    };
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        // An application registered before 0049 in a space with two workspaces has none.
        legacy_app = lake::control::new_uuid_v4();
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'legacy', "
                       "'[\"https://legacy.example.test\"]'::jsonb)",
                       legacy_app, second.customer_id, "app:" + legacy_app);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        keys_of(legacy_app, 1);
        for (const auto& [id, row] : legacy) {
            insert(tx, row);
        }
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        for (const auto& [id, row] : legacy) {
            for (const auto& key : row.keys) {
                memory.seed_legacy_application_signing_key(key);
            }
            memory.seed_legacy_application_webhook(row.webhook);
        }
    }
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[0], {.desired = "deleted"}));
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    std::string sent_signature, sent_event, sent_payload;
    operator_context.webhook_sender = [&](const std::string&, const json& headers,
                                          const std::string& payload) {
        sent_signature = headers.at("X-Lake-Signature");
        sent_event = headers.at("X-Lake-Event-Id");
        sent_payload = payload;
        return lake::api::WebhookDeliveryResult{.status = 204};
    };
    const auto call = [&](const std::string& name, json input) {
        operator_context.request_id = lake::control::new_uuid_v4();
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(name, input, operator_context));
        EXPECT_TRUE(completed);
        return completed ? *completed : lake::api::ApiResult::failure("store_error", "incomplete");
    };
    const auto copy = [&](std::string collection, std::string mode) {
        return call("copy_workspace_records", {{"collection", collection}, {"mode", mode}});
    };
    // Rows of the deleted workspace are reported and do not block; the legacy application's do.
    const std::size_t unresolved = GetParam() ? 2U : 1U;
    for (const std::string collection : {"auth_app_signing_keys", "auth_app_webhooks"}) {
        const auto plan = copy(collection, "plan");
        ASSERT_TRUE(plan.ok) << plan.error.message;
        EXPECT_EQ(plan.value["rows"], legacy.size()) << collection;
        EXPECT_EQ(plan.value["unresolved"].size(), unresolved) << collection;
        const auto before = copy(collection, "verify");
        EXPECT_EQ(before.value["missing"].size(), 2U) << collection;
        EXPECT_FALSE(before.value["complete"]);
        const auto copied = copy(collection, "copy");
        ASSERT_TRUE(copied.ok) << copied.error.message;
        EXPECT_EQ(copied.value["copied"], 2) << collection;
        const auto rerun = copy(collection, "copy");
        EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
        EXPECT_EQ(rerun.value["already_present"], 2);
        const auto verified = copy(collection, "verify");
        EXPECT_EQ(verified.value["verified"], 2) << collection;
        EXPECT_TRUE(verified.value["missing"].empty());
        EXPECT_TRUE(verified.value["mismatched"].empty());
        EXPECT_EQ(verified.value["complete"], !GetParam()) << collection;
    }
    // Each ring and webhook is only in its own application's workspace, keyed as lake reads it,
    // and the envelopes stay sealed.
    const std::string lab_url = "http://" + first.workspaces[1] + ".test";
    const std::string their_url = "http://" + second.workspaces[0] + ".test";
    ASSERT_EQ(t.zetta->records[lab_url].rows("auth_app_signing_keys").size(), 1U);
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_signing_keys").begin()->first, lab_app);
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_app_webhooks").size(), 1U);
    EXPECT_EQ(t.zetta->records[their_url].rows("auth_app_signing_keys").size(), 1U);
    EXPECT_EQ(t.zetta->records[their_url].rows("auth_app_webhooks").size(), 1U);
    EXPECT_TRUE(t.zetta->records["http://" + second.workspaces[1] + ".test"].collections.empty());
    for (const auto& [key, record] : t.zetta->records[lab_url].rows("auth_app_signing_keys")) {
        EXPECT_EQ(record.dump().find(legacy[lab_app].keys[0].public_key), std::string::npos);
    }

    // The JWKS reads both copied keys; the copied current key opens and signs.
    const auto jwks = call("application_jwks", {{"application_id", lab_app}});
    ASSERT_TRUE(jwks.ok) << jwks.error.message;
    ASSERT_EQ(jwks.value["keys"].size(), 2U);
    EXPECT_EQ(jwks.value["keys"][1]["x"], legacy[lab_app].keys[1].public_key);
    const auto user = t.store->create_user(
        {"taap:1:ed25519:copied-key-" + lake::control::new_uuid_v4(), "", "Copied key user"});
    ASSERT_TRUE(user) << user.error().message;
    {
        // The application's user lives in its workspace too (lake#353).
        auto records = lake::api::workspace_records(operator_context, first.customer_id,
                                                    first.workspaces[1]);
        ASSERT_TRUE(records) << records.error().message;
        lake::control::ApplicationUser member;
        member.application_id = lab_app;
        member.user_id = user->id;
        member.taap_id = user->taap_id;
        member.display_name = user->display_name;
        member.status = "active";
        ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(
            lake::control::create_application_user(*records, std::move(member)))));
    }
    const auto issued = call("issue_application_jwt", {{"application_id", lab_app},
                                                       {"workspace_id", first.workspaces[1]},
                                                       {"user_id", user->id}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_FALSE(issued.value["access_token"].get<std::string>().empty());

    // The copied webhook signs a delivery with the secret the table held.
    const auto webhooks = call("list_application_webhooks",
                               {{"application_id", lab_app}, {"workspace_id", first.workspaces[1]}});
    ASSERT_TRUE(webhooks.ok) << webhooks.error.message;
    ASSERT_EQ(webhooks.value["items"].size(), 1U);
    EXPECT_EQ(webhooks.value["items"][0]["id"], legacy[lab_app].webhook.id);
    ASSERT_TRUE(call("create_application_organization",
                     {{"application_id", lab_app},
                      {"workspace_id", first.workspaces[1]},
                      {"taap_id", "taap:1:ed25519:copied-org-" + lake::control::new_uuid_v4()},
                      {"name", "copied"}})
                    .ok);
    EXPECT_EQ(sent_signature, lake::control::apps::webhook_signature(legacy[lab_app].secret,
                                                                     sent_event, sent_payload));

    // A rotation after the copy (a later lake revision) is accepted by verify: every copied key is
    // still there, retired.
    ASSERT_TRUE(call("rotate_application_signing_key",
                     {{"application_id", lab_app}, {"workspace_id", first.workspaces[1]}})
                    .ok);
    const auto after_rotation = copy("auth_app_signing_keys", "verify");
    EXPECT_EQ(after_rotation.value["verified"], 2);
    EXPECT_TRUE(after_rotation.value["mismatched"].empty());

    if (GetParam()) {
        // Assigning the legacy application its workspace resolves its rows on the next run.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       legacy_app, second.workspaces[1]);
        tx.commit();
        for (const std::string collection : {"auth_app_signing_keys", "auth_app_webhooks"}) {
            const auto resolved = copy(collection, "copy");
            EXPECT_EQ(resolved.value["copied"], 1) << collection;
            EXPECT_EQ(resolved.value["unresolved"].size(), 1U) << "only the deleted workspace's";
            EXPECT_TRUE(copy(collection, "verify").value["complete"]) << collection;
        }
    }

    // A changed copy is reported, never silently accepted: a ring written under another key does
    // not open there.
    auto& their_rows = t.zetta->records[their_url].rows("auth_app_webhooks");
    auto& lab_rows = t.zetta->records[lab_url].rows("auth_app_webhooks");
    their_rows.begin()->second["value"] = lab_rows.begin()->second["value"];
    const auto tampered = copy("auth_app_webhooks", "verify");
    EXPECT_EQ(tampered.value["missing"].size(), 1U) << "a row that no longer opens is missing";
    EXPECT_FALSE(tampered.value["complete"]);
}

// lake#351: the one-shot copy moves the settings each application's auth_applications row still
// holds (name, allowed origins, redirect URIs, theme) into the application's workspace, keyed by
// the application, with the instant the row was created. The application then reads them there.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesApplicationSettings) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    std::map<std::string, lake::control::ApplicationSettings> legacy;
    const auto application = [&](const std::string& customer, const std::string& workspace,
                                 const std::string& name) {
        const std::string id = lake::control::new_uuid_v4();
        auto created = t.store->create_application({.id = id,
                                                    .customer_id = customer,
                                                    .pool_id = "app:" + id,
                                                    .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        legacy[id] = {.application_id = id,
                      .customer_id = customer,
                      .name = name,
                      .allowed_origins = {"https://" + id + ".example.test"},
                      .redirect_uris = {"https://" + id + ".example.test/callback"},
                      .theme = {{"accent_color", "#123456"}},
                      .at_us = 1'788'256'800'000'000};
        return id;
    };
    // The first space's application is in its lab workspace, the second space's in its first; one
    // more application's workspace is deleted before the copy.
    const std::string lab_app = application(first.customer_id, first.workspaces[1], "Lab app");
    const std::string their_app = application(second.customer_id, second.workspaces[0], "Theirs");
    const std::string gone_app = application(first.customer_id, first.workspaces[0], "Gone");
    std::string legacy_app; // PostgreSQL only: an application from before migration 0049
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const auto& [id, settings] : legacy) {
            tx.exec_params("UPDATE auth_applications SET name = $2, allowed_origins = $3::jsonb, "
                           "redirect_uris = $4::jsonb, theme = $5::jsonb, created_at = "
                           "to_timestamp($6::double precision / 1000000) WHERE id = $1::uuid",
                           id, settings.name, json(settings.allowed_origins).dump(),
                           json(settings.redirect_uris).dump(), settings.theme.dump(),
                           settings.at_us);
        }
        legacy_app = lake::control::new_uuid_v4();
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'Legacy', "
                       "'[\"https://legacy.example.test\"]'::jsonb)",
                       legacy_app, second.customer_id, "app:" + legacy_app);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        for (const auto& [id, settings] : legacy) {
            memory.seed_legacy_application_settings(settings);
        }
    }
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[0], {.desired = "deleted"}));
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    const auto call = [&](const std::string& name, json input) {
        operator_context.request_id = lake::control::new_uuid_v4();
        const auto completed =
            liquid::runtime::block_on(t.registry.call_async(name, input, operator_context));
        EXPECT_TRUE(completed);
        return completed ? *completed : lake::api::ApiResult::failure("store_error", "incomplete");
    };
    const auto copy = [&](std::string mode) {
        return call("copy_workspace_records", {{"collection", "auth_applications"}, {"mode", mode}});
    };
    const std::string lab_url = "http://" + first.workspaces[1] + ".test";
    const std::string their_url = "http://" + second.workspaces[0] + ".test";

    // Before the copy, an application whose settings are not in its workspace is not found, and
    // nothing falls back to the control store.
    EXPECT_EQ(call("get_application", {{"id", lab_app}}).error.code, "not_found");

    // Rows of the deleted workspace are reported and do not block; the legacy application's do.
    const std::size_t rows = legacy.size() + (GetParam() ? 1U : 0U);
    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], rows);
    EXPECT_EQ(plan.value["unresolved"].size(), GetParam() ? 2U : 1U);
    const auto before = copy("verify");
    EXPECT_EQ(before.value["missing"].size(), 2U);
    EXPECT_FALSE(before.value["complete"]);
    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 2);
    const auto rerun = copy("copy");
    EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
    EXPECT_EQ(rerun.value["already_present"], 2);
    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 2);
    EXPECT_TRUE(verified.value["missing"].empty());
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_EQ(verified.value["complete"], !GetParam());

    // Each application's settings are only in its own workspace, keyed by it, and sealed.
    ASSERT_EQ(t.zetta->records[lab_url].rows("auth_applications").size(), 1U);
    EXPECT_EQ(t.zetta->records[lab_url].rows("auth_applications").begin()->first, lab_app);
    EXPECT_EQ(t.zetta->records[their_url].rows("auth_applications").size(), 1U);
    EXPECT_TRUE(t.zetta->records["http://" + second.workspaces[1] + ".test"].collections.empty());
    for (const auto& [key, record] : t.zetta->records[lab_url].rows("auth_applications")) {
        EXPECT_EQ(record.dump().find("Lab app"), std::string::npos) << "sealed body";
        EXPECT_EQ(record.dump().find(lab_app + ".example.test"), std::string::npos);
    }

    // The application reads the copied settings, with the instant its row was created.
    const auto read = call("get_application", {{"id", lab_app}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["name"], "Lab app");
    EXPECT_EQ(read.value["redirect_uris"][0], "https://" + lab_app + ".example.test/callback");
    EXPECT_EQ(read.value["theme"]["accent_color"], "#123456");
    const auto listed = call("list_applications", {{"customer_id", first.customer_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 2U);
    for (const json& item : listed.value["items"]) {
        if (item["id"] == lab_app) {
            EXPECT_EQ(item["name"], "Lab app");
        } else {
            EXPECT_TRUE(item.contains("settings_unavailable")) << "the deleted workspace's";
        }
    }
    auto records =
        lake::api::workspace_records(operator_context, first.customer_id, first.workspaces[1]);
    ASSERT_TRUE(records) << records.error().message;
    const auto stored = lake::control::settled(liquid::runtime::block_on(
        lake::control::get_application_settings(*records, lab_app, first.customer_id)));
    ASSERT_TRUE(stored && *stored);
    EXPECT_EQ((*stored)->at_us, 1'788'256'800'000'000);

    // A theme changed after the copy (a later lake revision) is accepted by verify.
    ASSERT_TRUE(call("update_application_theme",
                     {{"application_id", lab_app},
                      {"workspace_id", first.workspaces[1]},
                      {"theme", {{"accent_color", "#abcdef"}}}})
                    .ok);
    const auto after_theme = copy("verify");
    EXPECT_EQ(after_theme.value["verified"], 2);
    EXPECT_TRUE(after_theme.value["mismatched"].empty());

    if (GetParam()) {
        // Assigning the legacy application its workspace resolves its settings on the next run.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       legacy_app, second.workspaces[1]);
        tx.commit();
        const auto resolved = copy("copy");
        EXPECT_EQ(resolved.value["copied"], 1);
        EXPECT_EQ(resolved.value["unresolved"].size(), 1U) << "only the deleted workspace's";
        EXPECT_TRUE(copy("verify").value["complete"]);
        const auto legacy_read = call("get_application", {{"id", legacy_app}});
        ASSERT_TRUE(legacy_read.ok) << legacy_read.error.message;
        EXPECT_EQ(legacy_read.value["name"], "Legacy");
        EXPECT_EQ(legacy_read.value["redirect_uris"], json::array());
    }

    // A changed copy is reported, never silently accepted: settings written under another key do
    // not open there.
    auto& their_rows = t.zetta->records[their_url].rows("auth_applications");
    auto& lab_rows = t.zetta->records[lab_url].rows("auth_applications");
    their_rows.begin()->second["value"] = lab_rows.begin()->second["value"];
    const auto tampered = copy("verify");
    EXPECT_EQ(tampered.value["missing"].size(), 1U) << "a row that no longer opens is missing";
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

// lake#348, lake#349: the retired metering rows move into the workspace that owns each one. Only
// transfer moves from metering_usage; build records are keyed by their result, so one a usage read
// recorded since the cutover is not copied twice.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesMeteringIntoEachSpacesWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    // The second space keeps one live workspace, so it owns its rows; the first has two, so its
    // rows need a reviewed selection.
    ASSERT_TRUE(t.store->update_workspace(second.workspaces[1], {.desired = "deleted"}));
    using lake::control::MeteredBuild;
    using lake::control::MeteringUsage;
    const std::vector<MeteringUsage> usage = {
        {second.customer_id, "2026-09-04T10:00:00Z", 4096, 10, 20, 3},
        {second.customer_id, "2026-09-04T11:00:00Z", 4096, 0, 5, 0},
        {second.customer_id, "2026-09-04T12:00:00Z", 4096, 0, 0, 4}, // gauge and compute only
        {first.customer_id, "2026-09-04T10:00:00Z", 4096, 1, 1, 0}};
    const std::vector<MeteredBuild> builds = {
        {second.customer_id, "result-a", "2026-09-04T10:00:00Z", 3, ""},
        {second.customer_id, "result-b", "2026-09-04T12:00:00Z", 4, ""},
        {first.customer_id, "result-c", "2026-09-04T10:00:00Z", 1, ""}};
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const MeteringUsage& row : usage) {
            tx.exec_params("INSERT INTO metering_usage (customer_id, hour, stored_bytes, "
                           "transfer_in_bytes, transfer_out_bytes, compute_seconds) VALUES "
                           "($1::uuid, $2::timestamptz, $3, $4, $5, $6)",
                           row.customer_id,
                           row.hour,
                           row.stored_bytes,
                           row.transfer_in_bytes,
                           row.transfer_out_bytes,
                           row.compute_seconds);
        }
        for (const MeteredBuild& row : builds) {
            tx.exec_params("INSERT INTO metering_build_records (customer_id, result_id, hour, "
                           "compute_seconds) VALUES ($1::uuid, $2, $3::timestamptz, $4)",
                           row.customer_id,
                           row.result_id,
                           row.hour,
                           row.compute_seconds);
        }
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        for (const MeteringUsage& row : usage) {
            memory.seed_legacy_metering_usage(row);
        }
        for (const MeteredBuild& row : builds) {
            memory.seed_legacy_metered_build(row);
        }
    }
    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy =
        [&](std::string collection, std::string mode, std::string after = {}, int limit = 1000) {
            json input{{"collection", collection}, {"mode", mode}, {"limit", limit}};
            if (!after.empty()) {
                input["after"] = after;
            }
            const auto completed = liquid::runtime::block_on(
                t.registry.call_async("copy_workspace_records", input, operator_context));
            EXPECT_TRUE(completed);
            return *completed;
        };
    const std::string url = "http://" + second.workspaces[0] + ".test";

    const auto plan = copy("metering_usage", "plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 3) << "a row without transfer has nothing to copy";
    EXPECT_EQ(plan.value["assigned"], 2);
    ASSERT_EQ(plan.value["unresolved"].size(), 1U);
    EXPECT_EQ(plan.value["unresolved"][0]["outcome"], "needs_selection");
    EXPECT_TRUE(t.zetta->records[url].collections.empty()) << "a plan writes nothing";
    EXPECT_EQ(copy("metering_usage", "verify").value["missing"].size(), 2U);

    // Lake counted transfer in the same hour after the cutover: the copy adds a record beside it
    // and never overwrites it.
    auto context = t.context(second);
    auto records = lake::api::workspace_records(context, second.customer_id, second.workspaces[0]);
    ASSERT_TRUE(records);
    ASSERT_TRUE(run(lake::control::add_metered_transfer(
        *records, second.customer_id, "2026-09-04T10:00:00Z", 100, 0)));
    // A usage read recorded result-a from its repository since the cutover.
    ASSERT_TRUE(run(lake::control::record_metered_build(
        *records, {second.customer_id, "result-a", "2026-09-04T10:00:00Z", 3, "repository-1"})));

    std::size_t copied = 0;
    std::string after;
    for (int page = 0; page < 10; ++page) {
        const auto result = copy("metering_usage", "copy", after, 1);
        ASSERT_TRUE(result.ok) << result.error.message;
        copied += result.value["copied"].get<std::size_t>();
        if (result.value["next_after"].is_null()) {
            break;
        }
        after = result.value["next_after"];
    }
    EXPECT_EQ(copied, 2U);
    EXPECT_EQ(copy("metering_usage", "copy").value["copied"], 0) << "a rerun copies nothing twice";
    const auto verified = copy("metering_usage", "verify");
    EXPECT_EQ(verified.value["verified"], 2);
    EXPECT_FALSE(verified.value["complete"]) << "the first space's row needs a selection";
    const auto transfer = run(lake::control::list_metered_transfer(*records, second.customer_id));
    ASSERT_TRUE(transfer);
    std::uint64_t in = 0, out = 0;
    for (const auto& row : *transfer) {
        in += row.transfer_in_bytes;
        out += row.transfer_out_bytes;
    }
    EXPECT_EQ(in, 110U) << "the copied row and lake's own counter both count";
    EXPECT_EQ(out, 25U);

    const auto built = copy("metering_build_records", "copy");
    ASSERT_TRUE(built.ok) << built.error.message;
    EXPECT_EQ(built.value["copied"], 1);
    EXPECT_EQ(built.value["already_present"], 1) << "result-a was recorded by a read";
    const auto builds_verified = copy("metering_build_records", "verify");
    EXPECT_EQ(builds_verified.value["verified"], 2);
    EXPECT_TRUE(builds_verified.value["missing"].empty());
    EXPECT_EQ(run(lake::control::list_metered_builds(*records, second.customer_id))->size(), 2U);

    // A record moved under another key does not open there, so verify reports it.
    auto& rows = t.zetta->records[url].rows("metering_build_records");
    ASSERT_EQ(rows.size(), 2U);
    rows.begin()->second["value"] = std::next(rows.begin())->second["value"];
    EXPECT_EQ(copy("metering_build_records", "verify").value["missing"].size(), 1U);

    if (GetParam()) {
        // Reviewed: the first space's rows are deleted (dev data may be). Then the usage pages
        // verify complete.
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM metering_usage WHERE customer_id = $1::uuid",
                       first.customer_id);
        tx.commit();
        EXPECT_TRUE(copy("metering_usage", "verify").value["complete"]);
    }
    const auto refused = [&] {
        auto person = t.context(first);
        return *liquid::runtime::block_on(
            t.registry.call_async("copy_workspace_records",
                                  {{"collection", "metering_usage"}, {"mode", "plan"}},
                                  person));
    }();
    EXPECT_FALSE(refused.ok) << "only the operator copies";
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

// lake#341: the operator's one-shot copy moves each forge_records row into its repository's
// workspace with its id and creation instant, and reports the rows it cannot place.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesForgeRecordsIntoTheirRepositorysWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    const auto repository = [&](const TwoSpaces::Space& space, std::size_t workspace,
                                const std::string& name) {
        const auto created = t.store->create_repository(
            {space.customer_id, name, "", "private", space.workspaces[workspace]});
        EXPECT_TRUE(created) << created.error().message;
        return *created;
    };
    const auto home = repository(first, 0, "home");
    const auto lab = repository(first, 1, "lab");
    const auto gone = repository(second, 1, "gone");
    std::int64_t at = 1'790'000'000'000'000;
    const auto row = [&](const lake::control::Repository& owner, std::string kind, json value) {
        lake::control::ForgeRecord record;
        record.id = lake::control::new_uuid_v4();
        record.repository_id = owner.id;
        record.kind = std::move(kind);
        record.value = std::move(value);
        record.at_us = at += 1'000'000;
        record.created_at = lake::control::record_instant(record.at_us);
        record.updated_at = lake::control::record_instant(record.at_us + 500'000);
        return record;
    };
    std::vector<lake::control::ForgeRecord> legacy;
    legacy.push_back(row(home, "issue", {{"title", "Copied"}, {"state", "open"}}));
    legacy.push_back(row(home, "issue_comment", {{"issue_id", legacy[0].id}, {"body", "Yes"}}));
    legacy.push_back(row(lab, "review", {{"change_id", "change-1"}, {"state", "approved"}}));
    legacy.push_back(row(gone, "release", {{"tag", "v1"}, {"change_id", "change-1"}}));
    std::string unassigned_id;
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        const auto insert = [&](const lake::control::ForgeRecord& record) {
            tx.exec_params("INSERT INTO forge_records (id, repository_id, kind, value, created_at, "
                           "updated_at) VALUES ($1::uuid, $2::uuid, $3, $4::jsonb, "
                           "to_timestamp($5::double precision / 1000000), "
                           "to_timestamp($6::double precision / 1000000))",
                           record.id, record.repository_id, record.kind, record.value.dump(),
                           record.at_us, record.at_us + 500'000);
        };
        for (const auto& record : legacy) {
            insert(record);
        }
        // A repository migration 0051 could not assign: its rows wait for its workspace.
        const auto orphan = repository(first, 0, "orphan");
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = NULL WHERE id = $1::uuid",
                       orphan.id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        auto waiting = row(orphan, "webhook", {{"url", "https://example.test"}});
        unassigned_id = waiting.id;
        insert(waiting);
        tx.commit();
    } else {
        for (const auto& record : legacy) {
            static_cast<lake::control::InMemoryStore&>(*t.store).seed_legacy_forge_record(record);
        }
    }
    // The release's repository is deleted, then its workspace: the release went with it.
    ASSERT_TRUE(t.call(second, "delete_repository", {{"id", gone.id}}).ok);
    ASSERT_TRUE(t.store->update_workspace(second.workspaces[1], {.desired = "deleted"}));

    lake::api::ApiContext operator_context;
    operator_context.store = t.store.get();
    operator_context.workspaces = t.service.get();
    operator_context.instance_kek = std::string(kek);
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto copy = [&](std::string mode) {
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "forge_records"}, {"mode", mode}},
            operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };
    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["collection"], "forge_records");
    const std::size_t rows = legacy.size() + (GetParam() ? 1 : 0);
    EXPECT_EQ(plan.value["rows"], rows);
    EXPECT_EQ(plan.value["assigned"], 3);
    std::map<std::string, std::string> outcomes;
    for (const auto& unresolved : plan.value["unresolved"]) {
        outcomes[unresolved["id"]] = unresolved["outcome"];
    }
    EXPECT_EQ(outcomes[legacy[3].id], "workspace_deleted");
    if (GetParam()) {
        EXPECT_EQ(outcomes[unassigned_id], "needs_selection");
    }
    EXPECT_TRUE(t.zetta->records["http://" + first.workspaces[0] + ".test"].collections.empty())
        << "a plan writes nothing";

    EXPECT_EQ(copy("verify").value["missing"].size(), 3U);
    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 3);
    EXPECT_EQ(copy("copy").value["already_present"], 3) << "a rerun copies nothing twice";
    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 3);
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_EQ(verified.value["complete"], !GetParam())
        << "a deleted workspace's rows do not hold the copy back; an unassigned repository's do";

    // Each row is now read through the capabilities, in its own workspace only, with its id.
    const auto issues = t.call(first, "list_issues", {{"repository_id", home.id}});
    ASSERT_TRUE(issues.ok) << issues.error.message;
    ASSERT_EQ(issues.value["items"].size(), 1U);
    EXPECT_EQ(issues.value["items"][0]["id"], legacy[0].id);
    EXPECT_EQ(issues.value["items"][0]["created_at"], legacy[0].created_at);
    EXPECT_EQ(issues.value["items"][0]["updated_at"], legacy[0].updated_at);
    const auto comments = t.call(first, "list_issue_comments", {{"repository_id", home.id}});
    ASSERT_EQ(comments.value["items"].size(), 1U);
    EXPECT_EQ(comments.value["items"][0]["value"]["issue_id"], legacy[0].id);
    const auto reviews = t.call(first, "list_reviews", {{"repository_id", lab.id}});
    ASSERT_EQ(reviews.value["items"].size(), 1U);
    EXPECT_EQ(reviews.value["items"][0]["id"], legacy[2].id);
    EXPECT_EQ(t.zetta->records["http://" + first.workspaces[0] + ".test"]
                  .rows("forge_records")
                  .size(),
              2U);
    EXPECT_EQ(t.zetta->records["http://" + first.workspaces[1] + ".test"]
                  .rows("forge_records")
                  .size(),
              1U);

    // A change made in the workspace after the copy is accepted; a swapped value is reported.
    ASSERT_TRUE(t.call(first, "update_issue",
                       {{"repository_id", home.id},
                        {"issue_id", legacy[0].id},
                        {"state", "closed"}})
                    .ok);
    EXPECT_EQ(copy("verify").value["verified"], 3);
    auto& home_rows =
        t.zetta->records["http://" + first.workspaces[0] + ".test"].rows("forge_records");
    home_rows.at(legacy[1].id)["value"] = "AAAA";
    EXPECT_EQ(copy("verify").value["missing"].size(), 1U);

    // Only the operator copies.
    auto person = t.context(first);
    const auto refused = liquid::runtime::block_on(t.registry.call_async(
        "copy_workspace_records", {{"collection", "forge_records"}, {"mode", "plan"}}, person));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->error.code, "unauthorized");
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
                                     {"value", {{"password", "pw-" + std::to_string(s) + "-" +
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
                EXPECT_EQ(dumped.find("pw-"), std::string::npos) << collection;
            }
        }
        const auto read = admin("read_secret", {{"mount_id", where.mount}, {"name", "database"}});
        ASSERT_TRUE(read.ok) << read.error.message;
        EXPECT_EQ(read.value["value"]["password"],
                  "pw-" + std::to_string(where.space) + "-" + std::to_string(where.workspace));
        const auto route = t.store->get_secret_mount_route(where.mount);
        ASSERT_TRUE(route);
        EXPECT_EQ(route->workspace_id, spaces[where.space].workspaces[where.workspace]);
    }
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const std::string table : {"secrets_mounts", "secrets_entries", "secrets_versions",
                                        "secrets_encryption_keys", "secrets_data_keys"}) {
            EXPECT_EQ(tx.exec("SELECT count(*) FROM " + table)[0][0].as<int>(), 0) << table;
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

// lake#328-#332: the operator's one-shot copy moves each row of the five retired tables into the
// workspace of its mount's route, where the capabilities then read it, unchanged.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesSecretsIntoTheirMountsWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto admin = [&](const std::string& capability, json input) {
        return secret_call(t, secret_operator(t), capability, std::move(input));
    };
    namespace crypto = lake::control::secrets_crypto;
    const SecretSpace sole = secret_space(t, "sole", 1);
    const SecretSpace several = secret_space(t, "several", 2);
    const SecretSpace doomed = secret_space(t, "doomed", 1);
    const std::string mount_id = lake::control::new_uuid_v4();
    const std::string waiting_id = lake::control::new_uuid_v4();
    const std::string gone_id = lake::control::new_uuid_v4();
    const std::string key_id = lake::control::new_uuid_v4();
    auto mount_key = crypto::random_key();
    auto transit_key = crypto::random_key();
    ASSERT_TRUE(mount_key && transit_key);
    const auto raw = [](const crypto::Key& key) {
        return std::string(reinterpret_cast<const char*>(key.data()), key.size());
    };
    const auto sealed = [&](std::uint64_t version, json value) {
        auto ciphertext = crypto::seal(*mount_key, value.dump(),
                                       "lake secret/" + mount_id + "/database/" +
                                           std::to_string(version));
        EXPECT_TRUE(ciphertext);
        return *ciphertext;
    };
    const std::string payload = "bGVnYWN5";
    std::string transit_ciphertext;
    {
        auto derived = crypto::derive(*transit_key, "lake transit encryption v1");
        ASSERT_TRUE(derived);
        auto decoded = crypto::base64url_decode(payload);
        auto sealed_payload = crypto::seal(*derived, *decoded, "lake transit/" + key_id + "/1");
        ASSERT_TRUE(sealed_payload);
        transit_ciphertext = crypto::format_ciphertext({key_id, 1, *sealed_payload});
    }
    std::int64_t at = 1'790'000'000'000'000;
    const std::int64_t mount_at = at += 1'000'000, entry_at = at += 1'000'000,
                       first_at = at += 1'000'000, second_at = at += 1'000'000,
                       key_at = at += 1'000'000;
    const std::string first_ciphertext = sealed(1, {{"password", "legacy-one"}});
    const std::string second_ciphertext = sealed(2, {{"password", "legacy-two"}});
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        const auto mount = [&](const std::string& id, const SecretSpace& space,
                               const std::string& name) {
            tx.exec_params("INSERT INTO secrets_mounts (id, customer_id, organization_id, name, "
                           "description, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, "
                           "'Legacy', to_timestamp($5::double precision / 1000000))",
                           id, space.customer_id, space.organization_id, name, mount_at);
        };
        mount(mount_id, sole, "legacy-vault");
        mount(waiting_id, several, "waiting-vault");
        mount(gone_id, doomed, "gone-vault");
        const std::string entry =
            tx.exec_params("INSERT INTO secrets_entries (mount_id, name, current_version, "
                           "custom_metadata, created_at) VALUES ($1::uuid, 'database', 2, "
                           "'{\"owner\":\"payments\"}'::jsonb, "
                           "to_timestamp($2::double precision / 1000000)) RETURNING id::text",
                           mount_id, entry_at)[0][0]
                .as<std::string>();
        tx.exec_params("INSERT INTO secrets_versions (entry_id, version, ciphertext, created_at, "
                       "deleted_at) VALUES ($1::uuid, 1, $2, to_timestamp($3::double precision "
                       "/ 1000000), now()), ($1::uuid, 2, $4, to_timestamp($5::double "
                       "precision / 1000000), NULL)",
                       entry, first_ciphertext, first_at, second_ciphertext, second_at);
        tx.exec_params("INSERT INTO secrets_encryption_keys (id, mount_id, name, created_at) "
                       "VALUES ($1::uuid, $2::uuid, 'orders', to_timestamp($3::double precision "
                       "/ 1000000))",
                       key_id, mount_id, key_at);
        const auto wrap = [&](const std::string& owner, const crypto::Key& key) {
            auto wrapped = t.store->wrap_secret_data_key(sole.customer_id, owner, 1, raw(key),
                                                         std::string(kek));
            EXPECT_TRUE(wrapped) << wrapped.error().message;
            return wrapped->wrapped_key;
        };
        tx.exec_params("INSERT INTO secrets_data_keys (customer_id, owner_kind, owner_id, "
                       "key_version, wrapped_key) VALUES ($1::uuid, 'mount', $2::uuid, 1, $3), "
                       "($1::uuid, 'encryption', $4::uuid, 1, $5)",
                       sole.customer_id, mount_id, wrap(mount_id, *mount_key), key_id,
                       wrap(key_id, *transit_key));
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        const auto mount = [&](const std::string& id, const SecretSpace& space,
                               const std::string& name) {
            lake::control::SecretMount legacy;
            legacy.id = id;
            legacy.customer_id = space.customer_id;
            legacy.organization_id = space.organization_id;
            legacy.name = name;
            legacy.description = "Legacy";
            legacy.at_us = mount_at;
            legacy.created_at = lake::control::record_instant(mount_at);
            legacy.updated_at = legacy.created_at;
            memory.seed_legacy_secret_mount(legacy);
        };
        mount(mount_id, sole, "legacy-vault");
        mount(waiting_id, several, "waiting-vault");
        mount(gone_id, doomed, "gone-vault");
        const lake::control::LegacySecretOwner owner{sole.customer_id, mount_id, "", ""};
        memory.seed_legacy_secret_entry({lake::control::new_uuid_v4(), "database", 2,
                                         {{"owner", "payments"}}, entry_at,
                                         lake::control::record_instant(entry_at), owner});
        memory.seed_legacy_secret_version({lake::control::new_uuid_v4(), "database", 1,
                                           first_ciphertext, first_at,
                                           lake::control::record_instant(second_at), "", owner});
        memory.seed_legacy_secret_version({lake::control::new_uuid_v4(), "database", 2,
                                           second_ciphertext, second_at, "", "", owner});
        lake::control::LegacyEncryptionKey legacy_key;
        legacy_key.key = {key_id, mount_id, "orders", "", 1,
                          lake::control::record_instant(key_at),
                          lake::control::record_instant(key_at)};
        legacy_key.at_us = key_at;
        legacy_key.owner = owner;
        memory.seed_legacy_encryption_key(legacy_key);
        ASSERT_TRUE(memory.seed_legacy_secret_data_key(
            {lake::control::new_uuid_v4(), "mount", mount_id, 1, {}, mount_at, owner},
            raw(*mount_key)));
        ASSERT_TRUE(memory.seed_legacy_secret_data_key(
            {lake::control::new_uuid_v4(), "encryption", key_id, 1, {}, key_at, owner},
            raw(*transit_key)));
    }

    auto operator_context = secret_operator(t);
    const auto copy = [&](const std::string& collection, const std::string& mode) {
        return secret_call(t, operator_context, "copy_workspace_records",
                           {{"collection", collection}, {"mode", mode}});
    };
    // The first plan records each mount's owner: the sole workspace's, and the doomed space's
    // only workspace, which is then deleted.
    const auto plan = copy("secrets_mounts", "plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 3);
    EXPECT_EQ(plan.value["assigned"], 2);
    ASSERT_TRUE(t.store->update_workspace(doomed.workspaces[0], {.desired = "deleted"}));
    std::map<std::string, std::string> outcomes;
    for (const auto& unresolved : copy("secrets_mounts", "plan").value["unresolved"]) {
        outcomes[unresolved["id"]] = unresolved["outcome"];
    }
    EXPECT_EQ(outcomes[waiting_id], "needs_selection");
    EXPECT_EQ(outcomes[gone_id], "workspace_deleted");
    EXPECT_TRUE(t.zetta->records["http://" + sole.workspaces[0] + ".test"].collections.empty())
        << "a plan writes nothing";

    // Mounts, data keys, keys, entries, versions: each copied once, then verified.
    const std::vector<std::pair<std::string, int>> collections{{"secrets_mounts", 1},
                                                               {"secrets_data_keys", 2},
                                                               {"secrets_encryption_keys", 1},
                                                               {"secrets_entries", 1},
                                                               {"secrets_versions", 2}};
    for (const auto& [collection, rows] : collections) {
        EXPECT_EQ(copy(collection, "verify").value["missing"].size(), static_cast<std::size_t>(rows))
            << collection;
        const auto copied = copy(collection, "copy");
        ASSERT_TRUE(copied.ok) << collection << ": " << copied.error.message;
        EXPECT_EQ(copied.value["copied"], rows) << collection;
        EXPECT_EQ(copy(collection, "copy").value["already_present"], rows)
            << collection << ": a rerun copies nothing twice";
        const auto verified = copy(collection, "verify");
        EXPECT_EQ(verified.value["verified"], rows) << collection;
        EXPECT_TRUE(verified.value["mismatched"].empty()) << collection;
        EXPECT_EQ(verified.value["complete"], collection != "secrets_mounts")
            << collection << ": only the unassigned mount holds a page back";
    }

    // Each row is now read through the capabilities, in its own workspace only.
    const auto read = admin("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["version"], 2);
    EXPECT_EQ(read.value["value"]["password"], "legacy-two");
    EXPECT_EQ(read.value["custom_metadata"]["owner"], "payments");
    EXPECT_EQ(admin("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}})
                  .error.code,
              "not_found")
        << "a version deleted before the copy stays deleted";
    const auto metadata = admin("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(metadata.ok) << metadata.error.message;
    EXPECT_EQ(metadata.value["versions"].size(), 2U);
    EXPECT_EQ(metadata.value["created_at"], lake::control::record_instant(entry_at));
    const auto mount = admin("get_secret_mount", {{"mount_id", mount_id}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    EXPECT_EQ(mount.value["name"], "legacy-vault");
    EXPECT_EQ(mount.value["workspace_id"], sole.workspaces[0]);
    const auto decrypted = admin("decrypt", {{"mount_id", mount_id},
                                             {"name", "orders"},
                                             {"ciphertext", transit_ciphertext}});
    ASSERT_TRUE(decrypted.ok) << decrypted.error.message;
    EXPECT_EQ(decrypted.value["value"], payload);
    const auto next = admin("put_secret", {{"mount_id", mount_id},
                                           {"name", "database"},
                                           {"value", {{"password", "after"}}},
                                           {"check_and_set", 2}});
    ASSERT_TRUE(next.ok) << next.error.message;
    EXPECT_EQ(next.value["version"], 3);
    EXPECT_EQ(admin("undelete_secret_versions",
                    {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                  .ok,
              true);
    // Changes made since the copy, and a customer KEK rotation, are accepted by verify.
    ASSERT_TRUE(admin("rotate_customer_kek", {{"customer_id", sole.customer_id}}).ok);
    for (const auto& [collection, rows] : collections) {
        if (collection != "secrets_mounts") {
            EXPECT_EQ(copy(collection, "verify").value["complete"], true) << collection;
        }
    }

    // A reviewed choice for the waiting mount; the next run routes and copies it.
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspace_assignments SET workspace_id = $2::uuid WHERE "
                       "object_table = 'secrets_mounts' AND object_id = $1::uuid",
                       waiting_id, several.workspaces[1]);
        tx.commit();
    } else {
        static_cast<lake::control::InMemoryStore&>(*t.store)
            .select_legacy_secret_mount_workspace(waiting_id, several.workspaces[1]);
    }
    EXPECT_EQ(copy("secrets_mounts", "copy").value["copied"], 1);
    EXPECT_EQ(copy("secrets_mounts", "verify").value["complete"], true)
        << "the deleted workspace's mount does not hold the copy back";
    EXPECT_TRUE(admin("get_secret_mount", {{"mount_id", waiting_id}}).ok);

    // A swapped record is reported.
    auto records = lake::api::workspace_records(operator_context, sole.customer_id,
                                                sole.workspaces[0]);
    ASSERT_TRUE(records);
    auto& rows = t.zetta->records["http://" + sole.workspaces[0] + ".test"].rows("secrets_versions");
    rows.at(lake::control::SecretVersionCodec::key(*records, mount_id, "database", 2))["value"] =
        "AAAA";
    EXPECT_EQ(copy("secrets_versions", "verify").value["missing"].size(), 1U);

    // Only the operator copies.
    const auto refused = secret_call(t, t.context(t.spaces[0]), "copy_workspace_records",
                                     {{"collection", "secrets_mounts"}, {"mode", "plan"}});
    EXPECT_EQ(refused.error.code, "unauthorized");
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
        EXPECT_EQ(tx.exec("SELECT count(*) FROM secrets_dynamic_leases")[0][0].as<int>(), 0);
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

// lake#362: the one-shot copy moves each secrets_dynamic_leases row into its mount's workspace,
// with its route, so the capabilities and the expiry loop then find it.
TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesDynamicLeasesIntoTheirMountsWorkspace) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    const auto admin = [&](const std::string& capability, json input) {
        return secret_call(t, secret_operator(t), capability, std::move(input));
    };
    const SecretSpace sole = secret_space(t, "sole", 1);
    const SecretSpace several = secret_space(t, "several", 2);
    // A mount made after 0057 (route, no secrets_mounts row) and a legacy mount waiting for its
    // workspace to be chosen.
    const auto mount = admin("create_secret_mount", {{"customer_id", sole.customer_id},
                                                     {"workspace_id", sole.workspaces[0]},
                                                     {"organization_id", sole.organization_id},
                                                     {"name", "routed"}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    const std::string waiting_mount = lake::control::new_uuid_v4();
    const std::string active = lake::control::new_uuid_v4();
    const std::string revoked = lake::control::new_uuid_v4();
    const std::string waiting = lake::control::new_uuid_v4();
    const std::int64_t created = (lease_now_us() / 1'000'000 - 120) * 1'000'000;
    const std::int64_t expires = (lease_now_us() / 1'000'000 + 3600) * 1'000'000;
    const std::int64_t revoked_at = (lease_now_us() / 1'000'000 - 60) * 1'000'000;
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO secrets_mounts (id, customer_id, organization_id, name) "
                       "VALUES ($1::uuid, $2::uuid, $3::uuid, 'waiting')",
                       waiting_mount, several.customer_id, several.organization_id);
        const auto lease = [&](const std::string& id, const std::string& on, const char* value,
                               bool is_revoked) {
            tx.exec_params("INSERT INTO secrets_dynamic_leases (id, mount_id, role, "
                           "encrypted_value, expires_at, revoked_at, renewals, created_at) VALUES "
                           "($1::uuid, $2::uuid, 'reader', $3, to_timestamp($4::double precision "
                           "/ 1000000), CASE WHEN $5 THEN to_timestamp($6::double precision / "
                           "1000000) END, 2, to_timestamp($7::double precision / 1000000))",
                           id, on, std::string(value), expires, is_revoked, revoked_at, created);
        };
        lease(active, mount_id, "sealed-active", false);
        lease(revoked, mount_id, "sealed-revoked", true);
        lease(waiting, waiting_mount, "sealed-waiting", false);
        tx.commit();
    } else {
        auto& memory = static_cast<lake::control::InMemoryStore&>(*t.store);
        lake::control::SecretMount legacy;
        legacy.id = waiting_mount;
        legacy.customer_id = several.customer_id;
        legacy.organization_id = several.organization_id;
        legacy.name = "waiting";
        memory.seed_legacy_secret_mount(legacy);
        const auto lease = [&](const std::string& id, const SecretSpace& space,
                               const std::string& on, const char* value, bool is_revoked) {
            memory.seed_legacy_dynamic_secret_lease(
                {id, "reader", value, created, expires,
                 is_revoked ? lake::control::record_instant(revoked_at) : "",
                 is_revoked ? revoked_at : 0, 2, {space.customer_id, on, "", ""}});
        };
        lease(active, sole, mount_id, "sealed-active", false);
        lease(revoked, sole, mount_id, "sealed-revoked", true);
        lease(waiting, several, waiting_mount, "sealed-waiting", false);
    }
    auto operator_context = secret_operator(t);
    const auto copy = [&](const std::string& mode) {
        return secret_call(t, operator_context, "copy_workspace_records",
                           {{"collection", "secrets_dynamic_leases"}, {"mode", mode}});
    };
    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 3);
    EXPECT_EQ(plan.value["assigned"], 2);
    ASSERT_EQ(plan.value["unresolved"].size(), 1U);
    EXPECT_EQ(plan.value["unresolved"][0]["id"], waiting);
    EXPECT_EQ(plan.value["unresolved"][0]["outcome"], "needs_selection");
    EXPECT_FALSE(t.store->get_secret_lease_route(active)) << "a plan writes nothing";
    EXPECT_EQ(copy("verify").value["missing"].size(), 2U);

    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 2);
    EXPECT_EQ(copy("copy").value["already_present"], 2) << "a rerun copies nothing twice";
    const auto verified = copy("verify");
    EXPECT_EQ(verified.value["verified"], 2);
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_EQ(verified.value["complete"], false) << "the waiting mount's lease holds it back";

    // The copied leases have routes due at their expiry, or at once when revoked, and are read
    // through the capabilities in their mount's workspace.
    const auto active_route = t.store->get_secret_lease_route(active);
    ASSERT_TRUE(active_route) << active_route.error().message;
    EXPECT_EQ(active_route->workspace_id, sole.workspaces[0]);
    EXPECT_EQ(active_route->due_us, expires);
    EXPECT_EQ(t.store->get_secret_lease_route(revoked)->due_us, revoked_at);
    auto records = lake::api::workspace_records(operator_context, sole.customer_id,
                                                sole.workspaces[0]);
    ASSERT_TRUE(records);
    {
        auto got = run(records->get(lake::control::SecretLeaseCodec::collection, active));
        ASSERT_TRUE(got && *got);
        const auto lease = lake::control::SecretLeaseCodec::decode(**got);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease->ciphertext, "sealed-active") << "the ciphertext is copied unopened";
        EXPECT_EQ(lease->renewals, 2);
        EXPECT_EQ(lease->at_us, created);
    }
    const auto renewed = admin("renew_dynamic_secret", {{"lease_id", active}});
    ASSERT_TRUE(renewed.ok) << renewed.error.message;
    EXPECT_EQ(renewed.value["renewals"], 3);
    EXPECT_EQ(admin("renew_dynamic_secret", {{"lease_id", revoked}}).error.code, "conflict");
    // The expiry loop settles the copied revoked lease (its sealed value erased); verify accepts
    // that and the renewal as later changes.
    const auto swept =
        liquid::runtime::block_on(lake::api::expire_secret_leases(operator_context));
    ASSERT_TRUE(swept);
    EXPECT_EQ(swept->settled, 1U);
    {
        auto got = run(records->get(lake::control::SecretLeaseCodec::collection, revoked));
        ASSERT_TRUE(got && *got);
        EXPECT_FALSE(lake::control::SecretLeaseCodec::decode(**got)->ciphertext);
    }
    EXPECT_EQ(copy("verify").value["verified"], 2);

    // A reviewed choice for the waiting mount routes it; the next copy moves its lease.
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspace_assignments SET workspace_id = $2::uuid WHERE "
                       "object_table = 'secrets_mounts' AND object_id = $1::uuid",
                       waiting_mount, several.workspaces[1]);
        tx.commit();
    } else {
        static_cast<lake::control::InMemoryStore&>(*t.store)
            .select_legacy_secret_mount_workspace(waiting_mount, several.workspaces[1]);
    }
    EXPECT_EQ(copy("copy").value["copied"], 1);
    EXPECT_EQ(copy("verify").value["complete"], true);
    EXPECT_EQ(t.store->get_secret_lease_route(waiting)->workspace_id, several.workspaces[1]);
    EXPECT_EQ(t.zetta->records["http://" + several.workspaces[1] + ".test"]
                  .rows("secrets_dynamic_leases")
                  .size(),
              1U);
    EXPECT_TRUE(t.zetta->records["http://" + several.workspaces[0] + ".test"]
                    .rows("secrets_dynamic_leases")
                    .empty());

    // A swapped record is reported.
    t.zetta->records["http://" + sole.workspaces[0] + ".test"]
        .rows("secrets_dynamic_leases")
        .at(active)["value"] = "AAAA";
    EXPECT_EQ(copy("verify").value["missing"].size(), 1U);

    // Only the operator copies.
    const auto refused = secret_call(t, t.context(t.spaces[0]), "copy_workspace_records",
                                     {{"collection", "secrets_dynamic_leases"}, {"mode", "plan"}});
    EXPECT_EQ(refused.error.code, "unauthorized");
}

INSTANTIATE_TEST_SUITE_P(Stores, WorkspaceRecordStores, testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Postgres" : "Memory"; });

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
}
