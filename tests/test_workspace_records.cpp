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

#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/stream_owners.hpp"
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
    EXPECT_TRUE(t.zetta->records[home_url].collections.empty()) << "a plan writes nothing";

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
    // Another record's sealed value under this key does not open, so the copy is reported.
    auto& copied_rows = t.zetta->records[home_url].rows("ask_messages");
    copied_rows.at(legacy[1].id)["value"] = copied_rows.at(legacy[0].id)["value"];
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
            t.store->create_repository({space->customer_id, "drive", "", "private"});
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
