#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;
using lake::control::ZettaControlResult;

constexpr std::string_view customer = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";

class FakeCaller final : public lake::control::WorkspaceCaller {
public:
    std::string url, token, capability;
    bool read = false;
    json input;
    json value = json::object();
    std::map<std::string, json> answers; // per capability, when set; else `value`
    std::vector<std::pair<std::string, json>> calls;

    liquid::runtime::task<ZettaControlResult<json>> call(std::string called_url,
                                                         std::string called_token,
                                                         std::string called_capability,
                                                         bool called_read,
                                                         json called_input) override {
        url = std::move(called_url);
        token = std::move(called_token);
        capability = std::move(called_capability);
        read = called_read;
        input = std::move(called_input);
        calls.emplace_back(capability, input);
        const auto answer = answers.find(capability);
        co_return ZettaControlResult<json>(answer == answers.end() ? value : answer->second);
    }
};

lake::control::ClerkIdentity minted(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity("workspace-test-kek");
    return lake::control::ClerkIdentity{subject,
                                        subject + "@example.test",
                                        "",
                                        "browser",
                                        identity->taap_id,
                                        identity->public_key,
                                        identity->wrapped_seed};
}

// A store with one signed-in person and their personal customer, and the service over it.
struct Workspaces {
    std::filesystem::path dir =
        lake::test::fixture_path("service-connections-" + lake::control::new_uuid_v4());
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    std::shared_ptr<FakeCaller> caller = std::make_shared<FakeCaller>();
    std::shared_ptr<lake::control::WorkspaceService> service;
    std::string customer_id, secret;

    explicit Workspaces(std::size_t limit = 5) {
        std::filesystem::create_directories(dir);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{dir.string(), limit}, store, caller);
        const auto session = store->sign_in_clerk(minted("owner"));
        EXPECT_TRUE(session);
        secret = session->secret;
        const auto principal = store->context_for(secret);
        const auto customers = store->list_customers(10, principal->user_id);
        customer_id = customers->front().id;
    }
    // What the operator does once a service is Ready.
    void deliver(const lake::control::ManagedService& service, json connection) {
        std::ofstream(dir / (service.slug + ".json")) << connection.dump();
    }
    template <class Task> auto run(Task task) {
        auto completed = liquid::runtime::block_on(std::move(task));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
};

} // namespace

TEST(Workspaces, SlugIsDerivedFromTheCustomerId) {
    EXPECT_EQ(lake::control::workspace_slug(customer), "ws-0f1e2d3c4b5a");
    EXPECT_EQ(lake::control::workspace_slug("0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0"),
              "ws-0f1e2d3c4b5a");
    EXPECT_EQ(lake::control::workspace_slug("not-a-uuid"), "");
    EXPECT_EQ(lake::control::workspace_slug("0f1e2d3c-4b5a"), "");
}

TEST(Workspaces, TheWorkspaceIsTheDefaultZettaServiceAndReadyNeedsTheOperator) {
    Workspaces w;
    auto state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested);
    EXPECT_FALSE(state->ready());

    auto requested = w.run(w.service->request(w.customer_id, "taap:owner"));
    ASSERT_TRUE(requested);
    EXPECT_TRUE(requested->requested);
    EXPECT_EQ(requested->slug, lake::control::workspace_slug(w.customer_id))
        << "the first workspace keeps decision 45's name, so its cluster is adopted";
    ASSERT_TRUE(w.run(w.service->request(w.customer_id)));
    const auto services = w.store->list_services(w.customer_id);
    ASSERT_TRUE(services);
    ASSERT_EQ(services->size(), 1U) << "asking again creates nothing";
    const lake::control::ManagedService workspace = services->front();
    EXPECT_EQ(workspace.kind, "zetta");
    EXPECT_TRUE(workspace.is_default);
    EXPECT_EQ(workspace.created_by_taap_id, "taap:owner");

    ASSERT_TRUE(w.store->set_service_status(workspace.id, "Ready", "", "http://ws.test:4800"));
    state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase, "Ready");
    EXPECT_FALSE(state->ready()) << "no credential delivered yet";
    w.deliver(workspace, {{"kind", "zetta"}, {"token", "workspace-admin-token"}});
    state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->ready());
    EXPECT_EQ(state->to_json()["ready"], true);
}

TEST(Workspaces, DeletingTheWorkspaceWithdrawsItAndARestartGetsANewSlug) {
    Workspaces w;
    auto refused = w.run(w.service->withdraw(w.customer_id));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "not_found") << "nothing to delete before a request";

    auto first = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(first);
    ASSERT_TRUE(w.store->set_service_status(first->service_id, "Ready", "", "http://a:4800"));
    auto withdrawn = w.run(w.service->withdraw(w.customer_id));
    ASSERT_TRUE(withdrawn);
    EXPECT_FALSE(withdrawn->requested);
    const auto old = w.store->get_service(first->service_id);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->desired, "absent");
    EXPECT_EQ(old->generation, 2);

    auto state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested) << "a deleting workspace is not the workspace";
    auto second = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(second);
    EXPECT_NE(second->service_id, first->service_id);
    EXPECT_TRUE(second->slug.starts_with("svc-")) << "a slug is never reused";
    EXPECT_EQ(second->phase, "") << "the old Ready does not carry over";

    ASSERT_TRUE(w.store->set_service_status(first->service_id, "Deleted", "", ""));
    const auto listed = w.store->list_services(w.customer_id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 1U) << "a finished deletion is history";
}

TEST(Workspaces, ServicesAreNamedPerCustomerAndCapped) {
    Workspaces w(2);
    lake::control::NewManagedService postgres{w.customer_id, "postgres", "crm", "taap:owner"};
    const auto created = w.service->create_service(postgres);
    ASSERT_TRUE(created);
    EXPECT_TRUE(created->slug.starts_with("svc-"));
    EXPECT_EQ(created->desired, "present");
    const auto duplicate = w.service->create_service(postgres);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "conflict");
    ASSERT_TRUE(w.service->create_service({w.customer_id, "zetta", "search", ""}));
    const auto over = w.service->create_service({w.customer_id, "zetta", "third", ""});
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, "quota_exceeded");

    const auto deleted = w.service->delete_service(w.customer_id, created->id);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->desired, "absent");
    EXPECT_TRUE(w.service->create_service({w.customer_id, "zetta", "third", ""}))
        << "a deleted service frees its place";
    const auto again = w.service->delete_service(w.customer_id, created->id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "conflict");
    const auto foreign =
        w.service->delete_service("11111111-2222-3333-4444-555555555555", created->id);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, "not_found") << "another customer cannot see the service";
}

TEST(Workspaces, StoresAreNamedPerCustomerAndOnlyAvailablePairsAreCreated) {
    Workspaces w;
    ASSERT_NE(lake::control::find_store_provider("volume", "cluster"), nullptr);
    EXPECT_TRUE(lake::control::find_store_provider("volume", "cluster")->available);
    EXPECT_EQ(lake::control::store_providers[0].type, "volume") << "the available pair is first";
    for (const lake::control::StoreProvider& entry : lake::control::store_providers) {
        EXPECT_EQ(entry.available, entry.type == "volume" && entry.provider == "cluster")
            << entry.type << "/" << entry.provider;
        EXPECT_EQ(entry.unavailable_reason.empty(), entry.available);
    }

    lake::control::NewManagedStore volume{w.customer_id, "volume", "cluster", "uploads", "taap:x"};
    volume.size_gi = 20;
    const auto created = w.service->create_store(volume);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->slug, lake::control::managed_service_slug("st", created->id));
    EXPECT_TRUE(created->slug.starts_with("st-"));
    EXPECT_EQ(created->size_gi, 20);
    EXPECT_FALSE(created->is_public) << "private by default";
    EXPECT_EQ(created->desired, "present");
    const auto duplicate = w.service->create_store(volume);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "conflict");

    const auto r2 = w.service->create_store({w.customer_id, "object", "r2", "bucket", "taap:x"});
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().code, "invalid_input");
    EXPECT_NE(r2.error().message.find("needs R2 enabled"), std::string::npos) << r2.error().message;
    const auto nonsense = w.service->create_store({w.customer_id, "volume", "r2", "odd", "taap:x"});
    ASSERT_FALSE(nonsense);
    EXPECT_EQ(nonsense.error().code, "invalid_input");

    const auto foreign =
        w.service->delete_store("11111111-2222-3333-4444-555555555555", created->id);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, "not_found") << "another customer cannot see the store";
    const auto deleted = w.service->delete_store(w.customer_id, created->id);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->desired, "absent");
    EXPECT_EQ(deleted->generation, 2);
    const auto again = w.service->delete_store(w.customer_id, created->id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "conflict");
    EXPECT_TRUE(w.service->create_store(volume)) << "a deleted store frees its name";
    ASSERT_EQ(w.service->stores(w.customer_id)->size(), 2U)
        << "a deleting store stays listed until the operator is done";
    ASSERT_TRUE(w.store->set_store_status(created->id, "Deleted", "", ""));
    EXPECT_EQ(w.service->stores(w.customer_id)->size(), 1U) << "a finished deletion is history";
}

TEST(Workspaces, CallsReachOnlyTheCustomersOwnCluster) {
    Workspaces w;
    auto refused = w.run(w.service->call(w.customer_id, "list_projects", true, {}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "workspace_not_ready");

    auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto workspace = w.store->get_service(requested->service_id);
    ASSERT_TRUE(w.store->set_service_status(
        workspace->id, "Ready", "", "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "workspace-admin-token"}});
    w.caller->value = {{"projects", json::array({"retention"})}};
    auto called =
        w.run(w.service->call(w.customer_id, "get_project", true, {{"name", "retention"}}));
    ASSERT_TRUE(called);
    EXPECT_EQ((*called)["projects"][0], "retention");
    EXPECT_EQ(w.caller->url, "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800");
    EXPECT_EQ(w.caller->token, "workspace-admin-token");
    EXPECT_EQ(w.caller->capability, "get_project");
    EXPECT_TRUE(w.caller->read);
    EXPECT_EQ(w.caller->input["name"], "retention");
}

TEST(Workspaces, CapabilitiesAreAllowListedAndAuthorizedOnTheCustomer) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto other = store.sign_in_clerk(minted("other"));
    ASSERT_TRUE(other);
    const auto owner_principal = store.context_for(w.secret);
    const auto other_principal = store.context_for(other->secret);
    ASSERT_TRUE(owner_principal);
    ASSERT_TRUE(other_principal);
    const std::string owner_customer = w.customer_id;

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call =
        [&](const lake::control::Principal& principal, const std::string& name, json input) {
            lake::api::ApiContext context;
            context.store = &store;
            context.workspaces = w.service.get();
            context.authorized = true;
            context.acting_user_id = principal.user_id;
            context.acting_identity = principal.taap_id;
            context.request_id = lake::control::new_uuid_v4();
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
            EXPECT_TRUE(completed);
            return std::move(*completed);
        };

    const auto started =
        call(*owner_principal, "start_workspace", {{"customer_id", owner_customer}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["workspace"], lake::control::workspace_slug(owner_customer));

    const auto foreign =
        call(*other_principal, "workspace_status", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign.ok) << "another customer's workspace is not readable";
    const auto foreign_start =
        call(*other_principal, "start_workspace", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign_start.ok);

    const auto admin_surface = call(*owner_principal,
                                    "workspace_query",
                                    {{"customer_id", owner_customer},
                                     {"capability", "sql"},
                                     {"input", {{"statement", "SELECT 1"}}}});
    ASSERT_FALSE(admin_surface.ok);
    EXPECT_EQ(admin_surface.error.code, "invalid_input");
    const auto action_as_read =
        call(*owner_principal,
             "workspace_query",
             {{"customer_id", owner_customer}, {"capability", "create_project"}});
    EXPECT_FALSE(action_as_read.ok) << "an action is not reachable through workspace_query";
    const auto not_ready = call(*owner_principal,
                                "workspace_query",
                                {{"customer_id", owner_customer}, {"capability", "list_projects"}});
    ASSERT_FALSE(not_ready.ok);
    EXPECT_EQ(not_ready.error.code, "workspace_not_ready");
    for (const char* action : {"create_connection",
                               "delete_connection",
                               "create_dataset_from_query",
                               "upload_dataset",
                               "delete_project"}) {
        const auto allowed = call(*owner_principal,
                                  "workspace_act",
                                  {{"customer_id", owner_customer}, {"capability", action}});
        EXPECT_EQ(allowed.error.code, "workspace_not_ready") << action << " is an allowed action";
    }

    // Services.
    const auto created = call(*owner_principal,
                              "create_service",
                              {{"customer_id", owner_customer},
                               {"kind", "postgres"},
                               {"name", "crm"},
                               {"storage_gi", 20}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["storage_gi"], 20);
    const std::string service_id = created.value["id"];
    const auto bad_name =
        call(*owner_principal,
             "create_service",
             {{"customer_id", owner_customer}, {"kind", "zetta"}, {"name", "Bad"}});
    EXPECT_FALSE(bad_name.ok);
    EXPECT_FALSE(call(*owner_principal,
                      "create_service",
                      {{"customer_id", owner_customer},
                       {"kind", "sqlite"},
                       {"name", "notes"},
                       {"instances", 2}})
                     .ok)
        << "SQLite has one writer";
    for (const char* kind : {"kafka", "clickhouse", "tidb", "cassandra", "valkey", "sqlite"}) {
        const auto catalog = call(
            *owner_principal,
            "create_service",
            {{"customer_id", owner_customer}, {"kind", kind}, {"name", std::string("c-") + kind}});
        ASSERT_TRUE(catalog.ok) << kind << ": " << catalog.error.message;
        EXPECT_EQ(catalog.value["kind"], kind);
        // Deleted at once, so the per-customer cap is not what this loop tests.
        ASSERT_TRUE(store.withdraw_service(catalog.value["id"]));
    }
    EXPECT_FALSE(call(*owner_principal,
                      "create_service",
                      {{"customer_id", owner_customer}, {"kind", "mongodb"}, {"name", "s"}})
                     .ok)
        << "a kind outside the catalog is refused";
    const auto listed = call(*owner_principal, "list_services", {{"customer_id", owner_customer}});
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["services"].size(), 8U) << "the workspace, crm and six deleting";
    EXPECT_FALSE(call(*other_principal, "list_services", {{"customer_id", owner_customer}}).ok);
    EXPECT_FALSE(call(*other_principal,
                      "create_service",
                      {{"customer_id", owner_customer}, {"kind", "zetta"}, {"name", "x"}})
                     .ok);

    const auto early = call(*owner_principal,
                            "service_connection",
                            {{"customer_id", owner_customer}, {"service_id", service_id}});
    ASSERT_FALSE(early.ok);
    EXPECT_EQ(early.error.code, "workspace_not_ready");
    const auto service = store.get_service(service_id);
    ASSERT_TRUE(store.set_service_status(service_id, "Ready", "", "prod-crm-rw:5432"));
    w.deliver(*service,
              {{"kind", "postgres"}, {"host", "prod-crm-rw"}, {"user", "app"}, {"password", "pw"}});
    const auto connection = call(*owner_principal,
                                 "service_connection",
                                 {{"customer_id", owner_customer}, {"service_id", service_id}});
    ASSERT_TRUE(connection.ok) << connection.error.message;
    EXPECT_EQ(connection.value["password"], "pw");
    EXPECT_EQ(listed.value.dump().find("pw"), std::string::npos);
    EXPECT_FALSE(connection.value.contains("kind"));

    // A SQLite service's credential is its bearer token.
    const auto sqlite =
        call(*owner_principal,
             "create_service",
             {{"customer_id", owner_customer}, {"kind", "sqlite"}, {"name", "notes"}});
    ASSERT_TRUE(sqlite.ok) << sqlite.error.message;
    const std::string sqlite_id = sqlite.value["id"];
    ASSERT_TRUE(store.set_service_status(sqlite_id, "Ready", "", "http://notes:8080"));
    w.deliver(*store.get_service(sqlite_id),
              {{"kind", "sqlite"},
               {"endpoint", "http://notes:8080"},
               {"token", "t0k"},
               {"database", "main"}});
    const auto sqlite_connection =
        call(*owner_principal,
             "service_connection",
             {{"customer_id", owner_customer}, {"service_id", sqlite_id}});
    ASSERT_TRUE(sqlite_connection.ok) << sqlite_connection.error.message;
    EXPECT_EQ(sqlite_connection.value["token"], "t0k");
    ASSERT_TRUE(store.withdraw_service(sqlite_id));
    const auto workspace_token =
        call(*owner_principal,
             "service_connection",
             {{"customer_id", owner_customer}, {"service_id", started.value["service_id"]}});
    EXPECT_FALSE(workspace_token.ok) << "a Zetta admin token is never handed out";

    const auto unconfirmed_service =
        call(*owner_principal,
             "delete_service",
             {{"customer_id", owner_customer}, {"service_id", service_id}, {"confirm", "yes"}});
    EXPECT_FALSE(unconfirmed_service.ok);
    const auto deleted_service =
        call(*owner_principal,
             "delete_service",
             {{"customer_id", owner_customer}, {"service_id", service_id}, {"confirm", "crm"}});
    ASSERT_TRUE(deleted_service.ok) << deleted_service.error.message;
    EXPECT_EQ(deleted_service.value["desired"], "absent");

    const std::string slug = lake::control::workspace_slug(owner_customer);
    const auto foreign_delete = call(
        *other_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    EXPECT_FALSE(foreign_delete.ok) << "another customer cannot delete the workspace";
    const auto unconfirmed = call(*owner_principal,
                                  "delete_workspace",
                                  {{"customer_id", owner_customer}, {"confirm", "yes"}});
    ASSERT_FALSE(unconfirmed.ok);
    EXPECT_EQ(unconfirmed.error.code, "invalid_input");
    const auto deleted = call(
        *owner_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["requested"], false);
}

TEST(Workspaces, StoreCapabilitiesRefuseUnavailablePairsAndCheckTheName) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto other = store.sign_in_clerk(minted("other"));
    ASSERT_TRUE(other);
    const auto owner_principal = store.context_for(w.secret);
    const auto other_principal = store.context_for(other->secret);
    ASSERT_TRUE(owner_principal);
    ASSERT_TRUE(other_principal);
    const std::string owner = w.customer_id;

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call =
        [&](const lake::control::Principal& principal, const std::string& name, json input) {
            lake::api::ApiContext context;
            context.store = &store;
            context.workspaces = w.service.get();
            context.authorized = true;
            context.acting_user_id = principal.user_id;
            context.acting_identity = principal.taap_id;
            context.request_id = lake::control::new_uuid_v4();
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
            EXPECT_TRUE(completed);
            return std::move(*completed);
        };

    const auto providers = call(*owner_principal, "list_store_providers", {{"customer_id", owner}});
    ASSERT_TRUE(providers.ok) << providers.error.message;
    ASSERT_EQ(providers.value["providers"].size(), std::size(lake::control::store_providers));
    EXPECT_EQ(providers.value["providers"][0]["type"], "volume");
    EXPECT_EQ(providers.value["providers"][0]["available"], true);
    EXPECT_NE(providers.value.dump().find("lake#196 phase 2"), std::string::npos);
    EXPECT_FALSE(call(*other_principal, "list_store_providers", {{"customer_id", owner}}).ok);

    const auto refused =
        call(*owner_principal,
             "create_store",
             {{"customer_id", owner}, {"type", "object"}, {"provider", "s3"}, {"name", "bucket"}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_NE(refused.error.message.find("your own credentials"), std::string::npos)
        << refused.error.message;
    EXPECT_FALSE(call(*owner_principal,
                      "create_store",
                      {{"customer_id", owner},
                       {"type", "volume"},
                       {"provider", "cluster"},
                       {"name", "big"},
                       {"size_gi", 101}})
                     .ok)
        << "size is 1-100";
    EXPECT_FALSE(
        call(*owner_principal,
             "create_store",
             {{"customer_id", owner}, {"type", "volume"}, {"provider", "cluster"}, {"name", "Bad"}})
            .ok);
    EXPECT_FALSE(call(*other_principal,
                      "create_store",
                      {{"customer_id", owner},
                       {"type", "volume"},
                       {"provider", "cluster"},
                       {"name", "theirs"}})
                     .ok);

    const auto created = call(*owner_principal,
                              "create_store",
                              {{"customer_id", owner},
                               {"type", "volume"},
                               {"provider", "cluster"},
                               {"name", "uploads"},
                               {"size_gi", 5}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["size_gi"], 5);
    EXPECT_EQ(created.value["public"], false);
    const std::string store_id = created.value["id"];

    const auto listed = call(*owner_principal, "list_stores", {{"customer_id", owner}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["stores"].size(), 1U);
    EXPECT_EQ(listed.value["stores"][0]["connected"], false);
    EXPECT_FALSE(call(*other_principal, "list_stores", {{"customer_id", owner}}).ok);

    const auto early = call(
        *owner_principal, "store_connection", {{"customer_id", owner}, {"store_id", store_id}});
    ASSERT_FALSE(early.ok);
    EXPECT_EQ(early.error.code, "workspace_not_ready");
    ASSERT_TRUE(store.set_store_status(store_id, "Ready", "the claim binds on first use", ""));
    const auto undelivered = call(
        *owner_principal, "store_connection", {{"customer_id", owner}, {"store_id", store_id}});
    ASSERT_FALSE(undelivered.ok) << "Ready, but the connection has not reached lake";
    EXPECT_NE(undelivered.error.message.find("still being delivered"), std::string::npos);
    const auto record = store.get_store(store_id);
    ASSERT_TRUE(record);
    std::ofstream(w.dir / (record->slug + ".json")) << json{{"type", "volume"},
                                                            {"provider", "cluster"},
                                                            {"claim", "prod-" + record->slug},
                                                            {"namespace", "lake"},
                                                            {"size_gi", 5},
                                                            {"storage_class", "managed-csi"}}
                                                           .dump();
    const auto connection = call(
        *owner_principal, "store_connection", {{"customer_id", owner}, {"store_id", store_id}});
    ASSERT_TRUE(connection.ok) << connection.error.message;
    EXPECT_EQ(connection.value["claim"], "prod-" + record->slug);
    EXPECT_FALSE(
        call(*other_principal, "store_connection", {{"customer_id", owner}, {"store_id", store_id}})
            .ok);

    const auto mismatch =
        call(*owner_principal,
             "delete_store",
             {{"customer_id", owner}, {"store_id", store_id}, {"confirm", "yes"}});
    ASSERT_FALSE(mismatch.ok);
    EXPECT_EQ(mismatch.error.code, "invalid_input");
    EXPECT_EQ(store.get_store(store_id)->desired, "present") << "nothing was deleted";
    EXPECT_FALSE(call(*other_principal,
                      "delete_store",
                      {{"customer_id", owner}, {"store_id", store_id}, {"confirm", "uploads"}})
                     .ok);
    const auto deleted =
        call(*owner_principal,
             "delete_store",
             {{"customer_id", owner}, {"store_id", store_id}, {"confirm", "uploads"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["desired"], "absent");
    EXPECT_FALSE(
        call(*owner_principal, "store_connection", {{"customer_id", owner}, {"store_id", store_id}})
            .ok)
        << "a deleting store is not handed out";
}

TEST(Workspaces, DataPageShowsStorageWithTypedDeleteAndRefreshesWhileChanging) {
    lake::api::WorkspacePage page;
    page.section = "data";
    page.customer_id = std::string(customer);
    page.data["list_stores"] = {{"stores", json::array()}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<h2>Storage</h2>"), std::string::npos)
        << "storage shows before the workspace is ready";
    EXPECT_NE(html.find("Start Your Workspace"), std::string::npos);
    EXPECT_NE(html.find("No storage yet."), std::string::npos);
    EXPECT_NE(html.find("<h3>New Storage</h3>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/store-create\""), std::string::npos);
    EXPECT_NE(html.find("<option value=\"volume:cluster\" selected>Volume — cluster disk</option>"),
              std::string::npos);
    EXPECT_NE(html.find("<option value=\"object:r2\" disabled>Bucket — Cloudflare R2 — not yet: "
                        "needs R2 enabled on Orama&#39;s Cloudflare account"),
              std::string::npos)
        << html;
    EXPECT_NE(html.find("value=\"network_filesystem:nfs\" disabled"), std::string::npos);
    EXPECT_NE(html.find("Filesystem — Drive — not yet: phase 3</option>"), std::string::npos);
    EXPECT_NE(html.find("name=\"size_gi\""), std::string::npos);
    EXPECT_EQ(html.find("content=\"3\""), std::string::npos) << "nothing is changing";

    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_stores"]["stores"] = json::array({{{"id", "s1"},
                                                       {"name", "uploads"},
                                                       {"type", "volume"},
                                                       {"provider", "cluster"},
                                                       {"size_gi", 10},
                                                       {"desired", "present"},
                                                       {"phase", "Ready"},
                                                       {"message", "claim binds on first use"},
                                                       {"connected", true}}});
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<td>uploads</td><td>Volume</td><td>cluster disk</td><td>10 GiB</td>"
                        "<td>Ready<br><span class=\"muted\">claim binds on first use</span>"),
              std::string::npos)
        << html;
    EXPECT_NE(html.find("action=\"/train/data/store-connection\""), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/store-delete\""), std::string::npos);
    EXPECT_NE(html.find("deletes the data in it"), std::string::npos);
    EXPECT_NE(html.find("Type <code>uploads</code> to delete it<input name=\"confirm\" required"),
              std::string::npos);
    EXPECT_NE(html.find("name=\"customer_id\" value=\"" + std::string(customer) + "\""),
              std::string::npos);
    EXPECT_EQ(html.find("content=\"3\""), std::string::npos) << "a Ready store is settled";

    page.data["list_stores"]["stores"][0]["phase"] = "";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Waiting for the operator"), std::string::npos);
    EXPECT_NE(html.find("<meta http-equiv=\"refresh\" content=\"3\">"), std::string::npos);
    EXPECT_EQ(html.find("action=\"/train/data/store-connection\""), std::string::npos)
        << "no connection before Ready";

    page.data["list_stores"]["stores"][0]["phase"] = "Ready";
    page.data["list_stores"]["stores"][0]["desired"] = "absent";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<td>Deleting"), std::string::npos);
    EXPECT_EQ(html.find("action=\"/train/data/store-delete\""), std::string::npos);
    EXPECT_NE(html.find("content=\"3\""), std::string::npos);

    page.data["store_connection"] = {
        {"name", "uploads"}, {"claim", "prod-st-0123456789ab"}, {"namespace", "lake"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<h2>Connection to uploads</h2>"), std::string::npos);
    EXPECT_NE(html.find("<code>prod-st-0123456789ab</code>"), std::string::npos);
}

TEST(Workspaces, PagesGateOnTheWorkspaceAndRenderZettaResults) {
    lake::api::WorkspacePage page;
    page.section = "projects";
    page.customer_id = std::string(customer);
    page.customers = {{std::string(customer), "grand-lantern-42", true},
                      {"11111111-2222-3333-4444-555555555555", "acme", false}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Start Your Workspace"), std::string::npos);
    EXPECT_NE(html.find("action=\"/workspace/start\""), std::string::npos);
    EXPECT_NE(html.find("grand-lantern-42 (your personal space)"), std::string::npos);
    EXPECT_NE(html.find("<p class=\"eyebrow\">Design</p>"), std::string::npos);

    page.state = {{"requested", true}, {"phase", "Provisioning"}, {"ready", false}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Preparing Your Workspace"), std::string::npos);
    EXPECT_NE(html.find("http-equiv=\"refresh\""), std::string::npos);

    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_projects"] = {{"projects", json::array({"retention<review>"})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("retention&lt;review&gt;"), std::string::npos);
    EXPECT_EQ(html.find("retention<review>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/projects/create\""), std::string::npos);
    EXPECT_EQ(html.find("action=\"/workspace/delete\""), std::string::npos)
        << "the delete form needs the workspace id from the state";
    page.state["workspace"] = "ws-0f1e2d3c4b5a";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("action=\"/workspace/delete\""), std::string::npos);
    EXPECT_NE(html.find("name=\"confirm\" value=\"ws-0f1e2d3c4b5a\" required"), std::string::npos);

    page.state = {{"requested", true},
                  {"phase", "Failed"},
                  {"ready", false},
                  {"workspace", "ws-0f1e2d3c4b5a"},
                  {"message", "workspace quota reached (3)"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("as many workspaces as it allows"), std::string::npos);
    EXPECT_NE(html.find(">Cancel workspace</button>"), std::string::npos);
    page.state = {
        {"requested", true}, {"phase", "Ready"}, {"ready", true}, {"workspace", "ws-0f1e2d3c4b5a"}};

    page.section = "project";
    page.subject = "retention";
    page.data["get_project"] = {
        {"graphs",
         json::array({{{"id", "g1"}, {"name", "proposed plan"}, {"approval", "proposed"}},
                      {{"id", "g2"}, {"name", "approved plan"}, {"approval", "approved"}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Approve</button>"), std::string::npos);
    EXPECT_NE(html.find(">Check and run</button>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/project/delete\""), std::string::npos);
    EXPECT_LT(html.find("/design/project/approve"), html.find("/design/project/run"))
        << "the proposed plan offers Approve, the approved one Check and run";
    page.subject.clear();

    page.section = "run";
    page.subject = "r1";
    page.data["get_run"] = {{"status", "running"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("The report appears when the run finishes"), std::string::npos);
    page.data["get_run"] = {{"status", "succeeded"}};
    page.data["get_run_report"] = {{"model", {{"auc", 0.9}}}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<h2>Report</h2>"), std::string::npos);
    page.subject.clear();

    page.section = "data";
    page.data["list_connections"] = json::object();
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("action=\"/train/data/connect\""), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/upload\" enctype=\"multipart/form-data\""),
              std::string::npos)
        << "a file can be uploaded without any connection";
    EXPECT_NE(html.find("type=\"password\" name=\"token\""), std::string::npos);
    EXPECT_EQ(html.find("action=\"/train/data/pull\""), std::string::npos)
        << "no dataset can be pulled before a connection exists";
    page.data["list_connections"] = {
        {"connections",
         json::array(
             {{{"name", "warehouse"},
               {"connector", {{"type", "postgres"}, {"host", "db.test"}, {"database", "crm"}}}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("db.test/crm"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/pull\""), std::string::npos);
    EXPECT_NE(html.find("<option>warehouse</option>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/disconnect\""), std::string::npos);

    page.section = "models";
    page.data["list_models"] = {{"models", json::array({{{"name", "churn"}, {"kind", "gbdt"}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<p class=\"eyebrow\">Ship</p>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/ship/models/publish\""), std::string::npos);

    page.unavailable = "Design, Train and Ship are not available on this lake yet";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("not available on this lake yet"), std::string::npos);
    EXPECT_EQ(html.find("Start Your Workspace"), std::string::npos);
}

TEST(Workspaces, ServicesPageListsEveryServiceWithoutCredentials) {
    lake::api::WorkspacePage page;
    page.section = "services";
    page.customer_id = std::string(customer);
    page.data["list_services"] = {{"services", json::array()}, {"limit", 5}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("No services yet."), std::string::npos);
    EXPECT_NE(html.find("action=\"/services/create\""), std::string::npos);
    EXPECT_NE(html.find("may run 5 services"), std::string::npos) << "a configured limit is stated";
    page.data["list_services"]["limit"] = 0;
    EXPECT_EQ(lake::api::web_ui_workspace(page, true).find("may run"), std::string::npos)
        << "no limit (the default) says nothing";
    page.data["list_services"]["limit"] = 5;
    EXPECT_EQ(html.find("Start Your Workspace"), std::string::npos)
        << "the Services page does not wait for the workspace";

    page.data["list_services"]["services"] =
        json::array({{{"id", "a"},
                      {"name", "workspace"},
                      {"kind", "zetta"},
                      {"default", true},
                      {"instances", 1},
                      {"storage_gi", 10},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"endpoint", "http://prod-ws-1-zetta-client:4800"}},
                     {{"id", "b"},
                      {"name", "crm"},
                      {"kind", "postgres"},
                      {"instances", 2},
                      {"storage_gi", 20},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"endpoint", "prod-svc-1-rw:5432"}},
                     {{"id", "c"},
                      {"name", "old"},
                      {"kind", "postgres"},
                      {"instances", 1},
                      {"storage_gi", 10},
                      {"desired", "absent"},
                      {"phase", "Ready"}}});
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("workspace <span class=\"muted\">(workspace)</span>"), std::string::npos);
    EXPECT_NE(html.find("2 × 20 GiB"), std::string::npos);
    EXPECT_NE(html.find("prod-svc-1-rw:5432"), std::string::npos);
    EXPECT_NE(html.find(">Deleting<"), std::string::npos) << "an absent service is deleting";
    EXPECT_NE(html.find("action=\"/services/connection\""), std::string::npos);
    EXPECT_NE(html.find(">Delete crm</button>"), std::string::npos);
    EXPECT_EQ(html.find(">Delete old</button>"), std::string::npos);
    EXPECT_NE(html.find("http-equiv=\"refresh\""), std::string::npos) << "one is still changing";
    page.data["list_services"]["services"][1]["connected"] = false;
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Connecting<"), std::string::npos)
        << "Ready but not yet delivered to lake's mount";
    EXPECT_EQ(html.find("action=\"/services/connection\""), std::string::npos);
    page.data["list_services"]["services"][1]["connected"] = true;

    page.data["service_connection"] = {{"name", "crm"}, {"uri", "postgresql://app:pw@host/app"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("postgresql://app:pw@host/app"), std::string::npos);
    page.data["service_connection"] = {{"name", "events"},
                                       {"bootstrap", "prod-svc-1-kafka-bootstrap:9092"},
                                       {"sasl_mechanism", "SCRAM-SHA-512"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<dt>bootstrap</dt>"), std::string::npos)
        << "every kind's connection fields are shown";
    // The menu: Zetta first and selected, then every other kind by label; each with its usual
    // start time; no kind is labelled preview (lake#197).
    const std::string menu =
        html.substr(html.find("<select name=\"kind\">"),
                    html.find("</select>", html.find("<select name=\"kind\">")) -
                        html.find("<select name=\"kind\">"));
    EXPECT_NE(menu.find("<select name=\"kind\"><option value=\"zetta\" selected>Zetta — ready in "
                        "about 30 seconds</option>"),
              std::string::npos)
        << menu;
    std::size_t last = 0;
    for (const char* key : {"cassandra", "clickhouse", "duckdb", "kafka", "polars", "postgres",
                            "sqlite", "tidb", "valkey"}) {
        const std::size_t at = menu.find(std::string("<option value=\"") + key + "\">");
        EXPECT_NE(at, std::string::npos) << key;
        EXPECT_GT(at, last) << key << " is out of order";
        last = at;
    }
    EXPECT_NE(menu.find("<option value=\"duckdb\">DuckDB — ready in about 30 seconds</option>"),
              std::string::npos);
    EXPECT_NE(menu.find("<option value=\"cassandra\">Cassandra — ready in about 7 minutes</option>"),
              std::string::npos);
    EXPECT_EQ(menu.find("preview"), std::string::npos);
    // A starting service says how long its kind usually takes and how long it has been going.
    page.data["list_services"]["services"] =
        json::array({{{"id", "d"},
                      {"name", "events"},
                      {"kind", "cassandra"},
                      {"instances", 1},
                      {"storage_gi", 2},
                      {"desired", "present"},
                      {"phase", "Provisioning"},
                      {"message", "waiting for the Cassandra datacenter"},
                      {"created_at", "2020-01-01 00:00:00.12+00"}},
                     {{"id", "e"},
                      {"name", "cache"},
                      {"kind", "valkey"},
                      {"instances", 1},
                      {"storage_gi", 2},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"connected", true}}});
    page.data.erase("service_connection");
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Usually ready in about 7 minutes; started "), std::string::npos)
        << "Postgres' own timestamp text is read";
    EXPECT_NE(html.find("taking longer than usual"), std::string::npos)
        << "a service far past its usual time says so";
    EXPECT_EQ(html.find("Usually ready in about 60 seconds"), std::string::npos)
        << "a Ready service shows no start hint";
    EXPECT_NE(html.find("every 3 seconds"), std::string::npos)
        << "a starting service keeps the page refreshing fast";

    // Zetta services are listed first.
    page.data["list_services"]["services"] = json::array(
        {{{"id", "p"}, {"name", "crm"}, {"kind", "postgres"}, {"desired", "present"}, {"phase", "Ready"}},
         {{"id", "z"}, {"name", "workspace"}, {"kind", "zetta"}, {"desired", "present"}, {"phase", "Ready"}}});
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_LT(html.find(">workspace<"), html.find(">crm<"));
}

TEST(Workspaces, NavigationOffersDesignTrainAndShip) {
    const std::string home = lake::api::web_ui_home(true);
    for (const std::string_view label : {"<p class=\"nav-label\">Design</p>",
                                         "<p class=\"nav-label\">Train</p>",
                                         "<p class=\"nav-label\">Ship</p>",
                                         "href=\"/design/projects\"",
                                         "href=\"/train/data\"",
                                         "href=\"/train/runs\"",
                                         "href=\"/ship/models\"",
                                         "href=\"/ship/indexes\"",
                                         "href=\"/deployments\"",
                                         "href=\"/services\""}) {
        EXPECT_NE(home.find(label), std::string::npos) << label;
    }
    EXPECT_EQ(lake::api::web_ui_home().find("/design/projects"), std::string::npos);
    lake::api::WebUiViewer viewer{true};
    viewer.path = "/train/run";
    EXPECT_NE(lake::api::web_ui_home(viewer).find("href=\"/train/runs\" aria-current=\"page\""),
              std::string::npos);
}

TEST(Workspaces, PackagesPageListsTheRegistriesWithInstallCommands) {
    const json catalog{{"oci", json::array({{{"name", "scoring"}, {"versions", {"v1", "v2"}}}})},
                       {"npm", json::array({{{"name", "@acme/ui<x>"}, {"versions", {"1.0.0"}}}})},
                       {"python", json::array()}};
    const std::string html =
        lake::api::web_ui_packages(catalog, true, "https://lake.example.test", true);
    EXPECT_NE(html.find("docker pull lake.example.test/scoring:v2"), std::string::npos);
    EXPECT_NE(html.find("npm install @acme/ui&lt;x&gt; --registry https://lake.example.test"),
              std::string::npos);
    EXPECT_NE(html.find("None published."), std::string::npos);
    EXPECT_NE(lake::api::web_ui_packages(json::object(), false, "", true)
                  .find("no registry storage configured"),
              std::string::npos);
    EXPECT_NE(lake::api::web_ui_home(true).find("href=\"/ship/packages\""), std::string::npos);
}

TEST(Workspaces, EditPipelineReadsValidatesAndSavesTheWholeGraph) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    ASSERT_TRUE(principal);
    auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_graph"] = {
        {"graph_id", "g-1"},
        {"name", "churn"},
        {"project", "retention"},
        {"approval", "approved"},
        {"nodes",
         json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull"}},
                      {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}})},
        {"edges", json::array({{{"from", "pull"}, {"to", "train"}, {"kind", "data_flow"}}})}};
    w.caller->answers["save_graph"] = {
        {"graph_id", "g-1"}, {"name", "churn"}, {"approval", "proposed"}};

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = w.customer_id;
        input["project"] = "retention";
        input["graph"] = "churn";
        auto completed =
            liquid::runtime::block_on(registry.call_async("edit_pipeline", input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };

    const auto added = call({{"op", "add_step"}, {"kind", "serve"}, {"name", "Rank"}});
    ASSERT_TRUE(added.ok) << added.error.message;
    EXPECT_EQ(added.value["step"], "serve-1");
    ASSERT_EQ(w.caller->calls.back().first, "save_graph");
    const json saved = w.caller->calls.back().second;
    EXPECT_EQ(saved["graph"], "g-1") << "saved by id, so a rename cannot orphan it";
    EXPECT_EQ(saved["nodes"].size(), 3U);
    EXPECT_EQ(saved["edges"].size(), 1U);
    EXPECT_FALSE(saved.contains("new_name"));

    const std::size_t before = w.caller->calls.size();
    const auto cycle = call({{"op", "add_edge"}, {"from", "train"}, {"to", "pull"}});
    ASSERT_FALSE(cycle.ok);
    EXPECT_EQ(cycle.error.code, "invalid_input");
    EXPECT_NE(cycle.error.message.find("cycle"), std::string::npos);
    EXPECT_EQ(w.caller->calls.size(), before + 1) << "read, refused, nothing saved";
    EXPECT_EQ(w.caller->calls.back().first, "get_graph");

    const auto renamed = call({{"op", "rename"}, {"name", "churn v2"}});
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value["name"], "churn v2");
    EXPECT_EQ(w.caller->calls.back().second["new_name"], "churn v2");
    EXPECT_TRUE(registry.find("edit_pipeline") != nullptr) << "REST, OpenAPI and MCP render it";

    // A proposal is applied as one save: every edit or none (lake#249).
    const auto applied = call({{"edits",
                                json::array({{{"op", "set_attribute"},
                                              {"step", "pull"},
                                              {"name", "table"},
                                              {"value", "ux_review_churn"}},
                                             {{"op", "remove_step"}, {"step", "train"}}})}});
    ASSERT_TRUE(applied.ok) << applied.error.message;
    const json batch = w.caller->calls.back().second;
    EXPECT_EQ(batch["nodes"].size(), 1U);
    EXPECT_EQ(batch["nodes"][0]["attributes"]["table"], "ux_review_churn");
    const std::size_t saves = w.caller->calls.size();
    const auto half = call({{"edits",
                             json::array({{{"op", "rename_step"}, {"step", "pull"}, {"name", "P"}},
                                          {{"op", "remove_step"}, {"step", "nope"}}})}});
    ASSERT_FALSE(half.ok);
    EXPECT_NE(half.error.message.find("edit 2"), std::string::npos);
    EXPECT_EQ(w.caller->calls.size(), saves + 1) << "read, refused, nothing saved";
    EXPECT_FALSE(call(json::object()).ok) << "an op or edits";
}

TEST(Workspaces, PipelinePageShowsTheGraphInspectorEditsGovernanceAndRuns) {
    lake::api::WorkspacePage page;
    page.section = "pipeline";
    page.customer_id = std::string(customer);
    page.project = "retention";
    page.subject = "churn";
    page.step = "train";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_graph"] = {
        {"graph_id", "g-1"},
        {"name", "churn"},
        {"project", "retention"},
        {"approval", "proposed"},
        {"nodes",
         json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull"}},
                      {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}})},
        {"edges", json::array({{{"from", "pull"}, {"to", "train"}, {"kind", "data_flow"}}})}};
    page.data["list_runs"] = {{"runs",
                               json::array({{{"id", "r1"},
                                             {"graph", "churn"},
                                             {"project", "retention"},
                                             {"status", "succeeded"},
                                             {"created", "2026-09-26T10:00:00Z"}}})}};
    page.data["latest_run"] = {{"status", "succeeded"},
                               {"lineage", {{"steps", json::array({"pull", "train"})}}}};
    page.data["list_datasets"] = {
        {"datasets", json::array({{{"name", "accounts"}, {"database", "datasets"}}})}};
    page.data["gate"] = {
        {"verdict", "fail"},
        {"block_deploy", true},
        {"checks",
         json::array({{{"name", "has_source"}, {"status", "fail"}, {"reason", "no data"}}})}};
    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<svg class=\"pipeline-graph\""), std::string::npos);
    EXPECT_NE(html.find("step-model status-succeeded selected"), std::string::npos);
    EXPECT_NE(html.find("<h2>Step Train</h2>"), std::string::npos);
    EXPECT_NE(html.find("proposed — approve before it runs"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/pipeline/approve\""), std::string::npos);
    EXPECT_NE(html.find("Check and run</button>"), std::string::npos);
    EXPECT_NE(html.find(" disabled>Check and run"), std::string::npos) << "not approved yet";
    EXPECT_NE(html.find("<option value=\"datasets.accounts\">"), std::string::npos);
    EXPECT_NE(html.find("has_source"), std::string::npos) << "every gate check is shown";
    EXPECT_NE(html.find("no data"), std::string::npos);
    for (const char* op : {"rename_step",
                           "kind_step",
                           "remove_step",
                           "add_step",
                           "add_edge",
                           "remove_edge",
                           "rename"}) {
        EXPECT_NE(html.find(std::string("name=\"op\" value=\"") + op + "\""), std::string::npos)
            << op;
    }
    EXPECT_NE(html.find("/train/run?customer_id="), std::string::npos) << "its runs link out";
    EXPECT_NE(html.find("action=\"/design/pipeline/ask\""), std::string::npos)
        << "the Project Agent can be asked for a change";
    EXPECT_NE(html.find("name=\"holdout_fraction\" type=\"number\" min=\"0\" max=\"0.9\" "
                        "step=\"0.05\" value=\"0.2\""),
              std::string::npos)
        << "a real hold-out by default, not a placeholder";
    EXPECT_NE(html.find("name=\"model_name\" value=\"retention_churn\""), std::string::npos)
        << "the model is named after this pipeline";
    EXPECT_EQ(html.find("Proposed by the Project Agent"), std::string::npos);

    // A proposal to review: the edits in words, the graph after, Apply and Discard.
    page.data["proposal"] = {
        {"summary", "Train on accounts only"},
        {"steps", json::array({"Remove step pull and its edges"})},
        {"edits", json::array({{{"op", "remove_step"}, {"step", "pull"}}})},
        {"run", {{"dataset", "datasets.accounts"}, {"holdout", 0.25}}},
        {"problems", json::array()},
        {"after",
         {{"nodes", json::array({{{"id", "train"}, {"kind", "model"}, {"name", "Train"}}})},
          {"edges", json::array()}}}};
    page.data["run_dataset"] = "datasets.accounts";
    const std::string reviewed = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(reviewed.find("Proposed by the Project Agent"), std::string::npos);
    EXPECT_NE(reviewed.find("Remove step pull and its edges"), std::string::npos);
    EXPECT_NE(reviewed.find("Hold out 25% of rows"), std::string::npos);
    EXPECT_NE(reviewed.find("action=\"/design/pipeline/apply\""), std::string::npos);
    EXPECT_NE(reviewed.find("<option value=\"datasets.accounts\" selected>"), std::string::npos);
    page.data["proposal"]["problems"] = json::array({"edit 1: there is no step pull"});
    const std::string refused = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(refused.find("Why it cannot be applied"), std::string::npos);
    EXPECT_EQ(refused.find("action=\"/design/pipeline/apply\""), std::string::npos)
        << "a proposal that would not apply cannot be applied";
    page.data.erase("proposal");
    page.data.erase("run_dataset");

    page.section = "run";
    page.subject = "r1";
    page.data = {{"get_run",
                  {{"status", "succeeded"},
                   {"graph", "churn"},
                   {"project", "retention"},
                   {"logs", json::array({{{"level", "info"}, {"message", "trained"}}})}}},
                 {"get_run_report",
                  {{"dataset", {{"accounts", 120}, {"features", json::array({"mrr", "seats"})}}},
                   {"drivers", json::array({{{"feature", "failed_payments"}, {"weight", 26.4}}})},
                   {"at_risk", json::array({{{"account", "acme"}, {"risk", 0.91}}})}}},
                 {"list_traces",
                  {{"spans",
                    json::array({{{"name", "run"},
                                  {"duration_ms", 12.5},
                                  {"ok", true},
                                  {"attributes", {{"run_id", "r1"}}}}})}}}};
    const std::string run = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(run.find("What the Model Leans On"), std::string::npos);
    EXPECT_NE(run.find("failed_payments"), std::string::npos);
    EXPECT_NE(run.find("Most at Risk"), std::string::npos);
    EXPECT_NE(run.find("mrr, seats"), std::string::npos);
    EXPECT_NE(run.find("<h2>Trace</h2>"), std::string::npos);
    EXPECT_NE(run.find("<h2>Log</h2>"), std::string::npos);
    EXPECT_NE(run.find("/design/pipeline?"), std::string::npos) << "a run links to its pipeline";
}

TEST(Workspaces, SearchIndexAnswersOnlyForAPublishedIndex) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_index"] = {{"id", "i-1"}, {"table", "docs"}, {"exposed", false}};
    w.caller->answers["search"] = {{"hits", json::array({{{"id", "doc-7"}, {"score", 2.5}}})}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto search = [&](json extra) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        json input{{"customer_id", w.customer_id}, {"index", "i-1"}, {"query", "renewal"}};
        input.update(extra);
        auto completed =
            liquid::runtime::block_on(registry.call_async("search_index", input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto refused = search(json::object());
    ASSERT_FALSE(refused.ok);
    EXPECT_NE(refused.error.message.find("not published"), std::string::npos);
    EXPECT_NE(w.caller->calls.back().first, "search") << "nothing reaches search";

    w.caller->answers["get_index"]["exposed"] = true;
    const auto found = search({{"field", "body"}, {"top_k", 3}});
    ASSERT_TRUE(found.ok) << found.error.message;
    EXPECT_EQ(found.value["hits"][0]["id"], "doc-7");
    EXPECT_EQ(w.caller->calls.back().first, "search");
    EXPECT_EQ(w.caller->calls.back().second["target"], "i-1");
    EXPECT_EQ(w.caller->calls.back().second["field"], "body");
    EXPECT_EQ(w.caller->calls.back().second["top_k"], 3);
    EXPECT_EQ(registry.find("search_index")->kind, lake::api::CapabilityKind::Read)
        << "GET /v1/search_index for outside callers";
}

TEST(Workspaces, IndexPageSearchesPublishesAndShowsHowToCallIt) {
    lake::api::WorkspacePage page;
    page.section = "index";
    page.customer_id = std::string(customer);
    page.subject = "i-1";
    page.origin = "https://lake.example";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_index"] = {{"id", "i-1"},
                              {"table", "docs"},
                              {"kind", "search"},
                              {"fields", json::array({"title", "body"})},
                              {"exposed", false}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Publish</button>"), std::string::npos);
    EXPECT_NE(html.find("Publish the index first"), std::string::npos);
    EXPECT_EQ(html.find("Create a search key"), std::string::npos);
    EXPECT_NE(html.find("<option>body</option>"), std::string::npos);

    page.data["get_index"]["exposed"] = true;
    page.data["search_query"] = "renewal";
    page.data["search"] = {{"hits", json::array({{{"id", "doc-7"}, {"score", 2.5}}})},
                           {"duration_ms", 1.2}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Unpublish</button>"), std::string::npos);
    EXPECT_NE(html.find("doc-7"), std::string::npos);
    EXPECT_NE(html.find("1 hits"), std::string::npos);
    EXPECT_NE(html.find("value=\"search_index\""), std::string::npos) << "a key scoped to search";
    EXPECT_NE(html.find("https://lake.example/v1/search_index?customer_id="), std::string::npos);
    EXPECT_NE(html.find("MCP tool <code>search_index</code>"), std::string::npos);
}

TEST(Workspaces, ASearchKeyFromAnOlderSessionLastsAsLongAsTheSession) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    ASSERT_TRUE(principal);
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.session_credential = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.credential_session_id = principal->session_id;
    context.credential_kind = principal->kind;
    context.pool_id = principal->pool_id;
    context.scopes = {"*"};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    context.credential_expires_at_unix = now + 10 * 24 * 60 * 60; // ten days left
    context.request_id = lake::control::new_uuid_v4();

    const auto capped =
        registry.call("create_token",
                      {{"name", "search docs"}, {"scopes", json::array({"search_index"})}},
                      context);
    ASSERT_TRUE(capped.ok) << capped.error.message;
    context.request_id = lake::control::new_uuid_v4();
    const auto too_long = registry.call("create_token",
                                        {{"name", "search docs"},
                                         {"scopes", json::array({"search_index"})},
                                         {"expires_in_seconds", 30 * 24 * 60 * 60}},
                                        context);
    EXPECT_FALSE(too_long.ok) << "an explicit lifetime past the session is still refused";
}

TEST(Workspaces, ConnectServiceFillsTheConnectionFromTheServiceItself) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    const auto crm = w.service->create_service({w.customer_id, "postgres", "crm", ""});
    ASSERT_TRUE(crm);
    const auto cache = w.service->create_service({w.customer_id, "valkey", "cache", ""});
    ASSERT_TRUE(cache);
    w.caller->answers["create_connection"] = {{"name", "crm"}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto connect = [&](const std::string& customer_id, const std::string& service_id) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(
            registry.call_async("connect_service",
                                {{"customer_id", customer_id}, {"service_id", service_id}},
                                context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto early = connect(w.customer_id, crm->id);
    ASSERT_FALSE(early.ok);
    EXPECT_EQ(early.error.code, "workspace_not_ready");

    ASSERT_TRUE(store.set_service_status(
        crm->id, "Ready", "", "prod-svc-1-rw.lake.svc.cluster.local:5432"));
    w.deliver(*store.get_service(crm->id),
              {{"kind", "postgres"},
               {"host", "prod-svc-1-rw.lake.svc.cluster.local"},
               {"port", 5432},
               {"database", "app"},
               {"user", "app"},
               {"password", "s3cret"}});
    const auto connected = connect(w.customer_id, crm->id);
    ASSERT_TRUE(connected.ok) << connected.error.message;
    EXPECT_EQ(connected.value["service_name"], "crm");
    ASSERT_EQ(w.caller->calls.back().first, "create_connection");
    const json sent = w.caller->calls.back().second;
    EXPECT_EQ(sent["name"], "crm");
    EXPECT_EQ(sent["connector"]["type"], "postgres");
    EXPECT_EQ(sent["connector"]["host"], "prod-svc-1-rw.lake.svc.cluster.local");
    EXPECT_EQ(sent["connector"]["port"], "5432");
    EXPECT_EQ(sent["connector"]["token"], "s3cret") << "the password goes to Zetta, not to a form";
    EXPECT_EQ(connected.value.dump().find("s3cret"), std::string::npos) << "and never back out";

    ASSERT_TRUE(store.set_service_status(cache->id, "Ready", "", "valkey:6379"));
    w.deliver(*store.get_service(cache->id), {{"kind", "valkey"}, {"host", "valkey"}});
    const auto unsupported = connect(w.customer_id, cache->id);
    ASSERT_FALSE(unsupported.ok);
    EXPECT_NE(unsupported.error.message.find("cannot read a valkey"), std::string::npos);

    const auto foreign = connect(w.customer_id, lake::control::new_uuid_v4());
    EXPECT_FALSE(foreign.ok) << "a service that is not the customer's is not found";
}

TEST(Workspaces, DataPageOffersTheSpacesServicesAndServicesWarnBeforeDeletingAUsedOne) {
    lake::api::WorkspacePage page;
    page.section = "data";
    page.customer_id = std::string(customer);
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_connections"] = {
        {"connections",
         json::array(
             {{{"name", "crm"},
               {"connector",
                {{"type", "postgres"}, {"host", "prod-svc-1-rw"}, {"database", "app"}}}}})}};
    page.data["list_services"] = {{"services",
                                   json::array({{{"id", "a"},
                                                 {"name", "crm"},
                                                 {"kind", "postgres"},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"},
                                                 {"connected", true},
                                                 {"endpoint", "prod-svc-1-rw:5432"}},
                                                {{"id", "b"},
                                                 {"name", "cache"},
                                                 {"kind", "valkey"},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"},
                                                 {"connected", true}},
                                                {{"id", "c"},
                                                 {"name", "workspace"},
                                                 {"kind", "zetta"},
                                                 {"default", true},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"}}})}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Connect One of Your Services"), std::string::npos);
    EXPECT_NE(html.find(">Refresh</button>"), std::string::npos) << "crm is already connected";
    EXPECT_NE(html.find("not supported yet: the workspace cannot read valkey"), std::string::npos);
    EXPECT_EQ(html.find(">workspace</td>"), std::string::npos)
        << "the workspace itself is not offered";
    EXPECT_NE(html.find("action=\"/train/data/connect-service\""), std::string::npos);
    EXPECT_NE(html.find("<td><a href=\"/services?customer_id="), std::string::npos)
        << "the connection shows the service it came from";

    page.section = "services";
    page.data["list_services"]["limit"] = 5;
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("connection <code>crm</code> uses it"), std::string::npos);
}

TEST(Workspaces, EveryNewFeatureIsInOpenApiAndWorksOverMcp) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_graph"] = {
        {"graph_id", "g-1"},
        {"name", "churn"},
        {"project", "retention"},
        {"approval", "approved"},
        {"nodes", json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull"}}})},
        {"edges", json::array()}};
    w.caller->answers["list_runs"] = {{"runs",
                                       json::array({{{"id", "r1"},
                                                     {"graph", "churn"},
                                                     {"status", "succeeded"},
                                                     {"created", "2026-09-26T10:00:00Z"}}})}};
    w.caller->answers["get_run"] = {{"status", "succeeded"},
                                    {"lineage", {{"steps", json::array({"pull"})}}}};
    w.caller->answers["gate"] = {{"block_deploy", true}, {"verdict", "fail"}};
    w.caller->answers["import_n8n"] = {{"input_id", "in-1"}};
    w.caller->answers["compile_n8n"] = {{"name", "n8n workflow"}, {"approval", "proposed"}};

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.workspaces = w.service.get();
    context.authorized = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;

    const std::vector<std::string> features = {
        "workspace_status",   "start_workspace",    "delete_workspace",     "workspace_query",
        "workspace_act",      "list_services",      "create_service",       "delete_service",
        "service_connection", "list_service_kinds", "list_store_providers", "list_stores",
        "create_store",       "delete_store",       "store_connection",     "connect_service",
        "search_index",       "edit_pipeline",      "get_pipeline",         "run_pipeline",
        "create_pipeline"};
    const json openapi = lake::api::openapi_document(registry, "test");
    const auto tools = json::parse(
        *liquid::runtime::block_on(lake::api::mcp_dispatch_async(
                                       registry,
                                       {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                       context,
                                       "test"))
             .value());
    for (const std::string& feature : features) {
        EXPECT_TRUE(openapi["paths"].contains("/v1/" + feature)) << feature << " is not in OpenAPI";
        EXPECT_TRUE(std::ranges::any_of(
            tools["result"]["tools"],
            [&](const json& tool) { return tool.value("name", "") == feature; }))
            << feature << " is not an MCP tool";
    }

    const auto call = [&](const std::string& name, json arguments) {
        context.request_id = lake::control::new_uuid_v4();
        if (name != "list_service_kinds") {
            arguments["customer_id"] = w.customer_id;
        }
        const auto response = liquid::runtime::block_on(
            lake::api::mcp_dispatch_async(registry,
                                          {{"jsonrpc", "2.0"},
                                           {"id", 2},
                                           {"method", "tools/call"},
                                           {"params", {{"name", name}, {"arguments", arguments}}}},
                                          context,
                                          "test"));
        EXPECT_TRUE(response && *response);
        return json::parse(**response)["result"];
    };
    const json pipeline = call("get_pipeline", {{"project", "retention"}, {"graph", "churn"}});
    const json value =
        json::parse(pipeline.at("content").at(0).at("text").get<std::string>()).at("value");
    EXPECT_EQ(value["step_status"]["pull"], "succeeded");
    EXPECT_EQ(value["runs"].size(), 1U);

    const json blocked = call("run_pipeline", {{"project", "retention"}, {"graph", "churn"}});
    EXPECT_NE(blocked.dump().find("block_deploy"), std::string::npos);
    EXPECT_NE(w.caller->calls.back().first, "run") << "a failing gate runs nothing";

    call("create_pipeline", {{"project", "retention"}, {"from", "n8n"}, {"content", "{}"}});
    ASSERT_GE(w.caller->calls.size(), 2U);
    EXPECT_EQ(w.caller->calls[w.caller->calls.size() - 2].first, "import_n8n");
    EXPECT_EQ(w.caller->calls.back().first, "compile_n8n");
    EXPECT_EQ(w.caller->calls.back().second["input_id"], "in-1");

    const json kinds = call("list_service_kinds", json::object());
    EXPECT_NE(kinds.dump().find("cassandra"), std::string::npos);

    const json providers = call("list_store_providers", json::object());
    EXPECT_NE(providers.dump().find("cluster disk"), std::string::npos);
    const json created =
        call("create_store", {{"type", "volume"}, {"provider", "cluster"}, {"name", "scratch"}});
    EXPECT_EQ(created.value("isError", true), false) << created.dump();
    const json stores = call("list_stores", json::object());
    EXPECT_NE(stores.dump().find("scratch"), std::string::npos);
}

TEST(Workspaces, WorkspaceSqlRunsOnlyOneReadOnlyQuery) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["sql"] = {{"columns", json::array({"one"})},
                                {"rows", json::array({{{"one", 1}}})}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto sql = [&](const std::string& statement) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry.call_async(
            "workspace_sql", {{"customer_id", w.customer_id}, {"statement", statement}}, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    for (const char* allowed :
         {"SELECT 1 AS one", "  with t as (select 1) select * from t;", "SELECT 'a;b' AS text"}) {
        const auto ran = sql(allowed);
        EXPECT_TRUE(ran.ok) << allowed << ": " << ran.error.message;
    }
    for (const char* refused : {"DROP TABLE datasets.accounts",
                                "DELETE FROM x",
                                "SELECT 1; DROP TABLE x",
                                "CREATE USER eve",
                                "insert into x values (1)"}) {
        const std::size_t before = w.caller->calls.size();
        const auto ran = sql(refused);
        EXPECT_FALSE(ran.ok) << refused;
        EXPECT_EQ(w.caller->calls.size(), before) << refused << " never reaches the workspace";
    }
}

TEST(Workspaces, DataPagesBrowseTablesShowADatasetAndQueryIt) {
    lake::api::WorkspacePage page;
    page.section = "data";
    page.customer_id = std::string(customer);
    page.subject = "crm";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_connections"] = {
        {"connections", json::array({{{"name", "crm"}, {"connector", {{"type", "postgres"}}}}})}};
    page.data["list_datasets"] = {
        {"datasets", json::array({{{"name", "accounts"}, {"database", "datasets"}}})}};
    page.data["list_source_tables"] = {
        {"tables",
         json::array({{{"schema", "public"},
                       {"table", "accounts"},
                       {"kind", "BASE TABLE"},
                       {"columns", json::array({{{"column", "id"}, {"type", "integer"}}})}}})}};
    page.data["list_dataset_generators"] = {
        {"generators", json::array({{{"kind", "base-agent"}, {"title", "Base agent"}}})}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Tables in crm"), std::string::npos);
    EXPECT_NE(html.find("id integer"), std::string::npos);
    EXPECT_NE(html.find("Browse tables"), std::string::npos);
    EXPECT_NE(html.find("/train/dataset?customer_id="), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/generate\""), std::string::npos);
    EXPECT_EQ(html.find("name=\"path\""), std::string::npos) << "no writing files on the node";

    page.section = "dataset";
    page.subject = "accounts";
    page.data = {{"get_dataset",
                  {{"database", "datasets"},
                   {"columns", json::array({"id", "plan"})},
                   {"partitions", json::array({{{"partition_id", "p1"}}})}}},
                 {"sample", {{"rows", json::array({{{"id", 1}, {"plan", "pro"}}})}}}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("First Rows"), std::string::npos);
    EXPECT_NE(html.find(">pro<"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/dataset/promote\""), std::string::npos);

    page.section = "query";
    page.data = {{"statement", "SELECT 1"},
                 {"workspace_sql", {{"rows", json::array({{{"one", 1}}})}}}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("1 rows"), std::string::npos);
}

// The top bar's breadcrumb mirrors the URL (docs/ui/breadcrumb.md): stage, list, then the
// entities by name, or by abbreviated id; every level but the current one is a link that keeps the
// space.
TEST(Workspaces, BreadcrumbMirrorsTheUrlWithNamesAndAbbreviatedIds) {
    const auto trail = [](const std::string& html) {
        const auto start = html.find("<nav class=\"breadcrumbs topbar-trail\"");
        return start == std::string::npos ? std::string()
                                          : html.substr(start, html.find("</nav>", start) - start);
    };
    const std::string space = "customer_id=" + std::string(customer);
    lake::api::WorkspacePage page;
    page.customer_id = std::string(customer);
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};

    page.section = "projects";
    std::string crumbs = trail(lake::api::web_ui_workspace(page, true));
    EXPECT_NE(crumbs.find("<a href=\"/design/projects?" + space + "\">Design</a>"),
              std::string::npos)
        << crumbs;
    EXPECT_NE(crumbs.find("<span aria-current=\"page\">Projects</span>"), std::string::npos)
        << crumbs;

    page.section = "pipeline";
    page.project = "retention";
    page.subject = "churn";
    crumbs = trail(lake::api::web_ui_workspace(page, true));
    EXPECT_NE(crumbs.find(">Design</a>"), std::string::npos) << crumbs;
    EXPECT_NE(crumbs.find(">Projects</a>"), std::string::npos) << crumbs;
    EXPECT_NE(
        crumbs.find("<a href=\"/design/project?" + space + "&amp;name=retention\">retention</a>"),
        std::string::npos)
        << crumbs;
    EXPECT_NE(crumbs.find("<span aria-current=\"page\">churn</span>"), std::string::npos) << crumbs;

    page.section = "run";
    page.project.clear();
    page.subject = "3c9e1a2b-7f00-4d2e-9b1a-0c5d6e7f8a9b";
    crumbs = trail(lake::api::web_ui_workspace(page, true));
    EXPECT_NE(crumbs.find(">Train</a>"), std::string::npos) << crumbs;
    EXPECT_NE(crumbs.find("<a href=\"/train/runs?" + space + "\">Runs</a>"), std::string::npos)
        << crumbs;
    EXPECT_NE(crumbs.find("title=\"3c9e1a2b-7f00-4d2e-9b1a-0c5d6e7f8a9b\">3c9e1a2b…</span>"),
              std::string::npos)
        << crumbs;

    page.section = "query";
    page.subject.clear();
    crumbs = trail(lake::api::web_ui_workspace(page, true));
    EXPECT_NE(crumbs.find(">Data</a>"), std::string::npos) << crumbs;
    EXPECT_NE(crumbs.find("<span aria-current=\"page\">Query</span>"), std::string::npos) << crumbs;
}

// Train > Agents (docs/api/train-agents.md, lake#229): the agents people build live in the
// workspace's Zetta, and the Agent Library lists every specialized agent Orama ships.
TEST(Workspaces, AgentCallsAreAllowListedAndChatIsAWriteZettaTakesWithGet) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    for (const std::string_view read : {"list_agents", "get_agent", "agent_training_status"}) {
        const auto result = call(
            "workspace_query", {{"customer_id", w.customer_id}, {"capability", std::string(read)}});
        EXPECT_TRUE(result.ok) << read << ": " << result.error.message;
        EXPECT_TRUE(w.caller->read) << read;
    }
    for (const std::string_view action :
         {"create_agent", "update_agent", "delete_agent", "set_training_schedule", "agent_run"}) {
        const auto result = call(
            "workspace_act", {{"customer_id", w.customer_id}, {"capability", std::string(action)}});
        EXPECT_TRUE(result.ok) << action << ": " << result.error.message;
        EXPECT_FALSE(w.caller->read) << action << " is a POST";
    }
    const auto chat_as_read =
        call("workspace_query", {{"customer_id", w.customer_id}, {"capability", "chat"}});
    EXPECT_FALSE(chat_as_read.ok) << "chat loads a model: never through the read path";
    const auto chat = call("workspace_act",
                           {{"customer_id", w.customer_id},
                            {"capability", "chat"},
                            {"input", {{"agent", "a-1"}, {"message", "hello"}}}});
    ASSERT_TRUE(chat.ok) << chat.error.message;
    EXPECT_EQ(w.caller->capability, "chat");
    EXPECT_TRUE(w.caller->read) << "Zetta takes chat with GET";
    EXPECT_TRUE(lake::api::workspace_uses_get("chat"));
    EXPECT_FALSE(lake::api::workspace_read_allowed("chat"));
}

TEST(Workspaces, TheAgentLibraryListsEverySpecializedAgentAndWhatRuns) {
    const auto endpoints = lake::control::parse_agent_endpoints(
        "agent-ask-orama=http://ask:8080,agent-project=http://project:8080,unknown=http://x,bad");
    EXPECT_EQ(endpoints.size(), 2U) << "unknown keys and malformed pairs are ignored";
    const json library = lake::control::agent_library_json(endpoints);
    const json agents = library.at("agents");
    std::vector<std::string> keys;
    for (const json& agent : agents) {
        keys.push_back(agent.at("key"));
    }
    EXPECT_EQ(keys,
              (std::vector<std::string>{"agent-ask-orama",
                                        "agent-project",
                                        "agent-autosuggest",
                                        "agent-creator",
                                        "agent-eval-generator",
                                        "agent-autosuggest-corpus",
                                        "agent-project-corpus"}));
    EXPECT_EQ(agents[0].at("name"), "Ask Orama");
    EXPECT_EQ(agents[0].at("status"), "Available");
    EXPECT_EQ(agents[2].at("status"), "Not running on this lake");
    EXPECT_EQ(agents[4].at("status"), "Offline tool");
    EXPECT_FALSE(agents[4].at("chat").get<bool>());
}

TEST(Workspaces, AgentsPageShowsYourAgentsTheStudioFormAndTheAgentLibrary) {
    lake::api::WorkspacePage page;
    page.section = "agents";
    page.customer_id = std::string(customer);
    page.customers = {{std::string(customer), "you", true}};
    page.data["list_agent_library"] =
        lake::control::agent_library_json({{"agent-ask-orama", "http://ask:8080"}});

    // Before the workspace is ready the library still shows, beside Start your workspace.
    const std::string waiting = lake::api::web_ui_workspace(page);
    EXPECT_NE(waiting.find("Agent Library"), std::string::npos);
    EXPECT_NE(waiting.find("Ask Orama"), std::string::npos);
    EXPECT_NE(waiting.find("Project Corpus Generator"), std::string::npos);

    page.state = {{"ready", true}, {"workspace", "ws-1"}};
    page.data["list_agents"] = {
        {"agents",
         json::array({{{"id", "a-1"},
                       {"name", "Renewal watcher"},
                       {"kind", "custom"},
                       {"runtime", "python"},
                       {"purpose", "Flag renewals at risk"},
                       {"skills", {"search", "summarize"}},
                       {"trigger", "every Monday"},
                       {"model_name", "gpt-4o"},
                       {"status", "idle"}},
                      {{"id", "a-2"}, {"name", "Hidden Orama agent"}, {"kind", "orama"}}})},
        {"default_agent_id", ""}};
    const std::string html = lake::api::web_ui_workspace(page);
    EXPECT_NE(html.find("Your Agents"), std::string::npos);
    EXPECT_NE(html.find("Renewal watcher"), std::string::npos);
    EXPECT_NE(html.find("Conversation and unattended"), std::string::npos);
    EXPECT_NE(html.find("Search data, Summarize and explain"), std::string::npos);
    EXPECT_EQ(html.find("Hidden Orama agent"), std::string::npos)
        << "Orama's agents are in the library, not in Your Agents";
    for (const std::string_view box : {"name=\"purpose\"",
                                       "name=\"work_area\"",
                                       "name=\"manifesto\"",
                                       "name=\"goal\"",
                                       "name=\"skills\" value=\"generate-data\"",
                                       "value=\"frontier\"",
                                       "name=\"trigger\"",
                                       "action=\"/train/agents/create\""}) {
        EXPECT_NE(html.find(box), std::string::npos) << box;
    }
    EXPECT_LT(html.find("<h2>Your Agents</h2>"), html.find("<h2>Agent Library</h2>"));

    // Train > Agents is the first Train link.
    const std::string home = lake::api::web_ui_home(true);
    EXPECT_NE(home.find("href=\"/train/agents\""), std::string::npos);
    EXPECT_LT(home.find("href=\"/train/agents\""), home.find("href=\"/train/data\""));
    lake::api::WebUiViewer viewer{true};
    viewer.path = "/train/agent";
    EXPECT_NE(lake::api::web_ui_home(viewer).find("href=\"/train/agents\" aria-current=\"page\""),
              std::string::npos);
}

TEST(Workspaces, AgentPageShowsTheDefinitionPipelinesRunsAndATypedDelete) {
    lake::api::WorkspacePage page;
    page.section = "agent";
    page.subject = "a-1";
    page.customer_id = std::string(customer);
    page.state = {{"ready", true}};
    page.data["get_agent"] = {
        {"id", "a-1"},
        {"name", "Renewal watcher"},
        {"runtime", "native"},
        {"model_name", "Orama base model"},
        {"work_area", {{"kind", "project"}, {"detail", "churn"}}},
        {"manifesto", "Answer from company data only."},
        {"pipelines",
         json::array(
             {{{"project", "churn"}, {"graph", "weekly"}, {"version", "2026-09-26T10:00:00Z"}}})}};
    page.data["agent_training_status"] = {{"reason", "Never trained. Its schedule will train it."}};
    page.data["approved_pipelines"] = json::array(
        {{{"project", "churn"}, {"graph", "weekly"}}, {{"project", "churn"}, {"graph", "daily"}}});
    const std::string html = lake::api::web_ui_workspace(page);
    EXPECT_NE(html.find("Agent: Renewal watcher"), std::string::npos) << "titled by name, not id";
    EXPECT_NE(html.find("A single project: churn"), std::string::npos);
    EXPECT_NE(html.find("In your workspace"), std::string::npos);
    EXPECT_NE(html.find("graph=weekly"), std::string::npos);
    EXPECT_NE(html.find("This agent only talks"), std::string::npos);
    EXPECT_NE(html.find("Never trained."), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/agent/delete\""), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/agent/revoke\""), std::string::npos);
    EXPECT_NE(html.find("Approved Pipelines You Can Grant"), std::string::npos);
    EXPECT_NE(html.find("value=\"daily\""), std::string::npos) << "an approved, ungranted one";
    EXPECT_EQ(html.find("<li>weekly in churn <form"), std::string::npos)
        << "a granted Pipeline is not offered again";
    EXPECT_NE(html.find("Type <strong>Renewal watcher</strong>"), std::string::npos);
}

// ---- Agent launches, schedules and turns (docs/api/agent-launches.md, lake#231) ----

namespace {

class FakeAgentService final : public lake::control::AgentCaller {
public:
    std::vector<std::tuple<std::string, std::string, json>> turns;
    json answer = {{"ok", true}, {"reply", "done"}};

    liquid::runtime::task<ZettaControlResult<json>>
    turn(std::string endpoint, std::string token, json body) override {
        turns.emplace_back(endpoint, token, body);
        co_return ZettaControlResult<json>(answer);
    }
};

struct AgentRuns {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    lake::control::Principal principal = *store.context_for(w.secret);
    lake::api::Registry registry;
    FakeAgentService agents;

    AgentRuns() {
        auto requested = w.run(w.service->request(w.customer_id));
        const auto workspace = store.get_service(requested->service_id);
        EXPECT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
        w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
        lake::api::register_core_capabilities(registry, "test");
    }
    lake::api::ApiResult call(const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.agent_caller = &agents;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_session_id = principal.session_id;
        context.scopes = principal.scopes;
        context.pool_id = "lake";
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = w.customer_id;
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
};

} // namespace

TEST(AgentRuns, TriggersBecomeCronExpressions) {
    using lake::control::trigger_cron;
    EXPECT_EQ(trigger_cron("every 15 minutes"), "*/15 * * * *");
    EXPECT_FALSE(trigger_cron("every 1 minute")) << "at most every five minutes";
    EXPECT_EQ(trigger_cron("every 120 minutes"), "0 */2 * * *");
    EXPECT_EQ(trigger_cron("hourly"), "0 * * * *");
    EXPECT_EQ(trigger_cron("Every 2 days"), "0 0 */2 * *");
    EXPECT_EQ(trigger_cron("every day at 09:00"), "0 9 * * *");
    EXPECT_EQ(trigger_cron("every Monday at 11:30"), "30 11 * * 1");
    EXPECT_EQ(trigger_cron("every weekday at 09:05"), "5 9 * * 1-5");
    EXPECT_FALSE(trigger_cron("when a new dataset lands"));
    EXPECT_FALSE(trigger_cron("every day at 25:00"));
}

TEST(AgentRuns, CronExpressionsAreCheckedAsACronJobWouldTakeThem) {
    using lake::control::normalize_cron;
    EXPECT_EQ(normalize_cron("0  9 * *   1-5"), "0 9 * * 1-5");
    EXPECT_EQ(normalize_cron("@weekly"), "0 0 * * 0");
    EXPECT_EQ(normalize_cron("*/5 * * * *"), "*/5 * * * *");
    EXPECT_EQ(normalize_cron("0,30 8-18 * jan-jun MON"), "0,30 8-18 * jan-jun MON");
    EXPECT_FALSE(normalize_cron("* * * * *")) << "every minute is too often";
    EXPECT_FALSE(normalize_cron("0,58 * * * *")) << "58 and the next hour's 0 are two minutes apart";
    EXPECT_FALSE(normalize_cron("0 24 * * *"));
    EXPECT_FALSE(normalize_cron("0 9 * *"));
    EXPECT_FALSE(normalize_cron("0 9 0 * *")) << "days of the month start at 1";
    EXPECT_FALSE(normalize_cron("0 9 * * 1; rm"));
}

TEST(AgentRuns, AScheduleCredentialActsAsWhoeverSetTheSchedule) {
    lake::control::InMemoryStore store;
    const auto session = store.sign_in_clerk(minted("scheduler"));
    const auto principal = store.context_for(session->secret);
    const std::string customer_id = store.list_customers(10, principal->user_id)->front().id;
    const auto schedule = store.create_agent_cron_schedule({.customer_id = customer_id,
                                                            .agent_id = "a-1",
                                                            .agent_name = "Watcher",
                                                            .cron = "0 9 * * 1",
                                                            .scopes =
                                                                lake::control::agent_launch_scopes,
                                                            .created_by_user_id =
                                                                principal->user_id,
                                                            .created_by_taap_id =
                                                                principal->taap_id});
    ASSERT_TRUE(schedule) << schedule.error().message;
    EXPECT_EQ(schedule->slug, lake::control::managed_service_slug("as", schedule->id));
    EXPECT_EQ(schedule->time_zone, "Etc/UTC");
    const std::string credential = "las." + schedule->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store.context_for(credential)) << "not before the operator reports its digest";
    ASSERT_TRUE(store.set_agent_cron_schedule_status(
        schedule->id, "Ready", "", lake::control::identity_crypto::sha256_hex(credential)));
    const auto acting = store.context_for(credential);
    ASSERT_TRUE(acting) << acting.error().message;
    EXPECT_EQ(acting->user_id, principal->user_id);
    EXPECT_EQ(acting->kind, "agent_schedule");
    EXPECT_EQ(acting->scopes, lake::control::agent_launch_scopes);
    EXPECT_FALSE(store.context_for("lal." + schedule->id + ".0123456789abcdef0123456789abcdef"))
        << "a schedule's credential is not a launch's";
    ASSERT_TRUE(store.withdraw_agent_cron_schedule(schedule->id));
    EXPECT_FALSE(store.context_for(credential)) << "a removed schedule's credential stops too";
    EXPECT_FALSE(store.withdraw_agent_cron_schedule(schedule->id)) << "already removed";
}

TEST(AgentRuns, ALaunchCredentialActsAsTheLauncherWithTheLaunchScopes) {
    lake::control::InMemoryStore store;
    const auto session = store.sign_in_clerk(minted("launcher"));
    const auto principal = store.context_for(session->secret);
    const std::string customer_id = store.list_customers(10, principal->user_id)->front().id;
    const auto launch = store.create_agent_launch({customer_id,
                                                   "a-1",
                                                   "Watcher",
                                                   "job",
                                                   "manual",
                                                   nullptr,
                                                   lake::control::agent_launch_scopes,
                                                   principal->user_id,
                                                   principal->taap_id});
    ASSERT_TRUE(launch);
    const std::string credential = "lal." + launch->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store.context_for(credential)) << "not before the operator reports its digest";
    ASSERT_TRUE(store.set_agent_launch_status(
        launch->id, "Running", "", "", lake::control::identity_crypto::sha256_hex(credential)));
    const auto acting = store.context_for(credential);
    ASSERT_TRUE(acting) << acting.error().message;
    EXPECT_EQ(acting->user_id, principal->user_id);
    EXPECT_EQ(acting->kind, "agent_launch");
    EXPECT_EQ(acting->scopes, lake::control::agent_launch_scopes);
    EXPECT_FALSE(store.context_for("lal." + launch->id + ".ffffffffffffffffffffffffffffffff"));
    ASSERT_TRUE(store.withdraw_agent_launch(launch->id));
    EXPECT_FALSE(store.context_for(credential)) << "a stopped launch's credential stops too";
}

TEST(AgentRuns, LaunchStopAndScheduleAPythonAgentButNeverANativeOne) {
    AgentRuns r;
    r.w.caller->answers["get_agent"] = {{"id", "a-1"}, {"name", "Watcher"}, {"runtime", "python"}};
    const auto launched = r.call("launch_agent", {{"agent_id", "a-1"}, {"input", "week 39"}});
    ASSERT_TRUE(launched.ok) << launched.error.message;
    EXPECT_EQ(launched.value["mode"], "job");
    const auto listed = r.call("list_agent_launches", {{"agent_id", "a-1"}});
    ASSERT_EQ(listed.value["launches"].size(), 1U);
    EXPECT_EQ(listed.value["launches"][0]["input"], "week 39");
    const auto stopped = r.call("stop_agent_launch", {{"launch_id", launched.value["id"]}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(stopped.value["desired"], "absent");

    // Several schedules per agent, each its own CronJob in the operator's hands.
    const auto scheduled =
        r.call("schedule_agent", {{"agent_id", "a-1"}, {"trigger", "every Monday at 09:00"}});
    ASSERT_TRUE(scheduled.ok) << scheduled.error.message;
    EXPECT_EQ(scheduled.value["cron"], "0 9 * * 1");
    EXPECT_EQ(scheduled.value["description"], "every Monday at 09:00");
    EXPECT_EQ(scheduled.value["input"]["trigger"], "every Monday at 09:00");
    const auto nightly = r.call("schedule_agent",
                                {{"agent_id", "a-1"},
                                 {"cron", "30 2 * * *"},
                                 {"time_zone", "America/Toronto"},
                                 {"input", "the nightly churn check"}});
    ASSERT_TRUE(nightly.ok) << nightly.error.message;
    EXPECT_EQ(nightly.value["time_zone"], "America/Toronto");
    EXPECT_EQ(nightly.value["input"], "the nightly churn check");
    auto schedules = r.call("list_agent_schedules", {{"agent_id", "a-1"}});
    ASSERT_TRUE(schedules.ok) << schedules.error.message;
    ASSERT_EQ(schedules.value["schedules"].size(), 2U);
    EXPECT_NE(schedules.value["schedules"][0]["slug"], schedules.value["schedules"][1]["slug"]);

    // The runs a CronJob starts sit beside the manual launches.
    ASSERT_TRUE(r.w.store->put_agent_cron_run(nightly.value["id"],
                                              lake::control::new_uuid_v4(),
                                              "prod-as-0123456789ab-29000000",
                                              "Succeeded",
                                              "finished",
                                              0,
                                              json{{"reply", "3 at risk"}}));
    const auto runs = r.call("list_agent_launches", {{"agent_id", "a-1"}});
    ASSERT_EQ(runs.value["launches"].size(), 2U);
    const auto run = std::ranges::find_if(runs.value["launches"], [](const json& launch) {
        return launch["trigger"] == "schedule";
    });
    ASSERT_NE(run, runs.value["launches"].end());
    EXPECT_EQ((*run)["result"]["reply"], "3 at risk");
    EXPECT_EQ((*run)["input"], "the nightly churn check");

    const auto removed = r.call("unschedule_agent", {{"schedule_id", scheduled.value["id"]}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_EQ(removed.value["desired"], "absent");
    schedules = r.call("list_agent_schedules", {{"agent_id", "a-1"}});
    ASSERT_EQ(schedules.value["schedules"].size(), 1U);

    const auto vague = r.call("schedule_agent", {{"agent_id", "a-1"}, {"trigger", "sometimes"}});
    ASSERT_FALSE(vague.ok);
    EXPECT_NE(vague.error.message.find("cannot place"), std::string::npos);
    const auto hurried = r.call("schedule_agent", {{"agent_id", "a-1"}, {"cron", "* * * * *"}});
    ASSERT_FALSE(hurried.ok);
    EXPECT_NE(hurried.error.message.find("five minutes"), std::string::npos);
    EXPECT_FALSE(r.call("schedule_agent", {{"agent_id", "a-1"}}).ok) << "cron or trigger";
    EXPECT_FALSE(r.call("schedule_agent",
                        {{"agent_id", "a-1"}, {"cron", "0 9 * * *"}, {"time_zone", "$(x)"}})
                     .ok);

    r.w.caller->answers["get_agent"] = {{"id", "n-1"}, {"name", "Native"}, {"runtime", "native"}};
    EXPECT_FALSE(r.call("launch_agent", {{"agent_id", "n-1"}}).ok);
}

TEST(AgentRuns, AgentTurnReachesLibraryNativeAndPythonAgents) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"},
                              {"agent-project", "http://project:8080"},
                              {"agent-autosuggest", "http://suggest:8080"}};
    config.internal_url = "http://lake.internal:4900";
    config.agent_service_token = "service-token";

    // Ask Orama gets a key for this turn only: read capabilities and agent_turn, never an action.
    const auto asked = r.call("agent_turn", {{"agent", "agent-ask-orama"}, {"message", "hi"}});
    ASSERT_TRUE(asked.ok) << asked.error.message;
    const auto& [ask_url, ask_token, ask_body] = r.agents.turns.back();
    EXPECT_EQ(ask_url, "http://ask:8080");
    EXPECT_EQ(ask_token, "service-token");
    EXPECT_EQ(ask_body["lake_url"], "http://lake.internal:4900");
    const std::string key = ask_body["lake_token"];
    EXPECT_FALSE(r.store.context_for(key)) << "the turn's key is revoked when the turn ends";

    const auto built = r.call("agent_turn", {{"agent", "agent-project"}, {"message", "churn"}});
    ASSERT_TRUE(built.ok) << built.error.message;
    EXPECT_EQ(std::get<2>(r.agents.turns.back())["zetta_base_url"],
              "http://lake.internal:4900/workspaces/" + r.w.customer_id + "/zetta");

    // A change to an existing pipeline: the agent is handed the graph, and its proposal is kept
    // for review on the pipeline page (lake#249).
    r.w.caller->answers["get_graph"] = {{"name", "churn"},
                                        {"nodes", json::array({{{"id", "pull"}, {"kind", "connect"},
                                                                {"name", "Pull"}}})},
                                        {"edges", json::array()}};
    r.agents.answer = {{"ok", true},
                       {"result",
                        {{"kind", "proposal"},
                         {"reply", "Proposed, not applied: rename"},
                         {"proposal",
                          {{"project", "retention"},
                           {"graph", "churn"},
                           {"edits", json::array({{{"op", "rename"}, {"name", "churn v2"}}})}}}}}};
    const auto proposed = r.call("agent_turn",
                                 {{"agent", "agent-project"},
                                  {"message", "rename it"},
                                  {"context", {{"project", "retention"}, {"graph", "churn"}}}});
    ASSERT_TRUE(proposed.ok) << proposed.error.message;
    EXPECT_EQ(std::get<2>(r.agents.turns.back())["context"]["pipeline"]["nodes"][0]["id"], "pull");
    const std::string review = proposed.value["review_url"];
    ASSERT_NE(review.find("/design/pipeline?customer_id=" + r.w.customer_id +
                          "&project=retention&graph=churn&proposal="),
              std::string::npos)
        << review;
    const auto kept = r.store.ask_conversation(review.substr(review.rfind('=') + 1),
                                               r.principal.user_id, 5);
    ASSERT_TRUE(kept && kept->size() == 1U);
    EXPECT_EQ(kept->front().payload["proposal"]["edits"][0]["op"], "rename");
    r.agents.answer = {{"ok", true}, {"reply", "done"}};

    r.w.caller->answers["list_datasets"] = {{"datasets", json::array({{{"name", "accounts"}}})}};
    r.w.caller->answers["get_dataset"] = {{"columns", json::array({{{"name", "churned"}}})}};
    const auto suggested = r.call("agent_turn", {{"agent", "agent-autosuggest"}, {"message", "?"}});
    ASSERT_TRUE(suggested.ok) << suggested.error.message;
    const json& profile = std::get<2>(r.agents.turns.back())["profile"];
    EXPECT_EQ(profile["tables"][0]["name"], "accounts");
    EXPECT_EQ(profile["tables"][0]["columns"][0]["name"], "churned");
    EXPECT_FALSE(std::get<2>(r.agents.turns.back()).contains("lake_token"))
        << "Auto-Suggest reads nothing itself";

    EXPECT_FALSE(r.call("agent_turn", {{"agent", "agent-creator"}, {"message", "x"}}).ok)
        << "a library agent this lake does not run";
    EXPECT_FALSE(r.call("agent_turn", {{"agent", "agent-eval-generator"}, {"message", "x"}}).ok);

    r.w.caller->answers["get_agent"] = {{"id", "n-1"}, {"runtime", "native"}};
    r.w.caller->answers["chat"] = {{"reply", "from the base model"}};
    const auto native = r.call("agent_turn", {{"agent", "n-1"}, {"message", "hello"}});
    ASSERT_TRUE(native.ok) << native.error.message;
    EXPECT_EQ(native.value["reply"], "from the base model");
    EXPECT_EQ(r.w.caller->capability, "chat");

    r.w.caller->answers["get_agent"] = {{"id", "p-1"}, {"name", "Py"}, {"runtime", "python"}};
    const auto starting = r.call("agent_turn", {{"agent", "p-1"}, {"message", "hello"}});
    ASSERT_TRUE(starting.ok) << starting.error.message;
    EXPECT_EQ(starting.value["status"], "starting");
    const std::string launch_id = starting.value["launch_id"];
    ASSERT_TRUE(r.store.set_agent_launch_status(launch_id, "Ready", "", "http://al-1:8080", "x"));
    const std::size_t before = r.agents.turns.size();
    const auto talked = r.call(
        "agent_turn", {{"agent", "p-1"}, {"message", "hello again"}, {"conversation_id", "c-1"}});
    ASSERT_TRUE(talked.ok) << talked.error.message;
    ASSERT_EQ(r.agents.turns.size(), before + 1);
    EXPECT_EQ(std::get<0>(r.agents.turns.back()), "http://al-1:8080");
    EXPECT_EQ(std::get<2>(r.agents.turns.back())["message"], "hello again");
}
