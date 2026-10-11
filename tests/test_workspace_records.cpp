// Lake's records in workspace Zettas (docs/architecture/workspace-records.md, lake#370), with Ask
// Orama conversations as the first collection and Design → Opportunities as the first mutable one
// (lake#376): sealing, structural isolation between workspaces and spaces, conditional writes,
// workspace deletion, and the operator's one-shot copy.

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

#include "api/forge_records_api.hpp"
#include "api/secret_records_api.hpp"
#include "api/workspace_records_api.hpp"
#include "audit_test_support.hpp"
#include "control/secrets_crypto.hpp"
#include "lake/api/audit_recorder.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/metering_recorder.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/transcription.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_runs.hpp"
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
#include "lake/control/memory_store.hpp"
#include "lake/control/metering.hpp"
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
    // Speech-to-text for transcribe and audio transcripts; empty: not configured.
    lake::api::Transcriber transcriber;

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
        context.transcriber = transcriber;
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
        t.call(first, "transcribe", {{"name", "memo"}, {"audio_base64", audio}});
    EXPECT_EQ(unconfigured.error.code, "not_supported");
    EXPECT_EQ(unconfigured.error.message, "Speech-to-text is not configured on this lake.");
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

    const auto transcribed = t.call(
        first, "transcribe", {{"name", "memo"}, {"audio_base64", audio}, {"language", "en"}});
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
    const auto failing =
        t.call(first, "transcribe", {{"name", "x.wav"}, {"audio_base64", "c2lsZW5jZQ=="}});
    EXPECT_EQ(failing.error.code, "engine_error");
    // A text file is refused before it reaches the speech service (lake#594).
    const std::size_t sent_before = heard.size();
    const auto text_file =
        t.call(first, "transcribe", {{"name", "memo.txt"}, {"audio_base64", audio}});
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

    // Bounded: an event beyond the bound is dropped and counted, never written centrally.
    lake::api::AuditRecorder bounded(t.store.get(), t.service.get(), std::string(kek), "", 1);
    const std::string kept = lake::control::new_uuid_v4();
    const std::string dropped = lake::control::new_uuid_v4();
    for (const auto& request_id : {kept, dropped}) {
        call_with(t.context(first), &bounded, "get_ask_conversation",
                  {{"customer_id", first.customer_id},
                   {"workspace_id", home},
                   {"conversation_id", lake::control::new_uuid_v4()}},
                  request_id);
    }
    EXPECT_EQ(bounded.pending(), 1U);
    EXPECT_EQ(bounded.totals().dropped, 1U);
    EXPECT_TRUE(central(dropped).empty());

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
// finish_call appends the recorded central event as the leaf; the leaf carries routing ids only,
// never the credential, the role or the mount's name.
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

    // Each leaf is exactly the central event's body, in order: lease, renew, revoke.
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
    ASSERT_GE(EVP_DecodeBlock(reinterpret_cast<unsigned char*>(leaves.data()),
                              reinterpret_cast<const unsigned char*>(encoded.data()),
                              static_cast<int>(encoded.size())),
              0);
    std::size_t previous = 0;
    std::set<std::string> operations;
    for (const auto& event : *events) {
        EXPECT_EQ(event.draft.kind, "dynamic_secret");
        operations.insert(event.draft.details.value("operation", ""));
        const std::vector<std::uint8_t> body = json::to_cbor(lake::audit::to_json(event));
        const auto at = leaves.find(std::string(body.begin(), body.end()));
        ASSERT_NE(at, std::string::npos) << event.draft.action << " has no instance log leaf";
        EXPECT_GE(at, previous);
        previous = at;
    }
    EXPECT_EQ(operations, (std::set<std::string>{"lease", "renew", "revoke"}));
    // Content-free: the lease id names the event; the credential, role and mount name never
    // reach the public log.
    EXPECT_NE(leaves.find(lease_id), std::string::npos);
    for (const std::string& secret : {username, password, role, mount_name}) {
        EXPECT_EQ(leaves.find(secret), std::string::npos) << secret;
    }
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
