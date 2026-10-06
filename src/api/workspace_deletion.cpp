#include "lake/api/workspace_deletion.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>
#include <map>
#include <set>
#include <string_view>

#include "lake/audit/audit.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/web_apps.hpp"
#include "lake/search/index.hpp"

namespace lake::api {

namespace {

using nlohmann::json;

// `<space>/<name>` from a build target, `<authority>/<space>/<name>:<tag>`; empty when the target
// is not in that space.
std::string target_repository(std::string_view target, std::string_view space) {
    const std::size_t authority = target.find('/');
    const std::size_t tag = target.rfind(':');
    if (authority == std::string_view::npos || tag == std::string_view::npos ||
        tag <= authority + 1) {
        return {};
    }
    const std::string_view repository = target.substr(authority + 1, tag - authority - 1);
    if (!repository.starts_with(space) || repository.size() <= space.size() + 1 ||
        repository[space.size()] != '/') {
        return {};
    }
    const std::string_view name = repository.substr(space.size() + 1);
    if (name.contains('/')) {
        return {};
    }
    return std::string(repository);
}

// The image names (`<name>` below `<space>/`) each workspace's apps and builds use.
std::map<std::string, std::set<std::string>> image_names_by_workspace(control::Store& store,
                                                                      const std::string& customer_id,
                                                                      const std::string& space) {
    std::map<std::string, std::set<std::string>> names;
    if (const auto apps = store.list_web_apps(customer_id)) {
        for (const control::WebApp& app : *apps) {
            names[app.workspace_id].insert(app.name);
        }
    }
    if (const auto builds = store.list_builds(customer_id)) {
        for (const control::Build& build : *builds) {
            const std::string repository = target_repository(build.target, space);
            if (!repository.empty()) {
                names[build.workspace_id].insert(repository.substr(space.size() + 1));
            }
        }
    }
    return names;
}

json items_json(const std::vector<control::WorkspaceDeletionItem>& items) {
    json values = json::array();
    for (const control::WorkspaceDeletionItem& item : items) {
        json value{{"name", item.name}, {"state", item.state}};
        if (!item.id.empty()) {
            value["id"] = item.id;
        }
        values.push_back(std::move(value));
    }
    return values;
}

// Seconds since the epoch of an ISO 8601 timestamp (the memory store's) or PostgreSQL's text form
// (`YYYY-MM-DD HH:MM:SS[.ffffff]±HH[:MM]`, in the session's time zone); nullopt when unreadable.
std::optional<std::time_t> unix_seconds(std::string_view value) {
    std::tm parts{};
    if (value.size() < 19 ||
        std::sscanf(std::string(value.substr(0, 19)).c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d",
                    &parts.tm_year, &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min,
                    &parts.tm_sec) != 6) {
        return std::nullopt;
    }
    parts.tm_year -= 1900;
    parts.tm_mon -= 1;
    std::time_t seconds = timegm(&parts);
    std::string_view zone = value.substr(19);
    while (!zone.empty() && (zone.front() == '.' || std::isdigit(static_cast<unsigned char>(zone.front())))) {
        zone.remove_prefix(1);
    }
    if (!zone.empty() && (zone.front() == '+' || zone.front() == '-')) {
        const int sign = zone.front() == '+' ? 1 : -1;
        int hours = 0;
        int minutes = 0;
        std::sscanf(std::string(zone.substr(1)).c_str(), "%2d:%2d", &hours, &minutes);
        seconds -= sign * (hours * 3600 + minutes * 60);
    }
    return seconds;
}

std::size_t count_state(const std::vector<control::WorkspaceDeletionItem>& items,
                        std::string_view state) {
    return static_cast<std::size_t>(std::ranges::count(items, state, &control::WorkspaceDeletionItem::state));
}

json counts_json(const std::vector<control::WorkspaceDeletionItem>& repositories,
                 const std::vector<control::WorkspaceDeletionItem>& apps,
                 const std::vector<control::WorkspaceDeletionItem>& builds,
                 const std::vector<control::WorkspaceDeletionItem>& images) {
    return {{"repositories", repositories.size()},
            {"live_repositories", count_state(repositories, "live")},
            {"tombstoned_repositories", count_state(repositories, "tombstoned")},
            {"apps", apps.size()},
            {"builds", builds.size()},
            {"images", images.size()}};
}

// What goes with the workspace Zetta whatever lake does: every record lake keeps there (the
// sealed lake_* collections) and the workspace's own audit chain.
json zetta_json() {
    return {{"records", "every lake record in the workspace's Zetta (the sealed lake_* collections), "
                        "with its datasets, topics and stores"},
            {"audit", "the workspace's own audit chain; the central log keeps one event per "
                      "destroyed repository, app, build and image"}};
}

// Records one central event of a deletion unless it is already recorded (a resumed deletion).
std::expected<void, std::string> record_once(const ApiContext& context,
                                             const control::WorkspaceDeletion& deletion,
                                             std::string kind,
                                             std::string object,
                                             json details) {
    const std::string request_id = workspace_deletion_request_id(deletion.workspace_id);
    const auto existing = context.store->list_audit_events(
        audit::Filter{.object = object, .limit = 1, .request_id = request_id});
    if (!existing) {
        return std::unexpected(existing.error().message);
    }
    if (!existing->empty()) {
        return {};
    }
    details["workspace_id"] = deletion.workspace_id;
    details["workspace_name"] = deletion.workspace_name;
    const audit::EventDraft draft{
        .customer_id = deletion.customer_id,
        .kind = std::move(kind),
        .principal = deletion.requested_by.empty() ? "admin" : deletion.requested_by,
        .object = std::move(object),
        .action = "delete_workspace",
        .outcome = "success",
        .request_id = request_id,
        .details = std::move(details),
    };
    const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                         .instance_kek = context.instance_kek};
    const auto recorded = audit::record(*context.store, draft, options);
    if (!recorded) {
        return std::unexpected(recorded.error().message);
    }
    return {};
}

} // namespace

std::string workspace_deletion_request_id(const std::string& workspace_id) {
    return "workspace-deletion:" + workspace_id;
}

std::vector<std::string> workspace_image_candidates(control::Store& store,
                                                    const control::Workspace& workspace) {
    const auto customer = store.get_customer(workspace.customer_id);
    if (!customer) {
        return {};
    }
    const std::string& space = customer->name;
    const auto names = image_names_by_workspace(store, workspace.customer_id, space);
    std::set<std::string> live;
    if (const auto workspaces = store.list_workspaces(workspace.customer_id)) {
        for (const control::Workspace& other : *workspaces) {
            if (other.id != workspace.id && other.desired != "deleted") {
                live.insert(other.id);
            }
        }
    }
    std::set<std::string> shared;
    for (const auto& [owner, used] : names) {
        if (live.contains(owner)) {
            shared.insert(used.begin(), used.end());
        }
    }
    std::vector<std::string> candidates;
    const auto own = names.find(workspace.id);
    if (own == names.end()) {
        return candidates;
    }
    for (const std::string& name : own->second) {
        if (!shared.contains(name)) {
            candidates.push_back(space + "/" + name);
            candidates.push_back(space + "/cache/" + name);
        }
    }
    std::ranges::sort(candidates);
    return candidates;
}

std::expected<json, ApiError> workspace_deletion_plan(const ApiContext& context,
                                                      const control::Workspace& workspace) {
    std::vector<control::WorkspaceDeletionItem> repositories, apps, builds, images;
    const auto rows = context.store->list_repositories(workspace.customer_id, 100'000, workspace.id);
    if (!rows) {
        return std::unexpected(ApiError{rows.error().code, rows.error().message});
    }
    const std::time_t now = std::time(nullptr);
    for (const control::Repository& repository : *rows) {
        if (repository.deleted_at.empty()) {
            repositories.push_back({repository.id, repository.name, "live"});
        } else if (const auto until = unix_seconds(repository.retention_until);
                   !until || *until > now) {
            repositories.push_back({repository.id, repository.name, "tombstoned"});
        }
    }
    std::ranges::sort(repositories, {}, &control::WorkspaceDeletionItem::name);
    if (const auto listed = context.store->list_web_apps(workspace.customer_id)) {
        for (const control::WebApp& app : *listed) {
            if (app.workspace_id == workspace.id && app.desired != "deleted") {
                apps.push_back({app.id, app.name, app.desired});
            }
        }
    }
    if (const auto listed = context.store->list_builds(workspace.customer_id)) {
        for (const control::Build& build : *listed) {
            if (build.workspace_id == workspace.id && build.desired == "queued" &&
                build.phase != "Succeeded" && build.phase != "Failed" &&
                build.phase != "Cancelled") {
                builds.push_back({build.id, build.target, build.phase});
            }
        }
    }
    json tags = json::object();
    if (context.workspace_content && context.workspace_content->image_tags) {
        for (const std::string& image : workspace_image_candidates(*context.store, workspace)) {
            const std::vector<std::string> held = context.workspace_content->image_tags(image);
            if (!held.empty()) {
                images.push_back({"", image, "tagged"});
                tags[image] = held;
            }
        }
    }
    // Routed tenant repositories (lake#383): their manifests and tags go with the workspace's
    // Zetta and their routes with the cascade.
    if (const auto routes = context.store->list_oci_repository_routes(workspace.customer_id,
                                                                      workspace.id)) {
        const auto customer = context.store->get_customer(workspace.customer_id);
        for (const control::OciRepositoryRoute& route : *routes) {
            const std::string image = (customer ? customer->name : std::string()) + "/" + route.name;
            if (std::ranges::find(images, image, &control::WorkspaceDeletionItem::name) ==
                images.end()) {
                images.push_back({"", image, "routed"});
            }
        }
    }
    std::ranges::sort(images, {}, &control::WorkspaceDeletionItem::name);
    json image_values = items_json(images);
    for (json& image : image_values) {
        image["tags"] = tags.value(image.value("name", std::string()), json::array());
    }
    return json{{"workspace_id", workspace.id},
                {"workspace_name", workspace.name},
                {"repositories", items_json(repositories)},
                {"apps", items_json(apps)},
                {"builds", items_json(builds)},
                {"images", std::move(image_values)},
                {"counts", counts_json(repositories, apps, builds, images)},
                {"zetta", zetta_json()},
                {"irreversible", true}};
}

json workspace_deletion_json(const control::WorkspaceDeletion& deletion) {
    return {{"workspace_id", deletion.workspace_id},
            {"workspace_name", deletion.workspace_name},
            {"requested_by", deletion.requested_by},
            {"requested_at", deletion.requested_at},
            {"completed", !deletion.completed_at.empty()},
            {"completed_at",
             deletion.completed_at.empty() ? json(nullptr) : json(deletion.completed_at)},
            {"repositories", items_json(deletion.repositories)},
            {"apps", items_json(deletion.apps)},
            {"builds", items_json(deletion.builds)},
            {"images", items_json(deletion.images)},
            {"counts", counts_json(deletion.repositories, deletion.apps, deletion.builds,
                                   deletion.images)},
            {"zetta", zetta_json()},
            {"audit_request_id", workspace_deletion_request_id(deletion.workspace_id)}};
}

std::expected<void, std::string> finish_workspace_deletion(const ApiContext& context,
                                                           const control::WorkspaceDeletion& deletion) {
    if (!context.store) {
        return std::unexpected("no control store");
    }
    // Repositories: their content stays in the space's liquid store until the collector reclaims
    // it (their retention has ended); what the space's search index holds of them is dropped now.
    // After lake#380 the workspace's store goes with its Zetta.
    for (const control::WorkspaceDeletionItem& repository : deletion.repositories) {
        if (context.workspace_content && context.workspace_content->drop_repository_index) {
            const auto dropped = context.workspace_content->drop_repository_index(
                deletion.customer_id, repository.id);
            if (!dropped) {
                return std::unexpected("could not drop the search segments of repository " +
                                       repository.name + ": " + dropped.error());
            }
        } else if (context.search) {
            const auto dropped = context.search->drop_repository(deletion.customer_id, repository.id);
            if (!dropped) {
                return std::unexpected("could not drop the search segments of repository " +
                                       repository.name + ": " + dropped.error().message);
            }
        }
        const auto recorded = record_once(context, deletion, "workspace.deletion.repository",
                                          "repository/" + repository.id,
                                          {{"repository_id", repository.id},
                                           {"repository_name", repository.name},
                                           {"state", repository.state}});
        if (!recorded) {
            return recorded;
        }
    }
    for (const control::WorkspaceDeletionItem& app : deletion.apps) {
        const auto recorded = record_once(context, deletion, "workspace.deletion.app",
                                          "app/" + app.id,
                                          {{"app_id", app.id}, {"app_name", app.name}});
        if (!recorded) {
            return recorded;
        }
    }
    for (const control::WorkspaceDeletionItem& build : deletion.builds) {
        const auto recorded = record_once(context, deletion, "workspace.deletion.build",
                                          "build/" + build.id,
                                          {{"build_id", build.id}, {"target", build.name},
                                           {"phase", build.state}});
        if (!recorded) {
            return recorded;
        }
    }
    // A routed image's records went with the workspace's Zetta and its route with the cascade;
    // whatever the instance registry still holds of it from before lake#383 is removed too.
    for (const control::WorkspaceDeletionItem& image : deletion.images) {
        if (context.workspace_content && context.workspace_content->remove_image_repository) {
            const auto removed = context.workspace_content->remove_image_repository(image.name);
            if (!removed) {
                return std::unexpected("could not remove image " + image.name + ": " +
                                       removed.error());
            }
        } else if (!context.workspace_content && image.state != "routed") {
            // The registry lives in this lake's store root; only a lake that has it removes the
            // tags. The deletion stays pending until one does.
            return std::unexpected("this lake has no registry storage to remove image " +
                                   image.name + " from");
        }
        const auto recorded = record_once(context, deletion, "workspace.deletion.image",
                                          "image/" + image.name, {{"image", image.name}});
        if (!recorded) {
            return recorded;
        }
    }
    // The workspace's search index (lake#381) goes before its store closes.
    if (context.workspace_content && context.workspace_content->drop_workspace_index) {
        const auto dropped = context.workspace_content->drop_workspace_index(deletion.workspace_id);
        if (!dropped) {
            return std::unexpected("could not drop the workspace's search index: " + dropped.error());
        }
    } else if (context.search) {
        const auto dropped = context.search->drop_workspace(deletion.workspace_id);
        if (!dropped) {
            return std::unexpected("could not drop the workspace's search index: " +
                                   dropped.error().message);
        }
    }
    if (context.workspace_content && context.workspace_content->close_workspace_store) {
        const auto closed = context.workspace_content->close_workspace_store(deletion.workspace_id);
        if (!closed) {
            return std::unexpected("could not close the workspace store: " + closed.error());
        }
    }
    const auto summary = record_once(
        context, deletion, "workspace.deletion", "workspace/" + deletion.workspace_id,
        {{"counts", counts_json(deletion.repositories, deletion.apps, deletion.builds,
                                deletion.images)}});
    if (!summary) {
        return summary;
    }
    const auto completed = context.store->complete_workspace_deletion(deletion.workspace_id);
    if (!completed) {
        return std::unexpected(completed.error().message);
    }
    return {};
}

std::expected<std::size_t, std::string> resume_workspace_deletions(const ApiContext& context) {
    if (!context.store) {
        return std::unexpected("no control store");
    }
    const auto pending = context.store->pending_workspace_deletions(100);
    if (!pending) {
        return std::unexpected(pending.error().message);
    }
    std::size_t completed = 0;
    std::string first_failure;
    for (const control::WorkspaceDeletion& deletion : *pending) {
        const auto finished = finish_workspace_deletion(context, deletion);
        if (finished) {
            ++completed;
        } else if (first_failure.empty()) {
            first_failure = "workspace " + deletion.workspace_id + ": " + finished.error();
        }
    }
    if (!first_failure.empty()) {
        return std::unexpected(first_failure);
    }
    return completed;
}

} // namespace lake::api
