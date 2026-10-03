#include "lake/control/platform.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <zetta/connect/http_transport.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/platform_records.hpp"

namespace lake::control {
namespace {

using nlohmann::json;

PlatformError platform_error(const StoreError& error) {
    return {error.code, error.message};
}

PlatformError platform_error(const ZettaControlError& error) {
    return {error.code, error.message};
}

PlatformError platform_error(const liquid::error& error) {
    return {"store_unavailable", error.message};
}

template <class T> PlatformResult<T> platform_failure(PlatformError error) {
    return std::unexpected(std::move(error));
}

// The error text of a step that answers one ("" when it succeeded).
std::string finished_text(std::expected<std::string, liquid::error> completed) {
    return completed ? std::move(*completed) : "store_unavailable: " + completed.error().message;
}

// A platform task's result, with the runtime's own failure as store_unavailable.
template <class T>
PlatformResult<T> finished(std::expected<PlatformResult<T>, liquid::error> completed) {
    if (!completed) {
        return platform_failure<T>(platform_error(completed.error()));
    }
    return std::move(*completed);
}

std::string text_of(const json& row, std::string_view key) {
    const std::string name(key);
    if (!row.contains(name) || row.at(name).is_null()) {
        return {};
    }
    if (row.at(name).is_string()) {
        return row.at(name).get<std::string>();
    }
    return row.at(name).dump();
}

bool bool_of(const json& row, std::string_view key) {
    const std::string name(key);
    if (!row.contains(name) || row.at(name).is_null()) {
        return false;
    }
    if (row.at(name).is_boolean()) {
        return row.at(name).get<bool>();
    }
    const std::string value = text_of(row, key);
    return value == "true" || value == "TRUE" || value == "1";
}

std::string percent_encode(std::string_view input) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output += static_cast<char>(character);
            continue;
        }
        output += '%';
        output += hex[character >> 4U];
        output += hex[character & 0x0fU];
    }
    return output;
}

template <std::size_t Size>
std::string select_columns(const std::array<std::string_view, Size>& columns) {
    std::string output;
    for (const std::string_view column : columns) {
        if (!output.empty()) {
            output += ", ";
        }
        output += column;
    }
    return output;
}

std::string deployment_key(const json& row) {
    return text_of(row, "deployment_id");
}

std::string environment_env_key(const json& row) {
    return text_of(row, "repository") + "/" + text_of(row, "environment") + "/" +
           text_of(row, "key");
}

std::string domain_key(const json& row) {
    return text_of(row, "repository") + "/" + text_of(row, "environment") + "/" +
           text_of(row, "host");
}

bool is_newer_row(const json& candidate, const json& current) {
    return platform_row_is_newer(candidate, current);
}

using KeyFunction = std::string (*)(const json&);

PlatformResult<std::map<std::string, json>> latest_by_key(const ZettaRows& rows,
                                                          KeyFunction key_function) {
    std::map<std::string, json> latest;
    for (const json& row : rows) {
        if (!row.is_object()) {
            return std::unexpected(PlatformError{"store_error", "Zetta returned a non-object row"});
        }
        const std::string key = key_function(row);
        const std::string updated_at = text_of(row, "updated_at");
        if (key.empty() || text_of(row, "repository").empty() ||
            text_of(row, "environment").empty() || updated_at.empty()) {
            return std::unexpected(
                PlatformError{"store_error", "Zetta returned an incomplete platform row"});
        }
        const auto found = latest.find(key);
        if (found == latest.end() || is_newer_row(row, found->second)) {
            latest[key] = row;
        }
    }
    return latest;
}

std::vector<json> new_deployment_phases(const ZettaRows& rows,
                                        const std::map<std::string, json>& previous_deployments) {
    std::map<std::string, std::vector<json>> rows_by_deployment;
    for (const json& row : rows) {
        const std::string key = deployment_key(row);
        const auto previous = previous_deployments.find(key);
        if (previous != previous_deployments.end() && !is_newer_row(row, previous->second)) {
            continue;
        }
        rows_by_deployment[key].push_back(row);
    }

    std::vector<json> phases;
    for (auto& [key, deployment_rows] : rows_by_deployment) {
        std::ranges::sort(deployment_rows, {}, platform_row_order);
        const auto previous = previous_deployments.find(key);
        std::string previous_phase = previous == previous_deployments.end()
                                         ? std::string{}
                                         : text_of(previous->second, "phase");
        for (const json& row : deployment_rows) {
            const std::string phase = text_of(row, "phase");
            if (phase.empty() || phase == previous_phase) {
                continue;
            }
            phases.push_back(row);
            previous_phase = phase;
        }
    }
    return phases;
}

std::vector<json> deployment_values(const std::vector<PlatformMirrorRow>& rows,
                                    const std::string& repository_id,
                                    const std::string& environment) {
    std::vector<json> output;
    for (const PlatformMirrorRow& row : rows) {
        if (text_of(row.value, "repository") != repository_id) {
            continue;
        }
        if (!environment.empty() && text_of(row.value, "environment") != environment) {
            continue;
        }
        output.push_back(row.value);
    }
    std::ranges::sort(output, std::greater{}, platform_row_order);
    return output;
}

json nullable_text(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

} // namespace

struct LokiHttpSource::Impl {
    std::string endpoint;
    std::string bearer_token;
};

LokiHttpSource::LokiHttpSource(std::string endpoint, std::string bearer_token)
    : impl_(std::make_unique<Impl>(Impl{std::move(endpoint), std::move(bearer_token)})) {
    while (impl_->endpoint.ends_with('/')) {
        impl_->endpoint.pop_back();
    }
}

LokiHttpSource::~LokiHttpSource() = default;
LokiHttpSource::LokiHttpSource(LokiHttpSource&&) noexcept = default;
LokiHttpSource& LokiHttpSource::operator=(LokiHttpSource&&) noexcept = default;

liquid::runtime::task<PlatformResult<json>> LokiHttpSource::query(std::string namespace_name,
                                                                  std::string since,
                                                                  int limit,
                                                                  std::string direction) {
    const zetta::connect::AsyncHttpTransport& transport =
        zetta::connect::default_async_http_transport();
    if (!transport || impl_->endpoint.empty()) {
        co_return platform_failure<json>(
            {"store_unavailable", "the Loki-compatible log source is not configured"});
    }
    const std::string selector = "{namespace=\"" + namespace_name + "\"}";
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = impl_->endpoint + "/loki/api/v1/query_range?query=" + percent_encode(selector) +
                  "&limit=" + std::to_string(limit) + "&direction=" + percent_encode(direction);
    if (!since.empty()) {
        request.url += "&start=" + percent_encode(since);
    }
    request.headers = {{"Accept", "application/json"}};
    if (!impl_->bearer_token.empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + impl_->bearer_token);
        request.secret_headers = {"Authorization"};
    }
    auto [transport_error, response] = co_await transport(request);
    if (transport_error || !response.error.empty()) {
        co_return platform_failure<json>(
            {"store_unavailable", response.error.empty() ? "log request failed" : response.error});
    }
    if (response.status < 200 || response.status >= 300) {
        co_return platform_failure<json>(
            {"store_unavailable", "log source returned HTTP " + std::to_string(response.status)});
    }
    const json body = json::parse(response.body, nullptr, false);
    if (body.is_discarded() || body.value("status", "") != "success" ||
        !body.value("data", json::object()).value("result", json::array()).is_array()) {
        co_return platform_failure<json>(
            {"store_error", "log source returned an invalid query response"});
    }
    std::vector<json> lines;
    for (const json& stream : body.at("data").at("result")) {
        const json labels = stream.value("stream", json::object());
        for (const json& value : stream.value("values", json::array())) {
            if (!value.is_array() || value.size() != 2 || !value.at(0).is_string() ||
                !value.at(1).is_string()) {
                continue;
            }
            lines.push_back({{"ts", value.at(0)},
                             {"pod", labels.value("pod", "")},
                             {"container", labels.value("container", "")},
                             {"line", value.at(1)}});
        }
    }
    std::ranges::sort(lines, {}, [](const json& line) { return line.at("ts").get<std::string>(); });
    if (direction == "backward") {
        std::ranges::reverse(lines);
    }
    if (lines.size() > static_cast<std::size_t>(limit)) {
        lines.resize(static_cast<std::size_t>(limit));
    }
    const std::string next_since = lines.empty() ? since : lines.back().at("ts").get<std::string>();
    co_return PlatformResult<json>(json{{"items", lines}, {"next_since", next_since}});
}

PlatformService::PlatformService(std::shared_ptr<Store> store,
                                 std::shared_ptr<ZettaControl> control,
                                 std::shared_ptr<LokiSource> logs,
                                 PlatformConfig config)
    : store_(std::move(store)), control_(std::move(control)), logs_(std::move(logs)),
      config_(std::move(config)) {
    if (!store_ || !control_ || config_.lake.empty()) {
        degraded_ = true;
        stale_since_ = now_iso8601();
    }
}

void PlatformService::mark_failed() {
    std::lock_guard lock(state_mutex_);
    degraded_ = true;
    if (!stale_since_) {
        stale_since_ = now_iso8601();
    }
    next_sync_ = std::chrono::steady_clock::now() + config_.sync_interval;
}

void PlatformService::mark_succeeded() {
    std::lock_guard lock(state_mutex_);
    degraded_ = false;
    stale_since_.reset();
    next_sync_ = std::chrono::steady_clock::now() + config_.sync_interval;
}

void PlatformService::mark_repository(const std::string& repository_id, bool mirrored) {
    std::lock_guard lock(state_mutex_);
    if (mirrored) {
        stale_repositories_.erase(repository_id);
        return;
    }
    stale_repositories_.try_emplace(repository_id, now_iso8601());
    mirrored_.erase(repository_id);
}

bool PlatformService::sync_due() const {
    std::lock_guard lock(state_mutex_);
    return std::chrono::steady_clock::now() >= next_sync_;
}

bool PlatformService::degraded() const {
    std::lock_guard lock(state_mutex_);
    return degraded_;
}

std::optional<std::string> PlatformService::stale_since() const {
    std::lock_guard lock(state_mutex_);
    return stale_since_;
}

std::optional<std::string> PlatformService::stale_since(const std::string& repository_id) const {
    std::lock_guard lock(state_mutex_);
    if (stale_since_) {
        return stale_since_;
    }
    const auto found = stale_repositories_.find(repository_id);
    if (found == stale_repositories_.end()) {
        return std::nullopt;
    }
    return found->second;
}

liquid::runtime::task<PlatformResult<PlatformEnvironment>>
PlatformService::ensure_environment(WorkspaceRecords& records, const Repository& repository,
                                    const std::string& name) {
    const std::string id = platform_environment_id(repository.id, name);
    // The policy object stays in the control store with its grants; its external id is the
    // environment's derived id, so authorization finds it without reading the workspace.
    auto object = store_->find_policy_object("environment", id);
    if (!object && object.error().code == "not_found") {
        const auto parent = store_->find_policy_object("repository", repository.id);
        if (!parent) {
            co_return platform_failure<PlatformEnvironment>(platform_error(parent.error()));
        }
        object = store_->create_policy_object(
            {"environment", id, parent->id, "lake", new_uuid_v4(), true});
        if (!object) {
            // Another replica created it first.
            object = store_->find_policy_object("environment", id);
        }
    }
    if (!object) {
        co_return platform_failure<PlatformEnvironment>(platform_error(object.error()));
    }
    PlatformEnvironment environment;
    environment.repository_id = repository.id;
    environment.name = name;
    environment.object_id = object->id;
    auto written =
        settled(co_await put_platform_environment_record(records, std::move(environment)));
    if (!written) {
        co_return platform_failure<PlatformEnvironment>(platform_error(written.error()));
    }
    co_return PlatformResult<PlatformEnvironment>(std::move(*written));
}

liquid::runtime::task<PlatformResult<void>> PlatformService::sync(PlatformRecordsOpener open) {
    if (!store_ || !control_ || config_.lake.empty() || !open) {
        mark_failed();
        co_return platform_failure<void>(
            {"store_unavailable", "the platform mirror is not configured"});
    }
    const auto quoted_lake = quote_zetta_literal(config_.lake);
    if (!quoted_lake) {
        mark_failed();
        co_return platform_failure<void>(platform_error(quoted_lake.error()));
    }
    const auto read_table = [&](std::string_view table, std::string columns)
        -> liquid::runtime::task<PlatformResult<ZettaRows>> {
        auto completed = co_await control_->sql("SELECT " + columns + " FROM " +
                                                    std::string(table) + " WHERE lake = " +
                                                    *quoted_lake,
                                                std::string(operator_control_database));
        if (!completed) {
            co_return platform_failure<ZettaRows>(platform_error(completed.error()));
        }
        if (!*completed) {
            co_return platform_failure<ZettaRows>(platform_error(completed->error()));
        }
        co_return PlatformResult<ZettaRows>(std::move(**completed));
    };
    auto deployments =
        finished(co_await read_table(deployments_table, select_columns(deployment_columns)));
    if (!deployments) {
        mark_failed();
        co_return platform_failure<void>(deployments.error());
    }
    auto environment_env = finished(
        co_await read_table(environment_env_table, select_columns(environment_env_columns)));
    if (!environment_env) {
        mark_failed();
        co_return platform_failure<void>(environment_env.error());
    }
    auto domains = finished(co_await read_table(domains_table, select_columns(domain_columns)));
    if (!domains) {
        mark_failed();
        co_return platform_failure<void>(domains.error());
    }
    const auto latest_deployments = latest_by_key(*deployments, deployment_key);
    const auto latest_environment_env = latest_by_key(*environment_env, environment_env_key);
    const auto latest_domains = latest_by_key(*domains, domain_key);
    if (!latest_deployments || !latest_environment_env || !latest_domains) {
        mark_failed();
        const PlatformError error = !latest_deployments
                                        ? latest_deployments.error()
                                        : (!latest_environment_env ? latest_environment_env.error()
                                                                   : latest_domains.error());
        co_return platform_failure<void>(error);
    }
    // The operator control plane answered: the service is healthy whatever a workspace says.
    mark_succeeded();

    // Every row goes to its repository's workspace, so the work is grouped by repository.
    struct RepositoryRows {
        std::map<std::string, std::map<std::string, json>> latest; // kind -> key -> row
        ZettaRows deployments;                                     // every deployment row
        std::set<std::string> environments;
    };
    std::map<std::string, RepositoryRows> by_repository;
    const auto group = [&](std::string_view kind, const std::map<std::string, json>& rows) {
        for (const auto& [key, row] : rows) {
            RepositoryRows& rows_of = by_repository[text_of(row, "repository")];
            rows_of.latest[std::string(kind)][key] = row;
            rows_of.environments.insert(text_of(row, "environment"));
        }
    };
    group("deployment", *latest_deployments);
    group("environment_env", *latest_environment_env);
    group("domain", *latest_domains);
    for (const json& row : *deployments) {
        by_repository[text_of(row, "repository")].deployments.push_back(row);
    }
    {
        std::lock_guard lock(state_mutex_);
        if (++syncs_since_full_ >= std::max(config_.full_sync_every, 1)) {
            syncs_since_full_ = 0;
            mirrored_.clear();
        }
    }

    std::vector<std::string> failures;
    for (const auto& [repository_id, rows] : by_repository) {
        const std::string fingerprint = json(rows.latest).dump();
        {
            std::lock_guard lock(state_mutex_);
            const auto known = mirrored_.find(repository_id);
            if (known != mirrored_.end() && known->second == fingerprint) {
                continue; // nothing changed since this replica last wrote it
            }
        }
        const auto failed = [&](const std::string& reason) {
            mark_repository(repository_id, false);
            failures.push_back(repository_id + ": " + reason);
        };
        const auto repository = store_->get_repository(repository_id);
        if (!repository) {
            if (repository.error().code != "not_found" &&
                repository.error().code != "invalid_input") {
                failed(repository.error().message);
            }
            continue; // a row for a repository lake does not have belongs to no workspace
        }
        if (repository->workspace_id.empty()) {
            failed("the repository has no workspace");
            continue;
        }
        auto records = open(repository->customer_id, repository->workspace_id);
        if (!records) {
            failed(records.error().code + ": " + records.error().message);
            continue;
        }
        auto environments =
            settled(co_await list_platform_environment_records(*records, repository_id));
        if (!environments) {
            failed(environments.error().code + ": " + environments.error().message);
            continue;
        }
        auto mirrored = settled(co_await list_platform_mirror_records(*records, repository_id));
        if (!mirrored) {
            failed(mirrored.error().code + ": " + mirrored.error().message);
            continue;
        }
        std::set<std::string> known_environments;
        for (const PlatformEnvironment& environment : *environments) {
            known_environments.insert(environment.name);
        }
        std::map<std::string, json> stored;              // mirror id -> row value
        std::map<std::string, json> previous_deployments; // deployment key -> row value
        for (const PlatformMirrorRow& row : *mirrored) {
            stored[platform_mirror_id(row.kind, row.key)] = row.value;
            if (row.kind == "deployment") {
                previous_deployments[row.key] = row.value;
            }
        }
        std::string error;
        for (const std::string& name : rows.environments) {
            if (known_environments.contains(name)) {
                continue;
            }
            auto ensured = finished(co_await ensure_environment(*records, *repository, name));
            if (!ensured) {
                error = ensured.error().code + ": " + ensured.error().message;
                break;
            }
        }
        const std::string seen_at = now_iso8601();
        const auto apply_kind = [&](std::string_view kind) -> liquid::runtime::task<std::string> {
            const auto found = rows.latest.find(std::string(kind));
            if (found == rows.latest.end()) {
                co_return std::string{};
            }
            for (const auto& [key, row] : found->second) {
                const auto current = stored.find(platform_mirror_id(std::string(kind), key));
                if (current != stored.end() && current->second == row) {
                    continue;
                }
                auto written = settled(co_await put_platform_mirror_record(
                    *records, repository_id, {std::string(kind), key, row, seen_at}));
                if (!written) {
                    co_return written.error().code + ": " + written.error().message;
                }
            }
            co_return std::string{};
        };
        if (error.empty()) {
            error = finished_text(co_await apply_kind("environment_env"));
        }
        if (error.empty()) {
            error = finished_text(co_await apply_kind("domain"));
        }
        // Each newly observed phase is audited before its deployment row is written, so a sync
        // that fails here records it again next time rather than never.
        if (error.empty()) {
            for (const json& row : new_deployment_phases(rows.deployments, previous_deployments)) {
                const audit::EventDraft event{
                    .customer_id = repository->customer_id,
                    .kind = "platform.deployment.phase",
                    .principal = text_of(row, "requested_by"),
                    .object = "deployment/" + deployment_key(row),
                    .action = "deployment_phase",
                    .outcome = "success",
                    .request_id = new_uuid_v4(),
                    .method = "zetta_mirror",
                    .details = {{"repository_id", repository->id},
                                {"environment", text_of(row, "environment")},
                                {"phase", text_of(row, "phase")},
                                {"updated_at", text_of(row, "updated_at")}},
                };
                const audit::IdentityOptions options{config_.identity_seed, config_.instance_kek};
                const auto recorded = audit::record(*store_, event, options);
                if (!recorded) {
                    error = recorded.error().code + ": " + recorded.error().message;
                    break;
                }
            }
        }
        if (error.empty()) {
            error = finished_text(co_await apply_kind("deployment"));
        }
        if (!error.empty()) {
            failed(error);
            continue;
        }
        mark_repository(repository_id, true);
        std::lock_guard lock(state_mutex_);
        mirrored_[repository_id] = fingerprint;
    }
    if (!failures.empty()) {
        std::string message = std::to_string(failures.size()) +
                              " repositories were not mirrored into their workspace: ";
        for (std::size_t index = 0; index < failures.size(); ++index) {
            message += (index == 0 ? "" : "; ") + failures[index];
        }
        co_return platform_failure<void>({"workspace_not_ready", message});
    }
    co_return PlatformResult<void>{};
}

namespace {

json environment_output(const PlatformEnvironment& environment,
                        const std::vector<PlatformMirrorRow>& rows) {
    std::vector<json> deployments;
    json env = json::array();
    json domains = json::array();
    for (const PlatformMirrorRow& row : rows) {
        if (text_of(row.value, "environment") != environment.name) {
            continue;
        }
        if (row.kind == "deployment") {
            deployments.push_back(row.value);
        } else if (row.kind == "environment_env" && !bool_of(row.value, "deleted")) {
            env.push_back({{"key", text_of(row.value, "key")},
                           {"value", text_of(row.value, "value")},
                           {"updated_at", text_of(row.value, "updated_at")}});
        } else if (row.kind == "domain" && !bool_of(row.value, "deleted")) {
            domains.push_back(row.value);
        }
    }
    std::ranges::sort(deployments, std::greater{}, platform_row_order);
    std::sort(env.begin(), env.end(), [](const json& left, const json& right) {
        return text_of(left, "key") < text_of(right, "key");
    });
    std::sort(domains.begin(), domains.end(), [](const json& left, const json& right) {
        return text_of(left, "host") < text_of(right, "host");
    });
    json current = deployments.empty() ? json::object() : deployments.front();
    return {{"id", environment.id},
            {"repository_id", environment.repository_id},
            {"name", environment.name},
            {"object_id", environment.object_id},
            {"deployment", current},
            {"deployments", deployments},
            {"env", env},
            {"domains", domains}};
}

} // namespace

liquid::runtime::task<PlatformResult<json>>
PlatformService::environment_document(WorkspaceRecords& records, std::string repository_id,
                                      std::string environment) const {
    auto local = settled(
        co_await get_platform_environment_record(records, repository_id, environment));
    if (!local) {
        co_return platform_failure<json>(platform_error(local.error()));
    }
    if (!*local) {
        co_return platform_failure<json>({"not_found", "no environment " + environment});
    }
    auto rows = settled(co_await list_platform_mirror_records(records, repository_id));
    if (!rows) {
        co_return platform_failure<json>(platform_error(rows.error()));
    }
    json output = environment_output(**local, *rows);
    if (const auto stale = stale_since(repository_id)) {
        output["stale_since"] = *stale;
    }
    co_return PlatformResult<json>(std::move(output));
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::list_environments(WorkspaceRecords& records, std::string repository_id) const {
    auto environments =
        settled(co_await list_platform_environment_records(records, repository_id));
    if (!environments) {
        co_return platform_failure<json>(platform_error(environments.error()));
    }
    std::vector<PlatformMirrorRow> rows;
    if (!environments->empty()) {
        auto listed = settled(co_await list_platform_mirror_records(records, repository_id));
        if (!listed) {
            co_return platform_failure<json>(platform_error(listed.error()));
        }
        rows = std::move(*listed);
    }
    json items = json::array();
    for (const PlatformEnvironment& environment : *environments) {
        const json document = environment_output(environment, rows);
        const json& deployment = document.at("deployment");
        items.push_back({{"id", environment.id},
                         {"repository_id", repository_id},
                         {"name", environment.name},
                         {"object_id", environment.object_id},
                         {"phase", text_of(deployment, "phase")},
                         {"change", text_of(deployment, "change")},
                         {"image", text_of(deployment, "image")},
                         {"url", nullable_text(text_of(deployment, "url"))},
                         {"domains", document.at("domains")},
                         {"updated_at", text_of(deployment, "updated_at")}});
    }
    json output{{"repository_id", repository_id}, {"items", items}};
    if (const auto stale = stale_since(repository_id)) {
        output["stale_since"] = *stale;
    }
    co_return PlatformResult<json>(std::move(output));
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::get_environment(WorkspaceRecords& records, std::string repository_id,
                                 std::string environment) const {
    auto completed = co_await environment_document(records, std::move(repository_id),
                                                   std::move(environment));
    co_return finished(std::move(completed));
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::list_deployments(WorkspaceRecords& records, std::string repository_id,
                                  std::string environment, int limit) const {
    auto rows = settled(co_await list_platform_mirror_records(records, repository_id));
    if (!rows) {
        co_return platform_failure<json>(platform_error(rows.error()));
    }
    std::vector<PlatformMirrorRow> deployment_rows;
    for (PlatformMirrorRow& row : *rows) {
        if (row.kind == "deployment") {
            deployment_rows.push_back(std::move(row));
        }
    }
    std::vector<json> deployments = deployment_values(deployment_rows, repository_id, environment);
    if (deployments.size() > static_cast<std::size_t>(limit)) {
        deployments.resize(static_cast<std::size_t>(limit));
    }
    json output{{"repository_id", repository_id}, {"items", deployments}};
    if (const auto stale = stale_since(repository_id)) {
        output["stale_since"] = *stale;
    }
    co_return PlatformResult<json>(std::move(output));
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::environment_logs(WorkspaceRecords& records,
                                  std::string repository_id,
                                  std::string environment,
                                  std::string since,
                                  int limit,
                                  std::string direction) {
    if (!logs_) {
        co_return platform_failure<json>(
            {"store_unavailable", "the Loki-compatible log source is not configured"});
    }
    const auto document =
        finished(co_await environment_document(records, repository_id, environment));
    if (!document) {
        co_return platform_failure<json>(document.error());
    }
    const std::string namespace_name = text_of(document->at("deployment"), "namespace");
    if (namespace_name.empty()) {
        co_return platform_failure<json>(
            {"not_found", "the environment has no deployment namespace"});
    }
    auto completed = co_await logs_->query(namespace_name, since, limit, direction);
    if (!completed) {
        co_return platform_failure<json>(platform_error(completed.error()));
    }
    auto result = std::move(*completed);
    if (!result) {
        co_return platform_failure<json>(result.error());
    }
    (*result)["repository_id"] = repository_id;
    (*result)["environment"] = environment;
    if (const auto stale = stale_since(repository_id)) {
        (*result)["stale_since"] = *stale;
    }
    co_return PlatformResult<json>(std::move(*result));
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::app_logs(std::string app_id, std::string since, int limit, std::string direction) {
    if (!logs_) {
        co_return platform_failure<json>(
            {"store_unavailable", "the Loki-compatible log source is not configured"});
    }
    const std::string namespace_name = web_app_namespace(app_id);
    if (namespace_name.empty()) {
        co_return platform_failure<json>({"invalid_input", "app id is not a UUID"});
    }
    auto completed = co_await logs_->query(namespace_name, since, limit, direction);
    if (!completed) {
        co_return platform_failure<json>(platform_error(completed.error()));
    }
    auto result = std::move(*completed);
    if (!result) {
        co_return platform_failure<json>(result.error());
    }
    (*result)["app_id"] = app_id;
    co_return PlatformResult<json>(std::move(*result));
}

PlatformResult<void> PlatformService::require_writable() const {
    if (!control_ || !store_) {
        return std::unexpected(
            PlatformError{"store_unavailable", "the operator control plane is not configured"});
    }
    if (degraded()) {
        return std::unexpected(
            PlatformError{"store_unavailable", "the operator control plane is unavailable"});
    }
    return {};
}

liquid::runtime::task<PlatformResult<void>>
PlatformService::set_environment_env(WorkspaceRecords& records,
                                     std::string repository_id,
                                     std::string environment,
                                     std::string key,
                                     std::string value,
                                     bool deleted,
                                     std::string actor) {
    if (auto writable = require_writable(); !writable) {
        co_return platform_failure<void>(writable.error());
    }
    auto local = settled(
        co_await get_platform_environment_record(records, repository_id, environment));
    if (!local) {
        co_return platform_failure<void>(platform_error(local.error()));
    }
    if (!*local) {
        co_return platform_failure<void>({"not_found", "no environment " + environment});
    }
    const std::string updated_at = now_iso8601();
    json row{{"lake", config_.lake},
             {"repository", repository_id},
             {"environment", environment},
             {"key", key},
             {"value", value},
             {"deleted", deleted},
             {"set_by", actor},
             {"updated_at", updated_at},
             {"ordering_token", new_uuid_v4()}};
    auto append_completed = co_await control_->append(std::string(environment_env_table), row);
    if (!append_completed) {
        mark_failed();
        co_return platform_failure<void>(platform_error(append_completed.error()));
    }
    auto appended = std::move(*append_completed);
    if (!appended) {
        mark_failed();
        co_return platform_failure<void>(platform_error(appended.error()));
    }
    auto mirrored = settled(co_await put_platform_mirror_record(
        records, repository_id, {"environment_env", environment_env_key(row), row, updated_at}));
    if (!mirrored) {
        co_return platform_failure<void>(platform_error(mirrored.error()));
    }
    co_return PlatformResult<void>{};
}

liquid::runtime::task<PlatformResult<void>>
PlatformService::change_domain(WorkspaceRecords& records,
                               std::string repository_id,
                               std::string environment,
                               std::string host,
                               std::string tls,
                               bool deleted,
                               std::string actor) {
    if (auto writable = require_writable(); !writable) {
        co_return platform_failure<void>(writable.error());
    }
    const auto document =
        finished(co_await environment_document(records, repository_id, environment));
    if (!document) {
        co_return platform_failure<void>(document.error());
    }
    if (deleted && tls.empty()) {
        for (const json& domain : document->at("domains")) {
            if (text_of(domain, "host") == host) {
                tls = text_of(domain, "tls");
                break;
            }
        }
    }
    if (tls.empty()) {
        tls = "acme";
    }
    const std::string updated_at = now_iso8601();
    json row{{"lake", config_.lake},
             {"repository", repository_id},
             {"environment", environment},
             {"host", host},
             {"tls", tls},
             {"status", "pending"},
             {"message", ""},
             {"deleted", deleted},
             {"requested_by", actor},
             {"updated_at", updated_at},
             {"ordering_token", new_uuid_v4()}};
    auto append_completed = co_await control_->append(std::string(domains_table), row);
    if (!append_completed) {
        mark_failed();
        co_return platform_failure<void>(platform_error(append_completed.error()));
    }
    auto appended = std::move(*append_completed);
    if (!appended) {
        mark_failed();
        co_return platform_failure<void>(platform_error(appended.error()));
    }
    auto mirrored = settled(co_await put_platform_mirror_record(
        records, repository_id, {"domain", domain_key(row), row, updated_at}));
    if (!mirrored) {
        co_return platform_failure<void>(platform_error(mirrored.error()));
    }
    co_return PlatformResult<void>{};
}

} // namespace lake::control
