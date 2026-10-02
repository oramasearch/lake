// The Orama operator reads and writes lake's control tables directly (docs/control-plane/). Twice
// a column the operator selects was missing from lake's schema: agent_launches.workspace_id
// (lake#442) and stores.workspace_id (lake#465). Each time every operator poll failed and nothing
// in lake's tests noticed. This test runs the operator's own SQL against a fresh lake schema.
//
// The statements below are copied verbatim from oramasearch/orama-operator at 188f8ac
// (internal/controller/*_pgx.go). When the operator changes one of them, copy it here again; when
// lake changes a table the operator reads, this test says which statement breaks.

#include "lake/control/agent_runs.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/store.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct OperatorStatement {
    std::string_view source; // the operator file the statement is copied from
    std::string_view name;   // its Go constant
    std::string_view sql;
    bool lists = false; // a SELECT the operator polls; run as well as prepared
};

// internal/controller/lake_stores_pgx.go
constexpr std::string_view listStoresSQL =
    R"(SELECT s.id::text, s.customer_id::text, s.workspace_id::text, COALESCE(w.namespace, ''),
       s.type, s.provider, s.name, s.slug, s.spec::text,
       s.desired, s.generation, st.store_id IS NOT NULL,
       COALESCE(st.phase, ''), COALESCE(st.message, ''), COALESCE(st.endpoint, ''),
       COALESCE(st.observed_generation, 0)
  FROM stores s
  JOIN workspaces w ON w.id = s.workspace_id
  LEFT JOIN store_status st ON st.store_id = s.id
 WHERE NOT (s.desired = 'absent' AND COALESCE(st.phase, '') = 'Deleted')
 ORDER BY s.created_at, s.id)";

// internal/controller/lake_stores_pgx.go
constexpr std::string_view upsertStoreStatusSQL =
    R"(INSERT INTO store_status (store_id, phase, message, endpoint, observed_generation, updated_at)
VALUES ($1::uuid, $2, $3, $4, $5, now())
ON CONFLICT (store_id) DO UPDATE SET
    phase = EXCLUDED.phase,
    message = EXCLUDED.message,
    endpoint = EXCLUDED.endpoint,
    observed_generation = EXCLUDED.observed_generation,
    updated_at = now())";

// internal/controller/lake_services_pgx.go
constexpr std::string_view listServicesSQL =
    R"(SELECT s.id::text, s.customer_id::text, s.workspace_id::text, COALESCE(w.namespace, ''),
       COALESCE(w.zetta_service_id::text, ''),
       s.kind, s.name, s.slug, s.is_default, s.spec::text,
       s.desired, s.generation, st.service_id IS NOT NULL,
       COALESCE(st.phase, ''), COALESCE(st.message, ''), COALESCE(st.endpoint, ''),
       COALESCE(st.observed_generation, 0)
  FROM services s
  JOIN workspaces w ON w.id = s.workspace_id
  LEFT JOIN service_status st ON st.service_id = s.id
 WHERE NOT (s.desired = 'absent' AND COALESCE(st.phase, '') = 'Deleted')
 ORDER BY s.created_at, s.id)";

// internal/controller/lake_services_pgx.go
constexpr std::string_view upsertServiceStatusSQL =
    R"(INSERT INTO service_status (service_id, phase, message, endpoint, observed_generation, updated_at)
VALUES ($1::uuid, $2, $3, $4, $5, now())
ON CONFLICT (service_id) DO UPDATE SET
    phase = EXCLUDED.phase,
    message = EXCLUDED.message,
    endpoint = EXCLUDED.endpoint,
    observed_generation = EXCLUDED.observed_generation,
    updated_at = now())";

// internal/controller/lake_agents_pgx.go
constexpr std::string_view listAgentLaunchesSQL =
    R"(SELECT l.id::text, l.customer_id::text, l.workspace_id::text, COALESCE(w.namespace, ''),
       l.agent_id, l.mode, l.input::text, l.slug,
       l.desired, l.generation, ls.launch_id IS NOT NULL,
       COALESCE(ls.phase, ''), COALESCE(ls.message, ''), COALESCE(ls.endpoint, ''), ls.exit_code,
       COALESCE(ls.result, 'null'::jsonb)::text, COALESCE(ls.credential_sha256, ''),
       COALESCE(ls.observed_generation, 0)
  FROM agent_launches l
  JOIN workspaces w ON w.id = l.workspace_id
  LEFT JOIN agent_launch_status ls ON ls.launch_id = l.id
 WHERE l.desired = 'present'
    OR ls.launch_id IS NULL
    OR ls.observed_generation <> l.generation
    OR ls.credential_sha256 <> ''
    OR ls.phase NOT IN ('Stopped', 'Succeeded', 'Failed')
 ORDER BY l.created_at, l.id)";

// internal/controller/lake_agents_pgx.go
constexpr std::string_view upsertAgentLaunchStatusSQL =
    R"(INSERT INTO agent_launch_status (launch_id, phase, message, endpoint, exit_code, result,
    credential_sha256, observed_generation, updated_at)
VALUES ($1::uuid, $2, $3, $4, $5, $6::jsonb, $7, $8, now())
ON CONFLICT (launch_id) DO UPDATE SET
    phase = EXCLUDED.phase,
    message = EXCLUDED.message,
    endpoint = EXCLUDED.endpoint,
    exit_code = EXCLUDED.exit_code,
    result = EXCLUDED.result,
    credential_sha256 = EXCLUDED.credential_sha256,
    observed_generation = EXCLUDED.observed_generation,
    updated_at = now())";

// internal/controller/lake_agents_pgx.go
constexpr std::string_view listAgentSchedulesSQL =
    R"(SELECT s.id::text, s.customer_id::text, s.workspace_id::text, COALESCE(w.namespace, ''),
       s.agent_id, s.cron, s.time_zone, s.input::text,
       s.slug, s.desired, s.generation, ss.schedule_id IS NOT NULL,
       COALESCE(ss.phase, ''), COALESCE(ss.message, ''), ss.last_schedule_at,
       COALESCE(ss.credential_sha256, ''), COALESCE(ss.observed_generation, 0),
       COALESCE((SELECT json_object_agg(r.id::text, r.phase) FROM (SELECT id, phase FROM agent_cron_runs
                  WHERE schedule_id = s.id ORDER BY started_at DESC LIMIT 20) r), '{}')::text
  FROM agent_cron_schedules s
  JOIN workspaces w ON w.id = s.workspace_id
  LEFT JOIN agent_cron_schedule_status ss ON ss.schedule_id = s.id
 WHERE s.desired = 'present'
    OR ss.schedule_id IS NULL
    OR ss.observed_generation <> s.generation
    OR ss.credential_sha256 <> ''
    OR ss.phase <> 'Stopped'
 ORDER BY s.created_at, s.id)";

// internal/controller/lake_agents_pgx.go
constexpr std::string_view upsertAgentScheduleStatusSQL =
    R"(INSERT INTO agent_cron_schedule_status (schedule_id, phase, message, last_schedule_at,
    credential_sha256, observed_generation, updated_at)
VALUES ($1::uuid, $2, $3, $4, $5, $6, now())
ON CONFLICT (schedule_id) DO UPDATE SET
    phase = EXCLUDED.phase,
    message = EXCLUDED.message,
    last_schedule_at = EXCLUDED.last_schedule_at,
    credential_sha256 = EXCLUDED.credential_sha256,
    observed_generation = EXCLUDED.observed_generation,
    updated_at = now())";

// internal/controller/lake_agents_pgx.go
constexpr std::string_view upsertAgentScheduleRunSQL =
    R"(INSERT INTO agent_cron_runs (id, schedule_id, job_name, phase, message, exit_code, result,
    started_at, finished_at, updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7::jsonb, $8, $9, now())
ON CONFLICT (id) DO UPDATE SET
    phase = EXCLUDED.phase,
    message = EXCLUDED.message,
    exit_code = EXCLUDED.exit_code,
    result = EXCLUDED.result,
    finished_at = EXCLUDED.finished_at,
    updated_at = now())";

// internal/controller/lake_apps_pgx.go
constexpr std::string_view listAppsSQL =
    R"(SELECT a.id::text, a.customer_id::text, a.workspace_id::text, COALESCE(w.namespace, ''), a.name, a.mode,
 COALESCE(a.source_repository, ''), COALESCE(a.source_revision, ''), COALESCE(a.source_path, ''),
 COALESCE(a.source_entrypoint, ''), COALESCE(a.image, ''), COALESCE(a.framework, ''),
 COALESCE(a.port, 8080), COALESCE(a.replicas, 1), COALESCE(a.host, ''), COALESCE(a.auth, 'lake'),
 COALESCE(a.bindings_json, '{}'::jsonb)::text, COALESCE(a.env_json, '{}'::jsonb)::text,
 COALESCE(a.secret_env_refs, '{}'::jsonb)::text, a.desired, a.generation,
 COALESCE(a.phase, 'Pending'), COALESCE(a.url, ''), COALESCE(a.ready, false), COALESCE(a.message, ''),
 COALESCE(a.observed_generation, 0), COALESCE(a.token_hashes_json, 'null'::jsonb)::text
 FROM apps a
 JOIN workspaces w ON w.id = a.workspace_id
 ORDER BY a.id)";

// internal/controller/lake_apps_pgx.go
constexpr std::string_view updateAppStatusSQL =
    R"(UPDATE apps SET phase=$5, url=$6, ready=$7, message=$8,
 observed_generation=$9, updated_at=now()
 WHERE id=$1::uuid AND customer_id=$2::uuid AND generation=$3 AND desired=$4)";

// internal/controller/lake_apps_pgx.go
constexpr std::string_view updateAppTokenHashesSQL =
    R"(UPDATE apps SET token_hashes_json=$5::jsonb
 WHERE id=$1::uuid AND customer_id=$2::uuid AND generation=$3 AND desired=$4)";

// internal/controller/lake_builds_pgx.go
constexpr std::string_view listBuildsSQL =
    R"(SELECT id::text, customer_id::text, repository, revision, context, dockerfile, target,
       COALESCE(build_args_json, '{}'::jsonb)::text, desired, COALESCE(phase, ''),
       COALESCE(digest, ''), COALESCE(size, 0), COALESCE(message, ''),
       COALESCE(token_hashes_json, '{}'::jsonb)::text
  FROM builds
 WHERE (desired = 'queued' AND COALESCE(phase, '') NOT IN ('Succeeded', 'Failed'))
    OR (desired = 'cancelled' AND COALESCE(phase, '') <> 'Cancelled')
 ORDER BY created_at, id)";

// internal/controller/lake_builds_pgx.go
constexpr std::string_view updateBuildStatusSQL =
    R"(UPDATE builds
   SET phase = $2, digest = $3, size = $4, message = $5, token_hashes_json = $6::jsonb, updated_at = now()
 WHERE id = $1::uuid)";

// internal/controller/lake_workspaces_pgx.go
constexpr std::string_view listWorkspacesSQL =
    R"(SELECT w.id::text, w.customer_id::text, w.name, w.environment, w.desired, w.generation,
 COALESCE(w.phase, ''), COALESCE(w.observed_generation, 0), COALESCE(w.message, ''),
 COALESCE(w.namespace, ''), COALESCE(w.zetta_service_id::text, ''), COALESCE(s.generation, 0),
 ss.service_id IS NOT NULL, COALESCE(ss.phase, ''), COALESCE(ss.message, ''),
 COALESCE(ss.endpoint, ''), COALESCE(ss.observed_generation, 0)
 FROM workspaces w
 LEFT JOIN services s ON s.id = w.zetta_service_id
 LEFT JOIN service_status ss ON ss.service_id = w.zetta_service_id
 WHERE NOT (w.desired = 'deleted' AND COALESCE(w.phase, '') = 'Deleted')
 ORDER BY w.created_at, w.id)";

// internal/controller/lake_workspaces_pgx.go
constexpr std::string_view updateWorkspaceStatusSQL =
    R"(UPDATE workspaces
 SET phase=$5, observed_generation=$6, message=$7, namespace=$8, updated_at=now()
 WHERE id=$1::uuid AND customer_id=$2::uuid AND generation=$3 AND desired=$4)";

const std::vector<OperatorStatement>& operator_statements() {
    static const std::vector<OperatorStatement> statements{
        {"lake_stores_pgx.go", "listStoresSQL", listStoresSQL, true},
        {"lake_stores_pgx.go", "upsertStoreStatusSQL", upsertStoreStatusSQL},
        {"lake_services_pgx.go", "listServicesSQL", listServicesSQL, true},
        {"lake_services_pgx.go", "upsertServiceStatusSQL", upsertServiceStatusSQL},
        {"lake_agents_pgx.go", "listAgentLaunchesSQL", listAgentLaunchesSQL, true},
        {"lake_agents_pgx.go", "upsertAgentLaunchStatusSQL", upsertAgentLaunchStatusSQL},
        {"lake_agents_pgx.go", "listAgentSchedulesSQL", listAgentSchedulesSQL, true},
        {"lake_agents_pgx.go", "upsertAgentScheduleStatusSQL", upsertAgentScheduleStatusSQL},
        {"lake_agents_pgx.go", "upsertAgentScheduleRunSQL", upsertAgentScheduleRunSQL},
        {"lake_apps_pgx.go", "listAppsSQL", listAppsSQL, true},
        {"lake_apps_pgx.go", "updateAppStatusSQL", updateAppStatusSQL},
        {"lake_apps_pgx.go", "updateAppTokenHashesSQL", updateAppTokenHashesSQL},
        {"lake_builds_pgx.go", "listBuildsSQL", listBuildsSQL, true},
        {"lake_builds_pgx.go", "updateBuildStatusSQL", updateBuildStatusSQL},
        {"lake_workspaces_pgx.go", "listWorkspacesSQL", listWorkspacesSQL, true},
        {"lake_workspaces_pgx.go", "updateWorkspaceStatusSQL", updateWorkspaceStatusSQL},
    };
    return statements;
}

// Every statement parses and plans against the schema: each table and column it names exists,
// with a type the statement can use. PostgreSQL checks this when the statement is prepared, as
// pgx does before the operator's first poll.
TEST(OperatorContract, EveryStatementTheOperatorRunsPreparesAgainstTheSchema) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    pqxx::connection connection(uri);
    for (const OperatorStatement& statement : operator_statements()) {
        try {
            connection.prepare(std::string(statement.name), std::string(statement.sql));
            connection.unprepare(statement.name);
        } catch (const pqxx::sql_error& error) {
            ADD_FAILURE() << "orama-operator internal/controller/" << statement.source << " "
                          << statement.name << " does not match lake's schema: " << error.what();
        }
    }
}

// What lake writes through its own store is what the operator reads: a store, service, app,
// build, launch and schedule created the way the capabilities create them each appear in the
// operator's poll, in the namespace of their workspace (lake#465 would have shown here as
// "column s.workspace_id does not exist").
TEST(OperatorContract, RowsLakeWritesAppearInTheOperatorsPolls) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto user = store->create_user({"taap:1:ed25519:contract", "", "Contract"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "contract"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "contract", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const std::string ns = "lake-ws-contract";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspaces SET namespace = $1 WHERE id = $2::uuid", ns,
                       workspace->id);
        tx.commit();
    }

    lake::control::NewManagedStore volume{customer->id, "volume", "cluster", "uploads", "taap:x"};
    volume.workspace_id = workspace->id;
    const auto created_store = store->create_store(volume);
    ASSERT_TRUE(created_store) << created_store.error().message;
    const auto repository = store->create_repository({customer->id, "source", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto app = store->create_web_app({.customer_id = customer->id,
                                            .workspace_id = workspace->id,
                                            .name = "app",
                                            .mode = "image",
                                            .image = "app:v1",
                                            .port = 3000,
                                            .host = "contract.test"});
    ASSERT_TRUE(app) << app.error().message;
    const auto build = store->create_build({.customer_id = customer->id,
                                            .workspace_id = workspace->id,
                                            .repository = repository->id,
                                            .revision = "main",
                                            .target = "app:v1"});
    ASSERT_TRUE(build) << build.error().message;
    const auto launch = store->create_agent_launch({.customer_id = customer->id,
                                                    .agent_id = "a-1",
                                                    .agent_name = "Watcher",
                                                    .mode = "job",
                                                    .trigger = "manual",
                                                    .scopes = lake::control::agent_launch_scopes,
                                                    .created_by_user_id = user->id,
                                                    .created_by_taap_id = user->taap_id,
                                                    .workspace_id = workspace->id});
    ASSERT_TRUE(launch) << launch.error().message;
    const auto schedule =
        store->create_agent_cron_schedule({.customer_id = customer->id,
                                           .agent_id = "a-1",
                                           .agent_name = "Watcher",
                                           .cron = "0 * * * *",
                                           .scopes = lake::control::agent_launch_scopes,
                                           .created_by_user_id = user->id,
                                           .created_by_taap_id = user->taap_id,
                                           .workspace_id = workspace->id});
    ASSERT_TRUE(schedule) << schedule.error().message;

    const std::vector<std::pair<std::string_view, std::string>> expected{
        {"listStoresSQL", created_store->id},
        {"listServicesSQL", workspace->zetta_service_id},
        {"listAgentLaunchesSQL", launch->id},
        {"listAgentSchedulesSQL", schedule->id},
        {"listAppsSQL", app->id},
        {"listBuildsSQL", build->id},
        {"listWorkspacesSQL", workspace->id},
    };
    pqxx::connection connection(uri);
    for (const OperatorStatement& statement : operator_statements()) {
        if (!statement.lists) {
            continue;
        }
        const auto wanted = std::ranges::find(expected, statement.name,
                                              &std::pair<std::string_view, std::string>::first);
        ASSERT_NE(wanted, expected.end()) << statement.name;
        pqxx::work tx(connection);
        pqxx::result rows;
        try {
            rows = tx.exec(std::string(statement.sql));
        } catch (const pqxx::sql_error& error) {
            ADD_FAILURE() << statement.source << " " << statement.name << ": " << error.what();
            continue;
        }
        const auto row = std::ranges::find_if(
            rows, [&](const pqxx::row& r) { return r[0].as<std::string>() == wanted->second; });
        ASSERT_NE(row, rows.end()) << statement.name << " does not return the row lake wrote";
        // Every query that joins a workspace places the object in the workspace's namespace.
        if (std::string_view(statement.sql).find("JOIN workspaces w ON w.id =") !=
            std::string_view::npos) {
            EXPECT_EQ((*row)[2].as<std::string>(), workspace->id) << statement.name;
            EXPECT_EQ((*row)[3].as<std::string>(), ns) << statement.name;
        }
    }
}

} // namespace
