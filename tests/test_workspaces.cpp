#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "experiment_fixture.hpp"
#include "temporary_directory.hpp"
#include "stream_fixture.hpp"

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
    std::optional<lake::control::ZettaControlError> failure;
    std::function<void()> before_reply;
    lake::test::StreamCaller topics;

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
        if (before_reply) {
            before_reply();
        }
        if (failure) {
            co_return ZettaControlResult<json>(std::unexpected(*failure));
        }
        if (capability == "create_topic" || capability == "update_topic" ||
            capability == "publish_event" || capability == "receive_events") {
            co_return lake::control::settled(co_await topics.call(url, token, capability, read, input));
        }
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

    std::string workspace_id() const {
        const auto services = store->list_workspaces(customer_id);
        for (const auto& workspace : *services) {
            if (workspace.desired != "deleted") {
                return workspace.id;
            }
        }
        return {};
    }

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
    EXPECT_EQ(requested->slug, lake::control::workspace_slug(requested->workspace_id));
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

TEST(Workspaces, DesiredStateCapabilitiesUseWorkspaceIdentityAndTypedDeletion) {
    Workspaces w;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = w.store.get();
    context.workspaces = w.service.get();
    context.authorized = true;
    context.acting_identity = "admin";
    const auto call = [&](const std::string& name, json input) {
        input["customer_id"] = w.customer_id;
        return *liquid::runtime::block_on(registry.call_async(name, input, context));
    };
    EXPECT_FALSE(call("create_workspace", {{"name", "missing-env"}}).ok);
    EXPECT_FALSE(call("create_workspace", {{"name", "invalid"}, {"environment", "Prod_1"}}).ok);
    const auto created = call("create_workspace", {{"name", "Research"}, {"environment", "dev"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const std::string service_id = created.value.at("zetta_service_id");
    EXPECT_NE(id, service_id);
    EXPECT_EQ(w.store->get_service(service_id)->workspace_id, id);
    EXPECT_EQ(w.service->workspace(w.customer_id, id)->id, service_id);
    EXPECT_FALSE(w.service->delete_service(w.customer_id, service_id));
    EXPECT_FALSE(w.service->workspace(w.customer_id, service_id));
    EXPECT_FALSE(call("get_workspace", {{"workspace_id", service_id}}).ok);
    EXPECT_EQ(created.value.at("phase"), "Pending");
    EXPECT_EQ(call("list_workspaces", json::object()).value.at("workspaces").size(), 1);
    const auto stopped = call("update_workspace", {{"workspace_id", id}, {"desired", "stopped"}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(stopped.value.at("generation"), 2);
    EXPECT_EQ(stopped.value.at("observed_generation"), 0);
    EXPECT_EQ(stopped.value.at("phase"), "Pending");
    EXPECT_EQ(w.service->workspace(w.customer_id, id).error().code, "workspace_not_ready");
    EXPECT_EQ(call("update_workspace", {{"workspace_id", id}, {"desired", "stopped"}}).value.at("generation"), 2);
    EXPECT_FALSE(call("update_workspace", {{"workspace_id", id}, {"phase", "Ready"}}).ok);
    ASSERT_TRUE(call("create_workspace", {{"name", "Another"}, {"environment", "staging"}}).ok);
    const auto resumed = call("update_workspace", {{"workspace_id", id}, {"desired", "running"}});
    ASSERT_TRUE(resumed.ok) << resumed.error.message;
    EXPECT_EQ(resumed.value.at("generation"), 3);
    EXPECT_EQ(w.store->get_service(service_id)->desired, "present");
    EXPECT_FALSE(call("delete_workspace", {{"workspace_id", id}, {"confirm", id}}).ok);
    const auto deleted = call("delete_workspace", {{"workspace_id", id}, {"confirm", "Research"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value.at("desired"), "deleted");
    EXPECT_EQ(w.store->get_service(service_id)->desired, "absent");
    EXPECT_FALSE(call("update_workspace", {{"workspace_id", id}, {"desired", "running"}}).ok);
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
    EXPECT_TRUE(second->slug.starts_with("ws-")) << "a slug is never reused";
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
    auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto workspace = w.store->get_service(requested->service_id);
    ASSERT_TRUE(w.store->set_service_status(
        workspace->id, "Ready", "", "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "workspace-admin-token"}});
    w.caller->value = {{"projects", json::array({"retention"})}};
    auto called =
        w.run(w.service->call(w.customer_id, workspace->workspace_id, "get_project", true, {{"name", "retention"}}));
    ASSERT_TRUE(called);
    EXPECT_EQ((*called)["projects"][0], "retention");
    EXPECT_EQ(w.caller->url, "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800");
    EXPECT_EQ(w.caller->token, "workspace-admin-token");
    EXPECT_EQ(w.caller->capability, "get_project");
    EXPECT_TRUE(w.caller->read);
    EXPECT_EQ(w.caller->input["name"], "retention");
}

TEST(Workspaces, RegistryCacheExpiresAndInvalidatesOnChangesWithoutCachingFailures) {
    Workspaces w;
    const auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto service = w.store->get_service(requested->service_id);
    ASSERT_TRUE(service);
    ASSERT_TRUE(w.store->set_service_status(service->id, "Ready", "", "http://registry.test"));
    w.deliver(*service, {{"token", "first-token"}});
    std::size_t observed = 0;
    const auto read = [&] {
        return w.run(w.service->call(w.customer_id, service->workspace_id,
            "list_methodologies", true, json::object(),
            [&](std::chrono::nanoseconds) {
                ++observed;
            }));
    };
    w.caller->value = {{"version", 1}};
    ASSERT_TRUE(read());
    w.caller->value = {{"version", 2}};
    EXPECT_EQ(read()->at("version"), 1);
    EXPECT_EQ(observed, 1U);
    w.service->invalidate_registry(w.customer_id, service->workspace_id);
    EXPECT_EQ(read()->at("version"), 2);
    EXPECT_EQ(observed, 2U);

    w.caller->value = {{"version", 3}};
    ASSERT_TRUE(w.run(w.service->call(w.customer_id, service->workspace_id,
        "save_graph", false, json::object())));
    EXPECT_EQ(read()->at("version"), 3);
    w.caller->value = {{"version", 4}};
    w.deliver(*service, {{"token", "rotated-token"}});
    EXPECT_EQ(read()->at("version"), 4);
    EXPECT_EQ(w.caller->token, "rotated-token");

    w.caller->value = {{"version", 5}};
    ASSERT_TRUE(w.store->set_service_status(service->id, "Ready", "", "http://replacement.test"));
    EXPECT_EQ(read()->at("version"), 5);
    ASSERT_TRUE(w.store->set_service_status(service->id, "Pending", "", "http://replacement.test"));
    EXPECT_FALSE(read());
    EXPECT_FALSE(w.run(w.service->call(std::string(customer), service->workspace_id,
        "list_methodologies", true, json::object())));
    ASSERT_TRUE(w.store->set_service_status(service->id, "Ready", "", "http://replacement.test"));

    w.caller->value = {{"version", 6}};
    std::this_thread::sleep_for(std::chrono::milliseconds(5100));
    EXPECT_EQ(read()->at("version"), 6);
    w.service->invalidate_registry(w.customer_id, service->workspace_id);
    w.caller->failure = lake::control::ZettaControlError{"store_error", "registry unavailable"};
    EXPECT_FALSE(read());
    w.caller->failure.reset();
    w.caller->value = {{"version", 7}};
    EXPECT_EQ(read()->at("version"), 7);
}

TEST(Workspaces, RegistryInvalidationRejectsInflightRefillsAndLeavesMutableReadsUncached) {
    Workspaces w;
    const auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto service = w.store->get_service(requested->service_id);
    ASSERT_TRUE(service);
    ASSERT_TRUE(w.store->set_service_status(service->id, "Ready", "", "http://registry.test"));
    w.deliver(*service, {{"token", "token"}});
    w.caller->value = {{"version", 1}};
    w.caller->before_reply = [&] {
        w.service->invalidate_registry(w.customer_id, service->workspace_id);
    };
    ASSERT_TRUE(w.run(w.service->call(w.customer_id, service->workspace_id,
        "list_methodologies", true, json::object())));
    w.caller->before_reply = {};
    w.caller->value = {{"version", 2}};
    EXPECT_EQ(w.run(w.service->call(w.customer_id, service->workspace_id,
        "list_methodologies", true, json::object()))->at("version"), 2);
    for (const std::string capability : {"list_datasets", "list_connections", "list_runs"}) {
        w.caller->value = {{"version", 3}};
        ASSERT_TRUE(w.run(w.service->call(w.customer_id, service->workspace_id,
            capability, true, json::object())));
        w.caller->value = {{"version", 4}};
        EXPECT_EQ(w.run(w.service->call(w.customer_id, service->workspace_id,
            capability, true, json::object()))->at("version"), 4);
    }
}

TEST(Workspaces, RegistryInvalidationIsScopedAcrossWorkspacesAndCatalogues) {
    Workspaces w;
    const auto first = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(first);
    const auto second = w.store->create_workspace({w.customer_id, "second", "prod", ""});
    ASSERT_TRUE(second);
    for (const auto& id : {first->service_id, second->zetta_service_id}) {
        const auto service = w.store->get_service(id);
        ASSERT_TRUE(service);
        ASSERT_TRUE(w.store->set_service_status(id, "Ready", "", "http://registry.test"));
        w.deliver(*service, {{"token", "token"}});
    }
    for (const std::string capability :
         {"list_methodologies", "list_solutions", "list_dataset_generators"}) {
        const auto read = [&](const std::string& workspace) {
            return w.run(w.service->call(w.customer_id, workspace, capability, true,
                json::object()));
        };
        w.caller->value = {{"version", 1}};
        ASSERT_TRUE(read(first->workspace_id));
        w.caller->value = {{"version", 2}};
        const auto other = read(second->id);
        ASSERT_TRUE(other);
        EXPECT_EQ(other->at("version"), 2);
        EXPECT_EQ(read(first->workspace_id)->at("version"), 1);
        w.service->invalidate_registry(w.customer_id, first->workspace_id);
        w.caller->value = {{"version", 3}};
        EXPECT_EQ(read(first->workspace_id)->at("version"), 3);
        EXPECT_EQ(read(second->id)->at("version"), 2);
    }
}

TEST(Workspaces, PipelineDefinitionCacheSeparatesInputsAndRefreshesAfterSave) {
    Workspaces w;
    const auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto service = w.store->get_service(requested->service_id);
    ASSERT_TRUE(service);
    ASSERT_TRUE(w.store->set_service_status(service->id, "Ready", "", "http://registry.test"));
    w.deliver(*service, {{"token", "token"}});
    const auto graph = [&](const std::string& name) {
        return w.run(w.service->call(w.customer_id, service->workspace_id, "get_graph", true,
            {{"project", "project"}, {"graph", name}}, {}, true));
    };
    w.caller->value = {{"nodes", json::array({"first"})}};
    ASSERT_TRUE(graph("a"));
    w.caller->value = {{"nodes", json::array({"second"})}};
    EXPECT_EQ(graph("a")->at("nodes")[0], "first");
    EXPECT_EQ(graph("b")->at("nodes")[0], "second");
    const auto current = w.run(w.service->call(w.customer_id, service->workspace_id,
        "get_graph", true, {{"project", "project"}, {"graph", "a"}}));
    ASSERT_TRUE(current);
    EXPECT_EQ(current->at("nodes")[0], "second") << "edits bypass the page cache";
    ASSERT_TRUE(w.run(w.service->call(w.customer_id, service->workspace_id, "save_graph", false,
        {{"project", "project"}, {"graph", "a"}})));
    EXPECT_EQ(graph("a")->at("nodes")[0], "second");
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
            if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
                input["workspace_id"] = w.workspace_id();
            }
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
            EXPECT_TRUE(completed);
            return std::move(*completed);
        };

    const auto started =
        call(*owner_principal, "start_workspace", {{"customer_id", owner_customer}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["workspace"], lake::control::workspace_slug(w.workspace_id()));

    const auto foreign =
        call(*other_principal, "workspace_status", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign.ok) << "another customer's workspace is not readable";
    const auto foreign_runs =
        call(*other_principal, "list_runs", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign_runs.ok) << "another customer's runs are not readable";
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

    const std::string slug = "workspace";
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
    EXPECT_EQ(deleted.value["desired"], "deleted");
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
            if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
                input["workspace_id"] = w.workspace_id();
            }
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
    // Only what can be created today; no internal plans or issue numbers (lake#257).
    EXPECT_EQ(html.find("value=\"object:r2\""), std::string::npos);
    EXPECT_EQ(html.find("not yet"), std::string::npos);
    EXPECT_EQ(html.find("lake#196"), std::string::npos);
    EXPECT_NE(html.find("are coming later.</small>"), std::string::npos);
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
    EXPECT_EQ(html.find("<select name=\"customer_id\">"), std::string::npos)
        << "workspace pages use the global top-bar selector";
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
    EXPECT_EQ(html.find("action=\"/workspace/delete\""), std::string::npos);
    EXPECT_NE(html.find("href=\"/workspaces\""), std::string::npos);

    page.state = {{"requested", true},
                  {"phase", "Failed"},
                  {"ready", false},
                  {"workspace", "ws-0f1e2d3c4b5a"},
                  {"message", "workspace quota reached (3)"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("as many workspaces as it allows"), std::string::npos);
    EXPECT_NE(html.find(">Cancel workspace</a>"), std::string::npos);
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
    EXPECT_LT(html.find("<h2>Load Data</h2>"), html.find("<h2>Connections</h2>"))
        << "loading data is the first thing on the page (lake#256)";
    EXPECT_NE(html.find("<details open><summary>Upload a file"), std::string::npos);
    page.data["loaded"] = {{"table", "accounts"}, {"rows", "200"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Loaded <strong>200</strong> rows into"), std::string::npos);
    EXPECT_NE(html.find("name=accounts\">accounts</a>"), std::string::npos);
    page.data.erase("loaded");
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
        input["workspace_id"] = w.workspace_id();
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

    page.data["project_agent_error"] = "Which dataset should I use?";
    page.data["project_agent_reply"] = true;
    page.data["project_agent_task_id"] = "task-question";
    const std::string question = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(question.find("Project Agent Question"), std::string::npos);
    EXPECT_NE(question.find("Which dataset should I use?"), std::string::npos);
    EXPECT_NE(question.find("<h3>Reply</h3>"), std::string::npos);
    EXPECT_NE(question.find("name=\"task_id\" value=\"task-question\""), std::string::npos);
    EXPECT_NE(question.find("name=\"reply\""), std::string::npos);
    page.data.erase("project_agent_error");
    page.data.erase("project_agent_reply");
    page.data.erase("project_agent_task_id");

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
        input["workspace_id"] = w.workspace_id();
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

TEST(Workspaces, PredictAnswersOnlyThroughAPublishedModel) {
    // UX review 2026-09-27 (lake#250): a published model could not be called from outside lake.
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_model"] = {{"id", "m-1"}, {"exposed", false}};
    w.caller->answers["score"] = {{"ranked", json::array({{{"entity", 0}, {"risk", 0.91}}})}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto predict = [&] {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        json input{{"customer_id", w.customer_id},
                   {"workspace_id", w.workspace_id()},
                   {"model", "m-1"},
                   {"rows", json::array({{{"support_tickets", 11}}})}};
        auto completed = liquid::runtime::block_on(registry.call_async("predict", input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto refused = predict();
    ASSERT_FALSE(refused.ok);
    EXPECT_NE(refused.error.message.find("not published"), std::string::npos);
    EXPECT_NE(w.caller->calls.back().first, "score") << "nothing is scored";

    w.caller->answers["get_model"]["exposed"] = true;
    const auto scored = predict();
    ASSERT_TRUE(scored.ok) << scored.error.message;
    EXPECT_EQ(scored.value["ranked"][0]["risk"], 0.91);
    EXPECT_EQ(w.caller->calls.back().first, "score");
    EXPECT_EQ(w.caller->calls.back().second["model"], "m-1");
    EXPECT_EQ(w.caller->calls.back().second["rows"][0]["support_tickets"], 11);
    EXPECT_EQ(registry.find("predict")->kind, lake::api::CapabilityKind::Action)
        << "POST /v1/predict with the rows in a JSON body, as Ship → Models shows";
    EXPECT_EQ(registry.find("predict")->action, "read") << "authorized as a read";
}

TEST(Workspaces, ModelsPageNamesScoresPublishesAndShowsHowToCallThem) {
    lake::api::WorkspacePage page;
    page.section = "models";
    page.customer_id = std::string(customer);
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["names"] = {{"projects", {{"p-1", "retention"}}}, {"graphs", json::object()}};
    page.data["list_models"] = {
        {"models",
         json::array({{{"id", "m-1-aaaaaaaa"}, {"name", "retention_churn"}, {"project", "p-1"},
                       {"created", "2026-09-27T13:29:23.873Z"}},
                      {{"id", "m-2-bbbbbbbb"}, {"name", "churn_model"}, {"project", "p-gone"},
                       {"created", "2026-09-25T18:48:53.966Z"}}})}};
    page.data["model_details"] = {
        {"m-1-aaaaaaaa",
         {{"exposed", true},
          {"metrics", {{"held_out_auc", 0.88}, {"in_sample_auc", 0.95}}},
          {"feature_schema", {{"features", json::array({"support_tickets"})}}}}},
        {"m-2-bbbbbbbb", {{"exposed", false}, {"metrics", {{"in_sample_auc", 0.94}}}}}};
    page.data["origin"] = "https://lake.example";
    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">retention</a>"), std::string::npos) << "the project by name";
    EXPECT_NE(html.find("a deleted project"), std::string::npos);
    EXPECT_NE(html.find("AUC 0.880 <small>held out</small>"), std::string::npos);
    EXPECT_NE(html.find("in-sample only"), std::string::npos);
    EXPECT_NE(html.find("2026-09-27 13:29 UTC"), std::string::npos);
    EXPECT_NE(html.find(">published</span>"), std::string::npos);
    EXPECT_NE(html.find("https://lake.example/v1/predict"), std::string::npos);
    EXPECT_NE(html.find("&quot;support_tickets&quot;: 0"), std::string::npos)
        << "a row keyed by the model's own features";
    EXPECT_EQ(html.find(">Publish</button></form> <form"), std::string::npos)
        << "one action per model, never both";
}

TEST(Workspaces, RunsShowPipelinesByNameAndSayWhyAFailureFailed) {
    lake::api::WorkspacePage page;
    page.section = "runs";
    page.customer_id = std::string(customer);
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["names"] = {{"projects", {{"p-1", "retention"}}},
                          {"graphs", {{"g-1", {{"name", "churn"}, {"project", "retention"}}}}}};
    page.data["list_runs"] = {
        {"runs",
         json::array({{{"id", "r-1"}, {"graph", "g-1"}, {"project", "p-1"}, {"status", "succeeded"},
                       {"created", "2026-09-27T13:29:22.572Z"}},
                      {{"id", "r-2"}, {"graph", "g-gone"}, {"project", "p-gone"},
                       {"status", "failed"}, {"created", "2026-09-26T15:46:54.278Z"}}})}};
    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">churn</a>"), std::string::npos);
    EXPECT_NE(html.find(">retention</a>"), std::string::npos);
    EXPECT_NE(html.find("a pipeline of a deleted project"), std::string::npos);
    EXPECT_NE(html.find(">Why it failed</a>"), std::string::npos);
    EXPECT_EQ(html.find(">r-1</a>"), std::string::npos) << "no raw run ids as the label";
}

TEST(Workspaces, SolutionProjectShowsRunFeedbackAndReadableOperatingState) {
    lake::api::WorkspacePage page;
    page.section = "project";
    page.customer_id = std::string(customer);
    page.subject = "retention";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_project"] = {
        {"name", "retention"},
        {"description", "Foundation awaiting owner confirmation."},
        {"graphs", json::array({{{"id", "g-1"}, {"name", "S-1 retention"}}})}};
    const json runs =
        json::array({{{"id", "run-failed"},
                      {"graph", "S-1 retention"},
                      {"status", "failed"},
                      {"stage", "M-2 Entity resolution"},
                      {"error", "M-2 Entity resolution failed: entities need a 64-bit integer key"},
                      {"created", "2026-09-30T12:00:02Z"}},
                     {{"id", "run-running"},
                      {"graph", "S-1 retention"},
                      {"status", "running"},
                      {"stage", "M-1 Propensity"},
                      {"created", "2026-09-30T12:00:01Z"}}});
    page.data["list_runs"] = {{"runs", runs}};
    json solution{
        {"graph", "S-1 retention"},
        {"graph_id", "g-1"},
        {"runs", runs},
        {"sources", {{"missing_policy_predicates", json::array()}}},
        {"stages",
         json::array({{{"stage", 1}, {"name", "Propensity"}, {"kind", "model"}},
                      {{"stage", 2}, {"name", "Entity resolution"}, {"kind", "transform"}}})}};
    solution["policy"] = {
        {"id", "policy-1"},
        {"current_version", 2},
        {"config",
         {{"autonomy", "A1"},
          {"threshold", 0.75},
          {"tiers",
           json::array({{{"name", "high"}, {"minimum_score", 0.8}, {"action", "send offer"}}})}}}};
    solution["experiment"] = {
        {"allocations",
         json::array({{{"name", "holdout"}, {"tier", "all"}, {"fraction", 0.1}},
                      {{"name", "delivery"}, {"tier", "all"}, {"fraction", 0.9}}})}};
    solution["monitor"] = {
        {"psi_threshold", 0.2},
        {"calibration_error_threshold", 0.05},
        {"guardrails",
         json::array(
             {{{"bound", {{"name", "contact rate"}}}, {"value", 0.12}, {"breached", false}}})}};
    const json checks = json::array(
        {{{"name", "s1_entity_resolution"},
          {"status", "fail"},
          {"reason", "Entity keys are incompatible."}},
         {{"name", "s4_pipeline"},
          {"status", "pass"},
          {"reason", "no bound S-4 Pipeline to validate"}},
         {{"name", "schema_type"}, {"status", "pass"}, {"reason", "generic stage is valid"}}});
    const json certificate{{"verdict", "fail"}, {"checks", checks}};
    solution["roi"] = {
        {"solution", "S-1"},
        {"roi_statement",
         {{"claimable", false}, {"reason", "The holdout has not accumulated enough outcomes."}}},
        {"correctness_certificate", certificate}};
    page.data["solution_project"] = std::move(solution);

    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<strong>Last run failed at M-2 Entity resolution.</strong>"),
              std::string::npos);
    EXPECT_EQ(html.find("Foundation awaiting owner confirmation."), std::string::npos);
    EXPECT_NE(html.find("M-2 Entity resolution failed: entities need a 64-bit integer key"),
              std::string::npos);
    EXPECT_NE(html.find(">running</span>"), std::string::npos);
    EXPECT_NE(html.find("http-equiv=\"refresh\" content=\"3\""), std::string::npos);
    EXPECT_NE(html.find("Open full run"), std::string::npos);
    EXPECT_NE(html.find("Tiers and Thresholds"), std::string::npos);
    EXPECT_NE(html.find("holdout</td><td>all</td><td>10.0%"), std::string::npos);
    EXPECT_NE(html.find("Guardrails"), std::string::npos);
    EXPECT_NE(html.find("The holdout has not accumulated enough outcomes."), std::string::npos);
    const std::size_t verdict = html.find("<strong>Verdict:</strong>");
    const std::size_t applicable = html.find("s1_entity_resolution");
    ASSERT_NE(verdict, std::string::npos);
    ASSERT_NE(applicable, std::string::npos);
    EXPECT_LT(verdict, applicable);
    EXPECT_NE(html.find("<summary>S-1 (1 checks)</summary>"), std::string::npos);
    EXPECT_NE(html.find("Show all checks (2 not applicable)"), std::string::npos);
    EXPECT_NE(html.find("<summary>View JSON</summary>"), std::string::npos);
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
                                {{"customer_id", customer_id}, {"workspace_id", w.workspace_id()},
                                 {"service_id", service_id}},
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
    EXPECT_NE(html.find("The workspace can't read valkey yet; Postgres works today"),
              std::string::npos);
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
                                                     {"project", "retention"},
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
        "list_workspaces",    "get_workspace",      "create_workspace",     "update_workspace",
        "workspace_status",   "start_workspace",    "delete_workspace",     "workspace_query",
        "workspace_act",      "list_services",      "create_service",       "delete_service",
        "service_connection", "list_service_kinds", "list_store_providers", "list_stores",
        "create_store",       "delete_store",       "store_connection",     "connect_service",
        "search_index",       "edit_pipeline",      "get_pipeline",         "run_pipeline",
        "create_pipeline",    "list_runs",          "start_solution",       "confirm_solution"};
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
        if (registry.find(name)->input_schema["properties"].contains("workspace_id")) {
            arguments["workspace_id"] = w.workspace_id();
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

    const json listed_runs = call("list_runs", {{"project", "retention"}, {"limit", 5}});
    const json run_value =
        json::parse(listed_runs.at("content").at(0).at("text").get<std::string>()).at("value");
    ASSERT_EQ(run_value["runs"].size(), 1U);
    EXPECT_EQ(run_value["runs"][0]["id"], "r1");
    EXPECT_EQ(run_value["runs"][0]["status"], "succeeded");
    EXPECT_EQ(run_value["runs"][0]["stage"], "Completed");

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
            "workspace_sql", {{"customer_id", w.customer_id}, {"workspace_id", w.workspace_id()},
                              {"statement", statement}}, context));
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

TEST(Workspaces, ApplicationAgentDatasetToolsUseWorkspaceDatasetIds) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["list_datasets"] = {
        {"datasets", json::array({{{"database", "customer_success"}, {"name", "accounts"}}})}};
    w.caller->answers["get_dataset"] = {
        {"database", "customer_success"},
        {"name", "accounts"},
        {"columns", json::array({"id", "health"})}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.credential_session_id = principal->session_id;
        context.credential_kind = principal->kind;
        context.scopes = principal->scopes;
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = w.customer_id;
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
            input["workspace_id"] = w.workspace_id();
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto listed = call("list_datasets", json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["datasets"].size(), 1U);
    EXPECT_EQ(listed.value["datasets"][0]["id"], "customer_success/accounts");
    const auto got = call("get_dataset", {{"id", "customer_success/accounts"}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value["id"], "customer_success/accounts");
    EXPECT_EQ(w.caller->capability, "get_dataset");
    EXPECT_EQ(w.caller->input,
              (json{{"database", "customer_success"}, {"dataset", "accounts"}}));
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
    EXPECT_EQ(html.find("<th>Rows</th>"), std::string::npos);
    EXPECT_EQ(html.find("<th>Bytes</th>"), std::string::npos);
    EXPECT_NE(html.find("Open a dataset for its row count, size, format and sample"),
              std::string::npos);

    page.section = "dataset";
    page.subject = "accounts";
    page.data = {{"get_dataset",
                  {{"database", "datasets"},
                   {"rows", 1234},
                   {"expected_rows", 2000},
                   {"size_bytes", 5678},
                   {"format", "parquet"},
                   {"columns", json::array({"id", "plan"})},
                   {"partitions", json::array({{{"partition_id", "p1"}}})}}},
                 {"sample", {{"rows", json::array({{{"id", 1}, {"plan", "pro"}}})}}}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("First Rows"), std::string::npos);
    EXPECT_NE(html.find(">pro<"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/dataset/promote\""), std::string::npos);
    EXPECT_NE(html.find("<dt>Rows</dt><dd>1234</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Bytes</dt><dd>5678</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Format</dt><dd>parquet</dd>"), std::string::npos);

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
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
            input["workspace_id"] = w.workspace_id();
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    for (const std::string_view read : {"list_agents",
                                        "get_agent",
                                        "agent_training_status",
                                        "get_policy",
                                        "get_experiment",
                                        "list_ledger",
                                        "get_monitor",
                                        "get_roi_readout"}) {
        const auto result = call(
            "workspace_query", {{"customer_id", w.customer_id}, {"capability", std::string(read)}});
        EXPECT_TRUE(result.ok) << read << ": " << result.error.message;
        EXPECT_TRUE(w.caller->read) << read;
    }
    for (const std::string_view action : {"create_agent",
                                          "update_agent",
                                          "delete_agent",
                                          "set_training_schedule",
                                          "agent_run",
                                          "update_policy",
                                          "decide",
                                          "approve_decision",
                                          "record_outcome",
                                          "run_solution"}) {
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
        "agent-ask-orama=http://ask:8080,agent-project=http://project:8080,"
        "agent-application=http://application:8080,"
        "agent-experiments=http://experiments:8080,unknown=http://x,bad");
    EXPECT_EQ(endpoints.size(), 4U) << "unknown keys and malformed pairs are ignored";
    const json library = lake::control::agent_library_json(endpoints);
    const json agents = library.at("agents");
    std::vector<std::string> keys;
    for (const json& agent : agents) {
        keys.push_back(agent.at("key"));
    }
    EXPECT_EQ(keys,
              (std::vector<std::string>{"agent-ask-orama",
                                        "agent-project",
                                        "agent-application",
                                        "agent-experiments",
                                        "agent-autosuggest",
                                        "agent-creator",
                                        "agent-eval-generator",
                                        "agent-autosuggest-corpus",
                                        "agent-project-corpus"}));
    EXPECT_EQ(agents[0].at("name"), "Ask Orama");
    EXPECT_EQ(agents[0].at("status"), "Available");
    EXPECT_EQ(agents[2].at("name"), "ApplicationAgent");
    EXPECT_EQ(agents[2].at("status"), "Available");
    EXPECT_EQ(agents[3].at("name"), "ExperimentsAgent");
    EXPECT_EQ(agents[3].at("status"), "Available");
    EXPECT_EQ(agents[4].at("status"), "Not running on this lake");
    // Every library agent is one you can talk to (lake#263): the generators too.
    EXPECT_EQ(agents[6].at("status"), "Not running on this lake");
    EXPECT_EQ(agents[6].at("runs_as"), "service");
    for (const auto& agent : agents) {
        EXPECT_TRUE(agent.at("chat").get<bool>()) << agent.at("key");
    }
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
    std::shared_ptr<lake::control::MemoryAgentBus> bus =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> agent_bus =
        std::make_shared<lake::control::AgentTaskBus>(
            w.store,
            bus,
            std::vector<std::string>{"agent-ask-orama",
                                     "agent-project",
                                     "agent-application",
                                     "agent-experiments",
                                     "agent-autosuggest",
                                     "agent-creator"},
            std::chrono::seconds(0));

    AgentRuns() {
        EXPECT_TRUE(liquid::runtime::block_on(agent_bus->start()).value());
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
        context.agent_bus = agent_bus.get();
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_session_id = principal.session_id;
        context.scopes = principal.scopes;
        context.pool_id = "lake";
        context.request_id = lake::control::new_uuid_v4();
        const auto capability = registry.find(name);
        if (capability != nullptr &&
            capability->input_schema.value("properties", json::object()).contains("customer_id")) {
            input["customer_id"] = w.customer_id;
        }
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
            input["workspace_id"] = w.workspace_id();
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
    lake::api::ApiResult call_as(const lake::control::Principal& caller,
                                 const std::string& name,
                                 json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.agent_caller = &agents;
        context.agent_bus = agent_bus.get();
        context.apps_wildcard = "apps.example.test";
        context.apps_per_customer = 10;
        context.app_replicas_per_customer = 20;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = caller.user_id;
        context.acting_identity = caller.taap_id;
        context.credential_session_id = caller.session_id;
        context.credential_kind = caller.kind;
        context.credential_workload_id = caller.workload_id;
        context.credential_customer_id = caller.workload_customer_id;
        context.credential_repository_id = caller.workload_repository_id;
        context.credential_revision = caller.workload_revision;
        context.credential_token_hash = caller.workload_token_hash;
        context.pool_id = caller.pool_id;
        context.scopes = caller.scopes;
        context.request_id = lake::control::new_uuid_v4();
        if (name != "get_dataset" &&
            registry.find(name)->input_schema["properties"].contains("customer_id") &&
            !input.contains("customer_id")) {
            input["customer_id"] = w.customer_id;
        }
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") && !input.contains("workspace_id")) {
            input["workspace_id"] = w.workspace_id();
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
    // A call as a library agent's service: the agent service token (docs/api/agent-bus.md).
    lake::api::ApiResult as_agent(const std::string& name,
                                  json input,
                                  bool service = true,
                                  const std::string& agent = "agent-ask-orama") {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.agent_bus = agent_bus.get();
        context.authorized = true;
        context.acting_identity = service ? "agent-service:" + agent : principal.taap_id;
        context.credential_kind = service ? "agent_service" : "session";
        context.agent_service = service ? agent : "";
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
    std::size_t pump() {
        auto pumped = liquid::runtime::block_on(agent_bus->pump(std::chrono::milliseconds(0)));
        return pumped && *pumped ? **pumped : 0;
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

TEST(AgentRuns, EveryAgentFlowTheSiteOffersIsACapability) {
    // UX review 2026-09-27: the Agent Factory's approve, Discover's create and reading a
    // conversation were page-only; REST and MCP now do the same through these.
    AgentRuns r;
    for (const char* name : {"create_agent_from_proposal",
                             "create_projects_from_opportunities",
                             "start_solution",
                             "confirm_solution",
                             "get_ask_conversation"}) {
        EXPECT_NE(r.registry.find(name), nullptr) << name;
    }
    r.w.caller->answers["create_agent"] = {{"id", "a-9"}};
    r.w.caller->answers["get_agent"] = {{"id", "a-9"}, {"name", "Renewal watcher"},
                                        {"runtime", "python"}};
    const json proposal = {{"type", "python"},
                           {"storage", "zetta"},
                           {"agent", {{"name", "Renewal watcher"}, {"purpose", "Flag renewals"},
                                      {"manifesto", "Company data only."}}},
                           {"files", json::array({{{"path", "agent.toml"},
                                                   {"contents", "entrypoint = \"agent.main:run\""}}})}};
    EXPECT_FALSE(r.call("create_agent_from_proposal",
                        {{"proposal", proposal}, {"defects", json::array({"no goal"})}})
                     .ok)
        << "a proposal with defects is refused";
    const auto created = r.call("create_agent_from_proposal", {{"proposal", proposal}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["agent_id"], "a-9");
    EXPECT_EQ(created.value["runtime"], "python");
    const auto made = std::ranges::find_if(r.w.caller->calls, [](const auto& call) {
        return call.first == "create_agent";
    });
    ASSERT_NE(made, r.w.caller->calls.end());
    EXPECT_EQ(made->second["definition"]["source"], "zetta");
    EXPECT_EQ(made->second["definition"]["entrypoint"], "agent.main:run");
    EXPECT_NE(made->second["definition"]["instructions"].get<std::string>().find("Company data only."),
              std::string::npos)
        << "instructions.md is written from the agent";
    EXPECT_FALSE(r.store.list_agent_launches(r.w.customer_id, "a-9", 5)->empty())
        << "its first check run started";
    EXPECT_TRUE(created.value.contains("check_launch_id"));

    r.w.caller->answers["create_project"] = {{"name", "x"}};
    const auto drafted = r.call("create_projects_from_opportunities",
                                {{"opportunities",
                                  json::array({{{"intent", "Predict churn next quarter"},
                                                {"solution", "S-1"}}})}});
    ASSERT_TRUE(drafted.ok) << drafted.error.message;
    EXPECT_TRUE(drafted.value["projects"][0].get<std::string>().starts_with("predict-churn-next-quarter-"));
    EXPECT_FALSE(r.call("create_projects_from_opportunities",
                        {{"opportunities", json::array({json::object()})}})
                     .ok)
        << "an Opportunity needs an intent";

    // lake#289: with the engine's methodology registry, a project gets its Opportunity's
    // methodology's own pipeline, or none, with the issue, when the engine cannot run it yet.
    r.w.caller->answers["list_methodologies"] = {
        {"methodologies",
         json::array({{{"id", "M-3"}, {"name", "Segmentation / clustering"},
                       {"solutions", json::array({"S-3"})}, {"runnable", true}, {"issue", "zetta#1042"}},
                      {{"id", "M-14"}, {"name", "Speech & audio analytics"},
                       {"solutions", json::array({"S-14"})}, {"runnable", false},
                       {"issue", "zetta#1053"}}})}};
    r.w.caller->answers["capture_intent"] = {{"input_id", "in-1"}};
    r.w.caller->answers["compile_intent"] = {{"name", "customer segments"}};
    // The fixture replaces the engine's deployment catalogue: deliver its change notification.
    r.w.service->invalidate_registry(r.w.customer_id, r.w.workspace_id());
    r.w.caller->calls.clear();
    const auto segmented = r.call(
        "create_projects_from_opportunities",
        {{"opportunities", json::array({{{"intent", "Group my customers by behaviour"},
                                         {"methodology", "M-3 Segmentation"}}})}});
    ASSERT_TRUE(segmented.ok) << segmented.error.message;
    EXPECT_TRUE(segmented.value["waiting"].empty());
    const auto made_project = std::ranges::find_if(r.w.caller->calls, [](const auto& call) {
        return call.first == "create_project";
    });
    ASSERT_NE(made_project, r.w.caller->calls.end());
    EXPECT_FALSE(made_project->second.contains("intent"))
        << "no skeleton compiled from the words: the methodology compiles the pipeline";
    const auto compiled = std::ranges::find_if(r.w.caller->calls, [](const auto& call) {
        return call.first == "compile_intent";
    });
    ASSERT_NE(compiled, r.w.caller->calls.end());
    EXPECT_EQ(compiled->second["methodology"], "M-3");

    r.w.caller->calls.clear();
    const auto speech = r.call(
        "create_projects_from_opportunities",
        {{"opportunities", json::array({{{"intent", "Find churn signals in call recordings"},
                                         {"solution", "S-14"}}})}});
    ASSERT_TRUE(speech.ok) << speech.error.message;
    ASSERT_EQ(speech.value["waiting"].size(), 1U);
    EXPECT_EQ(speech.value["waiting"][0]["methodology"], "M-14");
    EXPECT_EQ(speech.value["waiting"][0]["issue"], "zetta#1053");
    EXPECT_TRUE(std::ranges::none_of(r.w.caller->calls, [](const auto& call) {
        return call.first == "compile_intent";
    })) << "nothing is compiled in place of a methodology the engine cannot run";
    r.w.caller->answers.erase("list_methodologies");

    // The registry lookup: an id written in the text, else the solution it serves.
    const json registry = json::array(
        {{{"id", "M-3"}, {"solutions", json::array({"S-3"})}},
         {{"id", "M-8"}, {"solutions", json::array({"S-11"})}}});
    EXPECT_EQ(lake::api::find_methodology(registry, "portfolio M-3 (k-means)", "")["id"], "M-3");
    EXPECT_EQ(lake::api::find_methodology(registry, "", "S-11")["id"], "M-8");
    EXPECT_TRUE(lake::api::find_methodology(registry, "clustering", "S-99").is_null());
    EXPECT_TRUE(lake::api::find_methodology(json(nullptr), "M-3", "").is_null());

    const std::string conversation = lake::control::new_uuid_v4();
    ASSERT_TRUE(r.store.append_ask_message(
        {"", conversation, r.w.customer_id, r.principal.user_id, "user", "hi", nullptr, ""}));
    lake::api::ApiContext context;
    context.store = &r.store;
    context.authorized = true;
    context.acting_user_id = r.principal.user_id;
    context.acting_identity = r.principal.taap_id;
    context.request_id = lake::control::new_uuid_v4();
    const auto read = liquid::runtime::block_on(r.registry.call_async(
        "get_ask_conversation", {{"conversation_id", conversation}}, context));
    ASSERT_TRUE(read && read->ok) << (read ? read->error.message : "");
    ASSERT_EQ((*read).value["messages"].size(), 1U);
    EXPECT_EQ((*read).value["messages"][0]["content"], "hi");
}

TEST(AgentRuns, APersonStartsAndConfirmsTheProjectAgentsSolutionFoundation) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-project", "http://project:8080"}};
    config.internal_url = "http://lake.internal:4900";
    r.w.caller->answers["create_project"] = {{"name", "retention"}};
    r.w.caller->answers["get_project"] = {{"name", "retention"}, {"graphs", json::array()}};
    r.w.caller->answers["create_solution"] = {{"project", "retention"},
                                              {"solution", "S-1"},
                                              {"level", 1},
                                              {"graph", "S-1 CL-1"},
                                              {"policy_id", "policy-1"},
                                              {"experiment_id", "experiment-1"},
                                              {"monitor_id", "monitor-1"},
                                              {"readout_id", "readout-1"}};

    const auto started =
        r.call("start_solution",
               {{"project", "retention"},
                {"solution", "S-1"},
                {"level", 1},
                {"dataset", "datasets.customers"},
                {"delivery",
                 {{"kind", "webhook"},
                  {"endpoint", "https://crm.example.test/tasks"},
                  {"owner", "retention-team"}}},
                {"roi_model",
                 {{"value_per_outcome", 125.0}, {"investment", 4000.0}, {"currency", "USD"}}}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const std::string task_id = started.value["task_id"];
    const auto task = r.store.get_agent_task(task_id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->kind, "solution:retention");
    EXPECT_EQ(task->body["context"]["dataset"], "datasets.customers");
    EXPECT_EQ(task->body["context"]["roi_model"]["investment"], 4000.0);
    EXPECT_EQ(std::ranges::find(task->scopes, "workspace_act"), task->scopes.end())
        << "the proposing agent cannot create or run the solution";
    EXPECT_TRUE(std::ranges::none_of(r.w.caller->calls, [](const auto& call) {
        return call.first == "create_solution";
    })) << "Start only asks for a proposal";

    ASSERT_TRUE(r.store.set_agent_task_status(
        task_id,
        "Succeeded",
        "",
        "",
        {{"ok", true},
         {"result",
          {{"foundation",
            {{"sources",
              {{"source", "invented"},
               {"table", "invented"},
               {"M-2", {{"source", "crm"}, {"record_id", "id"}, {"name", "account"}}}}},
             {"snapshot",
              {{"database", "datasets"},
               {"source_table", "customers"},
               {"target_table", "retention_snapshot"},
               {"entity_column", "entity_id"},
               {"time_column", "observed_at"},
               {"as_of", "2026-09-28T00:00:00Z"}}},
             {"dataset",
              {{"entity_column", "entity_id"},
               {"label_column", "retained"},
               {"feature_columns", json::array({"usage"})}}},
             {"decision_source",
              {{"methodology", "M-2"}, {"entity_column", "record_id"}, {"score_column", "risk"}}},
             {"predicates",
              {{"in_collections", "in_collections"},
               {"open_complaint", "open_complaint"},
               {"vulnerable", "vulnerable"}}}}}}}}));

    const auto confirmed =
        r.call("confirm_solution",
               {{"task_id", task_id},
                {"edits", {{"dataset", {{"feature_columns", json::array({"usage", "tenure"})}}}}}});
    ASSERT_TRUE(confirmed.ok) << confirmed.error.message;
    EXPECT_FALSE(confirmed.value["actions_blocked"]);
    const auto created = std::ranges::find_if(
        r.w.caller->calls, [](const auto& call) { return call.first == "create_solution"; });
    ASSERT_NE(created, r.w.caller->calls.end());
    const json& input = created->second;
    EXPECT_EQ(input["sources"]["source"], "datasets")
        << "the person's selected dataset wins over an agent change";
    EXPECT_EQ(input["sources"]["table"], "customers");
    EXPECT_EQ(input["sources"]["M-2"]["record_id"], "id");
    EXPECT_EQ(input["sources"]["policy_predicates"]["vulnerable"], "vulnerable");
    EXPECT_TRUE(input["sources"]["missing_policy_predicates"].empty());
    EXPECT_EQ(input["foundation"]["delivery"]["owner"], "retention-team");
    EXPECT_EQ(input["foundation"]["roi_model"]["value_per_outcome"], 125.0);
    EXPECT_EQ(input["foundation"]["dataset"]["feature_columns"], json::array({"usage", "tenure"}));
    EXPECT_FALSE(input["foundation"].contains("predicates"))
        << "proposal evidence is not sent to the engine's closed foundation schema";

    const auto guarded =
        r.call("start_solution",
               {{"project", "guarded"},
                {"solution", "S-1"},
                {"level", 1},
                {"dataset", "datasets.customers"},
                {"delivery",
                 {{"kind", "webhook"},
                  {"endpoint", "https://crm.example.test/tasks"},
                  {"owner", "retention-team"}}},
                {"roi_model",
                 {{"value_per_outcome", 125.0}, {"investment", 4000.0}, {"currency", "USD"}}}});
    ASSERT_TRUE(guarded.ok) << guarded.error.message;
    const std::string guarded_task_id = guarded.value["task_id"];
    ASSERT_TRUE(r.store.set_agent_task_status(
        guarded_task_id,
        "Succeeded",
        "",
        "",
        {{"ok", true},
         {"result",
          {{"foundation",
            {{"sources", {{"M-2", {{"source", "crm"}, {"record_id", "id"}, {"name", "account"}}}}},
             {"snapshot",
              {{"database", "datasets"},
               {"source_table", "customers"},
               {"target_table", "retention_snapshot"},
               {"entity_column", "entity_id"},
               {"time_column", "observed_at"},
               {"as_of", "2026-09-28T00:00:00Z"}}},
             {"dataset",
              {{"entity_column", "entity_id"},
               {"label_column", "retained"},
               {"feature_columns", json::array({"usage"})}}},
             {"predicates",
              {{"in_collections", "in_collections"}, {"open_complaint", "open_complaint"}}},
             {"missing_predicates", json::array({"vulnerable"})}}}}}}));
    const auto blocked = r.call("confirm_solution", {{"task_id", guarded_task_id}});
    ASSERT_TRUE(blocked.ok) << blocked.error.message;
    EXPECT_TRUE(blocked.value["actions_blocked"]);
    EXPECT_EQ(blocked.value["missing_predicates"], json::array({"vulnerable"}));
    EXPECT_EQ(r.w.caller->calls.back().first, "create_solution");
    EXPECT_EQ(r.w.caller->calls.back().second["sources"]["missing_policy_predicates"],
              json::array({"vulnerable"}));
}

TEST(AgentRuns, LibraryAgentsTakeTasksAndAnswerOverTheBus) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"}};
    config.internal_url = "http://lake.internal:4900";
    const auto asked = r.call("agent_turn", {{"agent", "agent-ask-orama"}, {"message", "hi"}});
    ASSERT_TRUE(asked.ok) << asked.error.message;
    const std::string task_id = asked.value["task_id"];

    // Only a library agent's service reaches the bus's agent side.
    EXPECT_EQ(r.as_agent("receive_agent_tasks", {{"agent", "agent-ask-orama"}}, false).error.code,
              "unauthorized");
    EXPECT_EQ(r.as_agent("receive_agent_tasks", {{"agent", "agent-project"}}).error.code,
              "unauthorized")
        << "an agent reaches only its own inbox";
    EXPECT_EQ(
        r.as_agent("receive_agent_tasks", {{"agent", "agent-nope"}}, true, "agent-nope").error.code,
        "not_found");
    const auto inbox = r.as_agent("receive_agent_tasks", {{"agent", "agent-ask-orama"}});
    ASSERT_TRUE(inbox.ok) << inbox.error.message;
    ASSERT_EQ(inbox.value["tasks"].size(), 1U);
    EXPECT_EQ(inbox.value["tasks"][0]["task_id"], task_id);
    EXPECT_EQ(inbox.value["tasks"][0]["workspace_id"], r.w.workspace_id());
    EXPECT_EQ(inbox.value["next_sequence"], 2);
    const auto other_workspace =
        r.store.create_workspace({r.w.customer_id, "other", "prod", ""});
    ASSERT_TRUE(other_workspace);
    EXPECT_TRUE(r.as_agent("receive_agent_tasks",
                           {{"agent", "agent-ask-orama"},
                            {"workspace_id", other_workspace->id}})
                    .value["tasks"]
                    .empty());

    // One claim, by the task's own agent; the body arrives with a fresh credential.
    EXPECT_EQ(r.as_agent("claim_agent_task",
                         {{"task_id", task_id}, {"agent", "agent-project"}},
                         true,
                         "agent-project")
                  .error.code,
              "conflict");
    const auto wrong_workspace =
        r.as_agent("claim_agent_task",
                   {{"task_id", task_id},
                    {"workspace_id", other_workspace->id},
                    {"agent", "agent-ask-orama"}});
    EXPECT_EQ(wrong_workspace.error.code, "conflict");
    EXPECT_EQ(wrong_workspace.error.path, "/workspace_id");
    const auto claimed =
        r.as_agent("claim_agent_task", {{"task_id", task_id}, {"agent", "agent-ask-orama"}});
    ASSERT_TRUE(claimed.ok) << claimed.error.message;
    const std::string credential = claimed.value["body"]["lake_token"];
    EXPECT_TRUE(credential.starts_with("lat." + task_id + "."));
    EXPECT_TRUE(r.store.context_for(credential)) << "the credential acts while the task runs";
    EXPECT_EQ(r.as_agent("claim_agent_task", {{"task_id", task_id}, {"agent", "agent-ask-orama"}})
                  .error.code,
              "conflict")
        << "exactly one claim succeeds";
    EXPECT_TRUE(r.as_agent("receive_agent_tasks", {{"agent", "agent-ask-orama"}})
                    .value["tasks"]
                    .empty())
        << "a claimed task is not offered again";
    const auto wrong_event_workspace =
        r.as_agent("publish_agent_event",
                   {{"task_id", task_id},
                    {"workspace_id", other_workspace->id},
                    {"agent", "agent-ask-orama"},
                    {"type", "progress"}});
    EXPECT_EQ(wrong_event_workspace.error.code, "conflict");
    EXPECT_EQ(wrong_event_workspace.error.path, "/workspace_id");

    // Progress and the result go out on agent-events; lake's bus loop applies them.
    ASSERT_TRUE(r.as_agent("publish_agent_event",
                           {{"task_id", task_id},
                            {"agent", "agent-ask-orama"},
                            {"type", "progress"},
                            {"message", "Reading your datasets"}})
                    .ok);
    EXPECT_EQ(r.pump(), 0U);
    const auto live = r.call("receive_agent_events", {{"task_id", task_id}});
    ASSERT_TRUE(live.ok) << live.error.message;
    ASSERT_EQ(live.value["events"].size(), 1U);
    EXPECT_EQ(live.value["events"][0]["type"], "progress");
    EXPECT_EQ(live.value["task"]["status"], "running");
    EXPECT_EQ(live.value["task"]["message"], "Reading your datasets");
    EXPECT_EQ(r.as_agent("publish_agent_event",
                         {{"task_id", task_id}, {"agent", "agent-project"}, {"type", "failed"}},
                         true,
                         "agent-project")
                  .error.code,
              "conflict")
        << "only the agent that claimed it reports on it";
    ASSERT_TRUE(r.as_agent("publish_agent_event",
                           {{"task_id", task_id},
                            {"agent", "agent-ask-orama"},
                            {"type", "result"},
                            {"result", {{"ok", true}, {"reply", "hello"}}}})
                    .ok);
    EXPECT_EQ(r.pump(), 0U);
    const auto done = r.call("get_agent_task", {{"task_id", task_id}});
    EXPECT_EQ(done.value["status"], "succeeded");
    EXPECT_EQ(done.value["answer"]["reply"], "hello");
    EXPECT_FALSE(r.store.context_for(credential)) << "a finished task's credential stops";
    const auto followed = r.call("receive_agent_events",
                                 {{"task_id", task_id}, {"from_sequence", 2}});
    ASSERT_EQ(followed.value["events"].size(), 1U);
    EXPECT_EQ(followed.value["events"][0]["result"]["reply"], "hello");
    EXPECT_EQ(r.as_agent("publish_agent_event",
                         {{"task_id", task_id}, {"agent", "agent-ask-orama"}, {"type", "failed"}})
                  .error.code,
              "conflict")
        << "a finished task stays finished";

    // A task claimed later starts where the loop has read to, past every earlier task's events,
    // so following it never re-reads the topic's history (lake#275).
    const auto second = r.call("agent_turn", {{"agent", "agent-ask-orama"}, {"message", "more"}});
    const std::string second_id = second.value["task_id"];
    ASSERT_TRUE(r.as_agent("claim_agent_task", {{"task_id", second_id}, {"agent", "agent-ask-orama"}})
                    .ok);
    EXPECT_EQ(r.store.get_agent_task(second_id)->events_from, 1U)
        << "two events of the first task were read";
    ASSERT_TRUE(r.as_agent("publish_agent_event", {{"task_id", second_id},
                                                   {"agent", "agent-ask-orama"},
                                                   {"type", "progress"},
                                                   {"message", "second"}})
                    .ok);
    const auto own = r.call("receive_agent_events", {{"task_id", second_id}});
    ASSERT_EQ(own.value["events"].size(), 1U);
    EXPECT_EQ(own.value["events"][0]["sequence"], 3) << "read from the task's own start";
    EXPECT_EQ(own.value["events"][0]["message"], "second");

    // What the bus loses is recovered from the store: an unclaimed task is published again, and
    // one nobody took by its deadline fails.
    const auto lost = r.call("agent_turn", {{"agent", "agent-ask-orama"}, {"message", "again"}});
    const std::string lost_id = lost.value["task_id"];
    EXPECT_EQ(r.bus->events("agent-tasks.agent-ask-orama").size(), 3U);
    r.pump();
    EXPECT_EQ(r.bus->events("agent-tasks.agent-ask-orama").size(), 3U) << "not before a minute";
    r.store.age_agent_task(lost_id, std::chrono::seconds(61));
    r.pump();
    EXPECT_EQ(r.bus->events("agent-tasks.agent-ask-orama").size(), 4U);
    r.store.age_agent_task(lost_id, std::chrono::seconds(300));
    r.pump();
    const auto expired = r.call("get_agent_task", {{"task_id", lost_id}});
    EXPECT_EQ(expired.value["status"], "failed");
    EXPECT_NE(expired.value["message"].get<std::string>().find("No agent picked this up"),
              std::string::npos);
}

TEST(AgentRuns, AgentTurnReachesLibraryNativeAndPythonAgents) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"},
                              {"agent-project", "http://project:8080"},
                              {"agent-application", "http://application:8080"},
                              {"agent-experiments", "http://experiments:8080"},
                              {"agent-autosuggest", "http://suggest:8080"}};
    config.internal_url = "http://lake.internal:4900";
    config.agent_service_token = "service-token";

    // A library agent's turn runs in the background (lake#269): the request answers with the
    // task at once, and the agent is never called inside it.
    const std::size_t called = r.agents.turns.size();
    const auto asked = r.call("agent_turn", {{"agent", "agent-ask-orama"}, {"message", "hi"}});
    ASSERT_TRUE(asked.ok) << asked.error.message;
    EXPECT_EQ(asked.value["status"], "pending");
    EXPECT_EQ(r.agents.turns.size(), called) << "the request never waits on the agent";
    const auto ask_task = r.store.get_agent_task(asked.value["task_id"]);
    ASSERT_TRUE(ask_task);
    EXPECT_EQ(ask_task->body["lake_token"], "{{credential}}")
        << "the credential is made when an agent claims the task, and never stored";
    const auto published = r.bus->events("agent-tasks.agent-ask-orama");
    ASSERT_EQ(published.size(), 1U) << "the task is published on its agent's topic";
    EXPECT_EQ(published[0].correlation_id, ask_task->id);
    EXPECT_EQ(published[0].payload["task_id"], ask_task->id);
    EXPECT_FALSE(published[0].payload.contains("body")) << "no task body on the bus";
    EXPECT_EQ(ask_task->body["lake_url"], "http://lake.internal:4900");
    // Ask Orama reads what the person may read and changes nothing itself.
    const auto& scopes = ask_task->scopes;
    EXPECT_NE(std::ranges::find(scopes, "agent_turn"), scopes.end());
    EXPECT_NE(std::ranges::find(scopes, "get_agent_task"), scopes.end());
    EXPECT_NE(std::ranges::find(scopes, "list_agent_launches"), scopes.end());
    EXPECT_EQ(std::ranges::find(scopes, "launch_agent"), scopes.end());

    // Its credential acts as the person, with the task's scopes, while the task runs.
    const std::string credential =
        "lat." + ask_task->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(r.store.context_for(credential)) << "not before an agent claims the task";
    ASSERT_TRUE(r.store.claim_agent_task(
        ask_task->id, "agent-ask-orama", lake::control::identity_crypto::sha256_hex(credential)));
    const auto acting = r.store.context_for(credential);
    ASSERT_TRUE(acting) << acting.error().message;
    EXPECT_EQ(acting->user_id, r.principal.user_id);
    EXPECT_EQ(acting->kind, "agent_task");
    const auto delegated = r.call_as(
        *acting,
        "agent_turn",
        {{"agent", "agent-application"}, {"message", "Build a customer health app"}});
    ASSERT_TRUE(delegated.ok) << delegated.error.message;
    const auto delegated_task = r.store.get_agent_task(delegated.value["task_id"]);
    ASSERT_TRUE(delegated_task);
    EXPECT_EQ(delegated_task->agent, "agent-application");
    EXPECT_EQ(delegated_task->kind, "app");
    EXPECT_EQ(r.bus->events("agent-tasks.agent-application").size(), 1U);
    const auto delegated_experiment = r.call_as(
        *acting,
        "agent_turn",
        {{"agent", "agent-experiments"}, {"message", "Tune the churn pipeline"}});
    ASSERT_TRUE(delegated_experiment.ok) << delegated_experiment.error.message;
    const auto delegated_experiment_task =
        r.store.get_agent_task(delegated_experiment.value["task_id"]);
    ASSERT_TRUE(delegated_experiment_task);
    EXPECT_EQ(delegated_experiment_task->kind, "experiment");
    EXPECT_EQ(r.bus->events("agent-tasks.agent-experiments").size(), 1U);
    ASSERT_TRUE(r.store.record_agent_task_event(
        ask_task->id, "agent-ask-orama", "Succeeded", "", {{"ok", true}, {"reply", "hello"}}));
    EXPECT_FALSE(r.store.context_for(credential)) << "a finished task's credential stops";
    const auto followed = r.call("get_agent_task", {{"task_id", ask_task->id}});
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["status"], "succeeded");
    EXPECT_EQ(followed.value["answer"]["reply"], "hello");

    const auto built = r.call("agent_turn", {{"agent", "agent-project"}, {"message", "churn"}});
    ASSERT_TRUE(built.ok) << built.error.message;
    const auto project_task = r.store.get_agent_task(built.value["task_id"]);
    ASSERT_TRUE(project_task);
    EXPECT_EQ(project_task->body["zetta_base_url"],
              "http://lake.internal:4900/w/" + r.w.workspace_id() + "/workspaces/" + r.w.customer_id + "/zetta");
    EXPECT_NE(std::ranges::find(project_task->scopes, "agent_turn"),
              project_task->scopes.end());
    const std::string project_credential =
        "lat." + project_task->id + ".abcdef0123456789abcdef0123456789";
    ASSERT_TRUE(r.store.claim_agent_task(
        project_task->id,
        "agent-project",
        lake::control::identity_crypto::sha256_hex(project_credential)));
    const auto project_principal = r.store.context_for(project_credential);
    ASSERT_TRUE(project_principal);
    const auto project_delegated = r.call_as(
        *project_principal,
        "agent_turn",
        {{"agent", "agent-experiments"}, {"message", "Tune this project"}});
    ASSERT_TRUE(project_delegated.ok) << project_delegated.error.message;
    EXPECT_EQ(r.store.get_agent_task(project_delegated.value["task_id"])->kind, "experiment");

    // A change to an existing pipeline: the agent is handed the graph, and a finished proposal
    // names the pipeline page where it is reviewed (lake#249).
    r.w.caller->answers["get_graph"] = {{"name", "churn"},
                                        {"nodes", json::array({{{"id", "pull"}, {"kind", "connect"},
                                                                {"name", "Pull"}}})},
                                        {"edges", json::array()}};
    const auto proposed = r.call("agent_turn",
                                 {{"agent", "agent-project"},
                                  {"message", "rename it"},
                                  {"context", {{"project", "retention"}, {"graph", "churn"}}}});
    ASSERT_TRUE(proposed.ok) << proposed.error.message;
    const std::string proposal_task = proposed.value["task_id"];
    EXPECT_EQ(r.store.get_agent_task(proposal_task)->body["context"]["pipeline"]["nodes"][0]["id"],
              "pull");
    ASSERT_TRUE(r.store.set_agent_task_status(
        proposal_task, "Succeeded", "", "",
        {{"ok", true},
         {"result",
          {{"kind", "proposal"},
           {"proposal",
            {{"project", "retention"},
             {"graph", "churn"},
             {"edits", json::array({{{"op", "rename"}, {"name", "churn v2"}}})}}}}}}));
    const auto reviewed = r.call("get_agent_task", {{"task_id", proposal_task}});
    ASSERT_TRUE(reviewed.ok);
    EXPECT_EQ(reviewed.value["review_url"],
              "/design/pipeline?customer_id=" + r.w.customer_id +
                  "&project=retention&graph=churn&task=" + proposal_task);

    // Auto-Suggest builds the workspace profile itself, with the task's read-only credential.
    const auto suggested = r.call("agent_turn", {{"agent", "agent-autosuggest"}, {"message", "?"}});
    ASSERT_TRUE(suggested.ok) << suggested.error.message;
    const auto suggest_task = r.store.get_agent_task(suggested.value["task_id"]);
    EXPECT_FALSE(suggest_task->body.contains("profile")) << "lake reads nothing inside the request";
    EXPECT_EQ(suggest_task->body["zetta_token"], "{{credential}}");
    EXPECT_EQ(std::ranges::find(suggest_task->scopes, "workspace_act"), suggest_task->scopes.end());

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

TEST(AgentRuns, ApplicationAgentWritesAppsOnlyAfterTheProposalIsConfirmed) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-application", "http://application:8080"}};
    config.internal_url = "http://lake.internal:4900";

    const auto started =
        r.call("agent_turn", {{"agent", "agent-application"}, {"message", "Build reports"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto first = r.store.get_agent_task(started.value["task_id"]);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->kind, "app");
    EXPECT_EQ(first->body["context"], json::object());
    EXPECT_EQ(r.bus->events("agent-tasks.agent-application").size(), 1U);
    for (const std::string_view capability :
         {"create_repository", "commit_repository_files", "create_app", "deploy_app"}) {
        EXPECT_EQ(std::ranges::find(first->scopes, capability), first->scopes.end()) << capability;
    }
    for (const std::string_view capability :
         {"list_datasets", "get_dataset", "workspace_sql", "repository_file", "list_apps"}) {
        EXPECT_NE(std::ranges::find(first->scopes, capability), first->scopes.end()) << capability;
    }

    const std::string first_credential =
        "lat." + first->id + ".0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(r.store.claim_agent_task(
        first->id,
        "agent-application",
        lake::control::identity_crypto::sha256_hex(first_credential)));
    const auto first_principal = r.store.context_for(first_credential);
    ASSERT_TRUE(first_principal);
    r.w.caller->answers["get_dataset"] = {
        {"database", "datasets"}, {"name", "accounts"}, {"columns", json::array({"id"})}};
    const auto dataset = r.call_as(*first_principal, "get_dataset", {{"id", "datasets/accounts"}});
    ASSERT_TRUE(dataset.ok) << dataset.error.message;
    EXPECT_EQ(dataset.value["id"], "datasets/accounts");
    EXPECT_EQ(r.w.caller->input, (json{{"database", "datasets"}, {"dataset", "accounts"}}));
    const json create_input{{"name", "agent-reports"},
                            {"mode", "image"},
                            {"image", "registry.example.test/reports:first"},
                            {"framework", "container"},
                            {"port", 8080},
                            {"host", nullptr},
                            {"auth", "public"},
                            {"env", {{"API_KEY", {{"secret", "reports-api-key"}}}}}};
    EXPECT_EQ(r.call_as(*first_principal, "create_app", create_input).error.code, "unauthorized");
    EXPECT_EQ(r.call_as(*first_principal,
                        "deploy_app",
                        {{"app", "not-created"},
                         {"image", "registry.example.test/reports:first"}})
                  .error.code,
              "unauthorized");

    const json proposal{{"kind", "proposal"},
                        {"confirm", true},
                        {"files", json::array()},
                        {"app", {{"name", "agent-reports"}, {"mode", "image"}}}};
    const auto confirmed = r.call(
        "agent_turn",
        {{"agent", "agent-application"},
         {"message", "Confirm and deploy the reviewed proposal."},
         {"context", {{"confirmed_proposal", proposal}}}});
    ASSERT_TRUE(confirmed.ok) << confirmed.error.message;
    const auto second = r.store.get_agent_task(confirmed.value["task_id"]);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->body["context"]["confirmed_proposal"], proposal);
    for (const std::string_view capability :
         {"create_repository", "commit_repository_files", "create_app", "deploy_app"}) {
        EXPECT_NE(std::ranges::find(second->scopes, capability), second->scopes.end())
            << capability;
    }

    const std::string second_credential =
        "lat." + second->id + ".abcdef0123456789abcdef0123456789";
    ASSERT_TRUE(r.store.claim_agent_task(
        second->id,
        "agent-application",
        lake::control::identity_crypto::sha256_hex(second_credential)));
    const auto second_principal = r.store.context_for(second_credential);
    ASSERT_TRUE(second_principal);
    const auto created = r.call_as(*second_principal, "create_app", create_input);
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["auth"], "none");
    EXPECT_EQ(created.value["secret_env_refs"]["API_KEY"], "reports-api-key");
    const auto deployed = r.call_as(
        *second_principal,
        "deploy_app",
        {{"app", created.value["id"]}, {"image", "registry.example.test/reports:confirmed"}});
    ASSERT_TRUE(deployed.ok) << deployed.error.message;
    EXPECT_EQ(deployed.value["image"], "registry.example.test/reports:confirmed");
}

TEST(AgentRuns, AConversationFollowUpCarriesThePriorTurn) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-application", "http://application:8080"}};
    config.internal_url = "http://lake.internal:4900";

    const auto started =
        r.call("agent_turn", {{"agent", "agent-application"}, {"message", "Build reports"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto first = r.store.get_agent_task(started.value["task_id"]);
    ASSERT_TRUE(first);
    ASSERT_FALSE(first->conversation_id.empty());
    const json question{{"ok", true},
                        {"conversation_id", first->conversation_id},
                        {"reply", "Which dataset should the report use?"},
                        {"result", {{"kind", "question"}, {"confirm", false}}}};
    ASSERT_TRUE(r.store.set_agent_task_status(
        first->id, "Succeeded", "", "Which dataset should the report use?", question));

    const auto followed = r.call(
        "agent_turn",
        {{"agent", "agent-application"},
         {"kind", "app"},
         {"message", "Use datasets/accounts."},
         {"conversation_id", first->conversation_id}});
    ASSERT_TRUE(followed.ok) << followed.error.message;
    const auto second = r.store.get_agent_task(followed.value["task_id"]);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->conversation_id, first->conversation_id);
    EXPECT_EQ(second->body["message"], "Use datasets/accounts.");
    EXPECT_EQ(second->body["prior_turn"]["task_id"], first->id);
    EXPECT_EQ(second->body["prior_turn"]["answer"], question);
}

TEST(AgentRuns, AProjectAgentReplyIncludesTheQuestionAndAnswer) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-project", "http://project:8080"}};
    config.internal_url = "http://lake.internal:4900";
    r.w.caller->answers["get_graph"] = {
        {"name", "churn"}, {"project", "retention"}, {"nodes", json::array()}, {"edges", json::array()}};

    const json context{{"project", "retention"}, {"graph", "churn"}};
    const auto started = r.call(
        "agent_turn",
        {{"agent", "agent-project"},
         {"kind", "pipeline"},
         {"message", "Train a churn model"},
         {"context", context}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto first = r.store.get_agent_task(started.value["task_id"]);
    ASSERT_TRUE(first);
    const json question{{"ok", true},
                        {"result",
                         {{"kind", "question"},
                          {"proposal", nullptr},
                          {"reply", "Which dataset should I use?"}}}};
    ASSERT_TRUE(r.store.set_agent_task_status(
        first->id, "Succeeded", "", "Which dataset should I use?", question));

    const auto followed = r.call(
        "agent_turn",
        {{"agent", "agent-project"},
         {"kind", "pipeline"},
         {"message", "Use datasets/accounts."},
         {"conversation_id", first->conversation_id},
         {"context", context}});
    ASSERT_TRUE(followed.ok) << followed.error.message;
    const auto second = r.store.get_agent_task(followed.value["task_id"]);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->body["prior_turn"]["task_id"], first->id);
    EXPECT_NE(second->body["request"].get<std::string>().find("Which dataset should I use?"),
              std::string::npos);
    EXPECT_NE(second->body["request"].get<std::string>().find("Use datasets/accounts."),
              std::string::npos);
}

TEST(AgentRuns, ExperimentsAgentGrantsEachTurnOnlyItsStudyOperations) {
    AgentRuns r;
    auto& config = const_cast<lake::control::WorkspaceConfig&>(r.w.service->config());
    config.agent_endpoints = {{"agent-experiments", "http://experiments:8080"}};
    config.internal_url = "http://lake.internal:4900";

    const auto started = r.call(
        "agent_turn",
        {{"agent", "agent-experiments"}, {"message", "Tune retention"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto first = r.store.get_agent_task(started.value["task_id"]);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->kind, "experiment");
    EXPECT_EQ(first->body["kind"], "experiment");
    EXPECT_EQ(first->body["context"], json::object());
    EXPECT_EQ(first->body["lake_url"],
              "http://lake.internal:4900/w/" + r.w.workspace_id() + "/workspaces/" + r.w.customer_id + "/zetta");
    EXPECT_NE(std::ranges::find(first->scopes, "analyze_dataset"), first->scopes.end());
    EXPECT_EQ(std::ranges::find(first->scopes, "workspace_act"), first->scopes.end());
    EXPECT_EQ(std::ranges::find(first->scopes, "create_study"), first->scopes.end());

    const json proposal{{"kind", "proposal"},
                        {"study", lake::test::experiment_study().at("spec")},
                        {"explanations", {{"objective", "Retention quality"}}},
                        {"confirm", true}};
    ASSERT_TRUE(r.store.set_agent_task_status(
        first->id,
        "Succeeded",
        "",
        "",
        {{"ok", true}, {"reply", "Review it."}, {"result", proposal}}));
    const auto review = r.call("get_agent_task", {{"task_id", first->id}});
    ASSERT_TRUE(review.ok);
    EXPECT_EQ(review.value["review_url"],
              "/train/experiments?customer_id=" + r.w.customer_id + "&task=" + first->id +
                  "#experiment-proposal");

    const auto confirmed = r.call(
        "agent_turn",
        {{"agent", "agent-experiments"},
         {"kind", "experiment"},
         {"message", "Confirm and start the reviewed study proposal."},
         {"context", {{"confirmed_proposal", proposal}}}});
    ASSERT_TRUE(confirmed.ok) << confirmed.error.message;
    const auto second = r.store.get_agent_task(confirmed.value["task_id"]);
    ASSERT_TRUE(second);
    EXPECT_NE(std::ranges::find(second->scopes, "workspace_act"), second->scopes.end());
    EXPECT_NE(std::ranges::find(second->scopes, "create_study"), second->scopes.end());
    EXPECT_NE(std::ranges::find(second->scopes, "start_study"), second->scopes.end());
    EXPECT_EQ(std::ranges::find(second->scopes, "stop_trial"), second->scopes.end());
    const std::string confirmed_credential =
        "lat." + second->id + ".0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(r.store.claim_agent_task(
        second->id,
        "agent-experiments",
        lake::control::identity_crypto::sha256_hex(confirmed_credential)));
    const auto confirmed_principal = r.store.context_for(confirmed_credential);
    ASSERT_TRUE(confirmed_principal);
    r.w.caller->answers["create_study"] = lake::test::experiment_study();
    const auto created = r.call_as(
        *confirmed_principal,
        "workspace_act",
        {{"capability", "create_study"}, {"input", proposal.at("study")}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(r.w.caller->capability, "create_study");
    EXPECT_FALSE(r.w.caller->input.contains("confirm"));
    const auto refused = r.call_as(
        *confirmed_principal,
        "workspace_act",
        {{"capability", "promote_trial"},
         {"input", {{"study", "study-one"}, {"run", "trial-1"}}}});
    EXPECT_EQ(refused.error.code, "unauthorized");

    const auto status = r.call(
        "agent_turn",
        {{"agent", "agent-experiments"},
         {"kind", "experiment_status"},
         {"message", "Watch study-one"},
         {"context", {{"study", "study-one"}}}});
    ASSERT_TRUE(status.ok) << status.error.message;
    const auto status_task = r.store.get_agent_task(status.value["task_id"]);
    ASSERT_TRUE(status_task);
    EXPECT_NE(std::ranges::find(status_task->scopes, "stop_trial"),
              status_task->scopes.end());
    EXPECT_NE(std::ranges::find(status_task->scopes, "stop_experiment_trial"),
              status_task->scopes.end());
    EXPECT_EQ(std::ranges::find(status_task->scopes, "create_study"),
              status_task->scopes.end());
    const std::string status_credential =
        "lat." + status_task->id + ".abcdef0123456789abcdef0123456789";
    ASSERT_TRUE(r.store.claim_agent_task(
        status_task->id,
        "agent-experiments",
        lake::control::identity_crypto::sha256_hex(status_credential)));
    const auto status_principal = r.store.context_for(status_credential);
    ASSERT_TRUE(status_principal);
    r.w.caller->answers["stop_trial"] = {
        {"study", "study-one"}, {"run", "trial-2"}, {"state", "failed"}};
    const auto stopped = r.call_as(
        *status_principal,
        "workspace_act",
        {{"capability", "stop_trial"},
         {"input", {{"study", "study-one"}, {"run", "trial-2"}}}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;

    const json promotion{{"kind", "explanation"},
                         {"promotion", {{"study", "study-one"}, {"run", "trial-1"}}},
                         {"confirm", true}};
    const auto explained = r.call(
        "agent_turn",
        {{"agent", "agent-experiments"},
         {"kind", "experiment_explain"},
         {"message", "Promote the reviewed best trial."},
         {"context", {{"confirmed_promotion", promotion}}}});
    ASSERT_TRUE(explained.ok) << explained.error.message;
    const auto explained_task = r.store.get_agent_task(explained.value["task_id"]);
    ASSERT_TRUE(explained_task);
    EXPECT_NE(std::ranges::find(explained_task->scopes, "promote_trial"),
              explained_task->scopes.end());
    EXPECT_EQ(std::ranges::find(explained_task->scopes, "stop_trial"),
              explained_task->scopes.end());
    const std::string explained_credential =
        "lat." + explained_task->id + ".fedcba9876543210fedcba9876543210";
    ASSERT_TRUE(r.store.claim_agent_task(
        explained_task->id,
        "agent-experiments",
        lake::control::identity_crypto::sha256_hex(explained_credential)));
    const auto explained_principal = r.store.context_for(explained_credential);
    ASSERT_TRUE(explained_principal);
    r.w.caller->answers["promote_trial"] = {{"study", "study-one"},
                                             {"run", "trial-1"},
                                             {"model", "model-one"},
                                             {"project", "retention"}};
    const auto promoted = r.call_as(
        *explained_principal,
        "workspace_act",
        {{"capability", "promote_trial"},
         {"input", {{"study", "study-one"}, {"run", "trial-1"}}}});
    ASSERT_TRUE(promoted.ok) << promoted.error.message;

    EXPECT_EQ(r.call("agent_turn",
                     {{"agent", "agent-experiments"},
                      {"kind", "app"},
                      {"message", "wrong kind"}})
                  .error.code,
              "invalid_input");
}

TEST(AgentRuns, EachLibraryAgentHasItsOwnBusCredential) {
    const auto dir = lake::test::fixture_path("agent-bus-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "agent-project") << "project-token\n";
    std::ofstream(dir / "agent-ask-orama") << "ask-token";
    std::ofstream(dir / "agent-application") << "application-token";
    std::ofstream(dir / "agent-experiments") << "experiments-token";
    std::ofstream(dir / "empty") << "\n";
    std::filesystem::create_directories(dir / "..data");
    const auto tokens = lake::control::read_agent_bus_tokens(dir.string());
    EXPECT_EQ(tokens.size(), 4U) << "an empty file and the mount's own entries are skipped";
    EXPECT_EQ(tokens.at("agent-project"), "project-token");
    EXPECT_EQ(tokens.at("agent-ask-orama"), "ask-token");
    EXPECT_EQ(tokens.at("agent-application"), "application-token");
    EXPECT_EQ(tokens.at("agent-experiments"), "experiments-token");
    EXPECT_TRUE(lake::control::read_agent_bus_tokens("").empty());
    std::filesystem::remove_all(dir);
}
