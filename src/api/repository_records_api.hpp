#pragma once

#include <expected>
#include <map>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/repository_records.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// A repository's settings (lake#315 part 2, control/repository_records.hpp) are a record in the
// workspace Zetta of the repository's immutable workspace; the control store keeps only the
// routing row. These helpers join the two for the repository capabilities and pages.

// Creates a repository: with a description, its settings record is written `if_absent` in its
// workspace under a new id first, then the routing row under that id; a refused row removes the
// record, so nothing is left behind. Without a description, only the row (no record until one is
// set). The caller has checked that `in.workspace_id` is a live workspace of `in.customer_id`.
// Answers the new repository, its description filled in, or the refusal.
liquid::runtime::task<std::expected<control::Repository, ApiResult>>
create_repository_with_settings(ApiContext& context, control::NewRepository in,
                                std::string description);

// Fills each repository's description from its workspace: one get_record for a single repository,
// otherwise one list_records per workspace (every repository of a workspace is in its space),
// all started together. Answers, by repository id, why a description could not be read (no
// workspace yet, or the workspace refused); those repositories keep an empty description.
// A repository with no record has none, and is not unavailable.
liquid::runtime::task<std::map<std::string, std::string>>
fill_repository_settings(ApiContext& context, std::vector<control::Repository>& repositories);

// What a co_await of the two tasks above gives, with a runtime failure as a store_error.
inline std::expected<control::Repository, ApiResult>
settled_create(liquid::result<std::expected<control::Repository, ApiResult>> awaited) {
    if (!awaited) {
        return std::unexpected(ApiResult::failure("store_error", awaited.error().message));
    }
    return std::move(*awaited);
}
// A runtime failure of the read leaves every description unavailable, with its message.
inline std::map<std::string, std::string>
settled_fill(liquid::result<std::map<std::string, std::string>> awaited,
             const std::vector<control::Repository>& repositories) {
    if (awaited) {
        return std::move(*awaited);
    }
    std::map<std::string, std::string> unavailable;
    for (const control::Repository& repository : repositories) {
        unavailable[repository.id] = "workspace_not_ready: " + awaited.error().message;
    }
    return unavailable;
}

// A repository as the capabilities answer it: the routing row, the description, and
// `description_unavailable` with the reason when it could not be read.
nlohmann::json repository_json(const control::Repository& repository,
                               const std::map<std::string, std::string>& unavailable);

} // namespace lake::api
