#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/memory_store.hpp"

using nlohmann::json;
using namespace lake::api;

namespace {

struct Surface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext ctx;
    Surface() {
        register_core_capabilities(registry, "0.0.1");
        ctx.store = &store;
        ctx.node_name = "test";
        ctx.authorized = true;
        ctx.acting_identity = "admin";
    }
    ApiResult call(const std::string& name, json input = json::object()) { return registry.call(name, input, ctx); }
};

}  // namespace

TEST(Registry, ServerInfoNeedsNoAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("server_info");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value["name"], "lake");
    EXPECT_EQ(r.value["store"], "memory");
}

TEST(Registry, EverythingElseNeedsAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("list_users");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.code, "unauthorized");
    EXPECT_EQ(rest_status(r), 403);
}

TEST(Registry, ValidatesInput) {
    Surface s;
    ApiResult r = s.call("create_customer", json::object());
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(r.error.path, "/customer_type");
    r = s.call("get_user", {{"id", "x"}, {"bogus", 1}});
    EXPECT_EQ(r.error.path, "/bogus");
    r = s.call("list_users", {{"limit", "ten"}});
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(s.call("no_such").error.code, "unknown_capability");
}

TEST(Customers, IndividualLifecycleWithGeneratedHandle) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"email", "Ada@Example.com"}, {"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const std::string user_id = user.value["id"];
    EXPECT_EQ(user_id.size(), 36u);

    const ApiResult customer = s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string name = customer.value["name"];
    EXPECT_TRUE(lake::control::valid_handle(name)) << name;
    EXPECT_EQ(customer.value["user_id"], user_id);
    EXPECT_TRUE(customer.value["organization_id"].is_null());

    const ApiResult again = s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    EXPECT_EQ(again.error.code, "conflict");  // one customer per user
    EXPECT_EQ(rest_status(again), 409);

    const ApiResult dup_email = s.call("create_user", {{"email", "ada@example.com"}});
    EXPECT_EQ(dup_email.error.code, "conflict");  // case-insensitive email

    const ApiResult listed = s.call("list_customers");
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["items"].size(), 1u);
    const ApiResult got = s.call("get_customer", {{"id", customer.value["id"]}});
    ASSERT_TRUE(got.ok);
    EXPECT_EQ(got.value["name"], name);
    EXPECT_EQ(s.call("get_customer", {{"id", "missing"}}).error.code, "not_found");
}

TEST(Customers, OrganizationAndTheTenantRules) {
    Surface s;
    const ApiResult org = s.call("create_organization", {{"display_name", "Orama"}, {"taap_id", "taap:1:ed25519:abc"}});
    ASSERT_TRUE(org.ok);
    const std::string org_id = org.value["id"];
    EXPECT_EQ(s.call("create_organization", {{"taap_id", "taap:1:ed25519:abc"}}).error.code, "conflict");

    // xor: an organization customer must not carry a user_id; type must match the id given.
    ApiResult bad = s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", org_id}, {"user_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad = s.call("create_customer", {{"customer_type", "individual"}, {"organization_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad = s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", "nope"}});
    EXPECT_EQ(bad.error.code, "invalid_input");  // FK

    // Handle format and the single global namespace.
    bad = s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "Orama Inc"}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    const ApiResult ok = s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "orama"}});
    ASSERT_TRUE(ok.ok) << ok.error.message;
    const ApiResult user = s.call("create_user", {{"display_name", "Bob"}});
    const ApiResult taken = s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "orama"}});
    EXPECT_EQ(taken.error.code, "conflict");
}

TEST(Rest, PathsMethodsAndQueryCoercion) {
    Surface s;
    const auto list = s.registry.find("list_users");
    const auto create = s.registry.find("create_user");
    EXPECT_EQ(rest_path(*list), "/v1/list_users");
    EXPECT_EQ(rest_method(*list), "GET");
    EXPECT_EQ(rest_method(*create), "POST");
    const json input = rest_input_from_query(*list, {{"limit", "5"}, {"other", "x"}});
    EXPECT_EQ(input["limit"], 5);
    EXPECT_EQ(input["other"], "x");
    const json envelope = rest_envelope(ApiResult::failure("not_found", "gone", "/id"));
    EXPECT_FALSE(envelope["ok"]);
    EXPECT_EQ(envelope["error"]["code"], "not_found");
    EXPECT_EQ(rest_status(ApiResult::failure("store_unavailable", "")), 503);
}

TEST(Rest, OpenApiCoversEveryCapability) {
    Surface s;
    const json doc = openapi_document(s.registry, "0.0.1");
    EXPECT_EQ(doc["openapi"], "3.1.0");
    for (const auto& capability : s.registry.list()) {
        const std::string method = rest_method(*capability) == "GET" ? "get" : "post";
        ASSERT_TRUE(doc["paths"].contains(rest_path(*capability))) << capability->name;
        EXPECT_TRUE(doc["paths"][rest_path(*capability)].contains(method)) << capability->name;
    }
    EXPECT_TRUE(doc["paths"].contains("/mcp"));
    EXPECT_TRUE(doc["paths"]["/v1/server_info"]["get"]["security"].empty());
}

TEST(Mcp, InitializeListAndCall) {
    Surface s;
    const auto init = mcp_dispatch(s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, s.ctx, "0.0.1");
    ASSERT_TRUE(init);
    EXPECT_EQ(json::parse(*init)["result"]["serverInfo"]["name"], "lake");
    EXPECT_FALSE(mcp_dispatch(s.registry, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, s.ctx, "0.0.1"));

    const auto tools = json::parse(*mcp_dispatch(s.registry, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, s.ctx, "0.0.1"));
    EXPECT_EQ(tools["result"]["tools"].size(), s.registry.list().size());

    const json call = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                       {"params", {{"name", "create_user"}, {"arguments", {{"display_name", "Eve"}}}}}};
    const auto reply = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_FALSE(reply["result"]["isError"]);
    EXPECT_EQ(reply["result"]["structuredContent"]["value"]["display_name"], "Eve");

    s.ctx.authorized = false;
    const auto denied = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_TRUE(denied["result"]["isError"]);
    EXPECT_EQ(denied["result"]["structuredContent"]["error"]["code"], "unauthorized");
    const auto unknown = json::parse(*mcp_dispatch(s.registry, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "nope"}}, s.ctx, "0.0.1"));
    EXPECT_EQ(unknown["error"]["code"], -32601);
}

TEST(Repositories, OwnedByACustomerNamedWithinIt) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const std::string cid = customer.value["id"];

    ApiResult repo = s.call("create_repository", {{"customer_id", cid}, {"name", "liquid"}, {"description", "the tool"}});
    ASSERT_TRUE(repo.ok) << repo.error.message;
    EXPECT_EQ(repo.value["visibility"], "private");
    EXPECT_EQ(repo.value["customer_id"], cid);

    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "liquid"}}).error.code, "conflict");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "Bad Name"}}).error.code, "invalid_input");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", "nope"}, {"name", "x"}}).error.code, "invalid_input");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "y"}, {"visibility", "secret"}}).error.code, "invalid_input");
    ASSERT_TRUE(s.call("create_repository", {{"customer_id", cid}, {"name", "lake"}, {"visibility", "public"}}).ok);

    const ApiResult all = s.call("list_repositories");
    ASSERT_TRUE(all.ok);
    EXPECT_EQ(all.value["items"].size(), 2u);
    EXPECT_EQ(all.value["items"][0]["name"], "lake");  // newest first
    const ApiResult mine = s.call("list_repositories", {{"customer_id", cid}, {"limit", 1}});
    EXPECT_EQ(mine.value["items"].size(), 1u);
    const ApiResult none = s.call("list_repositories", {{"customer_id", "other"}});
    EXPECT_EQ(none.value["items"].size(), 0u);
    EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["name"], "liquid");
    EXPECT_EQ(s.call("get_repository", {{"id", "missing"}}).error.code, "not_found");
}
