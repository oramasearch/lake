// Lake's records in workspace Zettas (docs/architecture/workspace-records.md, lake#370), with Ask
// Orama conversations as the first collection: sealing, multi-part bodies, structural isolation
// between workspaces and spaces, workspace deletion, and the operator's one-shot copy.

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

#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "postgres_fixture.hpp"
#include "record_fixture.hpp"
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
                EXPECT_EQ(capability, "sql");
                EXPECT_TRUE(read) << "Zetta's sql is a read: a GET";
                co_return tables->run(input);
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

    const auto rows = direct.tables->rows("ask_messages");
    ASSERT_EQ(rows.size(), 2U);
    for (const json& row : rows) {
        const std::string text = row.dump();
        for (const std::string& plain :
             std::vector<std::string>{"what runs?", "two runs", conversation, "user-1"}) {
            EXPECT_EQ(text.find(plain), std::string::npos) << "the workspace holds " << plain;
        }
        EXPECT_EQ(row.at("k"), records.index_of(lake::control::AskMessageCodec::collection,
                                                lake::control::ask_conversation_lookup(
                                                    conversation, "user-1")));
    }

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
}

TEST(WorkspaceRecords, AMissingTableIsAnEmptyCollectionAndTheFirstWriteCreatesIt) {
    DirectRecords direct;
    auto records = direct.open();
    AskMessages messages(records);
    const auto none = run(messages.list(lake::control::ask_conversation_lookup("c", "u")));
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_TRUE(none->empty());
    EXPECT_TRUE(direct.tables->databases.empty()) << "reading creates nothing";
    ASSERT_TRUE(run(messages.append(message("c", "u", "user", "hello"))));
    EXPECT_TRUE(direct.tables->databases.contains("lake_records"));
    // A second store of the same workspace (another replica) finds the table already there.
    auto again = direct.open();
    AskMessages more(again);
    ASSERT_TRUE(run(more.append(message("c", "u", "user", "again"))));
    EXPECT_EQ(run(more.list(lake::control::ask_conversation_lookup("c", "u")))->size(), 2U);
}

TEST(WorkspaceRecords, ALargeBodyIsWrittenInPartsUnderTheRequestLineLimit) {
    DirectRecords direct;
    auto records = direct.open();
    AskMessages messages(records);
    // Incompressible content, as a long answer with reads would be once sealed.
    std::string content;
    for (int index = 0; content.size() < 40'000; ++index) {
        content += lake::control::new_uuid_v4();
    }
    const auto stored = run(messages.append(message("c", "u", "assistant", content)));
    ASSERT_TRUE(stored) << stored.error().message;
    const auto rows = direct.tables->rows("ask_messages");
    EXPECT_GT(rows.size(), 10U) << "the body is split into parts";
    EXPECT_LT(direct.tables->longest_statement, 3800U)
        << "every GET request line stays under Zetta's 4 KiB start-line limit";
    const auto read = run(messages.list(lake::control::ask_conversation_lookup("c", "u")));
    ASSERT_EQ(read->size(), 1U);
    EXPECT_EQ(read->front().content, content);

    // A record missing a part (a write that failed partway) is not shown; a part written twice (a
    // retried insert) is harmless.
    auto& table = direct.tables->rows("ask_messages");
    table.push_back(table.front());
    EXPECT_EQ(run(messages.list(lake::control::ask_conversation_lookup("c", "u")))->size(), 1U);
    std::erase_if(table, [](const json& row) { return row.at("part") == 1; });
    EXPECT_TRUE(run(messages.list(lake::control::ask_conversation_lookup("c", "u")))->empty());
}

TEST(WorkspaceRecords, ARecordOpensOnlyInTheWorkspaceThatWroteIt) {
    // Ownership is immutable: a row copied into another workspace's table, or re-keyed to another
    // lookup, does not open there (the AEAD binds workspace, collection, record and lookup).
    DirectRecords first;
    DirectRecords second;
    auto mine = first.open();
    AskMessages messages(mine);
    ASSERT_TRUE(run(messages.append(message("c", "u", "user", "private"))));
    auto other = second.open();
    second.tables->databases = first.tables->databases;
    AskMessages theirs(other);
    EXPECT_TRUE(run(theirs.list(lake::control::ask_conversation_lookup("c", "u")))->empty());
    for (json& row : second.tables->rows("ask_messages")) {
        row["k"] = other.index_of(lake::control::AskMessageCodec::collection,
                                  lake::control::ask_conversation_lookup("c", "u"));
    }
    EXPECT_TRUE(run(theirs.list(lake::control::ask_conversation_lookup("c", "u")))->empty())
        << "the same lookup in another workspace does not open the moved row";
    for (json& row : first.tables->rows("ask_messages")) {
        row["k"] = mine.index_of(lake::control::AskMessageCodec::collection,
                                 lake::control::ask_conversation_lookup("other", "u"));
    }
    AskMessages again(mine);
    EXPECT_TRUE(run(again.list(lake::control::ask_conversation_lookup("other", "u")))->empty())
        << "a row moved to another conversation does not open there either";
}

TEST(WorkspaceRecords, OpeningRefusesAMissingKeyOrAWorkspaceThatIsNotAUuid) {
    const auto call = [](std::string, bool, json) -> liquid::runtime::task<
                          lake::control::ZettaControlResult<json>> { co_return json::object(); };
    EXPECT_EQ(WorkspaceRecords::open(call, "not-a-workspace", kek).error().code, "invalid_input");
    EXPECT_EQ(WorkspaceRecords::open(call, lake::control::new_uuid_v4(), "").error().code,
              "store_unavailable");
    EXPECT_TRUE(lake::control::names_workspace_records("SELECT * FROM LAKE_Records.ask_messages"));
    EXPECT_FALSE(lake::control::names_workspace_records("SELECT * FROM datasets.lake"));
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
        auto context = this->context(space);
        const auto completed = liquid::runtime::block_on(
            registry.call_async("get_ask_conversation", input, context));
        EXPECT_TRUE(completed);
        return *completed;
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
            const auto& rows = t.zetta->records["http://" + workspace + ".test"].rows("ask_messages");
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

TEST_P(WorkspaceRecordStores, TheOneShotCopyMovesEachOwnedRowOnceAndVerifiesIt) {
    auto store = make_store();
    if (!store) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TwoSpaces t(store, database_uri_);
    auto& first = t.spaces[0];
    auto& second = t.spaces[1];
    // The first space keeps one workspace, so it owns its rows; the second has two and needs a
    // selection.
    ASSERT_TRUE(t.store->update_workspace(first.workspaces[1], {.desired = "deleted"}));
    const std::string conversation = lake::control::new_uuid_v4();
    std::vector<AskMessage> legacy = {
        {lake::control::new_uuid_v4(), conversation, first.customer_id, first.principal.user_id,
         "user", "what runs?", nullptr, "", 1'790'000'000'000'001},
        {lake::control::new_uuid_v4(), conversation, first.customer_id, first.principal.user_id,
         "assistant", "two runs", json{{"proposals", json::array({{{"capability", "x"}}})}}, "",
         1'790'000'000'000'002},
        {lake::control::new_uuid_v4(), conversation, second.customer_id, second.principal.user_id,
         "user", "unassigned", nullptr, "", 1'790'000'000'000'003},
        {lake::control::new_uuid_v4(), conversation, "", first.principal.user_id, "user",
         "no space", nullptr, "", 1'790'000'000'000'004}};
    if (GetParam()) {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        for (const AskMessage& row : legacy) {
            tx.exec_params("INSERT INTO ask_messages (id, conversation_id, customer_id, user_id, role, "
                           "content, payload, created_at) VALUES ($1::uuid, $2::uuid, "
                           "NULLIF($3, '')::uuid, $4::uuid, $5, $6, $7::jsonb, "
                           "to_timestamp($8::double precision / 1000000))",
                           row.id, row.conversation_id, row.customer_id, row.user_id, row.role,
                           row.content, row.payload.dump(), row.at_us);
        }
        tx.commit();
    } else {
        for (const AskMessage& row : legacy) {
            static_cast<lake::control::InMemoryStore&>(*t.store).seed_legacy_ask_message(row);
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
            "copy_workspace_records", {{"collection", "ask_messages"}, {"mode", mode}},
            operator_context));
        EXPECT_TRUE(completed);
        return *completed;
    };

    const auto plan = copy("plan");
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["rows"], 4);
    EXPECT_EQ(plan.value["assigned"], 2);
    ASSERT_EQ(plan.value["unresolved"].size(), 2U);
    std::set<std::string> outcomes;
    for (const auto& row : plan.value["unresolved"]) {
        outcomes.insert(row["outcome"].get<std::string>());
    }
    EXPECT_EQ(outcomes, (std::set<std::string>{"needs_selection", "no_customer"}));
    const std::string home_url = "http://" + first.workspaces[0] + ".test";
    EXPECT_TRUE(t.zetta->records[home_url].databases.empty()) << "a plan writes nothing";

    const auto before = copy("verify");
    ASSERT_TRUE(before.ok);
    EXPECT_EQ(before.value["missing"].size(), 2U);
    EXPECT_FALSE(before.value["complete"]);

    const auto copied = copy("copy");
    ASSERT_TRUE(copied.ok) << copied.error.message;
    EXPECT_EQ(copied.value["copied"], 2);
    EXPECT_EQ(t.zetta->records[home_url].rows("ask_messages").size(), 2U);
    const auto rerun = copy("copy");
    EXPECT_EQ(rerun.value["copied"], 0) << "a rerun copies nothing twice";
    EXPECT_EQ(rerun.value["already_present"], 2);
    EXPECT_EQ(t.zetta->records[home_url].rows("ask_messages").size(), 2U);

    const auto verified = copy("verify");
    ASSERT_TRUE(verified.ok);
    EXPECT_EQ(verified.value["verified"], 2);
    EXPECT_TRUE(verified.value["missing"].empty());
    EXPECT_TRUE(verified.value["mismatched"].empty());
    EXPECT_FALSE(verified.value["complete"]) << "unresolved rows keep the table from being dropped";

    // The person reads the copied conversation back, in order and with the original ids.
    const auto read = t.read(first, {{"customer_id", first.customer_id},
                                     {"workspace_id", first.workspaces[0]},
                                     {"conversation_id", conversation}});
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_EQ(read.value["messages"].size(), 2U);
    EXPECT_EQ(read.value["messages"][0]["id"], legacy[0].id);
    EXPECT_EQ(read.value["messages"][1]["payload"]["proposals"][0]["capability"], "x");

    // A changed copy is reported, never silently accepted.
    for (json& row : t.zetta->records[home_url].rows("ask_messages")) {
        if (row["rid"] == legacy[1].id) {
            row["at"] = row["at"].get<std::int64_t>() + 1;
        }
    }
    const auto tampered = copy("verify");
    EXPECT_EQ(tampered.value["missing"].size(), 1U) << "a row that no longer opens is missing";

    const auto refused = [&] {
        auto context = t.context(first);
        const auto completed = liquid::runtime::block_on(t.registry.call_async(
            "copy_workspace_records", {{"collection", "ask_messages"}, {"mode", "plan"}}, context));
        return *completed;
    }();
    EXPECT_FALSE(refused.ok) << "only the operator copies";
}

TEST(WorkspaceRecords, NoWorkspaceSurfaceReadsOrListsTheReservedDatabase) {
    TwoSpaces t(std::make_shared<lake::control::InMemoryStore>(), "");
    const auto& space = t.spaces[0];
    const auto call = [&](const std::string& name, json input) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[0];
        auto context = t.context(space);
        return *liquid::runtime::block_on(t.registry.call_async(name, input, context));
    };
    const auto sql = call("workspace_sql", {{"statement", "SELECT * FROM Lake_Records.ask_messages"}});
    EXPECT_EQ(sql.error.code, "invalid_input");
    EXPECT_EQ(sql.error.path, "/statement");
    const auto database =
        call("workspace_sql", {{"statement", "SELECT * FROM ask_messages"}, {"database", "lake_records"}});
    EXPECT_EQ(database.error.path, "/database");
    const auto query = call("workspace_query", {{"capability", "get_dataset"},
                                                {"input", {{"database", "lake_records"},
                                                           {"name", "ask_messages"}}}});
    EXPECT_EQ(query.error.code, "invalid_input");
    EXPECT_EQ(query.error.path, "/input");
    const auto act = call("workspace_act", {{"capability", "create_dataset_from_query"},
                                            {"input", {{"statement",
                                                        "SELECT * FROM lake_records.ask_messages"}}}});
    EXPECT_EQ(act.error.path, "/input");

    json listed = {{"datasets", json::array({{{"database", "lake_records"}, {"name", "ask_messages"}},
                                             {{"database", "datasets"}, {"name", "orders"}}})}};
    lake::api::hide_workspace_records(listed);
    ASSERT_EQ(listed["datasets"].size(), 1U);
    EXPECT_EQ(listed["datasets"][0]["name"], "orders");
    json rows = {{"rows", json::array({{{"database", "lake_records"}, {"dataset", "ask_messages"}}})}};
    lake::api::hide_workspace_records(rows);
    EXPECT_TRUE(rows["rows"].empty());
}

INSTANTIATE_TEST_SUITE_P(Stores, WorkspaceRecordStores, testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Postgres" : "Memory"; });

// Against a real Zetta when LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN name one (for example a
// local `zetta --headless`): the statements lake writes are the subset the engine accepts.
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
}
