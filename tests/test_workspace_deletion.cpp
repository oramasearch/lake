// Deleting a workspace cascades (decision 54 S12, lake#514, docs/api/workspaces.md#deletion): its
// repositories (live and tombstoned inside retention, retention cut short), apps, unfinished builds
// and tenant images go with it, the confirmation names them first, a deletion interrupted after its
// transaction resumes, only an administrator of the space may delete, and the central audit keeps
// one event per destroyed object. Every case runs on the memory store and, with LAKE_TEST_DB_URI,
// on PostgreSQL.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "audit_test_support.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/workspace_deletion.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

namespace {

using json = nlohmann::json;
using lake::api::ApiResult;

constexpr const char* memory_kek =
    "6c616b652d666f7267652d7265636f7264732d746573742d696e7374616e6365";

// The instance registry's tenant images as a map of repository to tags, with a switch that makes
// removal fail (a crash or an unreachable volume between the transaction and the cleanup).
struct FakeRegistry {
    std::map<std::string, std::vector<std::string>> tags;
    bool fail_removal = false;
    std::vector<std::string> dropped_indexes;
    lake::api::WorkspaceContent content() {
        return {
            .image_tags = [this](const std::string& repository) {
                const auto found = tags.find(repository);
                return found == tags.end() ? std::vector<std::string>{} : found->second;
            },
            .remove_image_repository = [this](const std::string& repository)
                -> std::expected<void, std::string> {
                if (fail_removal) {
                    return std::unexpected("the registry volume is unavailable");
                }
                tags.erase(repository);
                return {};
            },
            .drop_repository_index = [this](const std::string&, const std::string& repository)
                -> std::expected<void, std::string> {
                dropped_indexes.push_back(repository);
                return {};
            },
        };
    }
};

// Seconds since the epoch of the stores' timestamps: ISO 8601 (memory) or PostgreSQL's text form
// in the session's time zone.
std::time_t unix_seconds(const std::string& value) {
    std::tm parts{};
    EXPECT_EQ(std::sscanf(value.substr(0, 19).c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d", &parts.tm_year,
                          &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min,
                          &parts.tm_sec),
              6)
        << value;
    parts.tm_year -= 1900;
    parts.tm_mon -= 1;
    std::time_t seconds = timegm(&parts);
    std::size_t zone = 19;
    while (zone < value.size() && (value[zone] == '.' || std::isdigit(static_cast<unsigned char>(value[zone])))) {
        ++zone;
    }
    if (zone < value.size() && (value[zone] == '+' || value[zone] == '-')) {
        int hours = 0;
        int minutes = 0;
        std::sscanf(value.c_str() + zone + 1, "%2d:%2d", &hours, &minutes);
        seconds -= (value[zone] == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
    }
    return seconds;
}

class WorkspaceDeletion : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        const char* kek = std::getenv("LAKE_INSTANCE_KEK");
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (!uri || !kek) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI and LAKE_INSTANCE_KEK are not configured";
            }
            store = lake::control::make_postgres_store(uri, kek);
            context.instance_kek = kek;
        } else {
            store = std::make_unique<lake::control::InMemoryStore>();
            context.instance_kek = memory_kek;
        }
        lake::api::register_core_capabilities(registry, "test");
        content = images.content();
        context.store = store.get();
        context.workspace_content = &content;
        as_operator();
    }

    void as_operator() {
        context.authorized = true;
        context.operator_credential = true;
        context.session_credential = false;
        context.acting_identity = "admin";
        context.acting_user_id.clear();
        context.credential_device_id.clear();
        context.scopes.clear();
    }

    void as(const lake::control::Principal& principal) {
        context.authorized = true;
        context.operator_credential = false;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_device_id = principal.device_id;
        context.scopes = principal.scopes;
        context.authorization_cache = {};
    }

    ApiResult call(const std::string& name, json input) {
        context.request_id.clear();
        context.workspace_id.clear();
        context.audit_workspace_id.clear();
        return registry.call(name, input, context);
    }

    std::string space(const std::string& name) {
        const auto user = store->create_user(
            {"taap:1:ed25519:" + name, name + "@example.test", name});
        EXPECT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        EXPECT_TRUE(customer) << customer.error().message;
        return customer->id;
    }

    std::string workspace(const std::string& customer_id, const std::string& name) {
        const auto created = store->create_workspace({customer_id, name, "dev", ""});
        EXPECT_TRUE(created) << created.error().message;
        return created->id;
    }

    lake::control::Repository repository(const std::string& customer_id,
                                         const std::string& workspace_id,
                                         const std::string& name) {
        const auto created =
            store->create_repository({customer_id, name, "", "private", workspace_id});
        EXPECT_TRUE(created) << created.error().message;
        return *created;
    }

    // A repository deleted earlier, its tombstone retained until `retention_until`.
    lake::control::Repository tombstoned(const std::string& customer_id,
                                         const std::string& workspace_id,
                                         const std::string& name,
                                         const std::string& retention_until) {
        const lake::control::Repository created = repository(customer_id, workspace_id, name);
        const auto deleted =
            store->delete_repository(created.id, {"2026-01-01T00:00:00Z", retention_until});
        EXPECT_TRUE(deleted) << deleted.error().message;
        return *deleted;
    }

    lake::control::WebApp app(const std::string& customer_id,
                              const std::string& workspace_id,
                              const std::string& name,
                              const std::string& space_name) {
        const auto created = store->create_web_app(
            {.customer_id = customer_id,
             .workspace_id = workspace_id,
             .name = name,
             .mode = "image",
             .image = "lake.example.test/" + space_name + "/" + name + ":v1",
             .framework = "container",
             .port = 8080,
             .host = name + "." + space_name + ".apps.example.test"});
        EXPECT_TRUE(created) << created.error().message;
        return *created;
    }

    lake::control::Build build(const std::string& customer_id,
                               const std::string& workspace_id,
                               const std::string& repository_id,
                               const std::string& target) {
        const auto created = store->create_build({.customer_id = customer_id,
                                                  .workspace_id = workspace_id,
                                                  .repository = repository_id,
                                                  .revision = "main",
                                                  .target = target});
        EXPECT_TRUE(created) << created.error().message;
        return *created;
    }

    std::vector<lake::audit::Event> deletion_events(const std::string& workspace_id) {
        const auto events = lake::test::audit_events(
            *store, {.limit = 100,
                     .request_id = lake::api::workspace_deletion_request_id(workspace_id)});
        EXPECT_TRUE(events) << events.error().message;
        return events ? *events : std::vector<lake::audit::Event>{};
    }

    std::unique_ptr<lake::control::Store> store;
    lake::api::Registry registry;
    lake::api::ApiContext context;
    FakeRegistry images;
    lake::api::WorkspaceContent content;
};

std::multiset<std::string> objects_of(const std::vector<lake::audit::Event>& events) {
    std::multiset<std::string> objects;
    for (const lake::audit::Event& event : events) {
        objects.insert(event.draft.object);
    }
    return objects;
}

// Two spaces with two workspaces each. Deleting one workspace destroys its live and tombstoned
// repositories, its app, its unfinished builds and its images, after a confirmation that counts
// them, and touches nothing of the other workspace or the other space.
TEST_P(WorkspaceDeletion, CascadesToEverythingTheWorkspaceHoldsAndNothingElse) {
    const std::string ada = space("ada");
    const std::string grace = space("grace");
    const std::string research = workspace(ada, "research");
    const std::string product = workspace(ada, "product");
    const std::string lab = workspace(grace, "lab");
    const std::string ops = workspace(grace, "ops");

    const auto notes = repository(ada, research, "notes");
    const auto old = tombstoned(ada, research, "old", "2099-01-01T00:00:00Z");
    const auto ancient = tombstoned(ada, research, "ancient", "2026-01-02T00:00:00Z");
    const auto service = repository(ada, product, "service");
    const auto grace_notes = repository(grace, lab, "notes");
    const auto grace_ops = repository(grace, ops, "tools");

    const auto site = app(ada, research, "site", "ada");
    const auto shop = app(ada, product, "shop", "ada");
    const auto grace_site = app(grace, lab, "site", "grace");
    const auto worker = build(ada, research, notes.id, "lake.internal/ada/worker:v1");
    const auto research_common = build(ada, research, notes.id, "lake.internal/ada/common:v1");
    // The product workspace builds an image of the same name: it stays.
    const auto product_common = build(ada, product, service.id, "lake.internal/ada/common:v2");

    images.tags = {{"ada/site", {"build-1"}},
                   {"ada/cache/site", {"cache"}},
                   {"ada/worker", {"v1"}},
                   {"ada/common", {"v1", "v2"}},
                   {"ada/shop", {"v1"}},
                   {"grace/site", {"v1"}}};

    // The confirmation: a dry run counts everything and deletes nothing.
    const ApiResult plan = call("delete_workspace", {{"customer_id", ada},
                                                     {"workspace_id", research},
                                                     {"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_TRUE(plan.value["dry_run"]);
    EXPECT_EQ(plan.value["workspace_name"], "research");
    EXPECT_EQ(plan.value["counts"]["repositories"], 2);
    EXPECT_EQ(plan.value["counts"]["live_repositories"], 1);
    EXPECT_EQ(plan.value["counts"]["tombstoned_repositories"], 1);
    EXPECT_EQ(plan.value["counts"]["apps"], 1);
    EXPECT_EQ(plan.value["counts"]["builds"], 2);
    EXPECT_EQ(plan.value["counts"]["images"], 3) << plan.value["images"];
    std::set<std::string> planned_images;
    for (const json& image : plan.value["images"]) {
        planned_images.insert(image["name"]);
    }
    EXPECT_EQ(planned_images, (std::set<std::string>{"ada/cache/site", "ada/site", "ada/worker"}));
    EXPECT_TRUE(store->get_repository(notes.id)->deleted_at.empty());
    EXPECT_EQ(store->get_workspace(research)->desired, "running");
    // A dry run with a wrong name is refused too; a deletion needs the name.
    EXPECT_EQ(call("delete_workspace", {{"customer_id", ada}, {"workspace_id", research},
                                        {"dry_run", true}, {"confirm", "product"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(call("delete_workspace", {{"customer_id", ada}, {"workspace_id", research}})
                  .error.path,
              "/confirm");
    EXPECT_EQ(call("delete_workspace", {{"customer_id", ada}, {"workspace_id", research},
                                        {"confirm", "Research"}})
                  .error.code,
              "invalid_input");

    const ApiResult deleted = call("delete_workspace", {{"customer_id", ada},
                                                        {"workspace_id", research},
                                                        {"confirm", "research"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["desired"], "deleted");
    EXPECT_TRUE(deleted.value["deletion"]["completed"]) << deleted.value;
    EXPECT_EQ(deleted.value["deletion"]["counts"], plan.value["counts"]);

    // Each repository is tombstoned with its retention ended, and its name is free again.
    const std::time_t now = std::time(nullptr);
    for (const auto& destroyed : {notes, old}) {
        const auto row = store->get_repository(destroyed.id);
        ASSERT_TRUE(row);
        EXPECT_FALSE(row->deleted_at.empty()) << destroyed.name;
        EXPECT_LE(unix_seconds(row->retention_until), now) << destroyed.name;
    }
    // An earlier tombstone keeps when it was deleted; an expired one is left as it was.
    EXPECT_EQ(unix_seconds(store->get_repository(old.id)->deleted_at),
              unix_seconds("2026-01-01T00:00:00Z"));
    EXPECT_EQ(unix_seconds(store->get_repository(ancient.id)->retention_until),
              unix_seconds("2026-01-02T00:00:00Z"));
    const auto reused = store->create_repository({ada, "notes", "", "private", product});
    EXPECT_TRUE(reused) << reused.error().message;
    // The deleted workspace takes no new repository.
    EXPECT_FALSE(store->create_repository({ada, "late", "", "private", research}));
    EXPECT_EQ(images.dropped_indexes.size(), 2u);

    // Its app is deleted and its unfinished builds cancelled.
    EXPECT_EQ(store->get_web_app(site.id)->desired, "deleted");
    EXPECT_EQ(store->get_build(worker.id)->desired, "cancelled");
    EXPECT_EQ(store->get_build(research_common.id)->desired, "cancelled");

    // Its images are no longer tagged; a name another workspace builds stays.
    EXPECT_FALSE(images.tags.contains("ada/site"));
    EXPECT_FALSE(images.tags.contains("ada/cache/site"));
    EXPECT_FALSE(images.tags.contains("ada/worker"));
    EXPECT_TRUE(images.tags.contains("ada/common"));
    EXPECT_TRUE(images.tags.contains("ada/shop"));
    EXPECT_TRUE(images.tags.contains("grace/site"));

    // The other workspace of the space and the other space are untouched.
    EXPECT_EQ(store->get_workspace(product)->desired, "running");
    EXPECT_TRUE(store->get_repository(service.id)->deleted_at.empty());
    EXPECT_EQ(store->get_web_app(shop.id)->desired, "running");
    EXPECT_EQ(store->get_build(product_common.id)->desired, "queued");
    for (const std::string& id : {lab, ops}) {
        EXPECT_EQ(store->get_workspace(id)->desired, "running");
    }
    EXPECT_TRUE(store->get_repository(grace_notes.id)->deleted_at.empty());
    EXPECT_TRUE(store->get_repository(grace_ops.id)->deleted_at.empty());
    EXPECT_EQ(store->get_web_app(grace_site.id)->desired, "running");

    // The central audit keeps one event per destroyed object and one for the cascade.
    const auto events = deletion_events(research);
    EXPECT_EQ(objects_of(events),
              (std::multiset<std::string>{"repository/" + notes.id, "repository/" + old.id,
                                          "app/" + site.id, "build/" + worker.id,
                                          "build/" + research_common.id, "image/ada/cache/site",
                                          "image/ada/site", "image/ada/worker",
                                          "workspace/" + research}));
    for (const lake::audit::Event& event : events) {
        EXPECT_EQ(event.draft.customer_id, ada);
        EXPECT_EQ(event.draft.principal, "admin");
        EXPECT_EQ(event.draft.action, "delete_workspace");
        EXPECT_EQ(event.draft.details["workspace_id"], research);
    }
    const auto capability_events = lake::test::audit_events(*store, {.object = "workspace/" + research});
    ASSERT_TRUE(capability_events);
    EXPECT_TRUE(std::ranges::any_of(*capability_events, [](const lake::audit::Event& event) {
        return event.draft.kind == "workspace.delete" && event.draft.outcome == "success";
    }));

    // Repeating the deletion answers the recorded one and records nothing twice.
    const ApiResult again = call("delete_workspace", {{"customer_id", ada},
                                                      {"workspace_id", research},
                                                      {"confirm", "research"}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["deletion"]["counts"], deleted.value["deletion"]["counts"]);
    EXPECT_EQ(deletion_events(research).size(), events.size());
    const ApiResult after = call("delete_workspace", {{"customer_id", ada},
                                                      {"workspace_id", research},
                                                      {"dry_run", true}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_TRUE(after.value["completed"]);
}

// The transaction commits before what lives outside PostgreSQL is removed. When that part fails
// (a crash, an unavailable volume), the workspace is deleted and the deletion stays pending; the
// owner loop resumes it, and no object is destroyed or audited twice.
TEST_P(WorkspaceDeletion, AnInterruptedCascadeResumes) {
    const std::string ada = space("ada");
    const std::string research = workspace(ada, "research");
    const std::string product = workspace(ada, "product");
    const auto notes = repository(ada, research, "notes");
    const auto old = tombstoned(ada, research, "old", "2099-01-01T00:00:00Z");
    const auto service = repository(ada, product, "service");
    const auto site = app(ada, research, "site", "ada");
    images.tags = {{"ada/site", {"v1"}}, {"ada/cache/site", {"cache"}}};
    images.fail_removal = true;

    const ApiResult deleted = call("delete_workspace", {{"customer_id", ada},
                                                        {"workspace_id", research},
                                                        {"confirm", "research"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["desired"], "deleted");
    EXPECT_FALSE(deleted.value["deletion"]["completed"]);
    EXPECT_NE(deleted.value["deletion"]["pending"].get<std::string>().find("unavailable"),
              std::string::npos);
    // The transaction's part is done: repositories tombstoned, app deleted, workspace deleted.
    EXPECT_FALSE(store->get_repository(notes.id)->deleted_at.empty());
    EXPECT_EQ(store->get_web_app(site.id)->desired, "deleted");
    EXPECT_TRUE(images.tags.contains("ada/site"));
    const auto pending = store->pending_workspace_deletions(10);
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending->size(), 1u);
    EXPECT_EQ(pending->front().workspace_id, research);
    EXPECT_EQ(pending->front().images.size(), 2u);
    const std::size_t before = deletion_events(research).size();
    EXPECT_EQ(before, 3u) << "the repositories and the app were audited before the images failed";

    // Still failing: the pass reports it and the deletion stays pending.
    EXPECT_FALSE(lake::api::resume_workspace_deletions(context));
    EXPECT_EQ(store->pending_workspace_deletions(10)->size(), 1u);
    EXPECT_EQ(deletion_events(research).size(), before);

    images.fail_removal = false;
    const auto resumed = lake::api::resume_workspace_deletions(context);
    ASSERT_TRUE(resumed) << resumed.error();
    EXPECT_EQ(*resumed, 1u);
    EXPECT_TRUE(store->pending_workspace_deletions(10)->empty());
    EXPECT_FALSE(images.tags.contains("ada/site"));
    EXPECT_FALSE(images.tags.contains("ada/cache/site"));
    EXPECT_FALSE(store->get_workspace_deletion(research)->completed_at.empty());
    EXPECT_EQ(objects_of(deletion_events(research)),
              (std::multiset<std::string>{"repository/" + notes.id, "repository/" + old.id,
                                          "app/" + site.id, "image/ada/cache/site",
                                          "image/ada/site", "workspace/" + research}));
    // A later pass has nothing to do.
    EXPECT_EQ(*lake::api::resume_workspace_deletions(context), 0u);
    EXPECT_TRUE(store->get_repository(service.id)->deleted_at.empty());
}

// Deleting a workspace needs admin on its space: a member who may write is refused, as is a
// stranger and the administrator of another space, and nothing is destroyed or counted for them.
TEST_P(WorkspaceDeletion, OnlyAnAdministratorOfTheSpaceDeletes) {
    // People sign in with their TAAP identities; each holds a role on a space's policy object.
    const auto person = [&](const std::string& name) {
        lake::control::Principal principal;
        principal.taap_id = "taap:1:ed25519:" + name;
        const auto user = store->create_user({principal.taap_id, name + "@example.test", name});
        EXPECT_TRUE(user) << user.error().message;
        principal.user_id = user->id;
        principal.scopes = {"*"};
        return principal;
    };
    const auto owner = person("owner");
    const auto writer = person("writer");
    const auto stranger = person("stranger");
    const auto customer = store->create_customer({"individual", owner.user_id, "", "owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto stranger_customer =
        store->create_customer({"individual", stranger.user_id, "", "stranger"});
    ASSERT_TRUE(stranger_customer) << stranger_customer.error().message;
    const auto grant = [&](const lake::control::Principal& principal,
                           const std::string& customer_id, const std::string& role) {
        const auto object = store->find_policy_object("customer", customer_id);
        ASSERT_TRUE(object) << object.error().message;
        ASSERT_TRUE(store->create_grant({principal.taap_id, object->id, role,
                                         role + "-attestation", "admin", role + "-grant"}));
    };
    grant(owner, customer->id, "admin");
    grant(writer, customer->id, "write");
    grant(stranger, stranger_customer->id, "admin");
    const std::string doomed = workspace(customer->id, "doomed");
    const auto notes = repository(customer->id, doomed, "notes");
    const json input{{"customer_id", customer->id}, {"workspace_id", doomed}, {"confirm", "doomed"}};
    json dry_run = input;
    dry_run["dry_run"] = true;

    for (const auto& refused : {writer, stranger}) {
        as(refused);
        EXPECT_EQ(call("delete_workspace", dry_run).error.code, "unauthorized");
        EXPECT_EQ(call("delete_workspace", input).error.code, "unauthorized");
    }
    // The stranger administers their own space, not this one; naming their space beside this
    // workspace is refused before any handler runs.
    as(stranger);
    json foreign = input;
    foreign["customer_id"] = stranger_customer->id;
    EXPECT_FALSE(call("delete_workspace", foreign).ok);
    EXPECT_EQ(store->get_workspace(doomed)->desired, "running");
    EXPECT_TRUE(store->get_repository(notes.id)->deleted_at.empty());
    EXPECT_TRUE(deletion_events(doomed).empty());

    as(owner);
    const ApiResult planned = call("delete_workspace", dry_run);
    ASSERT_TRUE(planned.ok) << planned.error.message;
    EXPECT_EQ(planned.value["counts"]["repositories"], 1);
    const ApiResult deleted = call("delete_workspace", input);
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_TRUE(deleted.value["deletion"]["completed"]);
    EXPECT_EQ(deleted.value["deletion"]["requested_by"], owner.taap_id);
    for (const lake::audit::Event& event : deletion_events(doomed)) {
        EXPECT_EQ(event.draft.principal, owner.taap_id);
    }
    EXPECT_EQ(deletion_events(doomed).size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(Stores,
                         WorkspaceDeletion,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

} // namespace
