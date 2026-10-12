// A workspace's own methodologies from Python code beside Orama's read-only catalogue
// (docs/api/methodologies.md, lake#655).
//
// A workspace methodology is Python in one of the workspace's repositories: a manifest
// (methodology.json), an entrypoint with fit, predict and score, and a lock file. Its registry
// entry is the workspace engine's (zetta#1261, `W-<8 hex>`), next to Orama's `M-n`; its code is
// versioned in the repository; and it runs as untrusted code: Lake builds its image by the build
// path apps use (FROM oramasearch/methodology-runtime, the lock installed with hashes required) and
// writes one `methodology_runs` row per run, which the Orama operator runs as three pods (stage,
// run, collect). Only `stage` and `collect` hold the run's credential, and it reaches nothing but
// methodology_run_input and methodology_run_output for that run: the data goes in and the result
// and artifact come out only through Lake. What a run read and produced is its sealed record in the
// workspace Zetta (`lake_methodology_runs`); a training run's model is registered by the engine.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "methodology_serving.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/methodologies.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
using control::MethodologyFinding;
using control::MethodologyManifest;

constexpr control::RecordCollection run_records{"methodology_runs", 1};

// --- Small helpers -------------------------------------------------------------------------------

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

// The runner's two capabilities authorize by the run's credential in Registry::call; this is
// their policy object, which no person's call reaches past the handler's own check.
std::expected<std::string, ApiError> run_object(const json&, ApiContext& context) {
    if (context.credential_kind != "methodology_run") {
        return std::unexpected(
            ApiError{"unauthorized", "only a methodology run's own credential calls this", {}});
    }
    return std::string("methodology_run/") + context.credential_workload_id;
}

json text() {
    return {{"type", "string"}};
}

json workspace_input(json properties, json required) {
    properties["customer_id"] = text();
    properties["workspace_id"] = text();
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json files_input() {
    return {{"type", "array"},
            {"description",
             "files of the methodology's folder: {path, contents} (UTF-8 text), or {path, "
             "delete: true} to remove one"},
            {"maxItems", 64},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"path", text()}, {"contents", text()}, {"delete", {{"type", "boolean"}}}}},
              {"required", json::array({"path"})},
              {"additionalProperties", false}}}};
}

std::string acting_author(const ApiContext& context) {
    return context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id;
}

std::string now_text() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

ApiResult refused(std::string code, std::string message, std::string path = {}) {
    return ApiResult::failure(std::move(code), std::move(message), std::move(path));
}

ApiResult failed(const control::ZettaControlError& error) {
    if (error.code == "invalid_argument") {
        return ApiResult::failure("invalid_input", error.message, error.path);
    }
    return ApiResult::failure(error.code, error.message, error.path);
}

ApiResult store_failed(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

// Findings as the engine keeps them on a validation: check, ok and message only.
json engine_findings(const json& findings) {
    json out = json::array();
    if (!findings.is_array()) {
        return out;
    }
    for (const json& finding : findings) {
        if (!finding.is_object() || !finding.contains("check") ||
            !finding.at("check").is_string() || finding.at("check").get<std::string>().empty()) {
            continue;
        }
        json kept{{"check", finding.at("check")}, {"ok", finding.value("ok", false) == true}};
        if (finding.contains("message") && finding.at("message").is_string()) {
            kept["message"] = finding.at("message");
        }
        out.push_back(std::move(kept));
        if (out.size() >= 200) {
            break;
        }
    }
    return out;
}

json findings_json(const std::vector<MethodologyFinding>& findings) {
    json out = json::array();
    for (const MethodologyFinding& finding : findings) {
        out.push_back(control::finding_json(finding));
    }
    return out;
}

ApiResult finished(std::expected<ApiResult, liquid::error> done) {
    return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

template <class T>
std::expected<T, ApiResult>
settled_api(std::expected<std::expected<T, ApiResult>, liquid::error> done) {
    if (!done) {
        return std::unexpected(ApiResult::failure("store_error", done.error().message));
    }
    return std::move(*done);
}

// Another capability, called as the same person with its own authorization and audit.
liquid::runtime::task<ApiResult>
call_capability(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return refused("unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    inner.audit_kind.clear();
    inner.audit_details = json();
    auto done = co_await context.registry->call_async(name, input, inner);
    co_return done ? std::move(*done) : refused("store_error", done.error().message);
}

// One engine call in a workspace.
liquid::runtime::task<control::ZettaControlResult<json>> engine(ApiContext& context,
                                                                std::string customer_id,
                                                                std::string workspace_id,
                                                                std::string capability,
                                                                bool read,
                                                                json input) {
    if (!context.workspaces) {
        co_return control::ZettaControlResult<json>(std::unexpected(
            control::ZettaControlError{"workspace_not_ready", "a workspace Zetta is required"}));
    }
    ApiContext bound = context;
    bound.workspace_id = std::move(workspace_id);
    co_return control::settled(co_await call_workspace(
        bound, std::move(customer_id), std::move(capability), read, std::move(input)));
}

// The entry an engine answer carries: {methodology: {...}} or the entry itself.
json entry_of(const json& answer) {
    if (answer.is_object() && answer.contains("methodology") &&
        answer.at("methodology").is_object()) {
        return answer.at("methodology");
    }
    return answer.is_object() ? answer : json(nullptr);
}

// One methodology, Orama's or the workspace's, from the engine: nullopt when it lists none by that
// id.
liquid::runtime::task<control::ZettaControlResult<std::optional<json>>> methodology_entry(
    ApiContext& context, std::string customer_id, std::string workspace_id, std::string id) {
    using Found = std::optional<json>;
    auto got = control::settled(
        co_await engine(context, customer_id, workspace_id, "get_methodology", true, {{"id", id}}));
    if (got) {
        json entry = entry_of(*got);
        if (entry.is_object() && entry.value("id", std::string()) == id) {
            co_return control::ZettaControlResult<Found>(Found{std::move(entry)});
        }
        co_return control::ZettaControlResult<Found>(Found{});
    }
    if (got.error().code == "not_found") {
        co_return control::ZettaControlResult<Found>(Found{});
    }
    if (got.error().code != "unknown_capability") {
        co_return control::ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    // An engine without get_methodology still lists its catalogue.
    auto listed = control::settled(co_await engine(
        context, customer_id, workspace_id, "list_methodologies", true, json::object()));
    if (!listed) {
        co_return control::ZettaControlResult<Found>(std::unexpected(listed.error()));
    }
    for (const json& entry : listed->value("methodologies", json::array())) {
        if (entry.is_object() && entry.value("id", std::string()) == id) {
            co_return control::ZettaControlResult<Found>(Found{entry});
        }
    }
    co_return control::ZettaControlResult<Found>(Found{});
}

// A catalogue entry marked read-only; a workspace entry marked editable, whatever the engine says.
json marked(json entry) {
    if (!entry.is_object()) {
        return entry;
    }
    const bool own = control::workspace_methodology_id(entry.value("id", std::string()));
    entry["source"] = own ? "workspace" : "orama";
    entry["editable"] = own;
    if (own) {
        entry["runnable"] = control::validated_for_revision(entry);
    }
    return entry;
}

liquid::runtime::task<ApiResult> refuse_orama_change(ApiContext& context,
                                                     const std::string& customer_id,
                                                     const std::string& workspace_id,
                                                     const std::string& id,
                                                     std::string_view verb) {
    auto entry =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (entry && !*entry) {
        co_return refused("not_found", "no such methodology in this workspace", "/id");
    }
    std::string named = id;
    if (entry && *entry) {
        if (const std::string name = (**entry).value("name", std::string()); !name.empty()) {
            named += " (" + name + ")";
        }
    }
    co_return refused("forbidden",
                      named +
                          " is one of Orama's methodologies, which are read-only and cannot be " +
                          std::string(verb) +
                          ". Duplicate it (duplicate_methodology) to make an editable methodology "
                          "of your own.",
                      "/id");
}

// --- The workspace's repositories ----------------------------------------------------------------

std::expected<control::Repository, ApiResult> workspace_repository(ApiContext& context,
                                                                   const std::string& customer_id,
                                                                   const std::string& workspace_id,
                                                                   const std::string& repository_id,
                                                                   const std::string& path) {
    const auto repository = context.store->get_repository(repository_id);
    if (!repository || repository->customer_id != customer_id ||
        repository->workspace_id != workspace_id || !repository->deleted_at.empty()) {
        return std::unexpected(refused("not_found", "no such repository in this workspace", path));
    }
    return *repository;
}

std::optional<ApiResult> no_repositories(const ApiContext& context) {
    if (!context.sync_store) {
        return refused("store_unavailable", "no repository store is configured");
    }
    return std::nullopt;
}

ApiResult sync_failed(const SyncStoreError& error) {
    return refused(error.code == "not_found" ? "not_found" : "store_error", error.message);
}

// The immutable change `ref` names (a branch such as main, or a change id).
std::expected<std::string, ApiResult> resolved_revision(ApiContext& context,
                                                        const control::Repository& repository,
                                                        const std::string& ref) {
    const auto refs = context.sync_store->repository_refs(repository.customer_id, repository.id);
    if (!refs) {
        return std::unexpected(sync_failed(refs.error()));
    }
    const std::string wanted = ref.empty() ? std::string("main") : ref;
    for (const RepositoryContentRef& entry : *refs) {
        if (entry.name == wanted) {
            return entry.change;
        }
    }
    if (ref.empty()) {
        return std::unexpected(refused("not_found", "the repository has no main branch yet"));
    }
    return wanted;
}

constexpr std::size_t max_source_files = 64;
constexpr std::size_t max_source_file_bytes = std::size_t{1} << 20;

// The methodology folder's files at `revision`, by path relative to the folder.
std::expected<std::map<std::string, std::string>, ApiResult>
source_files(ApiContext& context,
             const control::Repository& repository,
             const std::string& revision,
             const std::string& folder) {
    const auto tree =
        context.sync_store->repository_tree(repository.customer_id, repository.id, revision);
    if (!tree) {
        return std::unexpected(sync_failed(tree.error()));
    }
    std::string prefix = folder;
    while (prefix.starts_with('/')) {
        prefix.erase(0, 1);
    }
    while (prefix.ends_with('/')) {
        prefix.pop_back();
    }
    if (prefix == ".") {
        prefix.clear();
    }
    std::map<std::string, std::string> files;
    for (const RepositoryTreeEntry& entry : *tree) {
        if (entry.kind == "tree" || entry.kind == "folder" || entry.kind == "directory") {
            continue;
        }
        std::string relative = entry.path;
        while (relative.starts_with('/')) {
            relative.erase(0, 1);
        }
        if (!prefix.empty()) {
            if (!relative.starts_with(prefix + "/")) {
                continue;
            }
            relative.erase(0, prefix.size() + 1);
        }
        if (relative.empty() || relative.ends_with('/') ||
            relative.find("/.") != std::string::npos || relative.starts_with(".")) {
            continue;
        }
        if (files.size() >= max_source_files) {
            break;
        }
        if (entry.size > max_source_file_bytes) {
            files.emplace(relative, std::string());
            continue;
        }
        const auto file = context.sync_store->repository_file(
            repository.customer_id, repository.id, revision, entry.path);
        if (!file) {
            return std::unexpected(sync_failed(file.error()));
        }
        files.emplace(relative,
                      file->contents.empty() && !file->raw_contents.empty()
                          ? std::string(reinterpret_cast<const char*>(file->raw_contents.data()),
                                        file->raw_contents.size())
                          : file->contents);
    }
    return files;
}

struct FileChange {
    std::string path;
    std::optional<std::string> contents; // nullopt: delete it
};

// Commits the changes to the folder on `main`; answers the new change id.
std::expected<std::string, ApiResult> commit_files(ApiContext& context,
                                                   const control::Repository& repository,
                                                   const std::string& folder,
                                                   const std::vector<FileChange>& changes,
                                                   const std::string& message) {
    std::vector<RepositoryWriteOperation> operations;
    for (std::size_t index = 0; index < changes.size(); ++index) {
        const auto path = control::methodology_path(folder, changes[index].path);
        if (!path) {
            return std::unexpected(refused("invalid_input",
                                           "a file's path is inside the methodology's folder, "
                                           "without .. or a leading /",
                                           "/files/" + std::to_string(index) + "/path"));
        }
        RepositoryWriteOperation operation;
        operation.path = *path;
        if (changes[index].contents) {
            if (changes[index].contents->size() > max_source_file_bytes) {
                return std::unexpected(refused("invalid_input",
                                               "a file is larger than 1 MiB",
                                               "/files/" + std::to_string(index) + "/contents"));
            }
            operation.kind = RepositoryWriteKind::File;
            operation.contents.resize(changes[index].contents->size());
            std::ranges::transform(*changes[index].contents,
                                   operation.contents.begin(),
                                   [](char c) { return static_cast<std::byte>(c); });
        } else {
            operation.kind = RepositoryWriteKind::DeletedFile;
        }
        operations.push_back(std::move(operation));
    }
    const auto committed = context.sync_store->commit_repository_files(
        repository.customer_id, repository.id, "main", message, std::move(operations));
    if (!committed) {
        return std::unexpected(sync_failed(committed.error()));
    }
    return *committed;
}

std::string slug_of(std::string_view name) {
    std::string slug;
    for (const unsigned char c : name) {
        if (std::isalnum(c) != 0) {
            slug += static_cast<char>(std::tolower(c));
        } else if (!slug.empty() && slug.back() != '-') {
            slug += '-';
        }
        if (slug.size() >= 40) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? std::string("methodology") : slug;
}

// A new private repository in the workspace for a methodology's code, named after it.
liquid::runtime::task<std::expected<control::Repository, ApiResult>>
new_repository(ApiContext& context,
               const std::string& customer_id,
               const std::string& workspace_id,
               const std::string& name) {
    const std::string base = "methodology-" + slug_of(name);
    for (int attempt = 0; attempt < 12; ++attempt) {
        const std::string candidate =
            attempt == 0 ? base
                         : base + "-" +
                               (attempt < 10 ? std::to_string(attempt + 1)
                                             : control::new_uuid_v4().substr(0, 8));
        ApiResult created = finished(co_await call_capability(context,
                                                              "create_repository",
                                                              {{"customer_id", customer_id},
                                                               {"workspace_id", workspace_id},
                                                               {"name", candidate},
                                                               {"visibility", "private"}}));
        if (created.ok) {
            const std::string id = created.value.value("id", std::string());
            const auto repository = context.store->get_repository(id);
            if (!repository) {
                co_return std::expected<control::Repository, ApiResult>(
                    std::unexpected(store_failed(repository.error())));
            }
            co_return std::expected<control::Repository, ApiResult>(*repository);
        }
        if (created.error.code != "conflict") {
            co_return std::expected<control::Repository, ApiResult>(
                std::unexpected(std::move(created)));
        }
    }
    co_return std::expected<control::Repository, ApiResult>(std::unexpected(
        refused("conflict", "could not find a free repository name for this methodology")));
}

// --- Checking the code ---------------------------------------------------------------------------

struct Checked {
    std::optional<MethodologyManifest> manifest;
    std::vector<MethodologyFinding> findings;
};

Checked check_code(const std::map<std::string, std::string>& files) {
    Checked checked;
    const std::string manifest_name(control::methodology_manifest_file);
    const auto manifest_file = files.find(manifest_name);
    if (manifest_file == files.end()) {
        checked.findings.push_back({"manifest",
                                    false,
                                    "the folder has no " + manifest_name +
                                        "; it says what the methodology needs, its parameters "
                                        "and its metrics",
                                    manifest_name});
        return checked;
    }
    control::ManifestRead read = control::read_manifest(manifest_file->second);
    checked.findings = std::move(read.findings);
    checked.manifest = std::move(read.manifest);
    if (!checked.manifest) {
        return checked;
    }
    const MethodologyManifest& manifest = *checked.manifest;
    if (const auto source = files.find(manifest.entrypoint); source == files.end()) {
        checked.findings.push_back({"entrypoint",
                                    false,
                                    "the manifest names " + manifest.entrypoint +
                                        " as its entrypoint, and the folder has no such file",
                                    manifest.entrypoint});
    } else {
        for (MethodologyFinding& finding :
             control::check_entrypoint(source->second, manifest, manifest.entrypoint)) {
            checked.findings.push_back(std::move(finding));
        }
    }
    if (!manifest.packages.empty()) {
        if (const auto lock = files.find(manifest.packages); lock == files.end()) {
            checked.findings.push_back({"packages",
                                        false,
                                        "the manifest names " + manifest.packages +
                                            " as its lock file, and the folder has no such file "
                                            "(an empty one means the standard library only)",
                                        manifest.packages});
        } else {
            for (MethodologyFinding& finding :
                 control::check_lock(lock->second, manifest.packages)) {
                checked.findings.push_back(std::move(finding));
            }
        }
    }
    return checked;
}

// --- Runs ----------------------------------------------------------------------------------------

std::string run_lookup(const std::string& customer_id, const std::string& methodology_id) {
    return "customer:" + customer_id + ":methodology:" + methodology_id;
}

// What a person is told about a run: the phase from the operator, Lake's own words while it
// waits, and the record's outcome once the result is in.
json run_json(const control::MethodologyRun& run, const std::optional<json>& record) {
    std::string phase = run.phase.empty() ? std::string("Pending") : run.phase;
    std::string message;
    if (run.desired == "absent" && run.phase != "Succeeded" && run.phase != "Failed") {
        phase = "Stopped";
        message = run.stop_reason.empty() ? std::string("Stopped.") : run.stop_reason;
    } else if (run.phase.empty()) {
        message = "Waiting for the Orama operator to start this run.";
    } else if (run.phase == "Building") {
        message = "Building the methodology's image.";
    } else if (run.phase == "Queued") {
        // The operator holds it back (orama-operator#293): the workspace is at its cap of
        // concurrent runs, or no node of its GPU class exists yet. Its message says which.
        message = run.message.empty()
                      ? std::string("Queued: waiting for a place among this workspace's runs.")
                      : run.message;
    } else if (run.phase == "Staging") {
        message = "Handing the data to the sandbox.";
    } else if (run.phase == "Running") {
        message = "Running your code in its sandbox.";
    } else if (run.phase == "Collecting") {
        message = "Collecting the result.";
    } else {
        message = run.message;
    }
    json out{{"id", run.id},
             {"methodology_id", run.methodology_id},
             {"mode", run.mode},
             {"phase", phase},
             {"message", message},
             {"operator_phase", run.phase},
             {"operator_message", run.message},
             {"exit_code", run.exit_code ? json(*run.exit_code) : json(nullptr)},
             {"network", run.network},
             {"resources",
              {{"cpu_millis", run.cpu_millis},
               {"memory_bytes", run.memory_bytes},
               {"gpus", run.gpus},
               {"gpu_class", run.gpu_class},
               {"timeout_seconds", run.timeout_seconds}}},
             {"build_id", run.build_id.empty() ? json(nullptr) : json(run.build_id)},
             {"created_at", run.created_at},
             {"created_by", run.created_by_user_id},
             {"started_at", run.started_at.empty() ? json(nullptr) : json(run.started_at)},
             {"finished_at", run.finished_at.empty() ? json(nullptr) : json(run.finished_at)},
             {"stop_reason", run.stop_reason.empty() ? json(nullptr) : json(run.stop_reason)}};
    if (record && record->is_object()) {
        const json& body = *record;
        for (const char* key : {"dataset",
                                "target",
                                "features",
                                "params",
                                "holdout_fraction",
                                "revision",
                                "project",
                                "graph"}) {
            out[key] = body.value(key, json(nullptr));
        }
        const json result = body.value("result", json(nullptr));
        out["result"] = result;
        out["model_id"] = body.value("model_id", json(nullptr));
        if (result.is_object()) {
            out["outcome"] = result.value("status", std::string()) == "ok" ? "passed" : "failed";
        } else {
            out["outcome"] = nullptr;
        }
    }
    return out;
}

liquid::runtime::task<control::ZettaControlResult<std::optional<json>>>
run_record(ApiContext& context,
           const std::string& customer_id,
           const std::string& workspace_id,
           const std::string& run_id) {
    using Found = std::optional<json>;
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return control::ZettaControlResult<Found>(std::unexpected(records.error()));
    }
    auto got = control::settled(co_await records->get(run_records, run_id));
    if (!got) {
        co_return control::ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    if (!*got || (**got).body.value("customer_id", std::string()) != customer_id) {
        co_return control::ZettaControlResult<Found>(Found{});
    }
    co_return control::ZettaControlResult<Found>(Found{(**got).body});
}

// The methodology's image for its current code revision: the build recorded on its entry when it
// built this revision and did not fail, otherwise a new build by the build path apps use.
liquid::runtime::task<std::expected<control::Build, ApiResult>>
image_build(ApiContext& context,
            const std::string& customer_id,
            const std::string& workspace_id,
            const json& entry,
            const MethodologyManifest& manifest) {
    const json code = entry.value("code", json::object());
    const std::string repository = code.value("repository_id", std::string());
    const std::string revision = code.value("revision", std::string());
    const std::string build_id =
        entry.value("image", json::object()).value("build_id", std::string());
    if (!build_id.empty()) {
        const auto build = context.store->get_build(build_id);
        if (build && build->customer_id == customer_id && build->revision == revision &&
            build->phase != "Failed" && build->phase != "Cancelled" &&
            build->desired != "cancelled") {
            co_return std::expected<control::Build, ApiResult>(*build);
        }
    }
    if (context.methodology_runtime_image.empty()) {
        co_return std::expected<control::Build, ApiResult>(std::unexpected(refused(
            "store_unavailable",
            "workspace methodologies cannot run here: LAKE_METHODOLOGY_RUNTIME_IMAGE is not "
            "configured")));
    }
    const std::string id = entry.value("id", std::string());
    std::string tag = revision;
    tag.erase(std::remove_if(
                  tag.begin(), tag.end(), [](unsigned char c) { return std::isalnum(c) == 0; }),
              tag.end());
    tag = "rev-" + tag.substr(0, std::min<std::size_t>(tag.size(), 20)) + "-" +
          control::new_uuid_v4().substr(0, 6);
    std::string name = "methodology-" + id;
    std::ranges::transform(
        name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string folder = code.value("path", std::string());
    if (folder.empty()) {
        folder = ".";
    }
    ApiResult built = finished(co_await call_capability(
        context,
        "build_image",
        {{"customer_id", customer_id},
         {"workspace_id", workspace_id},
         {"repository", repository},
         {"revision", revision},
         {"context", folder},
         {"dockerfile", control::methodology_dockerfile(manifest.packages)},
         {"target", name + ":" + tag},
         {"build_args", {{"METHODOLOGY_RUNTIME_IMAGE", context.methodology_runtime_image}}}}));
    if (!built.ok) {
        co_return std::expected<control::Build, ApiResult>(std::unexpected(std::move(built)));
    }
    const auto build = context.store->get_build(built.value.value("build_id", std::string()));
    if (!build) {
        co_return std::expected<control::Build, ApiResult>(
            std::unexpected(store_failed(build.error())));
    }
    co_return std::expected<control::Build, ApiResult>(*build);
}

struct RunRequest {
    std::string mode;
    std::string dataset; // database.table; empty: the validation sample
    std::string target;
    json features = nullptr; // a list, or null: every column but the target
    json params = json::object();
    double holdout_fraction = 0.2;
    std::string project, graph;
};

// Queues one run: the dispatch row, then the sealed record of what it reads.
liquid::runtime::task<ApiResult> queue_run(ApiContext& context,
                                           const std::string& customer_id,
                                           const std::string& workspace_id,
                                           const json& entry,
                                           const MethodologyManifest& manifest,
                                           const control::Build& build,
                                           RunRequest request) {
    const std::string id = entry.value("id", std::string());
    const json code = entry.value("code", json::object());
    control::NewMethodologyRun row;
    row.customer_id = customer_id;
    row.workspace_id = workspace_id;
    row.methodology_id = id;
    row.mode = request.mode;
    row.build_id = build.id;
    if (build.phase == "Succeeded" && !build.digest.empty()) {
        row.image = control::build_json(build, context.public_host).value("image", std::string());
    }
    row.runtime_image = context.methodology_runtime_image;
    row.cpu_millis = manifest.resources.cpu_millis;
    row.memory_bytes = manifest.resources.memory_bytes;
    row.gpus = manifest.resources.gpus;
    row.gpu_class = manifest.resources.gpu_class;
    row.timeout_seconds = manifest.resources.timeout_seconds;
    if (request.mode == "validate") {
        // Validate runs a small sample: it never needs the whole allowance.
        row.timeout_seconds = std::min(row.timeout_seconds, 900);
    }
    row.network = entry.value("network", false) == true;
    row.created_by_user_id = context.acting_user_id;
    row.created_by_taap_id = context.acting_identity;
    const auto created = context.store->create_methodology_run(row);
    if (!created) {
        co_return store_failed(created.error());
    }
    json body{{"customer_id", customer_id},
              {"methodology_id", id},
              {"mode", request.mode},
              {"dataset", request.dataset.empty() ? json(nullptr) : json(request.dataset)},
              {"target", request.target.empty() ? json(nullptr) : json(request.target)},
              {"features", request.features},
              {"params", request.params},
              {"holdout_fraction", request.holdout_fraction},
              {"seed", 42},
              {"network", row.network},
              {"manifest", control::manifest_json(manifest)},
              {"repository_id", code.value("repository_id", std::string())},
              {"revision", code.value("revision", std::string())},
              {"project", request.project.empty() ? json(nullptr) : json(request.project)},
              {"graph", request.graph.empty() ? json(nullptr) : json(request.graph)},
              {"created_by", acting_author(context)},
              {"created_at", created->created_at},
              {"result", nullptr},
              {"model_id", nullptr}};
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        static_cast<void>(context.store->withdraw_methodology_run(
            created->id, "Lake could not keep this run's record: " + records.error().message));
        co_return failed(records.error());
    }
    auto written =
        control::settled(co_await records->put(run_records,
                                               created->id,
                                               run_lookup(customer_id, id),
                                               body,
                                               control::RecordCondition{.if_absent = true}));
    if (!written) {
        static_cast<void>(context.store->withdraw_methodology_run(
            created->id, "Lake could not keep this run's record: " + written.error().message));
        co_return failed(written.error());
    }
    context.audit_details["methodology_id"] = id;
    context.audit_details["run_id"] = created->id;
    co_return ApiResult::success({{"run", run_json(*created, body)}});
}

// Parameters as sent, checked against the manifest's, with the defaults filled in.
std::expected<json, ApiResult> checked_params(const MethodologyManifest& manifest,
                                              const json& sent) {
    json out = json::object();
    for (const control::MethodologyParameter& parameter : manifest.parameters) {
        out[parameter.name] = parameter.default_value;
    }
    if (sent.is_null()) {
        return out;
    }
    for (const auto& [name, value] : sent.items()) {
        const auto found =
            std::ranges::find(manifest.parameters, name, &control::MethodologyParameter::name);
        const std::string path = "/params/" + name;
        if (found == manifest.parameters.end()) {
            return std::unexpected(refused(
                "invalid_input", name + " is not one of this methodology's parameters", path));
        }
        bool fits = false;
        if (found->kind == "int") {
            fits = value.is_number_integer();
        } else if (found->kind == "float") {
            fits = value.is_number();
        } else if (found->kind == "bool") {
            fits = value.is_boolean();
        } else {
            fits = std::ranges::find(found->choices, value) != found->choices.end();
        }
        if (fits && value.is_number()) {
            const double number = value.get<double>();
            fits = (!found->minimum || number >= *found->minimum) &&
                   (!found->maximum || number <= *found->maximum);
        }
        if (!fits) {
            return std::unexpected(
                refused("invalid_input",
                        name + " takes a " + found->kind + " within its bounds or choices",
                        path));
        }
        out[name] = value;
    }
    return out;
}

// The manifest a workspace entry's code has at its current revision, with the static findings.
liquid::runtime::task<std::expected<Checked, ApiResult>>
checked_entry_code(ApiContext& context,
                   const std::string& customer_id,
                   const std::string& workspace_id,
                   const json& entry) {
    if (auto missing = no_repositories(context)) {
        co_return std::expected<Checked, ApiResult>(std::unexpected(*missing));
    }
    const json code = entry.value("code", json::object());
    const auto repository = workspace_repository(
        context, customer_id, workspace_id, code.value("repository_id", std::string()), "/id");
    if (!repository) {
        co_return std::expected<Checked, ApiResult>(
            std::unexpected(refused("conflict",
                                    "this methodology's repository is gone; delete the "
                                    "methodology or duplicate it from another",
                                    "/id")));
    }
    const auto files = source_files(context,
                                    *repository,
                                    code.value("revision", std::string()),
                                    code.value("path", std::string()));
    if (!files) {
        co_return std::expected<Checked, ApiResult>(std::unexpected(files.error()));
    }
    co_return std::expected<Checked, ApiResult>(check_code(*files));
}

// --- Capabilities --------------------------------------------------------------------------------

liquid::runtime::task<ApiResult> draft_methodology(const json& input, ApiContext&) {
    const std::string description = input.at("description");
    if (description.find_first_not_of(" \t\r\n") == std::string::npos) {
        co_return refused("invalid_input", "say what the methodology should do", "/description");
    }
    const control::MethodologyDraft draft = control::draft_methodology(
        description, input.value("name", std::string()), input.value("target_kind", std::string()));
    json files = json::array();
    for (const control::MethodologyFile& file : draft.files) {
        files.push_back({{"path", file.path}, {"contents", file.contents}});
    }
    co_return ApiResult::success({{"draft",
                                   {{"name", draft.manifest.name},
                                    {"template", draft.template_name},
                                    {"reason", draft.reason},
                                    {"manifest", control::manifest_json(draft.manifest)},
                                    {"files", std::move(files)}}}});
}

// Registers a workspace entry for the code at `revision`, with `extra` (duplicated_from, network).
liquid::runtime::task<ApiResult> register_entry(ApiContext& context,
                                                const std::string& customer_id,
                                                const std::string& workspace_id,
                                                const MethodologyManifest& manifest,
                                                const std::string& repository_id,
                                                const std::string& revision,
                                                const std::string& folder,
                                                json extra) {
    json body = control::registry_fields(manifest);
    body["code"] = {{"repository_id", repository_id}, {"revision", revision}, {"path", folder}};
    body["created_by"] = acting_author(context);
    for (auto& [key, value] : extra.items()) {
        body[key] = value;
    }
    auto registered = control::settled(
        co_await engine(context, customer_id, workspace_id, "register_methodology", false, body));
    if (!registered) {
        co_return failed(registered.error());
    }
    co_return ApiResult::success({{"methodology", marked(entry_of(*registered))}});
}

liquid::runtime::task<ApiResult> create_methodology(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return refused("store_unavailable", "workspaces are not configured");
    }
    if (auto missing = no_repositories(context)) {
        co_return *missing;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    std::string folder = input.value("path", std::string());
    if (!folder.empty() && !control::methodology_path("", folder)) {
        co_return refused("invalid_input", "path is a folder inside the repository", "/path");
    }
    control::Repository repository;
    std::string revision;
    std::vector<FileChange> changes;
    if (input.contains("repository_id")) {
        // Code that already lives in one of the workspace's repositories.
        if (input.contains("files")) {
            co_return refused("invalid_input",
                              "send files or name a repository_id with the code, not both",
                              "/files");
        }
        auto found = workspace_repository(
            context, customer_id, workspace_id, input.at("repository_id"), "/repository_id");
        if (!found) {
            co_return found.error();
        }
        repository = *found;
        auto resolved =
            resolved_revision(context, repository, input.value("revision", std::string()));
        if (!resolved) {
            co_return resolved.error();
        }
        revision = *resolved;
    } else {
        // New code: the files sent, or a draft from the description.
        if (input.contains("files")) {
            const json& files = input.at("files");
            for (std::size_t index = 0; index < files.size(); ++index) {
                if (files[index].value("delete", false)) {
                    continue;
                }
                if (!files[index].contains("contents")) {
                    co_return refused("invalid_input",
                                      "a file needs its contents",
                                      "/files/" + std::to_string(index) + "/contents");
                }
                changes.push_back(
                    {files[index].at("path"), files[index].at("contents").get<std::string>()});
            }
        } else {
            const std::string description = input.value("description", std::string());
            const std::string name = input.value("name", std::string());
            if (description.empty() && name.empty()) {
                co_return refused("invalid_input",
                                  "give the methodology a name or a description, its files, or "
                                  "the repository_id of its code",
                                  "/name");
            }
            const control::MethodologyDraft draft =
                control::draft_methodology(description.empty() ? name : description,
                                           name,
                                           input.value("target_kind", std::string()));
            for (const control::MethodologyFile& file : draft.files) {
                changes.push_back({file.path, file.contents});
            }
        }
        std::map<std::string, std::string> staged;
        for (const FileChange& change : changes) {
            staged[change.path] = change.contents.value_or(std::string());
        }
        const Checked probe = check_code(staged);
        if (!probe.manifest) {
            co_return refused(
                "invalid_input",
                "the methodology's manifest cannot be read: " +
                    (probe.findings.empty() ? std::string() : probe.findings.front().message),
                "/files");
        }
        auto created = settled_api(co_await new_repository(
            context, customer_id, workspace_id, input.value("name", probe.manifest->name)));
        if (!created) {
            co_return created.error();
        }
        repository = *created;
        auto committed = commit_files(
            context, repository, folder, changes, "Create the methodology " + probe.manifest->name);
        if (!committed) {
            co_return committed.error();
        }
        revision = *committed;
    }
    auto files = source_files(context, repository, revision, folder);
    if (!files) {
        co_return files.error();
    }
    Checked checked = check_code(*files);
    if (!checked.manifest) {
        std::string why;
        for (const MethodologyFinding& finding : checked.findings) {
            if (!finding.ok) {
                why += (why.empty() ? "" : "; ") + finding.message;
            }
        }
        co_return refused("invalid_input",
                          "the methodology's manifest (" +
                              std::string(control::methodology_manifest_file) +
                              ") cannot be read: " + why,
                          input.contains("repository_id") ? "/repository_id" : "/files");
    }
    if (const std::string name = input.value("name", std::string());
        !name.empty() && input.contains("repository_id")) {
        checked.manifest->name = name;
    }
    json extra{{"network", input.value("network", false)}};
    ApiResult registered = finished(co_await register_entry(context,
                                                            customer_id,
                                                            workspace_id,
                                                            *checked.manifest,
                                                            repository.id,
                                                            revision,
                                                            folder,
                                                            std::move(extra)));
    if (!registered.ok) {
        co_return registered;
    }
    registered.value["findings"] = findings_json(checked.findings);
    registered.value["ready_to_validate"] = control::all_ok(checked.findings);
    context.audit_details["methodology_id"] =
        registered.value["methodology"].value("id", std::string());
    context.audit_details["repository_id"] = repository.id;
    co_return registered;
}

liquid::runtime::task<ApiResult> get_methodology(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    auto entry =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!entry) {
        co_return failed(entry.error());
    }
    if (!*entry) {
        co_return refused("not_found", "no methodology " + id + " in this workspace", "/id");
    }
    json out{{"methodology", marked(**entry)}};
    if (input.value("include_source", false) && control::workspace_methodology_id(id)) {
        const json code = (**entry).value("code", json::object());
        json source{{"repository_id", code.value("repository_id", std::string())},
                    {"revision", code.value("revision", std::string())},
                    {"path", code.value("path", std::string())},
                    {"files", json::array()},
                    {"findings", json::array()},
                    {"available", false},
                    {"message", nullptr}};
        auto checked =
            settled_api(co_await checked_entry_code(context, customer_id, workspace_id, **entry));
        if (!checked) {
            source["message"] = checked.error().error.message;
        } else {
            const auto repository =
                context.store->get_repository(code.value("repository_id", std::string()));
            if (repository) {
                if (auto files = source_files(context,
                                              *repository,
                                              code.value("revision", std::string()),
                                              code.value("path", std::string()))) {
                    for (const auto& [path, contents] : *files) {
                        source["files"].push_back(
                            {{"path", path}, {"contents", contents}, {"size", contents.size()}});
                    }
                }
                source["repository_name"] = repository->name;
            }
            source["findings"] = findings_json(checked->findings);
            source["available"] = true;
        }
        out["source"] = std::move(source);
    }
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> update_methodology(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["methodology_id"] = id;
    if (!control::workspace_methodology_id(id)) {
        co_return finished(
            co_await refuse_orama_change(context, customer_id, workspace_id, id, "edited"));
    }
    if (auto missing = no_repositories(context)) {
        co_return *missing;
    }
    auto found =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return refused("not_found", "no such methodology in this workspace", "/id");
    }
    const json entry = **found;
    const json code = entry.value("code", json::object());
    const std::string folder = code.value("path", std::string());
    auto repository = workspace_repository(
        context, customer_id, workspace_id, code.value("repository_id", std::string()), "/id");
    if (!repository) {
        co_return refused("conflict", "this methodology's repository is gone", "/id");
    }
    std::string revision = code.value("revision", std::string());
    std::vector<FileChange> changes;
    if (input.contains("files")) {
        const json& files = input.at("files");
        for (std::size_t index = 0; index < files.size(); ++index) {
            if (files[index].value("delete", false)) {
                changes.push_back({files[index].at("path"), std::nullopt});
            } else if (!files[index].contains("contents")) {
                co_return refused("invalid_input",
                                  "a file needs its contents or delete: true",
                                  "/files/" + std::to_string(index) + "/contents");
            } else {
                changes.push_back(
                    {files[index].at("path"), files[index].at("contents").get<std::string>()});
            }
        }
    }
    // A new name or description is the manifest's: it is written to methodology.json, so the code
    // stays the one source of what the methodology is.
    if (input.contains("name") || input.contains("description")) {
        const auto current = source_files(context, *repository, revision, folder);
        if (!current) {
            co_return current.error();
        }
        std::string manifest_text;
        if (const auto staged = std::ranges::find(
                changes, std::string(control::methodology_manifest_file), &FileChange::path);
            staged != changes.end() && staged->contents) {
            manifest_text = *staged->contents;
        } else if (const auto file = current->find(std::string(control::methodology_manifest_file));
                   file != current->end()) {
            manifest_text = file->second;
        }
        json manifest = json::parse(manifest_text, nullptr, false);
        if (!manifest.is_object()) {
            co_return refused("conflict",
                              "the methodology's manifest cannot be read, so its name cannot be "
                              "changed; fix methodology.json first",
                              "/name");
        }
        if (input.contains("name")) {
            const std::string name = input.at("name");
            if (name.find_first_not_of(" \t\r\n") == std::string::npos || name.size() > 200) {
                co_return refused("invalid_input", "a name of 1 to 200 bytes", "/name");
            }
            manifest["name"] = name;
        }
        if (input.contains("description")) {
            manifest["description"] = input.at("description");
        }
        std::erase_if(changes, [](const FileChange& change) {
            return change.path == control::methodology_manifest_file;
        });
        changes.push_back(
            {std::string(control::methodology_manifest_file), manifest.dump(2) + "\n"});
    }
    if (!changes.empty()) {
        auto committed = commit_files(context,
                                      *repository,
                                      folder,
                                      changes,
                                      input.value("message", std::string("Edit the methodology")));
        if (!committed) {
            co_return committed.error();
        }
        revision = *committed;
    } else if (input.contains("revision")) {
        // Code pushed to the repository another way: follow it.
        auto resolved = resolved_revision(context, *repository, input.at("revision"));
        if (!resolved) {
            co_return resolved.error();
        }
        revision = *resolved;
    }
    json body{{"id", id}};
    std::vector<MethodologyFinding> findings;
    if (revision != code.value("revision", std::string())) {
        auto files = source_files(context, *repository, revision, folder);
        if (!files) {
            co_return files.error();
        }
        Checked checked = check_code(*files);
        findings = checked.findings;
        if (checked.manifest) {
            body.update(control::registry_fields(*checked.manifest));
        }
        body["code"] = {
            {"repository_id", repository->id}, {"revision", revision}, {"path", folder}};
        // A new revision has not been validated: the engine resets validation for it.
    }
    if (input.contains("network")) {
        body["network"] = input.at("network");
    }
    if (body.size() == 1) {
        co_return refused("invalid_input",
                          "send at least one change: files, name, description, revision or network",
                          "/files");
    }
    auto updated = control::settled(
        co_await engine(context, customer_id, workspace_id, "update_methodology", false, body));
    if (!updated) {
        co_return failed(updated.error());
    }
    context.audit_details["revision"] = revision;
    co_return ApiResult::success(
        {{"methodology", marked(entry_of(*updated))},
         {"findings", findings_json(findings)},
         {"ready_to_validate", findings.empty() || control::all_ok(findings)}});
}

liquid::runtime::task<ApiResult> delete_methodology(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["methodology_id"] = id;
    if (!control::workspace_methodology_id(id)) {
        co_return finished(
            co_await refuse_orama_change(context, customer_id, workspace_id, id, "deleted"));
    }
    auto found =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return refused("not_found", "no such methodology in this workspace", "/id");
    }
    // Its runs under way stop first: nothing may run code whose methodology is gone.
    if (const auto runs =
            context.store->list_methodology_runs(customer_id, workspace_id, id, 100)) {
        for (const control::MethodologyRun& run : *runs) {
            if (run.desired == "present") {
                static_cast<void>(context.store->withdraw_methodology_run(
                    run.id, "Stopped: its methodology was deleted."));
            }
        }
    }
    auto removed = control::settled(co_await engine(
        context, customer_id, workspace_id, "delete_methodology", false, {{"id", id}}));
    if (!removed) {
        co_return failed(removed.error());
    }
    const json code = (**found).value("code", json::object());
    co_return ApiResult::success(
        {{"deleted", true}, {"repository_id", code.value("repository_id", std::string())}});
}

liquid::runtime::task<ApiResult> duplicate_methodology(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return refused("store_unavailable", "workspaces are not configured");
    }
    if (auto missing = no_repositories(context)) {
        co_return *missing;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    const std::string requested = input.value("name", std::string());
    if (requested.size() > 200) {
        co_return refused("invalid_input", "a name is longer than 200 bytes", "/name");
    }
    auto found =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return refused("not_found", "no methodology " + id + " in this workspace", "/id");
    }
    const json entry = **found;
    std::vector<FileChange> changes;
    std::string name;
    json extra{{"duplicated_from", id}, {"network", false}};
    if (control::workspace_methodology_id(id)) {
        // One of the workspace's own: its files at its revision, its manifest renamed.
        const json code = entry.value("code", json::object());
        auto repository = workspace_repository(
            context, customer_id, workspace_id, code.value("repository_id", std::string()), "/id");
        if (!repository) {
            co_return refused("conflict", "this methodology's repository is gone", "/id");
        }
        auto files = source_files(context,
                                  *repository,
                                  code.value("revision", std::string()),
                                  code.value("path", std::string()));
        if (!files) {
            co_return files.error();
        }
        name =
            requested.empty() ? control::methodology_copy_name(entry.value("name", id)) : requested;
        for (const auto& [path, contents] : *files) {
            std::string copied = contents;
            if (path == control::methodology_manifest_file) {
                json manifest = json::parse(contents, nullptr, false);
                if (manifest.is_object()) {
                    manifest["name"] = name;
                    copied = manifest.dump(2) + "\n";
                }
            }
            changes.push_back({path, std::move(copied)});
        }
        extra["network"] = entry.value("network", false);
    } else {
        // One of Orama's: its description and parameter ranges, with a Python template, since it runs
        // natively and has no Python source.
        const control::MethodologyDraft draft = control::draft_from_catalogue(entry, requested);
        name = draft.manifest.name;
        for (const control::MethodologyFile& file : draft.files) {
            changes.push_back({file.path, file.contents});
        }
    }
    auto repository =
        settled_api(co_await new_repository(context, customer_id, workspace_id, name));
    if (!repository) {
        co_return repository.error();
    }
    auto committed =
        commit_files(context, *repository, "", changes, "Duplicate " + id + " as " + name);
    if (!committed) {
        co_return committed.error();
    }
    auto files = source_files(context, *repository, *committed, "");
    if (!files) {
        co_return files.error();
    }
    Checked checked = check_code(*files);
    if (!checked.manifest) {
        co_return refused(
            "conflict", "the copy's manifest cannot be read; the source's is broken", "/id");
    }
    ApiResult registered = finished(co_await register_entry(context,
                                                            customer_id,
                                                            workspace_id,
                                                            *checked.manifest,
                                                            repository->id,
                                                            *committed,
                                                            "",
                                                            std::move(extra)));
    if (!registered.ok) {
        co_return registered;
    }
    registered.value["findings"] = findings_json(checked.findings);
    context.audit_details["methodology_id"] =
        registered.value["methodology"].value("id", std::string());
    context.audit_details["duplicated_from"] = id;
    co_return registered;
}

liquid::runtime::task<ApiResult> validate_methodology(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["methodology_id"] = id;
    if (!control::workspace_methodology_id(id)) {
        co_return refused("invalid_input",
                          id + " is one of Orama's methodologies: it runs natively in the engine "
                               "and needs no validation",
                          "/id");
    }
    auto found =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return refused("not_found", "no such methodology in this workspace", "/id");
    }
    const json entry = **found;
    const std::string revision =
        entry.value("code", json::object()).value("revision", std::string());
    auto checked =
        settled_api(co_await checked_entry_code(context, customer_id, workspace_id, entry));
    if (!checked) {
        co_return checked.error();
    }
    const json findings = findings_json(checked->findings);
    if (!checked->manifest || !control::all_ok(checked->findings)) {
        // What is missing is known without running anything: say so, and record it.
        static_cast<void>(
            control::settled(co_await engine(context,
                                             customer_id,
                                             workspace_id,
                                             "update_methodology",
                                             false,
                                             {{"id", id},
                                              {"validation",
                                               {{"status", "failed"},
                                                {"findings", engine_findings(findings)},
                                                {"run_id", ""},
                                                {"revision", revision},
                                                {"validated_at", now_text()}}}})));
        co_return ApiResult::success({{"findings", findings}, {"ready", false}, {"run", nullptr}});
    }
    RunRequest request;
    request.mode = "validate";
    request.dataset = input.value("dataset", std::string());
    request.target = input.value("target", std::string());
    if (request.dataset.empty()) {
        request.target = checked->manifest->target_required ? "target" : "";
    } else if (checked->manifest->target_required && request.target.empty()) {
        co_return refused("invalid_input",
                          "this methodology has a target: name its column in the dataset",
                          "/target");
    }
    if (!request.dataset.empty() && request.dataset.find('.') == std::string::npos) {
        co_return refused("invalid_input", "dataset is <database>.<table>", "/dataset");
    }
    request.features = input.value("features", json(nullptr));
    auto params = checked_params(*checked->manifest, json(nullptr));
    request.params = *params;
    auto build = settled_api(
        co_await image_build(context, customer_id, workspace_id, entry, *checked->manifest));
    if (!build) {
        co_return build.error();
    }
    ApiResult queued = finished(co_await queue_run(
        context, customer_id, workspace_id, entry, *checked->manifest, *build, std::move(request)));
    if (!queued.ok) {
        co_return queued;
    }
    const std::string run_id = queued.value["run"].value("id", std::string());
    auto updated = control::settled(
        co_await engine(context,
                        customer_id,
                        workspace_id,
                        "update_methodology",
                        false,
                        {{"id", id},
                         {"image", {{"build_id", build->id}, {"reference", build->target}}},
                         {"validation",
                          {{"status", "running"},
                           {"findings", engine_findings(findings)},
                           {"run_id", run_id},
                           {"revision", revision}}}}));
    if (!updated) {
        co_return failed(updated.error());
    }
    co_return ApiResult::success(
        {{"findings", findings}, {"ready", true}, {"run", queued.value["run"]}});
}

liquid::runtime::task<ApiResult> run_methodology(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["methodology_id"] = id;
    if (!control::workspace_methodology_id(id)) {
        co_return refused("invalid_input",
                          id + " is one of Orama's methodologies: it runs in the engine, through a "
                               "pipeline (start_run)",
                          "/id");
    }
    auto found =
        control::settled(co_await methodology_entry(context, customer_id, workspace_id, id));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return refused("not_found", "no such methodology in this workspace", "/id");
    }
    const json entry = **found;
    if (!control::validated_for_revision(entry)) {
        co_return refused("conflict",
                          "validate this methodology first: its current code has not passed "
                          "Validate (validate_methodology)",
                          "/id");
    }
    const std::string dataset = input.at("dataset");
    if (dataset.find('.') == std::string::npos) {
        co_return refused("invalid_input", "dataset is <database>.<table>", "/dataset");
    }
    auto checked =
        settled_api(co_await checked_entry_code(context, customer_id, workspace_id, entry));
    if (!checked) {
        co_return checked.error();
    }
    if (!checked->manifest) {
        co_return refused("conflict", "the methodology's manifest cannot be read", "/id");
    }
    RunRequest request;
    request.mode = "train";
    request.dataset = dataset;
    request.target = input.value("target", std::string());
    if (checked->manifest->target_required && request.target.empty()) {
        co_return refused("invalid_input",
                          "this methodology has a target: name its column in the dataset",
                          "/target");
    }
    request.features = input.value("features", json(nullptr));
    auto params = checked_params(*checked->manifest, input.value("params", json(nullptr)));
    if (!params) {
        co_return params.error();
    }
    request.params = *params;
    request.holdout_fraction = input.value("holdout_fraction", 0.2);
    request.project = input.value("project", std::string());
    request.graph = input.value("graph", std::string());
    auto build = settled_api(
        co_await image_build(context, customer_id, workspace_id, entry, *checked->manifest));
    if (!build) {
        co_return build.error();
    }
    co_return finished(co_await queue_run(
        context, customer_id, workspace_id, entry, *checked->manifest, *build, std::move(request)));
}

// Read on the space, for list_methodology_runs, which authorizes itself as every list_ read does.
std::optional<ApiResult> authorize_space_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

liquid::runtime::task<ApiResult> list_methodology_runs(const json& input, ApiContext& context) {
    if (auto refusal = authorize_space_read(input, context)) {
        co_return *refusal;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const auto runs =
        context.store->list_methodology_runs(customer_id,
                                             workspace_id,
                                             input.value("methodology_id", std::string()),
                                             input.value("limit", 20));
    if (!runs) {
        co_return store_failed(runs.error());
    }
    json items = json::array();
    std::size_t read = 0;
    for (const control::MethodologyRun& run : *runs) {
        std::optional<json> record;
        // The newest twenty carry their outcome; the rest only their state.
        if (read++ < 20) {
            if (auto got = control::settled(
                    co_await run_record(context, customer_id, workspace_id, run.id));
                got) {
                record = *got;
            }
        }
        items.push_back(run_json(run, record));
    }
    co_return ApiResult::success({{"runs", std::move(items)}});
}

std::expected<control::MethodologyRun, ApiResult> workspace_run(ApiContext& context,
                                                                const std::string& customer_id,
                                                                const std::string& workspace_id,
                                                                const std::string& run_id) {
    const auto run = context.store->get_methodology_run(run_id);
    if (!run || run->customer_id != customer_id || run->workspace_id != workspace_id) {
        return std::unexpected(refused("not_found", "no such run in this workspace", "/run_id"));
    }
    return *run;
}

liquid::runtime::task<ApiResult> get_methodology_run(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto run = workspace_run(context, customer_id, workspace_id, input.at("run_id"));
    if (!run) {
        co_return run.error();
    }
    auto record =
        control::settled(co_await run_record(context, customer_id, workspace_id, run->id));
    if (!record) {
        co_return failed(record.error());
    }
    json out = run_json(*run, *record);
    if (*record) {
        out["log"] = (**record).value("log", json(nullptr));
    }
    co_return ApiResult::success({{"run", std::move(out)}});
}

liquid::runtime::task<ApiResult> stop_methodology_run(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto run = workspace_run(context, customer_id, workspace_id, input.at("run_id"));
    if (!run) {
        co_return run.error();
    }
    auto stopped = context.store->withdraw_methodology_run(run->id, "");
    if (!stopped) {
        co_return store_failed(stopped.error());
    }
    context.audit_details["run_id"] = run->id;
    co_return ApiResult::success({{"run", run_json(*stopped, std::nullopt)}});
}

// --- The runner's side ---------------------------------------------------------------------------

std::expected<control::MethodologyRun, ApiResult> own_run(ApiContext& context,
                                                          const std::string& run_id) {
    if (context.credential_kind != "methodology_run" || context.credential_workload_id != run_id) {
        return std::unexpected(
            refused("unauthorized", "only this run's own credential reaches its input and output"));
    }
    const auto run = context.store->get_methodology_run(run_id);
    if (!run || run->desired != "present") {
        return std::unexpected(refused("not_found", "this run is not running", "/run_id"));
    }
    context.audit_workspace_id = run->workspace_id;
    return *run;
}

std::string quoted_table(const std::string& dataset) {
    // The engine's SQL subset takes database.table names as they are; refuse anything else.
    const bool plain = std::ranges::all_of(dataset, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '.' || c == '-';
    });
    return plain && std::ranges::count(dataset, '.') == 1 ? dataset : std::string();
}

liquid::runtime::task<ApiResult> methodology_run_input(const json& input, ApiContext& context) {
    const std::string run_id = input.at("run_id");
    auto run = own_run(context, run_id);
    if (!run) {
        co_return run.error();
    }
    auto record = control::settled(
        co_await run_record(context, run->customer_id, run->workspace_id, run->id));
    if (!record) {
        co_return failed(record.error());
    }
    if (!*record) {
        co_return refused("not_found", "this run has no record", "/run_id");
    }
    const json& body = **record;
    const std::int64_t offset = std::max<std::int64_t>(input.value("offset", 0), 0);
    const std::int64_t limit =
        std::clamp<std::int64_t>(input.value("limit", control::max_methodology_input_page),
                                 1,
                                 control::max_methodology_input_page);
    const std::string dataset = body.value("dataset", json(nullptr)).is_string()
                                    ? body.at("dataset").get<std::string>()
                                    : std::string();
    const std::string mode = body.value("mode", std::string());
    std::string target = body.value("target", json(nullptr)).is_string()
                             ? body.at("target").get<std::string>()
                             : std::string();
    json rows = json::array();
    std::int64_t total = 0;
    std::vector<std::string> columns;
    if (dataset.empty()) {
        control::ManifestRead manifest =
            control::read_manifest(body.value("manifest", json::object()).dump());
        const auto sample =
            control::validation_sample(manifest.manifest.value_or(MethodologyManifest{}),
                                       control::methodology_validation_rows);
        total = static_cast<std::int64_t>(sample.size());
        for (std::int64_t index = offset; index < total && index < offset + limit; ++index) {
            rows.push_back(sample[static_cast<std::size_t>(index)]);
        }
        if (!sample.empty()) {
            for (const auto& [name, value] : sample.front().items()) {
                columns.push_back(name);
            }
        }
    } else {
        const std::string table = quoted_table(dataset);
        if (table.empty()) {
            co_return refused("invalid_input", "the run's dataset is not <database>.<table>");
        }
        const std::int64_t cap = mode == "validate" ? control::methodology_validation_rows
                                                    : control::max_methodology_train_rows;
        auto counted =
            control::settled(co_await engine(context,
                                             run->customer_id,
                                             run->workspace_id,
                                             "sql",
                                             true,
                                             {{"statement", "SELECT COUNT(*) FROM " + table}}));
        if (!counted) {
            co_return failed(counted.error());
        }
        const json first = counted->value("rows", json::array()).empty()
                               ? json::object()
                               : counted->at("rows").at(0);
        for (const auto& [name, value] : first.items()) {
            if (value.is_number()) {
                total = value.get<std::int64_t>();
            }
        }
        total = std::min(total, cap);
        const std::int64_t wanted = std::min(limit, std::max<std::int64_t>(total - offset, 0));
        if (wanted > 0) {
            auto page = control::settled(
                co_await engine(context,
                                run->customer_id,
                                run->workspace_id,
                                "sql",
                                true,
                                {{"statement",
                                  "SELECT * FROM " + table + " LIMIT " + std::to_string(wanted) +
                                      " OFFSET " + std::to_string(offset)}}));
            if (!page) {
                co_return failed(page.error());
            }
            rows = page->value("rows", json::array());
            for (const json& column : page->value("columns", json::array())) {
                if (column.is_string()) {
                    columns.push_back(column.get<std::string>());
                }
            }
        }
    }
    json features = body.value("features", json(nullptr));
    if (!features.is_array()) {
        features = json::array();
        for (const std::string& column : columns) {
            if (column != target) {
                features.push_back(column);
            }
        }
    }
    const std::int64_t next = offset + static_cast<std::int64_t>(rows.size());
    json spec{{"interface", 1},
              {"run_id", run->id},
              {"mode", mode},
              {"methodology_id", run->methodology_id},
              {"params", body.value("params", json::object())},
              {"target", target.empty() ? json(nullptr) : json(target)},
              {"features", std::move(features)},
              {"holdout_fraction", body.value("holdout_fraction", 0.2)},
              {"seed", body.value("seed", 42)},
              {"network", run->network},
              {"manifest", body.value("manifest", json::object())}};
    context.audit_details = {{"run_id", run->id}, {"offset", offset}, {"rows", rows.size()}};
    co_return ApiResult::success({{"spec", std::move(spec)},
                                  {"rows", std::move(rows)},
                                  {"next_offset", next < total ? json(next) : json(nullptr)},
                                  {"total_rows", total}});
}

liquid::runtime::task<ApiResult> methodology_run_output(const json& input, ApiContext& context) {
    const std::string run_id = input.at("run_id");
    auto run = own_run(context, run_id);
    if (!run) {
        co_return run.error();
    }
    const json result = input.at("result");
    std::string artifact;
    if (const std::string encoded = input.value("artifact_base64", std::string());
        !encoded.empty()) {
        // Base64 grows a payload by a third: the decoded bound is checked on the encoded length.
        if (encoded.size() / 4 * 3 > control::max_methodology_artifact_bytes + 3) {
            co_return refused("invalid_input",
                              "the artifact is larger than 24 MiB; a larger one is not kept yet",
                              "/artifact_base64");
        }
        artifact = encoded;
    }
    // An artifact over 24 MiB was sent first with methodology_run_artifact (lake#690); the output
    // names its size and SHA-256, which the engine checks against what it staged.
    json staged = nullptr;
    if (input.contains("artifact")) {
        if (!artifact.empty()) {
            co_return refused("invalid_input",
                              "send the artifact inline or in chunks, not both",
                              "/artifact");
        }
        const json& named = input.at("artifact");
        const json bytes = named.value("bytes", json(nullptr));
        const std::string digest = named.value("sha256", std::string());
        if (!bytes.is_number_integer() || bytes.get<std::int64_t>() <= 0 ||
            bytes.get<std::uint64_t>() > control::max_methodology_staged_artifact_bytes) {
            co_return refused("invalid_input",
                              "the artifact's size is 1 byte to 1 GiB",
                              "/artifact/bytes");
        }
        if (digest.size() != 64 || !std::ranges::all_of(digest, [](unsigned char c) {
                return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
            })) {
            co_return refused(
                "invalid_input", "the artifact's sha256 is 64 lowercase hex", "/artifact/sha256");
        }
        staged = {{"bytes", bytes}, {"sha256", digest}};
    }
    std::string log = input.value("log", std::string());
    if (log.size() > control::max_methodology_log_bytes) {
        log = log.substr(log.size() - control::max_methodology_log_bytes);
    }
    // The metrics a result may carry are numbers named in_sample_ or held_out_; anything else is
    // dropped and named, never stored as a metric.
    std::vector<std::string> dropped;
    json kept = result;
    kept["metrics"] = control::numeric_metrics(result.value("metrics", json::object()), &dropped);
    if (!dropped.empty()) {
        kept["dropped_metrics"] = dropped;
    }
    for (const char* bounded_key : {"error", "predictions_sample", "findings"}) {
        if (kept.contains(bounded_key) && kept.at(bounded_key).dump().size() > 64 * 1024) {
            kept[bounded_key] = nullptr;
        }
    }
    const bool ok = kept.value("status", std::string()) == "ok";
    auto records = workspace_records(context, run->customer_id, run->workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    // Accepted once: the record is written only while it has no result.
    bool second = false;
    json stored_body;
    auto updated = control::settled(co_await records->update(
        run_records,
        run->id,
        run_lookup(run->customer_id, run->methodology_id),
        [&](const std::optional<control::StoredRecord>& current) -> std::optional<json> {
            if (!current) {
                return std::nullopt;
            }
            json body = current->body;
            if (!body.value("result", json(nullptr)).is_null()) {
                second = true;
                return body;
            }
            body["result"] = kept;
            body["log"] = log;
            body["finished_at"] = now_text();
            stored_body = body;
            return body;
        }));
    if (!updated) {
        co_return failed(updated.error());
    }
    if (!*updated) {
        co_return refused("not_found", "this run has no record", "/run_id");
    }
    if (second) {
        co_return refused("conflict", "this run's result is already recorded", "/run_id");
    }
    const std::string mode = stored_body.value("mode", std::string());
    json model_id = nullptr;
    if (mode == "validate") {
        // Validate: the static findings and the sandbox's, for the revision it ran.
        json findings = engine_findings(kept.value("findings", json::array()));
        auto recorded = control::settled(
            co_await engine(context,
                            run->customer_id,
                            run->workspace_id,
                            "update_methodology",
                            false,
                            {{"id", run->methodology_id},
                             {"validation",
                              {{"status", ok ? "passed" : "failed"},
                               {"findings", std::move(findings)},
                               {"run_id", run->id},
                               {"revision", stored_body.value("revision", std::string())},
                               {"validated_at", now_text()}}}}));
        if (!recorded) {
            co_return failed(recorded.error());
        }
    } else if (ok) {
        json body{{"methodology", run->methodology_id},
                  {"run_id", run->id},
                  {"mode", mode},
                  {"dataset_path", stored_body.value("dataset", json(nullptr))},
                  {"params", stored_body.value("params", json::object())},
                  {"metrics", kept.value("metrics", json::object())},
                  {"rows", kept.value("rows", json::object())}};
        if (!artifact.empty()) {
            body["artifact_base64"] = artifact;
            body["artifact_format"] = kept.value("artifact_format", std::string("pickle"));
        } else if (!staged.is_null()) {
            body["artifact_staged"] = staged;
            body["artifact_format"] = kept.value("artifact_format", std::string("pickle"));
        }
        for (const char* key : {"project", "graph"}) {
            if (stored_body.value(key, json(nullptr)).is_string()) {
                body[key] = stored_body.at(key);
            }
        }
        auto recorded = control::settled(co_await engine(context,
                                                         run->customer_id,
                                                         run->workspace_id,
                                                         "record_methodology_run",
                                                         false,
                                                         std::move(body)));
        if (!recorded) {
            co_return failed(recorded.error());
        }
        model_id = recorded->value("model_id", json(nullptr));
        if (!model_id.is_null()) {
            static_cast<void>(control::settled(co_await records->update(
                run_records,
                run->id,
                run_lookup(run->customer_id, run->methodology_id),
                [&](const std::optional<control::StoredRecord>& current) -> std::optional<json> {
                    if (!current) {
                        return std::nullopt;
                    }
                    json body = current->body;
                    body["model_id"] = model_id;
                    return body;
                })));
        }
    }
    context.audit_details = {
        {"run_id", run->id}, {"status", ok ? "ok" : "failed"}, {"model_id", model_id}};
    co_return ApiResult::success({{"recorded", true}, {"model_id", model_id}});
}

// One chunk of a large artifact (lake#690), before the output: forwarded in order to the engine,
// which stages it until record_methodology_run adopts it as the model's weights.
liquid::runtime::task<ApiResult> methodology_run_artifact(const json& input, ApiContext& context) {
    const std::string run_id = input.at("run_id");
    auto run = own_run(context, run_id);
    if (!run) {
        co_return run.error();
    }
    if (run->mode != "train") {
        co_return refused("invalid_input", "only a training run keeps an artifact", "/run_id");
    }
    const std::string encoded = input.at("chunk_base64");
    if (encoded.empty() ||
        encoded.size() / 4 * 3 > control::methodology_artifact_chunk_bytes + 3) {
        co_return refused("invalid_input",
                          "a chunk is 1 byte to 8 MiB, base64",
                          "/chunk_base64");
    }
    const std::int64_t offset = input.at("offset").get<std::int64_t>();
    if (offset < 0 || static_cast<std::uint64_t>(offset) + encoded.size() / 4 * 3 >
                          control::max_methodology_staged_artifact_bytes + 3) {
        co_return refused("invalid_input", "an artifact is at most 1 GiB", "/offset");
    }
    auto record = control::settled(
        co_await run_record(context, run->customer_id, run->workspace_id, run->id));
    if (!record) {
        co_return failed(record.error());
    }
    if (!*record) {
        co_return refused("not_found", "this run has no record", "/run_id");
    }
    if (!(**record).value("result", json(nullptr)).is_null()) {
        co_return refused("conflict", "this run's result is already recorded", "/run_id");
    }
    auto appended = control::settled(co_await engine(context,
                                                     run->customer_id,
                                                     run->workspace_id,
                                                     "append_methodology_artifact",
                                                     false,
                                                     {{"methodology", run->methodology_id},
                                                      {"run_id", run->id},
                                                      {"offset", offset},
                                                      {"chunk_base64", encoded}}));
    if (!appended) {
        co_return failed(appended.error());
    }
    const json received = appended->value("received", json(nullptr));
    context.audit_details = {{"run_id", run->id}, {"offset", offset}, {"received", received}};
    co_return ApiResult::success({{"received", received}});
}

// --- Serving a workspace methodology's model (lake#690) -----------------------------------------

// The workspace methodology a model's kind names (`python:W-<8 hex>`), or empty.
std::string served_methodology_of(const json& model) {
    const std::string kind = model.is_object() ? model.value("kind", std::string()) : std::string();
    constexpr std::string_view prefix = "python:";
    if (!kind.starts_with(prefix)) {
        return {};
    }
    const std::string id = kind.substr(prefix.size());
    return control::workspace_methodology_id(id) ? id : std::string();
}

// What a person is told about a deployment: the operator's phase, in Lake's words while it waits.
json deployment_json(const control::MethodologyDeployment& deployment) {
    std::string phase = deployment.phase.empty() ? std::string("Pending") : deployment.phase;
    std::string message;
    if (deployment.desired == "absent") {
        phase = "Stopped";
        message = deployment.stop_reason.empty() ? std::string("Not served.")
                                                 : deployment.stop_reason;
    } else if (deployment.phase.empty()) {
        message = "Waiting for the Orama operator to start serving this model.";
    } else if (deployment.phase == "Building") {
        message = "Building the methodology's image.";
    } else if (deployment.phase == "Queued") {
        message = deployment.message.empty() ? std::string("Queued.") : deployment.message;
    } else if (deployment.phase == "Staging") {
        message = "Handing the model to its server.";
    } else if (deployment.phase == "Starting") {
        message = "Starting the server: checking its sandbox and loading the model.";
    } else if (deployment.phase == "Ready") {
        message = "Serving predictions.";
    } else {
        message = deployment.message;
    }
    return {{"id", deployment.id},
            {"model_id", deployment.model_id},
            {"methodology_id", deployment.methodology_id},
            {"run_id", deployment.run_id.empty() ? json(nullptr) : json(deployment.run_id)},
            {"phase", phase},
            {"message", message},
            {"operator_phase", deployment.phase},
            {"operator_message", deployment.message},
            {"ready", deployment.desired == "present" && deployment.phase == "Ready"},
            {"network", deployment.network},
            {"resources",
             {{"cpu_millis", deployment.cpu_millis},
              {"memory_bytes", deployment.memory_bytes},
              {"gpus", deployment.gpus},
              {"gpu_class", deployment.gpu_class}}},
            {"created_at", deployment.created_at},
            {"created_by", deployment.created_by_user_id},
            {"ready_at", deployment.ready_at.empty() ? json(nullptr) : json(deployment.ready_at)},
            {"stopped_at",
             deployment.stopped_at.empty() ? json(nullptr) : json(deployment.stopped_at)}};
}

// The present deployment of a model in a workspace, if any.
std::optional<control::MethodologyDeployment> present_deployment(ApiContext& context,
                                                                 const std::string& customer_id,
                                                                 const std::string& workspace_id,
                                                                 const std::string& model_id) {
    const auto deployments =
        context.store->list_methodology_deployments(customer_id, workspace_id, model_id, 20);
    if (!deployments) {
        return std::nullopt;
    }
    for (const control::MethodologyDeployment& deployment : *deployments) {
        if (deployment.desired == "present") {
            return deployment;
        }
    }
    return std::nullopt;
}

// Starts serving a model: one deployment row from the training run that made it (its image, its
// resources, its network grant). Answers the deployment.
liquid::runtime::task<ApiResult> start_serving(ApiContext& context,
                                               const std::string& customer_id,
                                               const std::string& workspace_id,
                                               const json& model) {
    const std::string model_id = model.value("id", model.value("model_id", std::string()));
    const std::string methodology_id = served_methodology_of(model);
    if (auto existing = present_deployment(context, customer_id, workspace_id, model_id)) {
        co_return ApiResult::success({{"deployment", deployment_json(*existing)}});
    }
    if (context.methodology_runtime_image.empty()) {
        co_return refused("store_unavailable",
                          "workspace methodologies cannot be served here: "
                          "LAKE_METHODOLOGY_RUNTIME_IMAGE is not configured",
                          "/model_id");
    }
    const json schema = model.value("feature_schema", json::object());
    const std::string lake_run = schema.is_object() ? schema.value("lake_run_id", std::string())
                                                    : std::string();
    const auto run = lake_run.empty() ? control::Result<control::MethodologyRun>(std::unexpected(
                                            control::StoreError{"not_found", "no run"}))
                                      : context.store->get_methodology_run(lake_run);
    if (!run || run->customer_id != customer_id || run->workspace_id != workspace_id ||
        run->build_id.empty()) {
        co_return refused("conflict",
                          "this model's training run is not in Lake any more, so the image that "
                          "serves it is unknown; train it again with run_methodology",
                          "/model_id");
    }
    control::NewMethodologyDeployment row;
    row.customer_id = customer_id;
    row.workspace_id = workspace_id;
    row.methodology_id = methodology_id;
    row.model_id = model_id;
    row.run_id = run->id;
    row.build_id = run->build_id;
    row.image = run->image;
    row.runtime_image = context.methodology_runtime_image;
    row.cpu_millis = run->cpu_millis;
    row.memory_bytes = run->memory_bytes;
    row.gpus = run->gpus;
    row.gpu_class = run->gpu_class;
    row.network = run->network;
    row.created_by_user_id = context.acting_user_id;
    row.created_by_taap_id = context.acting_identity;
    const auto created = context.store->create_methodology_deployment(row);
    if (!created) {
        if (created.error().code == "conflict") {
            if (auto existing = present_deployment(context, customer_id, workspace_id, model_id)) {
                co_return ApiResult::success({{"deployment", deployment_json(*existing)}});
            }
        }
        co_return store_failed(created.error());
    }
    co_return ApiResult::success({{"deployment", deployment_json(*created)}});
}

liquid::runtime::task<ApiResult> get_methodology_deployment(const json& input,
                                                            ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string model_id = input.at("model_id");
    const auto deployments =
        context.store->list_methodology_deployments(customer_id, workspace_id, model_id, 1);
    if (!deployments) {
        co_return store_failed(deployments.error());
    }
    co_return ApiResult::success(
        {{"deployment",
          deployments->empty() ? json(nullptr) : deployment_json(deployments->front())}});
}

liquid::runtime::task<ApiResult> list_methodology_deployments(const json& input,
                                                              ApiContext& context) {
    if (auto refusal = authorize_space_read(input, context)) {
        co_return *refusal;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const auto deployments = context.store->list_methodology_deployments(
        customer_id, workspace_id, "", input.value("limit", 50));
    if (!deployments) {
        co_return store_failed(deployments.error());
    }
    json out = json::array();
    for (const control::MethodologyDeployment& deployment : *deployments) {
        out.push_back(deployment_json(deployment));
    }
    co_return ApiResult::success({{"deployments", std::move(out)}});
}

// The stage Job's side (lake#690), with the deployment's own credential: the spec its server
// needs and the model's artifact, read from the engine 8 MiB at a time.
liquid::runtime::task<ApiResult> methodology_deployment_input(const json& input,
                                                              ApiContext& context) {
    const std::string deployment_id = input.at("deployment_id");
    if (context.credential_kind != "methodology_deployment" ||
        context.credential_workload_id != deployment_id) {
        co_return refused("unauthorized",
                          "only this deployment's own credential reaches its input");
    }
    const auto deployment = context.store->get_methodology_deployment(deployment_id);
    if (!deployment || deployment->desired != "present") {
        co_return refused("not_found", "this deployment is not present", "/deployment_id");
    }
    context.audit_workspace_id = deployment->workspace_id;
    const std::int64_t offset = std::max<std::int64_t>(input.value("offset", 0), 0);
    const std::int64_t limit = std::clamp<std::int64_t>(
        input.value("limit", static_cast<std::int64_t>(control::methodology_artifact_chunk_bytes)),
        1,
        static_cast<std::int64_t>(control::methodology_artifact_chunk_bytes));
    auto weights = control::settled(co_await engine(context,
                                                    deployment->customer_id,
                                                    deployment->workspace_id,
                                                    "read_model_weights",
                                                    true,
                                                    {{"model_id", deployment->model_id},
                                                     {"offset", offset},
                                                     {"limit", limit}}));
    if (!weights) {
        co_return failed(weights.error());
    }
    auto model = control::settled(co_await engine(context,
                                                  deployment->customer_id,
                                                  deployment->workspace_id,
                                                  "get_model",
                                                  true,
                                                  {{"model_id", deployment->model_id}}));
    if (!model) {
        co_return failed(model.error());
    }
    const json schema = model->value("feature_schema", json::object());
    json manifest = json::object();
    if (!deployment->run_id.empty()) {
        auto record = control::settled(co_await run_record(
            context, deployment->customer_id, deployment->workspace_id, deployment->run_id));
        if (record && *record) {
            manifest = (**record).value("manifest", json::object());
        }
    }
    json spec{{"interface", 1},
              {"deployment_id", deployment->id},
              {"model_id", deployment->model_id},
              {"methodology_id", deployment->methodology_id},
              {"artifact_format",
               schema.is_object() ? schema.value("artifact_format", std::string("pickle"))
                                  : std::string("pickle")},
              {"features",
               schema.is_object() ? schema.value("features", json::array()) : json::array()},
              {"network", deployment->network},
              {"manifest", std::move(manifest)}};
    context.audit_details = {{"deployment_id", deployment->id}, {"offset", offset}};
    co_return ApiResult::success({{"spec", std::move(spec)},
                                  {"chunk_base64", weights->value("chunk_base64", std::string())},
                                  {"offset", offset},
                                  {"next_offset", weights->value("next_offset", json(nullptr))},
                                  {"total_bytes", weights->value("total_bytes", 0)},
                                  {"sha256", weights->value("sha256", std::string())}});
}

std::expected<std::string, ApiError> deployment_object(const json&, ApiContext& context) {
    if (context.credential_kind != "methodology_deployment") {
        return std::unexpected(ApiError{
            "unauthorized", "only a methodology deployment's own credential calls this", {}});
    }
    return std::string("methodology_deployment/") + context.credential_workload_id;
}

// --- Registration --------------------------------------------------------------------------------

json methodology_schema() {
    return {{"type", "object"},
            {"description",
             "a methodology as the workspace's engine lists it, with `source` (orama or "
             "workspace) and `editable`; a workspace's own also carries its code, image and "
             "validation"},
            {"properties",
             {{"id", text()},
              {"source", {{"type", "string"}, {"enum", {"orama", "workspace"}}}},
              {"editable", {{"type", "boolean"}}},
              {"name", text()},
              {"runnable", {{"type", "boolean"}}}}},
            {"required", json::array({"id", "source", "editable"})}};
}

json findings_schema() {
    return {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"check", text()},
                {"ok", {{"type", "boolean"}}},
                {"message", text()},
                {"file", text()}}},
              {"required", json::array({"check", "ok", "message"})}}}};
}

json run_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", text()},
              {"methodology_id", text()},
              {"mode", {{"type", "string"}, {"enum", {"validate", "train"}}}},
              {"phase", text()},
              {"message", text()}}},
            {"required", json::array({"id", "methodology_id", "mode", "phase", "message"})}};
}

Capability
capability(std::string name,
           std::string title,
           std::string description,
           json input_schema,
           json output_schema,
           bool read,
           std::function<liquid::runtime::task<ApiResult>(const json&, ApiContext&)> handler) {
    return Capability{.name = std::move(name),
                      .title = std::move(title),
                      .description = std::move(description),
                      .input_schema = std::move(input_schema),
                      .output_schema = std::move(output_schema),
                      .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
                      .object = "customer",
                      .action = read ? "read" : "write",
                      .resolve_object = customer_object,
                      .granted_by_scope = read ? "workspace_query" : "workspace_act",
                      .async_handler = std::move(handler)};
}

} // namespace

// Ship → Models publishing a model (lake#690): for a workspace methodology's model, publish starts
// serving it and unpublish (or delete) stops it. Answers {deployment} for such a model, null for
// any other, or the refusal.
liquid::runtime::task<ApiResult> serve_methodology_model(ApiContext& context,
                                                         const std::string& customer_id,
                                                         const std::string& workspace_id,
                                                         const std::string& model_id,
                                                         bool serve) {
    if (!context.store) {
        co_return ApiResult::success(nullptr);
    }
    if (!serve) {
        // Unpublished or deleted: stop whatever serves it, whatever its kind says now.
        auto existing = present_deployment(context, customer_id, workspace_id, model_id);
        if (!existing) {
            co_return ApiResult::success(nullptr);
        }
        auto stopped = context.store->withdraw_methodology_deployment(existing->id, "Unpublished.");
        if (!stopped) {
            co_return store_failed(stopped.error());
        }
        co_return ApiResult::success({{"deployment", deployment_json(*stopped)}});
    }
    auto model = control::settled(
        co_await engine(context, customer_id, workspace_id, "get_model", true,
                        {{"model_id", model_id}}));
    if (!model) {
        co_return failed(model.error());
    }
    if (served_methodology_of(*model).empty()) {
        co_return ApiResult::success(nullptr);
    }
    json described = *model;
    described["id"] = model_id;
    co_return co_await start_serving(context, customer_id, workspace_id, described);
}

// predict for a workspace methodology's model (lake#690): the rows go to its serve pod through
// the deployment's Service and its answer comes back, one prediction per row. `model` is the
// engine's get_model answer. Answers null when the model is not a workspace methodology's.
liquid::runtime::task<std::optional<ApiResult>>
predict_methodology_model(ApiContext& context,
                          const std::string& customer_id,
                          const std::string& workspace_id,
                          const json& model,
                          const json& input) {
    const std::string model_id = input.value("model_id", std::string());
    if (served_methodology_of(model).empty()) {
        co_return std::nullopt;
    }
    const json& rows = input.at("rows");
    if (rows.size() > control::max_methodology_predict_rows) {
        co_return refused("invalid_input",
                          "at most 10000 rows in one call to a workspace methodology's model",
                          "/rows");
    }
    if (!context.methodology_server) {
        co_return refused("not_supported",
                          "this Lake serves no workspace methodology's model");
    }
    const auto deployment = present_deployment(context, customer_id, workspace_id, model_id);
    if (!deployment) {
        co_return refused("conflict",
                          "model " + model_id +
                              " is published, but nothing serves it; publish it again",
                          "/model_id");
    }
    if (deployment->phase != "Ready" ||
        !control::methodology_deployment_endpoint_valid(deployment->endpoint)) {
        const json described = deployment_json(*deployment);
        co_return refused("conflict",
                          "model " + model_id + " is not serving yet (" +
                              described.value("phase", std::string()) + "): " +
                              described.value("message", std::string()),
                          "/model_id");
    }
    MethodologyServeRequest request{
        deployment->endpoint, "/predict", json{{"rows", rows}}.dump()};
    struct Served {
        MethodologyServeResponse response;
    };
    auto ran = co_await run_blocking(
        [server = context.methodology_server, request = std::move(request)]() -> Served {
            return {server(request)};
        });
    if (!ran) {
        co_return refused("busy", ran.error().message);
    }
    const MethodologyServeResponse& response = ran->response;
    if (!response.error.empty() || response.status == 0) {
        co_return refused("store_unavailable",
                          "the model's server did not answer: " + response.error);
    }
    const json answer = json::parse(response.body, nullptr, false);
    if (response.status != 200) {
        std::string message = "the model's server answered " + std::to_string(response.status);
        if (answer.is_object() && answer.contains("error") && answer.at("error").is_object()) {
            message = answer.at("error").value("message", message);
        }
        co_return refused(response.status >= 500 ? std::string("store_unavailable")
                                                 : std::string("invalid_input"),
                          message,
                          "/rows");
    }
    if (!answer.is_object() || !answer.contains("predictions") ||
        !answer.at("predictions").is_array() || answer.at("predictions").size() != rows.size()) {
        co_return refused("store_unavailable",
                          "the model's server did not answer one prediction per row");
    }
    const std::string key = input.value("entity_column", std::string("entity"));
    json predictions = json::array();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        json entry{{"row", index}, {"prediction", answer.at("predictions").at(index)}};
        if (rows.at(index).is_object() && rows.at(index).contains(key)) {
            entry["entity"] = rows.at(index).at(key);
        }
        predictions.push_back(std::move(entry));
    }
    context.audit_details["deployment_id"] = deployment->id;
    co_return ApiResult::success({{"model", model_id},
                                  {"runner", "methodology"},
                                  {"methodology_id", deployment->methodology_id},
                                  {"predictions", std::move(predictions)},
                                  {"entity_column", key}});
}

void register_methodology_capabilities(Registry& registry) {
    const json id{{"type", "string"},
                  {"description",
                   "a methodology's id: M-1, … for Orama's, W-<8 hex> for one of the workspace's "
                   "own"}};
    const json one{{"type", "object"},
                   {"properties",
                    {{"methodology", methodology_schema()},
                     {"findings", findings_schema()},
                     {"ready_to_validate", {{"type", "boolean"}}},
                     {"source", {{"type", "object"}}}}},
                   {"required", json::array({"methodology"})}};
    const json one_run{{"type", "object"},
                       {"properties", {{"run", run_schema()}}},
                       {"required", json::array({"run"})}};
    Capability draft = capability(
        "draft_methodology",
        "Draft a methodology",
        "A first version of a methodology's files from a description of what it should do: "
        "methodology.json, methodology.py with fit, predict and score, and an empty lock file. "
        "The template follows the problem the description names (groups with no target, an "
        "amount, one of several categories, otherwise a yes-or-no outcome), or `target_kind`. "
        "Nothing is saved: pass the files to create_methodology.",
        workspace_input(
            {{"description", text()},
             {"name", text()},
             {"target_kind",
              {{"type", "string"}, {"enum", {"binary", "multiclass", "regression", "none"}}}}},
            json::array({"description"})),
        {{"type", "object"},
         {"properties", {{"draft", {{"type", "object"}}}}},
         {"required", json::array({"draft"})}},
        true,
        draft_methodology);
    draft.post_alias = true;
    registry.add(std::move(draft));
    registry.add(capability(
        "create_methodology",
        "Create a methodology",
        "Create a methodology of the workspace's own from Python code, kept in a workspace "
        "repository so it is versioned and reviewable. Send `files` ({path, contents}: "
        "methodology.json, the entrypoint, the lock file), or a `name` and `description` to start "
        "from a draft, and Lake creates a private repository `methodology-<name>` and commits "
        "them; or name the `repository_id` (and `path`, `revision`) of code already there. The "
        "engine registers it next to Orama's as W-<8 hex>, not yet validated. `findings` is what "
        "Validate will refuse, read without running anything. `network` (default false) lets its "
        "runs reach the internet.",
        workspace_input(
            {{"name", text()},
             {"description", text()},
             {"target_kind",
              {{"type", "string"}, {"enum", {"binary", "multiclass", "regression", "none"}}}},
             {"files", files_input()},
             {"repository_id", text()},
             {"path", text()},
             {"revision", text()},
             {"network", {{"type", "boolean"}}}},
            json::array()),
        one,
        false,
        create_methodology));
    registry.add(capability(
        "get_methodology",
        "Read a methodology",
        "One methodology, Orama's or the workspace's own, marked with `source` and `editable`. "
        "With `include_source`, a workspace methodology's files at its current revision and "
        "what Validate would refuse.",
        workspace_input({{"id", id}, {"include_source", {{"type", "boolean"}}}},
                        json::array({"id"})),
        one,
        true,
        get_methodology));
    registry.add(capability(
        "update_methodology",
        "Edit a methodology",
        "Change one of the workspace's own methodologies: `files` are committed to its "
        "repository (a path with `delete: true` removes that file), a new `name` or "
        "`description` is written to its manifest, `revision` follows code pushed another way, "
        "and `network` sets whether its runs reach the internet. A new revision must be validated "
        "again before it runs. Orama's methodologies are read-only: `forbidden`, with the reason "
        "(duplicate_methodology makes an editable copy).",
        workspace_input({{"id", id},
                         {"files", files_input()},
                         {"message", text()},
                         {"name", text()},
                         {"description", text()},
                         {"revision", text()},
                         {"network", {{"type", "boolean"}}}},
                        json::array({"id"})),
        one,
        false,
        update_methodology));
    registry.add(capability(
        "delete_methodology",
        "Delete a methodology",
        "Delete one of the workspace's own methodologies from the registry and stop its runs. "
        "Its repository, models and run records are kept. Orama's are read-only: `forbidden`.",
        workspace_input({{"id", id}}, json::array({"id"})),
        {{"type", "object"},
         {"properties",
          {{"deleted", {{"type", "boolean"}, {"const", true}}}, {"repository_id", text()}}},
         {"required", json::array({"deleted"})}},
        false,
        delete_methodology));
    registry.add(capability(
        "duplicate_methodology",
        "Duplicate a methodology",
        "Copy a methodology into a new editable one of the workspace's own, in a new repository, "
        "named `name` or \"<name> (copy)\". From the workspace's own: its files, Python source "
        "included. From Orama's: its description and the ranges of its parameters, with a Python template for "
        "its kind of target, since Orama's run natively in the engine and have no Python source.",
        workspace_input({{"id", id}, {"name", text()}}, json::array({"id"})),
        one,
        false,
        duplicate_methodology));
    registry.add(capability(
        "validate_methodology",
        "Validate a methodology",
        "Check a workspace methodology before it can be used. First without running anything: "
        "the manifest, the entrypoint's fit, predict and score, and the lock file; anything "
        "missing is answered in `findings` at once and the methodology stays unusable. When that "
        "passes, Lake builds its image (once per revision) and runs the interface on a small "
        "sample in its sandbox (a 200-row sample of `dataset` when given, otherwise a generated "
        "one); the run's result marks it usable or says what failed.",
        workspace_input({{"id", id},
                         {"dataset", {{"type", "string"}, {"description", "<database>.<table>"}}},
                         {"target", text()},
                         {"features", {{"type", "array"}, {"items", text()}}}},
                        json::array({"id"})),
        {{"type", "object"},
         {"properties",
          {{"findings", findings_schema()},
           {"ready", {{"type", "boolean"}}},
           {"run", {{"type", json::array({"object", "null"})}}}}},
         {"required", json::array({"findings", "ready", "run"})}},
        false,
        validate_methodology));
    registry.add(capability(
        "run_methodology",
        "Train with a methodology",
        "Train one of the workspace's own methodologies on a dataset (`<database>.<table>`, at "
        "most 1,000,000 rows) as untrusted code in its own container: its pinned packages, its "
        "resources, no network unless granted, and no Lake or Zetta credentials. `target` names "
        "the target column; `features` defaults to every other column; `params` are checked "
        "against its parameters; `holdout_fraction` (default 0.2) is held out for held_out_ "
        "metrics. The model it trains is registered in the workspace and appears in Train → "
        "Models. Compute is metered as CPU, RAM and GPU time. The methodology must have passed "
        "Validate for its current code.",
        workspace_input(
            {{"id", id},
             {"dataset", {{"type", "string"}, {"description", "<database>.<table>"}}},
             {"target", text()},
             {"features", {{"type", "array"}, {"items", text()}}},
             {"params", {{"type", "object"}}},
             {"holdout_fraction", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 0.5}}},
             {"project", text()},
             {"graph", text()}},
            json::array({"id", "dataset"})),
        one_run,
        false,
        run_methodology));
    Capability runs = capability(
        "list_methodology_runs",
        "List methodology runs",
        "A workspace's methodology runs, newest first (one methodology's with "
        "`methodology_id`): Validate and training runs with their phase, message, resources and, "
        "for the newest twenty, their outcome, metrics and model.",
        workspace_input({{"methodology_id", text()},
                         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
                        json::array()),
        {{"type", "object"},
         {"properties", {{"runs", {{"type", "array"}, {"items", run_schema()}}}}},
         {"required", json::array({"runs"})}},
        true,
        list_methodology_runs);
    runs.authorizes_candidates = true;
    registry.add(std::move(runs));
    registry.add(capability(
        "get_methodology_run",
        "Read a methodology run",
        "One methodology run: its phase, what it read (dataset, target, features, parameters), "
        "its result (findings, in_sample_ and held_out_ metrics, row counts), its model and the "
        "last 64 KiB of its log.",
        workspace_input({{"run_id", text()}}, json::array({"run_id"})),
        one_run,
        true,
        get_methodology_run));
    registry.add(capability(
        "stop_methodology_run",
        "Stop a methodology run",
        "Stop a methodology run: the operator removes its pods; what it produced so far is not "
        "kept.",
        workspace_input({{"run_id", text()}}, json::array({"run_id"})),
        one_run,
        false,
        stop_methodology_run));
    Capability run_input{
        .name = "methodology_run_input",
        .title = "Methodology run input",
        .description =
            "The runner's side of a methodology run (its stage pod), with the run's own "
            "credential only: the run's spec and its data, a page of rows at a time (`offset`, "
            "`limit` up to 5,000), until `next_offset` is null.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"run_id", text()},
                           {"offset", {{"type", "integer"}, {"minimum", 0}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}}}},
                         {"required", json::array({"run_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"spec", {{"type", "object"}}},
                            {"rows", {{"type", "array"}}},
                            {"next_offset", {{"type", json::array({"integer", "null"})}}},
                            {"total_rows", {{"type", "integer"}}}}},
                          {"required", json::array({"spec", "rows", "next_offset", "total_rows"})}},
        .kind = CapabilityKind::Read,
        .post_alias = true,
        .object = "methodology_run",
        .action = "read",
        .resolve_object = run_object,
        .async_handler = methodology_run_input};
    registry.add(std::move(run_input));
    registry.add(Capability{
        .name = "methodology_run_output",
        .title = "Methodology run output",
        .description =
            "The runner's side of a methodology run (its collect pod), with the run's own "
            "credential only: the result (status, findings, in_sample_ and held_out_ metrics, row "
            "counts), the artifact (base64, at most 24 MiB; a larger one, up to 1 GiB, sent first "
            "with methodology_run_artifact and named by `artifact`) and the log. Accepted once. A "
            "training run's artifact becomes the weights of a model the engine registers.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"run_id", text()},
                           {"result", {{"type", "object"}}},
                           {"artifact_base64", text()},
                           {"artifact",
                            {{"type", "object"},
                             {"description",
                              "an artifact over 24 MiB sent first with methodology_run_artifact: "
                              "its size and SHA-256"},
                             {"properties",
                              {{"bytes", {{"type", "integer"}, {"minimum", 1}}},
                               {"sha256", text()}}},
                             {"required", json::array({"bytes", "sha256"})}}},
                           {"log", text()}}},
                         {"required", json::array({"run_id", "result"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"recorded", {{"type", "boolean"}}},
                            {"model_id", {{"type", json::array({"string", "null"})}}}}},
                          {"required", json::array({"recorded", "model_id"})}},
        .kind = CapabilityKind::Action,
        .object = "methodology_run",
        .action = "write",
        .resolve_object = run_object,
        .async_handler = methodology_run_output});
    registry.add(Capability{
        .name = "methodology_run_artifact",
        .title = "Methodology run artifact",
        .description =
            "The runner's side of a training run (its collect pod), with the run's own credential "
            "only: an artifact over 24 MiB, up to 1 GiB, in order, 8 MiB at a time (`offset`, "
            "`chunk_base64`), before methodology_run_output names its size and SHA-256. Answers "
            "the bytes received; a chunk at an offset below that replaces what was held from "
            "there.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"run_id", text()},
                           {"offset", {{"type", "integer"}, {"minimum", 0}}},
                           {"chunk_base64", text()}}},
                         {"required", json::array({"run_id", "offset", "chunk_base64"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"received", {{"type", "integer"}}}}},
                          {"required", json::array({"received"})}},
        .kind = CapabilityKind::Action,
        .object = "methodology_run",
        .action = "write",
        .resolve_object = run_object,
        .async_handler = methodology_run_artifact});
    const json deployment_schema{
        {"type", json::array({"object", "null"})},
        {"description",
         "a served workspace methodology model: its phase (Pending, Building, Queued, Staging, "
         "Starting, Ready, Failed, Stopped), message, resources and whether it is ready"},
        {"properties",
         {{"id", text()},
          {"model_id", text()},
          {"methodology_id", text()},
          {"phase", text()},
          {"message", text()},
          {"ready", {{"type", "boolean"}}}}}};
    registry.add(capability(
        "get_methodology_deployment",
        "Read a served methodology model",
        "Whether a model a workspace methodology trained is being served (Ship → Models publishes "
        "it): the newest deployment of `model_id` with its phase and message, or null.",
        workspace_input({{"model_id", text()}}, json::array({"model_id"})),
        {{"type", "object"},
         {"properties", {{"deployment", deployment_schema}}},
         {"required", json::array({"deployment"})}},
        true,
        get_methodology_deployment));
    Capability deployments = capability(
        "list_methodology_deployments",
        "List served methodology models",
        "The workspace's deployments of models its own methodologies trained, newest first, "
        "stopped ones included: each with its model, phase, message and resources.",
        workspace_input({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
                        json::array()),
        {{"type", "object"},
         {"properties", {{"deployments", {{"type", "array"}, {"items", deployment_schema}}}}},
         {"required", json::array({"deployments"})}},
        true,
        list_methodology_deployments);
    deployments.authorizes_candidates = true;
    registry.add(std::move(deployments));
    Capability deployment_input{
        .name = "methodology_deployment_input",
        .title = "Methodology deployment input",
        .description =
            "The stage side of a served methodology model (lake#690), with the deployment's own "
            "credential only: the spec its server needs (model, methodology, artifact format, "
            "features, network, manifest) and the model's artifact, 8 MiB at a time (`offset`, "
            "`limit`), until `next_offset` is null, with its total size and SHA-256.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"deployment_id", text()},
                           {"offset", {{"type", "integer"}, {"minimum", 0}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8388608}}}}},
                         {"required", json::array({"deployment_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"spec", {{"type", "object"}}},
                            {"chunk_base64", text()},
                            {"offset", {{"type", "integer"}}},
                            {"next_offset", {{"type", json::array({"integer", "null"})}}},
                            {"total_bytes", {{"type", "integer"}}},
                            {"sha256", text()}}},
                          {"required",
                           json::array({"spec", "chunk_base64", "next_offset", "total_bytes",
                                        "sha256"})}},
        .kind = CapabilityKind::Read,
        .post_alias = true,
        .object = "methodology_deployment",
        .action = "read",
        .resolve_object = deployment_object,
        .async_handler = methodology_deployment_input};
    registry.add(std::move(deployment_input));
}

} // namespace lake::api
