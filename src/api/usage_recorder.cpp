#include "lake/api/usage_recorder.hpp"

#include "lake/control/store.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

// A refusal no retry can change: the workspace defines the metric differently, or a point is
// invalid. Anything else (not ready, unavailable, busy) is retried.
bool refused_for_good(const control::ZettaControlError& error) {
    return error.code == "conflict" || error.code == "invalid_input";
}

std::int64_t epoch_ms(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::int64_t hour_of(std::int64_t at_ms) {
    const std::int64_t offset = at_ms % UsageRecorder::resolution_ms;
    return at_ms - (offset < 0 ? offset + UsageRecorder::resolution_ms : offset);
}

} // namespace

const std::vector<UsageDimension>& usage_dimensions() {
    static const std::vector<UsageDimension> dimensions{
        {"storage", "bytes", "last", "Storage"},
        // What a workspace engine holds on its own volume (decision 61, lake#663): measured and
        // shown, never billed, since the engine's disk is billed as its volume (`volumes`).
        {"engine_storage", "bytes", "last", "Engine Storage", false, false},
        {"transfer", "bytes", "sum", "Transfer"},
        {"cpu", "mcpu_seconds", "sum", "CPU", true, false},
        {"ram", "mib_seconds", "sum", "RAM", true, false},
        {"gpu", "gpu_seconds", "sum", "GPU", true, true},
        {"vram", "mib_seconds", "sum", "GPU Memory", true, true},
        {"build_compute", "seconds", "sum", "Build Time", false, false},
        {"run_compute", "seconds", "sum", "Workspace Run Time", false, false},
        {"speech", "seconds", "sum", "Speech to Text"},
        {"calls", "calls", "sum", "API and MCP Calls"},
        {"predictions", "calls", "sum", "Model Predictions"},
        {"searches", "calls", "sum", "Index Searches"},
        {"model_tokens", "tokens", "sum", "Model Tokens", true, true},
        {"instances", "instance_seconds", "sum", "Service and App Instances", false, false},
        {"volumes", "gib_seconds", "sum", "Service and Store Volumes"},
    };
    return dimensions;
}

const std::vector<CapabilityMeter>& capability_meters() {
    static const std::vector<CapabilityMeter> meters{
        // Workloads: what the Orama operator schedules them with (workload_resources), sampled.
        {"create_workspace",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "its engine's resources and volume while it runs; what the engine holds on that volume is "
         "engine_storage, shown and not billed (decision 61)"},
        {"start_workspace",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "its engine's resources and volume while it runs"},
        {"set_workspace_storage", {"volumes"}, "its engine's volume, by size"},
        {"create_service",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "the service's resources and volumes while it is Ready"},
        {"create_service_and_bind_to_app",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "as create_service"},
        {"create_store", {"volumes"}, "the store's volume while it is Ready"},
        {"create_app", {"cpu", "ram", "gpu", "vram"}, "the app's replicas while they run"},
        {"deploy_app", {"cpu", "ram", "gpu", "vram"}, "as create_app"},
        {"start_app", {"cpu", "ram", "gpu", "vram"}, "as create_app"},
        {"scale_app", {"cpu", "ram", "gpu", "vram"}, "as create_app, per replica"},
        {"launch_agent",
         {"cpu", "ram", "gpu", "vram"},
         "a service launch while it runs; a job once, for how long it ran"},
        // Work that runs in the workspace's engine is the engine's CPU, RAM and GPU.
        {"run_pipeline", {"cpu", "ram", "gpu", "vram"}, "in the workspace's engine"},
        {"start_experiment", {"cpu", "ram", "gpu", "vram"}, "its trials, in the engine"},
        {"start_solution", {"cpu", "ram", "gpu", "vram"}, "its first run, in the engine"},
        // What the engine keeps is on its own volume, billed as `volumes` by its size (decision 61,
        // lake#663); the bytes are engine_storage, shown and never billed.
        {"load_drive_dataset", {"volumes"}, "the dataset, on the engine's volume"},
        // The engine's own capabilities, named in Lake (lake#650): their work and what they keep
        // are the workspace engine's CPU, RAM, GPU and volume.
        {"train_model",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "training in the engine; the model, on its volume"},
        {"start_run",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "the run in the engine; its artifacts, on its volume"},
        {"run_solution", {"cpu", "ram", "gpu", "vram", "volumes"}, "as start_run"},
        {"deploy_pipeline", {"cpu", "ram", "gpu", "vram"}, "the deployed pipeline, in the engine"},
        {"create_index", {"cpu", "ram", "volumes"}, "built in the engine; kept on its volume"},
        // The quickstart process (lake#1128): each capability it calls is metered as itself.
        {"start_quickstart",
         {"cpu", "ram", "gpu", "vram", "volumes", "transfer"},
         "as start_workspace, upload_dataset, create_index and expose_index, which it calls"},
        {"train_quickstart",
         {"cpu", "ram", "gpu", "vram", "volumes"},
         "as train_model and expose_model, which it calls"},
        {"create_dataset_from_query",
         {"cpu", "ram", "volumes"},
         "the query in the engine; the dataset, on its volume"},
        {"upload_dataset", {"transfer", "volumes"}, "the rows, on the engine's volume"},
        {"load_drive_model", {"volumes"}, "the model, on the engine's volume"},
        {"import_n8n",
         {"volumes"},
         "the workflow, kept as a project artifact on the engine's volume"},
        {"create_stream", {"volumes"}, "its topic's bytes, on the engine's volume"},
        {"publish_stream_message", {"volumes"}, "as create_stream"},
        {"publish_agent_event", {"volumes"}, "the event, in the workspace's topic"},
        {"publish_app_event", {"volumes"}, "the event, in the workspace's topic"},
        {"publish_build_event", {"volumes"}, "the event, in the workspace's topic"},
        // Serving.
        {"predict", {"predictions"}, "each answered prediction, by model"},
        {"search_index", {"searches"}, "each answered search, by index"},
        // Speech to text: seconds, or the tokens a model reports instead.
        {"transcribe", {"speech", "model_tokens"}, "the dictation's audio, in its workspace"},
        {"create_transcript",
         {"speech", "model_tokens", "storage"},
         "the audio, and the transcript's text"},
        {"record_model_usage", {"model_tokens"}, "the tokens an agent reports"},
        // Repository sync: exact hourly transfer records; the store's bytes, sampled.
        {"find_missing", {"transfer"}, "the request and answer bytes"},
        {"push", {"transfer", "storage"}, "the pushed bytes; the repository's store"},
        {"pull", {"transfer"}, "the pulled bytes"},
        {"upload_chunks", {"transfer", "storage"}, "the chunk bytes; the repository's store"},
        {"read_chunk", {"transfer"}, "the chunk bytes"},
        {"import_repository", {"storage"}, "the imported history, in the repository's store"},
        {"import_github_repository", {"storage"}, "as import_repository"},
        // Files in and out of a workspace (lake#658).
        {"repository_file", {"transfer"}, "the file's bytes, out"},
        {"resolve_model_file", {"transfer"}, "the file's bytes, out"},
        {"resolve_dataset_file", {"transfer"}, "the file's bytes, out"},
        {"stage_repository_file", {"transfer", "storage"}, "the staged bytes, in"},
        {"commit_repository_files", {"transfer", "storage"}, "inline file bytes, in"},
        {"download_drive_file", {"transfer"}, "the file's bytes, out"},
        {"download_drive_share_link", {"transfer"}, "the shared file's bytes, out"},
        {"upload_drive_files", {"transfer", "storage"}, "the files' bytes, in"},
        {"upload_drive_entries", {"transfer", "storage"}, "the files' bytes, in"},
        {"append_drive_upload", {"transfer", "storage"}, "the chunk's bytes, in"},
        {"read_drive_file", {"transfer"}, "the bytes read, out"},
        // MCP connections (lake#652): each exchange with the remote server is a call, and its
        // request and answer bytes are transfer (kinds mcp_out, mcp_in). A tool re-exported
        // through Lake's own MCP runs through call_mcp_tool or read_mcp_tool, so it is metered
        // here too; mirrored notifications count as calls of their connection.
        {"call_mcp_tool", {"calls", "transfer"}, "the call to the server and its bytes, each way"},
        {"read_mcp_tool", {"calls", "transfer"}, "as call_mcp_tool"},
        {"create_mcp_connection", {"calls", "transfer"}, "discovery: the server's lists and bytes"},
        {"refresh_mcp_connection", {"calls", "transfer"}, "as create_mcp_connection"},
        {"test_mcp_connection", {"calls", "transfer"}, "a saved connection's test, as discovery"},
        {"complete_mcp_connection_sign_in",
         {"calls", "transfer"},
         "the discovery after a sign-in; the token exchange is a call"},
        // A workspace methodology's runs (lake#655): its sandbox pod's resources, charged once for
        // the time its code ran (methodology_run); the image build is build's.
        {"validate_methodology",
         {"cpu", "ram", "gpu", "vram"},
         "its sample run in the sandbox, and its image build once per revision"},
        {"run_methodology", {"cpu", "ram", "gpu", "vram"}, "as validate_methodology, on a dataset"},
    };
    return meters;
}

const std::vector<std::pair<std::string_view, std::string_view>>& calls_only_capabilities() {
    static const std::vector<std::pair<std::string_view, std::string_view>> calls{
        // Identity, account and control-plane configuration: no workspace resource.
        {"append_transparency_entries", "the instance's transparency log"},
        {"begin_admin_recovery", "account recovery"},
        {"begin_application_authorization", "sign-in"},
        {"begin_email_recovery", "account recovery"},
        {"begin_identity_proof", "sign-in"},
        {"begin_own_identity", "sign-in"},
        {"begin_passkey_registration", "sign-in"},
        {"begin_passkey_sign_in", "sign-in"},
        {"begin_root_recovery", "account recovery"},
        {"create_application_organization", "an identity record"},
        {"create_application_webhook", "an identity record"},
        {"create_customer", "an account"},
        {"create_domain", "a routing record"},
        {"create_encryption_key", "a key record"},
        {"create_federation_issuer", "identity configuration"},
        {"create_grant", "a policy record"},
        {"create_invitation", "a membership record"},
        {"create_membership", "a membership record"},
        {"create_object", "a policy object"},
        {"create_organization", "an account"},
        {"create_organization_space", "an account"},
        {"create_pki_authority", "a key record"},
        {"create_secret_mount", "a secret mount record"},
        {"create_team", "a membership record"},
        {"create_token", "a credential"},
        {"create_browser_key", "a credential"},
        {"create_user", "an identity record"},
        {"publish_transparency_checkpoint", "the instance's transparency log"},
        {"publish_tuf_metadata", "the instance's update metadata"},
        {"read_secret", "a secret record"},
        {"replicate_repository", "Orama's replication between lakes"},
        {"replicate_transparency_log", "Orama's replication between lakes"},
        {"copy_workspace_records", "the operator's one-time copy"},
        {"start_oauth", "sign-in"},
        {"start_sso_oidc", "sign-in"},
        {"start_sso_saml", "sign-in"},
        // Records: what they write is a few records, never a workload or a file's bytes.
        {"begin_drive_upload", "the upload's record; its bytes are append_drive_upload's"},
        {"finish_drive_upload", "the file's record; its bytes were counted as they came"},
        {"create_agent_from_proposal", "an agent definition; its runs are launch_agent's"},
        {"begin_mcp_connection_sign_in",
         "an OAuth state record; the authorization server's metadata is not the workspace's data"},
        {"create_mcp_connection_agent", "an agent definition; its runs are launch_agent's"},
        {"create_drive", "a drive record"},
        {"create_drive_folder", "a folder"},
        {"create_drive_folder_entry", "a folder record"},
        {"create_drive_share_link", "a link record"},
        {"create_drive_shortcut", "a shortcut record"},
        {"create_image_repository", "a routing record; its images are storage when pushed"},
        {"create_issue", "a forge record"},
        {"create_issue_comment", "a forge record"},
        {"create_pipeline", "a pipeline definition; its runs are run_pipeline's"},
        {"create_projects_from_opportunities", "project records"},
        {"create_release", "a forge record"},
        {"create_repository", "an empty repository; its bytes are storage when pushed"},
        {"create_review", "a forge record"},
        {"create_review_comment", "a forge record"},
        {"create_solution", "a solution record"},
        {"create_methodology",
         "a repository commit and a registry entry; its files are its repository's storage"},
        {"create_network_connection", "a desired network-policy record"},
        {"create_network_egress_rule", "a desired network-policy record"},
        {"grant_network_tunnel", "a desired network-policy record"},
        {"issue_network_tunnel_certificate", "a short-lived identity credential"},
        {"remove_network_connection", "a desired network-policy record"},
        {"revoke_network_tunnel", "a desired network-policy record"},
        {"rotate_network_connection_credential", "a desired network-policy record"},
        {"duplicate_methodology", "as create_methodology"},
        {"draft_methodology", "drafted files, kept nowhere"},
        {"create_webhook", "a forge record"},
        {"resolve_conflict", "a repository change, stored as its repository's storage"},
        {"resolve_drive_share_link", "a link's metadata"},
        {"resolve_model", "a model's metadata"},
        {"set_app_auth", "app configuration"},
        {"set_app_env", "app configuration"},
        {"set_app_stores", "app configuration; the storage is billed as the store's"},
        {"set_service_stores", "service configuration; the storage is billed as the store's"},
        {"set_app_zetta_binding", "app configuration"},
        // Search runs in lake's own process over indexes lake keeps: a call.
        {"search", "lake's repository search; the index is in the repositories' storage"},
        {"search_drives", "lake's drive search"},
        // Engine reads and records (lake#650): a workspace member's own scoring and searching run
        // on the workspace engine, whose CPU, RAM and GPU are billed; the served routes, predict
        // and search_index, are what is priced per request.
        {"score", "scoring in the workspace engine, billed as its CPU, RAM and GPU"},
        {"search_workspace_index", "searching in the workspace engine, billed as its CPU and RAM"},
        {"create_project", "a project record"},
        {"create_connection", "a connection record; its loads are datasets in storage"},
        {"create_agent", "an agent record; its serving is the engine's CPU, RAM and GPU"},
        {"record_outcome", "an outcome record in the engine's ledger"},
    };
    return calls;
}

const std::vector<std::string_view>& metered_workload_kinds() {
    static const std::vector<std::string_view> kinds{
        "workspace", "service", "store", "app", "build", "agent_launch", "agent_cron_run",
        "methodology_run", "methodology_deployment"};
    return kinds;
}

const std::vector<std::string_view>& metered_store_types() {
    static const std::vector<std::string_view> types{"volume"};
    return types;
}

std::string_view store_metered_as(std::string_view type, std::string_view provider) {
    if (type == "volume") {
        return "volumes";
    }
    if (control::store_kept_by_lake(type, provider)) {
        return provider == "zetta" ? "storage (buckets)" : "storage (repositories)";
    }
    if (control::store_external_bucket(type, provider)) {
        return "transfer";
    }
    return {};
}

const UsageDimension* find_usage_dimension(std::string_view name) {
    for (const UsageDimension& dimension : usage_dimensions()) {
        if (dimension.name == name) {
            return &dimension;
        }
    }
    return nullptr;
}

UsageRecorder::UsageRecorder(control::Store* store,
                             control::WorkspaceService* workspaces,
                             std::size_t max_pending)
    : store_(store), workspaces_(workspaces), max_pending_(std::max<std::size_t>(max_pending, 1)) {}

bool UsageRecorder::gauge(const Key& key) {
    return std::get<1>(key) == storage_metric;
}

void UsageRecorder::merge_locked(const Key& key, const Held& held) {
    auto found = held_.find(key);
    if (found == held_.end()) {
        held_.emplace(key, held);
        return;
    }
    if (gauge(key)) {
        if (held.at_ms >= found->second.at_ms) {
            found->second = held;
        }
        return;
    }
    found->second.value = add_saturated(found->second.value, held.value);
}

void UsageRecorder::add(const std::string& workspace_id,
                        std::string_view dimension,
                        std::uint64_t value,
                        std::map<std::string, std::string> labels,
                        std::chrono::system_clock::time_point at) {
    std::vector<Increment> one;
    one.push_back({std::string(dimension), value, std::move(labels)});
    add_together(workspace_id, std::move(one), at);
}

void UsageRecorder::add_transfer(const std::string& workspace_id,
                                 std::uint64_t in,
                                 std::uint64_t out,
                                 std::chrono::system_clock::time_point at) {
    add(workspace_id, "transfer", in, {{"kind", "in"}}, at);
    add(workspace_id, "transfer", out, {{"kind", "out"}}, at);
}

void UsageRecorder::add_together(const std::string& workspace_id,
                                 std::vector<Increment> increments,
                                 std::chrono::system_clock::time_point at) {
    if (workspace_id.empty()) {
        return;
    }
    std::erase_if(increments, [](const Increment& increment) {
        return increment.value == 0 || increment.dimension.empty();
    });
    if (increments.empty()) {
        return;
    }
    const std::int64_t hour = hour_of(epoch_ms(at));
    std::vector<std::pair<Key, std::uint64_t>> keys;
    keys.reserve(increments.size());
    for (Increment& increment : increments) {
        nlohmann::json labels = nlohmann::json::object();
        for (auto& [name, value] : increment.labels) {
            if (!value.empty()) {
                labels[name] = std::move(value);
            }
        }
        labels["dimension"] = std::move(increment.dimension);
        keys.emplace_back(Key{workspace_id, std::string(usage_metric), hour, labels.dump()},
                          increment.value);
    }
    std::lock_guard lock(mutex_);
    std::size_t needed = 0;
    std::set<Key> fresh;
    for (const auto& [key, value] : keys) {
        if (!held_.contains(key) && fresh.insert(key).second) {
            ++needed;
        }
    }
    if (held_.size() + needed > max_pending_) {
        ++totals_.dropped_bound;
        return;
    }
    for (const auto& [key, value] : keys) {
        merge_locked(key, Held{value, hour});
    }
}

void UsageRecorder::sample(const std::string& workspace_id,
                           std::string_view kind,
                           std::uint64_t bytes,
                           std::chrono::system_clock::time_point at) {
    if (workspace_id.empty() || kind.empty()) {
        return;
    }
    const std::int64_t at_ms = epoch_ms(at);
    Key key{workspace_id,
            std::string(storage_metric),
            hour_of(at_ms),
            nlohmann::json{{"kind", std::string(kind)}}.dump()};
    std::lock_guard lock(mutex_);
    if (!held_.contains(key) && held_.size() >= max_pending_) {
        ++totals_.dropped_bound;
        return;
    }
    merge_locked(key, Held{bytes, at_ms});
}

std::size_t UsageRecorder::pending() const {
    std::lock_guard lock(mutex_);
    return held_.size();
}

UsageRecorder::Totals UsageRecorder::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

liquid::runtime::task<void> UsageRecorder::flush() {
    if (flushing_.exchange(true)) {
        co_return {};
    }
    std::map<Key, Held> held;
    std::set<std::string> created;
    {
        std::lock_guard lock(mutex_);
        held.swap(held_);
        created = created_;
    }
    using Values = std::vector<std::pair<Key, Held>>;
    std::map<std::string, std::map<std::string, Values>> by_workspace;
    for (auto& [key, value] : held) {
        by_workspace[std::get<0>(key)][std::get<1>(key)].emplace_back(key, value);
    }

    Totals delta;
    Values retry;
    std::set<std::string> now_created, forget_created;
    const auto count = [](std::uint64_t& total, std::size_t values) {
        total = add_saturated(total, values);
    };
    for (auto& [workspace_id, metrics] : by_workspace) {
        // The workspace's space, and whether it still exists: one routing-row read per workspace
        // and flush, off the request path.
        std::string customer_id;
        bool gone = store_ == nullptr || workspaces_ == nullptr;
        if (!gone) {
            const auto workspace = store_->get_workspace(workspace_id);
            if (!workspace) {
                if (workspace.error().code == "not_found" ||
                    workspace.error().code == "invalid_input") {
                    gone = true;
                } else {
                    ++delta.flush_errors;
                    for (const auto& [name, values] : metrics) {
                        retry.insert(retry.end(), values.begin(), values.end());
                    }
                    continue;
                }
            } else if (workspace->desired == "deleted") {
                gone = true;
            } else {
                customer_id = workspace->customer_id;
            }
        }
        if (gone) {
            for (const auto& [name, values] : metrics) {
                count(delta.dropped_workspace_deleted, values.size());
            }
            continue;
        }
        ApiContext context;
        context.store = store_;
        context.workspaces = workspaces_;
        context.workspace_id = workspace_id;
        for (const auto& [name, values] : metrics) {
            const bool is_gauge = name == storage_metric;
            const std::string defined_key = workspace_id + "/" + name;
            if (!created.contains(defined_key)) {
                auto defined = control::settled(
                    co_await call_workspace(context,
                                            customer_id,
                                            "create_metric",
                                            false,
                                            nlohmann::json{{"metric", name},
                                                           {"kind", is_gauge ? "gauge" : "counter"},
                                                           {"resolution_ms", resolution_ms},
                                                           {"retention_ms", retention_ms}}));
                if (!defined) {
                    ++delta.flush_errors;
                    if (refused_for_good(defined.error())) {
                        count(delta.dropped_refused, values.size());
                    } else {
                        retry.insert(retry.end(), values.begin(), values.end());
                    }
                    continue;
                }
                now_created.insert(defined_key);
                created.insert(defined_key);
            }
            for (std::size_t from = 0; from < values.size(); from += max_points_per_call) {
                const std::size_t to = std::min(values.size(), from + max_points_per_call);
                nlohmann::json points = nlohmann::json::array();
                for (std::size_t index = from; index < to; ++index) {
                    const auto& [key, value] = values[index];
                    points.push_back({{"labels", nlohmann::json::parse(std::get<3>(key))},
                                      {"value", value.value},
                                      {"at_ms", value.at_ms}});
                }
                auto recorded = control::settled(co_await call_workspace(
                    context,
                    customer_id,
                    "record_metric",
                    false,
                    nlohmann::json{{"metric", name}, {"points", std::move(points)}}));
                if (recorded) {
                    delta.points += to - from;
                    count(is_gauge ? delta.readings : delta.increments, to - from);
                    continue;
                }
                ++delta.flush_errors;
                if (recorded.error().code == "not_found") {
                    // The metric is gone (a recreated workspace Zetta): define it again next time.
                    forget_created.insert(defined_key);
                    now_created.erase(defined_key);
                }
                if (refused_for_good(recorded.error())) {
                    count(delta.dropped_refused, to - from);
                } else {
                    retry.insert(retry.end(),
                                 values.begin() + static_cast<std::ptrdiff_t>(from),
                                 values.begin() + static_cast<std::ptrdiff_t>(to));
                }
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        totals_.increments = add_saturated(totals_.increments, delta.increments);
        totals_.readings = add_saturated(totals_.readings, delta.readings);
        totals_.points += delta.points;
        totals_.flush_errors += delta.flush_errors;
        totals_.dropped_workspace_deleted =
            add_saturated(totals_.dropped_workspace_deleted, delta.dropped_workspace_deleted);
        totals_.dropped_refused = add_saturated(totals_.dropped_refused, delta.dropped_refused);
        created_.insert(now_created.begin(), now_created.end());
        for (const auto& defined_key : forget_created) {
            created_.erase(defined_key);
        }
        for (const auto& [key, value] : retry) {
            if (held_.contains(key) || held_.size() < max_pending_) {
                merge_locked(key, value);
            } else {
                ++totals_.dropped_bound;
            }
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
