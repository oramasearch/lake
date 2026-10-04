#include "repository_records_api.hpp"

#include <memory>
#include <optional>
#include <utility>

#include "workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
using Settings = std::vector<control::RepositorySettings>;

struct SettingsRead {
    liquid::result<control::ZettaControlResult<Settings>> value;
};

boost::capy::io_task<SettingsRead>
complete_read(liquid::runtime::task<control::ZettaControlResult<Settings>> task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<SettingsRead>{{}, {std::move(result)}};
}

// One repository's record, or every record of its space in one workspace.
liquid::runtime::task<control::ZettaControlResult<Settings>>
read_settings(std::shared_ptr<control::WorkspaceRecords> records, std::string customer_id,
              std::optional<std::string> repository_id) {
    if (repository_id) {
        auto found = control::settled(co_await control::get_repository_settings(
            *records, *repository_id, std::move(customer_id)));
        if (!found) {
            co_return control::ZettaControlResult<Settings>(std::unexpected(found.error()));
        }
        Settings settings;
        if (*found) {
            settings.push_back(std::move(**found));
        }
        co_return control::ZettaControlResult<Settings>(std::move(settings));
    }
    co_return control::settled(
        co_await control::list_repository_settings(*records, std::move(customer_id)));
}

liquid::runtime::task<control::ZettaControlResult<Settings>>
refused(control::ZettaControlError error) {
    co_return control::ZettaControlResult<Settings>(std::unexpected(std::move(error)));
}

} // namespace

liquid::runtime::task<std::expected<control::Repository, ApiResult>>
create_repository_with_settings(ApiContext& context, control::NewRepository in,
                                std::string description) {
    using Created = std::expected<control::Repository, ApiResult>;
    if (description.empty()) {
        auto created = create_repository_row(context, in);
        if (!created) {
            co_return Created(std::unexpect,
                              ApiResult::failure(created.error().code, created.error().message));
        }
        co_return Created(std::move(*created));
    }
    in.id = control::new_uuid_v4();
    auto records = workspace_records(context, in.customer_id, in.workspace_id);
    if (!records) {
        co_return Created(std::unexpect,
                          ApiResult::failure(records.error().code, records.error().message));
    }
    auto settings = control::settled(co_await control::create_repository_settings(
        *records, {.repository_id = in.id,
                   .customer_id = in.customer_id,
                   .description = std::move(description)}));
    if (!settings) {
        co_return Created(std::unexpect,
                          ApiResult::failure(settings.error().code, settings.error().message));
    }
    auto created = create_repository_row(context, in);
    if (!created) {
        // The row was refused (a taken name, a deleted workspace): the record goes too, so a
        // record never names a repository that does not exist. It is the record just written, so
        // its revision is the one read.
        (void)control::settled(co_await records->remove(
            control::RepositorySettingsCodec::collection, in.id, settings->revision));
        co_return Created(std::unexpect,
                          ApiResult::failure(created.error().code, created.error().message));
    }
    created->description = settings->description;
    co_return Created(std::move(*created));
}

liquid::runtime::task<std::map<std::string, std::string>>
fill_repository_settings(ApiContext& context, std::vector<control::Repository>& repositories) {
    std::map<std::string, std::string> unavailable;
    // The workspaces to read, each with the space that owns it.
    std::vector<std::pair<std::string, std::string>> workspaces; // workspace, space
    std::map<std::string, std::size_t> index_of;
    for (control::Repository& repository : repositories) {
        repository.description.clear();
        if (repository.workspace_id.empty()) {
            unavailable[repository.id] =
                "the repository has no workspace yet; an operator must assign one";
            continue;
        }
        if (index_of.emplace(repository.workspace_id, workspaces.size()).second) {
            workspaces.emplace_back(repository.workspace_id, repository.customer_id);
        }
    }
    const bool single = repositories.size() == 1;
    std::vector<std::shared_ptr<control::WorkspaceRecords>> opened;
    std::vector<boost::capy::io_task<SettingsRead>> pending;
    for (const auto& [workspace_id, customer_id] : workspaces) {
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            opened.push_back(nullptr);
            pending.push_back(complete_read(refused(records.error())));
            continue;
        }
        opened.push_back(std::make_shared<control::WorkspaceRecords>(std::move(*records)));
        pending.push_back(complete_read(read_settings(
            opened.back(), customer_id,
            single ? std::optional<std::string>(repositories.front().id) : std::nullopt)));
    }
    if (pending.empty()) {
        co_return unavailable;
    }
    auto [wait_error, reads] = co_await liquid::runtime::when_all(std::move(pending));
    std::map<std::string, std::string> refusals; // workspace -> reason
    std::map<std::string, control::RepositorySettings> found;
    for (std::size_t index = 0; index < workspaces.size(); ++index) {
        const std::string& workspace_id = workspaces[index].first;
        if (wait_error) {
            refusals[workspace_id] = "workspace_not_ready: " + wait_error.message();
            continue;
        }
        auto& value = reads[index].value;
        if (!value) {
            refusals[workspace_id] = "workspace_not_ready: " + value.error().message;
            continue;
        }
        if (!*value) {
            refusals[workspace_id] = value->error().code + ": " + value->error().message;
            continue;
        }
        for (control::RepositorySettings& settings : **value) {
            const std::string id = settings.repository_id;
            found.insert_or_assign(id, std::move(settings));
        }
    }
    for (control::Repository& repository : repositories) {
        if (repository.workspace_id.empty()) {
            continue;
        }
        if (const auto refusal = refusals.find(repository.workspace_id);
            refusal != refusals.end()) {
            unavailable[repository.id] = refusal->second;
            continue;
        }
        // A record opens only in the workspace and space it was sealed for, and names its
        // repository and space, so one found here is this repository's.
        if (const auto settings = found.find(repository.id);
            settings != found.end() && settings->second.customer_id == repository.customer_id) {
            repository.description = settings->second.description;
        }
    }
    co_return unavailable;
}

json repository_json(const control::Repository& repository,
                     const std::map<std::string, std::string>& unavailable) {
    json value = control::to_json(repository);
    if (const auto reason = unavailable.find(repository.id); reason != unavailable.end()) {
        value["description_unavailable"] = reason->second;
    }
    return value;
}

} // namespace lake::api
