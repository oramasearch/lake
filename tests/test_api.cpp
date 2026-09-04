#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"

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
    ctx.instance_kek = "test-instance-kek";
    ctx.authorized = true;
    ctx.acting_identity = "admin";
  }
  ApiResult call(const std::string &name, json input = json::object()) {
    ctx.request_id.clear();
    return registry.call(name, input, ctx);
  }
};

std::string http_get(unsigned short port, std::string_view path,
                     std::string_view authorization = {}) {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0)
    return {};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
      ::connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    ::close(socket_fd);
    return {};
  }

  std::string request = "GET " + std::string(path) +
                        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
  if (!authorization.empty())
    request += "Authorization: " + std::string(authorization) + "\r\n";
  request += "\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t count =
        ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
    if (count <= 0) {
      ::close(socket_fd);
      return {};
    }
    sent += static_cast<std::size_t>(count);
  }

  std::string response;
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
    if (count <= 0)
      break;
    response.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(socket_fd);
  return response;
}

} // namespace

TEST(Registry, ServerInfoNeedsNoAuth) {
  Surface s;
  s.ctx.authorized = false;
  const ApiResult r = s.call("server_info");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.value["name"], "lake");
  EXPECT_EQ(r.value["store"], "memory");
}

TEST(Telemetry, PrometheusCounterAndHistogram) {
  lake::telemetry::Metrics metrics;
  metrics.increment_counter("test_requests_total", {{"result", "ok"}});
  metrics.increment_counter("test_requests_total", {{"result", "ok"}}, 2.0);
  metrics.observe_histogram("test_duration_seconds", {{"operation", "read"}},
                            0.012);

  const std::string text = metrics.prometheus_text();
  EXPECT_NE(text.find("# TYPE test_requests_total counter\n"),
            std::string::npos);
  EXPECT_NE(text.find("test_requests_total{result=\"ok\"} 3\n"),
            std::string::npos);
  EXPECT_NE(text.find("# TYPE test_duration_seconds histogram\n"),
            std::string::npos);
  EXPECT_NE(
      text.find(
          "test_duration_seconds_bucket{operation=\"read\",le=\"0.01\"} 0\n"),
      std::string::npos);
  EXPECT_NE(
      text.find(
          "test_duration_seconds_bucket{operation=\"read\",le=\"0.025\"} 1\n"),
      std::string::npos);
  EXPECT_NE(
      text.find(
          "test_duration_seconds_bucket{operation=\"read\",le=\"+Inf\"} 1\n"),
      std::string::npos);
  EXPECT_NE(text.find("test_duration_seconds_count{operation=\"read\"} 1\n"),
            std::string::npos);
}

TEST(Telemetry, StorePingIsMeasured) {
  lake::telemetry::Metrics metrics;
  auto store = lake::telemetry::instrument_store(
      std::make_shared<lake::control::InMemoryStore>(), metrics);
  ASSERT_TRUE(store->ping());

  const std::string text = metrics.prometheus_text();
  EXPECT_NE(text.find("lake_store_calls_total{store=\"memory\",operation="
                      "\"ping\",result=\"ok\"} 1\n"),
            std::string::npos);
  EXPECT_NE(text.find("lake_store_call_duration_seconds_count{store=\"memory\","
                      "operation=\"ping\",result=\"ok\"} 1\n"),
            std::string::npos);
}

TEST(TelemetryHttp, HealthAndMemoryReadinessNeedNoAuth) {
  ServerOptions options;
  options.port = 0;
  options.node_name = "test-node";
  options.store = std::make_shared<lake::control::InMemoryStore>();
  ApiServer server(options);
  ASSERT_TRUE(server.start()) << server.reason();

  const std::string health = http_get(server.port(), "/healthz");
  EXPECT_NE(health.find(" 200 "), std::string::npos);
  EXPECT_TRUE(health.ends_with("{\"ok\":true}"));

  const std::string ready = http_get(server.port(), "/readyz");
  EXPECT_NE(ready.find(" 200 "), std::string::npos);
  EXPECT_TRUE(ready.ends_with("{\"ok\":true,\"store\":\"memory\"}"));
  server.stop();
}

TEST(TelemetryHttp, MetricsRequireExistingAuthAndUsePrometheusContentType) {
  ServerOptions options;
  options.port = 0;
  options.node_name = "test-node";
  options.admin_token = "test-token";
  options.store = std::make_shared<lake::control::InMemoryStore>();
  ApiServer server(options);
  ASSERT_TRUE(server.start()) << server.reason();

  const std::string denied = http_get(server.port(), "/metrics");
  EXPECT_NE(denied.find(" 403 "), std::string::npos);
  EXPECT_NE(denied.find("\"code\":\"unauthorized\""), std::string::npos);

  const std::string served =
      http_get(server.port(), "/metrics", "Bearer test-token");
  EXPECT_NE(served.find(" 200 "), std::string::npos);
  EXPECT_NE(served.find("Content-Type: text/plain; version=0.0.4"),
            std::string::npos);
  EXPECT_NE(
      served.find("lake_build_info{version=\"0.0.1\",node=\"test-node\"} 1\n"),
      std::string::npos);
  EXPECT_NE(served.find("lake_up 1\n"), std::string::npos);
  EXPECT_NE(served.find("lake_http_requests_total{method=\"GET\",route=\"/"
                        "metrics\",status=\"403\"} 1\n"),
            std::string::npos);
  server.stop();
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

TEST(Audit, RegistryRecordsEachCapabilityOutcomeAndDetectsDeletedRows) {
  Surface s;
  for (const auto &capability : s.registry.list()) {
    s.call(capability->name);
  }
  auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
  ASSERT_TRUE(events);
  ASSERT_EQ(events->size(), s.registry.list().size());
  for (const auto &capability : s.registry.list()) {
    const auto count = std::count_if(
        events->begin(), events->end(), [&](const lake::audit::Event &event) {
          return event.draft.action == capability->name;
        });
    EXPECT_EQ(count, 1) << capability->name;
  }
  EXPECT_TRUE(lake::audit::verify(s.store, *events));

  std::vector<lake::audit::Event> deleted = *events;
  deleted.erase(deleted.begin() + 1);
  const auto verification = lake::audit::verify(s.store, deleted);
  EXPECT_FALSE(verification);
  EXPECT_EQ(verification.error().code, "invalid_chain");
}

TEST(Audit, DeniedAuthorizationAndJsonLinesExportAreRecorded) {
  Surface s;
  s.ctx.authorized = false;
  const ApiResult denied = s.call("list_users");
  EXPECT_FALSE(denied.ok);
  EXPECT_EQ(denied.error.code, "unauthorized");

  s.ctx.authorized = true;
  const ApiResult exported = s.call("export_audit");
  ASSERT_TRUE(exported.ok) << exported.error.message;
  std::istringstream lines(exported.value["jsonl"].get<std::string>());
  std::string line;
  std::vector<json> decoded;
  while (std::getline(lines, line)) {
    if (!line.empty()) {
      decoded.push_back(json::parse(line));
    }
  }
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0]["outcome"], "denied");
  EXPECT_EQ(decoded[0]["action"], "list_users");

  auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
  ASSERT_TRUE(events);
  ASSERT_EQ(events->size(), 2u);
  EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(Audit, IdentityRotatesAndRetentionFloorIsEnforced) {
  Surface s;
  const ApiResult initial = s.call("identity");
  ASSERT_TRUE(initial.ok) << initial.error.message;
  const std::string initial_identity = initial.value["taap_id"];
  const ApiResult rotated = s.call("rotate_identity");
  ASSERT_TRUE(rotated.ok) << rotated.error.message;
  EXPECT_NE(rotated.value["taap_id"], initial_identity);

  lake::control::InMemoryStore externally_seeded_store;
  const lake::audit::IdentityOptions externally_seeded_options{
      .configured_seed = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
      .instance_kek = "test-instance-kek",
  };
  auto externally_seeded =
      lake::audit::identity(externally_seeded_store, externally_seeded_options);
  ASSERT_TRUE(externally_seeded) << externally_seeded.error().message;
  auto externally_rotated = lake::audit::rotate_identity(
      externally_seeded_store, externally_seeded_options);
  ASSERT_TRUE(externally_rotated) << externally_rotated.error().message;
  lake::audit::EventDraft externally_seeded_event{
      .kind = "administrative",
      .principal = "admin",
      .object = "lake",
      .action = "verify_external_rotation",
      .outcome = "success",
      .request_id = lake::control::new_uuid_v4(),
  };
  auto recorded =
      lake::audit::record(externally_seeded_store, externally_seeded_event,
                          externally_seeded_options);
  ASSERT_TRUE(recorded) << recorded.error().message;
  auto externally_seeded_events = externally_seeded_store.list_audit_events(
      lake::audit::Filter{.limit = 1000});
  ASSERT_TRUE(externally_seeded_events);
  EXPECT_TRUE(
      lake::audit::verify(externally_seeded_store, *externally_seeded_events));

  const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
  ASSERT_TRUE(user.ok) << user.error.message;
  const ApiResult customer =
      s.call("create_customer", {{"customer_type", "individual"},
                                 {"user_id", user.value["id"]},
                                 {"name", "ada"}});
  ASSERT_TRUE(customer.ok) << customer.error.message;
  const ApiResult accepted =
      s.call("set_audit_retention",
             {{"customer_id", customer.value["id"]}, {"days", 500}});
  EXPECT_TRUE(accepted.ok);
  const ApiResult lowered =
      s.call("set_audit_retention",
             {{"customer_id", customer.value["id"]}, {"days", 400}});
  EXPECT_FALSE(lowered.ok);
  EXPECT_EQ(lowered.error.code, "conflict");
  const ApiResult refused =
      s.call("set_audit_retention",
             {{"customer_id", customer.value["id"]}, {"days", 399}});
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(refused.error.code, "invalid_input");

  auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
  ASSERT_TRUE(events);
  const auto retention = std::find_if(
      events->rbegin(), events->rend(), [](const lake::audit::Event &event) {
        return event.draft.kind == "retention" &&
               event.draft.outcome == "failure";
      });
  ASSERT_NE(retention, events->rend());
  EXPECT_EQ(retention->draft.action, "set_audit_retention");
  EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(PostgresAudit, RegistryEventsPersistAndVerify) {
  const char *database_uri = std::getenv("LAKE_TEST_DB_URI");
  if (database_uri == nullptr || *database_uri == '\0') {
    GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
  }
  auto store = lake::control::make_postgres_store(database_uri);
  const auto ready = store->ping();
  ASSERT_TRUE(ready) << ready.error().message;
  const auto before =
      store->list_audit_events(lake::audit::Filter{.limit = 1000000});
  ASSERT_TRUE(before) << before.error().message;
  Registry registry;
  register_core_capabilities(registry, "0.0.1");
  ApiContext context;
  context.store = store.get();
  context.node_name = "postgres-test";
  context.instance_kek = "postgres-test-instance-kek";
  context.authorized = true;
  context.acting_identity = "admin";

  for (const auto &capability : registry.list()) {
    context.request_id.clear();
    registry.call(capability->name, json::object(), context);
  }
  context.authorized = false;
  context.request_id.clear();
  const ApiResult result = registry.call("list_users", json::object(), context);
  EXPECT_EQ(result.error.code, "unauthorized");

  auto events = store->list_audit_events(lake::audit::Filter{.limit = 1000000});
  ASSERT_TRUE(events) << events.error().message;
  ASSERT_EQ(events->size(), before->size() + registry.list().size() + 1);
  EXPECT_EQ(events->back().draft.outcome, "denied");
  const auto verification = lake::audit::verify(*store, *events);
  EXPECT_TRUE(verification) << verification.error().message;
}

TEST(Customers, IndividualLifecycleWithGeneratedHandle) {
  Surface s;
  const ApiResult user = s.call(
      "create_user", {{"email", "Ada@Example.com"}, {"display_name", "Ada"}});
  ASSERT_TRUE(user.ok) << user.error.message;
  const std::string user_id = user.value["id"];
  EXPECT_EQ(user_id.size(), 36u);

  const ApiResult customer =
      s.call("create_customer",
             {{"customer_type", "individual"}, {"user_id", user_id}});
  ASSERT_TRUE(customer.ok) << customer.error.message;
  const std::string name = customer.value["name"];
  EXPECT_TRUE(lake::control::valid_handle(name)) << name;
  EXPECT_EQ(customer.value["user_id"], user_id);
  EXPECT_TRUE(customer.value["organization_id"].is_null());

  const ApiResult again =
      s.call("create_customer",
             {{"customer_type", "individual"}, {"user_id", user_id}});
  EXPECT_EQ(again.error.code, "conflict"); // one customer per user
  EXPECT_EQ(rest_status(again), 409);

  const ApiResult dup_email =
      s.call("create_user", {{"email", "ada@example.com"}});
  EXPECT_EQ(dup_email.error.code, "conflict"); // case-insensitive email

  const ApiResult listed = s.call("list_customers");
  ASSERT_TRUE(listed.ok);
  EXPECT_EQ(listed.value["items"].size(), 1u);
  const ApiResult got = s.call("get_customer", {{"id", customer.value["id"]}});
  ASSERT_TRUE(got.ok);
  EXPECT_EQ(got.value["name"], name);
  EXPECT_EQ(s.call("get_customer", {{"id", "missing"}}).error.code,
            "not_found");
}
TEST(Customers, OrganizationAndTheTenantRules) {
  Surface s;
  const ApiResult org =
      s.call("create_organization",
             {{"display_name", "Orama"}, {"taap_id", "taap:1:ed25519:abc"}});
  ASSERT_TRUE(org.ok);
  const std::string org_id = org.value["id"];
  EXPECT_EQ(s.call("create_organization", {{"taap_id", "taap:1:ed25519:abc"}})
                .error.code,
            "conflict");

  // xor: an organization customer must not carry a user_id; type must match the
  // id given.
  ApiResult bad = s.call("create_customer", {{"customer_type", "organization"},
                                             {"organization_id", org_id},
                                             {"user_id", org_id}});
  EXPECT_EQ(bad.error.code, "invalid_input");
  bad = s.call("create_customer",
               {{"customer_type", "individual"}, {"organization_id", org_id}});
  EXPECT_EQ(bad.error.code, "invalid_input");
  bad = s.call("create_customer", {{"customer_type", "organization"},
                                   {"organization_id", "nope"}});
  EXPECT_EQ(bad.error.code, "invalid_input"); // FK

  // Handle format and the single global namespace.
  bad = s.call("create_customer", {{"customer_type", "organization"},
                                   {"organization_id", org_id},
                                   {"name", "Orama Inc"}});
  EXPECT_EQ(bad.error.code, "invalid_input");
  const ApiResult ok =
      s.call("create_customer", {{"customer_type", "organization"},
                                 {"organization_id", org_id},
                                 {"name", "orama"}});
  ASSERT_TRUE(ok.ok) << ok.error.message;
  const ApiResult user = s.call("create_user", {{"display_name", "Bob"}});
  const ApiResult taken =
      s.call("create_customer", {{"customer_type", "individual"},
                                 {"user_id", user.value["id"]},
                                 {"name", "orama"}});
  EXPECT_EQ(taken.error.code, "conflict");
}

TEST(Rest, PathsMethodsAndQueryCoercion) {
  Surface s;
  const auto list = s.registry.find("list_users");
  const auto create = s.registry.find("create_user");
  EXPECT_EQ(rest_path(*list), "/v1/list_users");
  EXPECT_EQ(rest_method(*list), "GET");
  EXPECT_EQ(rest_method(*create), "POST");
  const json input =
      rest_input_from_query(*list, {{"limit", "5"}, {"other", "x"}});
  EXPECT_EQ(input["limit"], 5);
  EXPECT_EQ(input["other"], "x");
  const json envelope =
      rest_envelope(ApiResult::failure("not_found", "gone", "/id"));
  EXPECT_FALSE(envelope["ok"]);
  EXPECT_EQ(envelope["error"]["code"], "not_found");
  EXPECT_EQ(rest_status(ApiResult::failure("store_unavailable", "")), 503);
}

TEST(Rest, OpenApiCoversEveryCapability) {
  Surface s;
  const json doc = openapi_document(s.registry, "0.0.1");
  EXPECT_EQ(doc["openapi"], "3.1.0");
  for (const auto &capability : s.registry.list()) {
    const std::string method =
        rest_method(*capability) == "GET" ? "get" : "post";
    ASSERT_TRUE(doc["paths"].contains(rest_path(*capability)))
        << capability->name;
    EXPECT_TRUE(doc["paths"][rest_path(*capability)].contains(method))
        << capability->name;
  }
  EXPECT_TRUE(doc["paths"].contains("/mcp"));
  EXPECT_TRUE(doc["paths"]["/v1/server_info"]["get"]["security"].empty());
}

TEST(Mcp, InitializeListAndCall) {
  Surface s;
  const auto init = mcp_dispatch(
      s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}},
      s.ctx, "0.0.1");
  ASSERT_TRUE(init);
  EXPECT_EQ(json::parse(*init)["result"]["serverInfo"]["name"], "lake");
  EXPECT_FALSE(mcp_dispatch(
      s.registry, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
      s.ctx, "0.0.1"));

  const auto tools = json::parse(*mcp_dispatch(
      s.registry, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
      s.ctx, "0.0.1"));
  EXPECT_EQ(tools["result"]["tools"].size(), s.registry.list().size());

  const json call = {
      {"jsonrpc", "2.0"},
      {"id", 3},
      {"method", "tools/call"},
      {"params",
       {{"name", "create_user"}, {"arguments", {{"display_name", "Eve"}}}}}};
  const auto reply =
      json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
  EXPECT_FALSE(reply["result"]["isError"]);
  EXPECT_EQ(reply["result"]["structuredContent"]["value"]["display_name"],
            "Eve");

  s.ctx.authorized = false;
  const auto denied =
      json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
  EXPECT_TRUE(denied["result"]["isError"]);
  EXPECT_EQ(denied["result"]["structuredContent"]["error"]["code"],
            "unauthorized");
  const auto unknown = json::parse(*mcp_dispatch(
      s.registry, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "nope"}}, s.ctx,
      "0.0.1"));
  EXPECT_EQ(unknown["error"]["code"], -32601);
}

TEST(Repositories, OwnedByACustomerNamedWithinIt) {
  Surface s;
  const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
  const ApiResult customer =
      s.call("create_customer", {{"customer_type", "individual"},
                                 {"user_id", user.value["id"]},
                                 {"name", "ada"}});
  ASSERT_TRUE(customer.ok);
  const std::string cid = customer.value["id"];

  ApiResult repo = s.call(
      "create_repository",
      {{"customer_id", cid}, {"name", "liquid"}, {"description", "the tool"}});
  ASSERT_TRUE(repo.ok) << repo.error.message;
  EXPECT_EQ(repo.value["visibility"], "private");
  EXPECT_EQ(repo.value["customer_id"], cid);

  EXPECT_EQ(
      s.call("create_repository", {{"customer_id", cid}, {"name", "liquid"}})
          .error.code,
      "conflict");
  EXPECT_EQ(
      s.call("create_repository", {{"customer_id", cid}, {"name", "Bad Name"}})
          .error.code,
      "invalid_input");
  EXPECT_EQ(
      s.call("create_repository", {{"customer_id", "nope"}, {"name", "x"}})
          .error.code,
      "invalid_input");
  EXPECT_EQ(
      s.call("create_repository",
             {{"customer_id", cid}, {"name", "y"}, {"visibility", "secret"}})
          .error.code,
      "invalid_input");
  ASSERT_TRUE(
      s.call("create_repository",
             {{"customer_id", cid}, {"name", "lake"}, {"visibility", "public"}})
          .ok);

  const ApiResult all = s.call("list_repositories");
  ASSERT_TRUE(all.ok);
  EXPECT_EQ(all.value["items"].size(), 2u);
  EXPECT_EQ(all.value["items"][0]["name"], "lake"); // newest first
  const ApiResult mine =
      s.call("list_repositories", {{"customer_id", cid}, {"limit", 1}});
  EXPECT_EQ(mine.value["items"].size(), 1u);
  const ApiResult none =
      s.call("list_repositories", {{"customer_id", "other"}});
  EXPECT_EQ(none.value["items"].size(), 0u);
  EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["name"],
            "liquid");
  EXPECT_EQ(s.call("get_repository", {{"id", "missing"}}).error.code,
            "not_found");
}

TEST(Repositories, LifecycleUsesPolicyAndAuditRegistry) {
  Surface surface;
  const ApiResult first_user =
      surface.call("create_user", {{"display_name", "Ada"}});
  const ApiResult second_user =
      surface.call("create_user", {{"display_name", "Grace"}});
  ASSERT_TRUE(first_user.ok);
  ASSERT_TRUE(second_user.ok);
  const ApiResult first_customer =
      surface.call("create_customer", {{"customer_type", "individual"},
                                       {"user_id", first_user.value["id"]},
                                       {"name", "ada"}});
  const ApiResult second_customer =
      surface.call("create_customer", {{"customer_type", "individual"},
                                       {"user_id", second_user.value["id"]},
                                       {"name", "grace"}});
  ASSERT_TRUE(first_customer.ok);
  ASSERT_TRUE(second_customer.ok);
  const ApiResult created = surface.call(
      "create_repository",
      {{"customer_id", first_customer.value["id"]}, {"name", "liquid"}});
  ASSERT_TRUE(created.ok);
  const std::string repository_id = created.value["id"];

  ASSERT_TRUE(surface
                  .call("set_repository_visibility",
                        {{"id", repository_id}, {"visibility", "organization"}})
                  .ok);
  ASSERT_TRUE(surface
                  .call("rename_repository",
                        {{"id", repository_id}, {"name", "liquid-next"}})
                  .ok);
  const ApiResult transferred = surface.call(
      "transfer_repository",
      {{"id", repository_id}, {"customer_id", second_customer.value["id"]}});
  ASSERT_TRUE(transferred.ok);
  EXPECT_EQ(transferred.value["customer_id"], second_customer.value["id"]);
  const ApiResult deleted =
      surface.call("delete_repository", {{"id", repository_id}});
  ASSERT_TRUE(deleted.ok);
  EXPECT_FALSE(deleted.value["deleted_at"].is_null());
  EXPECT_FALSE(deleted.value["retention_until"].is_null());
  EXPECT_EQ(
      surface
          .call("rename_repository", {{"id", repository_id}, {"name", "again"}})
          .error.code,
      "conflict");

  const ApiResult events = surface.call("audit", {{"limit", 100}});
  ASSERT_TRUE(events.ok);
  EXPECT_TRUE(std::ranges::any_of(events.value["items"], [](const json &event) {
    return event["action"] == "delete_repository" &&
           event["outcome"] == "success";
  }));

  surface.ctx.acting_identity = "guest";
  EXPECT_EQ(surface
                .call("rename_repository",
                      {{"id", repository_id}, {"name", "denied"}})
                .error.code,
            "unauthorized");
}
