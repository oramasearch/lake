#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "lake/control/agent_bus.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "record_fixture.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"

namespace lake::test {

// Workspace Zettas for tests that keep lake's records in them (docs/architecture/
// workspace-records.md): a WorkspaceService over the test's store whose workspaces are Ready, each
// at its own endpoint `http://<workspace id>.test` with its own RecordTables (StreamCaller). Works
// over the in-memory store, or over PostgreSQL when `database_uri` is given (operator status is
// then written to service_status).
struct RecordWorkspaces {
    std::shared_ptr<control::Store> store;
    std::string database_uri;
    std::filesystem::path dir =
        fixture_path("record-workspaces-" + control::new_uuid_v4());
    std::shared_ptr<StreamCaller> zetta = std::make_shared<StreamCaller>();
    std::shared_ptr<control::WorkspaceService> service;

    explicit RecordWorkspaces(std::shared_ptr<control::Store> backing, std::string uri = {})
        : store(std::move(backing)), database_uri(std::move(uri)) {
        std::filesystem::create_directories(dir);
        service = std::make_shared<control::WorkspaceService>(
            control::WorkspaceConfig{dir.string(), 0}, store, zetta);
    }
    RecordWorkspaces(const RecordWorkspaces&) = delete;
    RecordWorkspaces& operator=(const RecordWorkspaces&) = delete;
    ~RecordWorkspaces() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    // A new Ready workspace in `customer_id`'s space.
    std::string create(const std::string& customer_id, const std::string& name) {
        const auto created = store->create_workspace({customer_id, name, "dev", ""});
        if (!created) {
            throw std::runtime_error("create_workspace: " + created.error().message);
        }
        ready(created->id);
        return created->id;
    }

    // What the operator reports for a workspace's Zetta.
    void status(const std::string& workspace_id, const std::string& phase) {
        const auto workspace = store->get_workspace(workspace_id);
        const auto zetta_service = store->get_service(workspace->zetta_service_id);
        const std::string endpoint = "http://" + workspace_id + ".test";
        if (database_uri.empty()) {
            static_cast<void>(static_cast<control::InMemoryStore&>(*store).set_service_status(
                zetta_service->id, phase, "", endpoint));
            return;
        }
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO service_status (service_id, phase, endpoint) VALUES ($1::uuid, "
                       "$2, $3) ON CONFLICT (service_id) DO UPDATE SET phase = EXCLUDED.phase, "
                       "endpoint = EXCLUDED.endpoint",
                       zetta_service->id, phase, endpoint);
        tx.commit();
    }

    void ready(const std::string& workspace_id) {
        status(workspace_id, "Ready");
        const auto workspace = store->get_workspace(workspace_id);
        const auto zetta_service = store->get_service(workspace->zetta_service_id);
        std::ofstream(dir / (zetta_service->slug + ".json"))
            << nlohmann::json{{"kind", "zetta"}, {"token", "token-" + workspace_id}}.dump();
    }

    // A forge record of `repository`, written into its workspace's Zetta as lake writes it
    // (lake#341). The workspace must be Ready; `kek` is the lake's LAKE_INSTANCE_KEK.
    control::ForgeRecord forge(const control::Repository& repository, std::string kind,
                               nlohmann::json value, std::string_view kek) {
        auto records = control::WorkspaceRecords::open(
            [service = service, customer = repository.customer_id,
             workspace = repository.workspace_id](std::string capability, bool read,
                                                  nlohmann::json input) {
                return service->call(customer, workspace, std::move(capability), read,
                                     std::move(input));
            },
            repository.workspace_id, kek);
        if (!records) {
            throw std::runtime_error("open workspace records: " + records.error().message);
        }
        control::ForgeRecord record;
        record.repository_id = repository.id;
        record.kind = std::move(kind);
        record.value = std::move(value);
        control::ForgeRecords set(*records);
        auto written =
            control::settled(liquid::runtime::block_on(set.append(std::move(record))));
        if (!written) {
            throw std::runtime_error("write forge record: " + written.error().message);
        }
        return *written;
    }

    // The record store of one workspace's Zetta.
    RecordTables& records(const std::string& workspace_id) {
        return zetta->records["http://" + workspace_id + ".test"];
    }
};

} // namespace lake::test
