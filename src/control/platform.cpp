#include "lake/control/platform.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <zetta/connect/http_transport.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/platform_store.hpp"

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
        if (found == latest.end() || text_of(found->second, "updated_at") < updated_at) {
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
        if (previous != previous_deployments.end() &&
            text_of(row, "updated_at") <= text_of(previous->second, "updated_at")) {
            continue;
        }
        rows_by_deployment[key].push_back(row);
    }

    std::vector<json> phases;
    for (auto& [key, deployment_rows] : rows_by_deployment) {
        std::ranges::sort(
            deployment_rows, {}, [](const json& row) { return text_of(row, "updated_at"); });
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
    std::ranges::sort(
        output, std::greater{}, [](const json& row) { return text_of(row, "updated_at"); });
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
    : store_(std::move(store)), platform_store_(dynamic_cast<PlatformStore*>(store_.get())),
      control_(std::move(control)), logs_(std::move(logs)), config_(std::move(config)) {
    if (!store_ || !platform_store_ || !control_ || config_.lake.empty()) {
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

liquid::runtime::task<PlatformResult<void>> PlatformService::sync() {
    if (!store_ || !platform_store_ || !control_ || config_.lake.empty()) {
        mark_failed();
        co_return platform_failure<void>(
            {"store_unavailable", "the platform mirror is not configured"});
    }
    const auto quoted_lake = quote_zetta_literal(config_.lake);
    if (!quoted_lake) {
        mark_failed();
        co_return platform_failure<void>(platform_error(quoted_lake.error()));
    }
    auto deployments_completed =
        co_await control_->sql("SELECT " + select_columns(deployment_columns) + " FROM " +
                                   std::string(deployments_table) + " WHERE lake = " + *quoted_lake,
                               std::string(operator_control_database));
    if (!deployments_completed) {
        mark_failed();
        co_return platform_failure<void>(platform_error(deployments_completed.error()));
    }
    auto deployments = std::move(*deployments_completed);
    if (!deployments) {
        mark_failed();
        co_return platform_failure<void>(platform_error(deployments.error()));
    }
    auto environment_env_completed = co_await control_->sql(
        "SELECT " + select_columns(environment_env_columns) + " FROM " +
            std::string(environment_env_table) + " WHERE lake = " + *quoted_lake,
        std::string(operator_control_database));
    if (!environment_env_completed) {
        mark_failed();
        co_return platform_failure<void>(platform_error(environment_env_completed.error()));
    }
    auto environment_env = std::move(*environment_env_completed);
    if (!environment_env) {
        mark_failed();
        co_return platform_failure<void>(platform_error(environment_env.error()));
    }
    auto domains_completed =
        co_await control_->sql("SELECT " + select_columns(domain_columns) + " FROM " +
                                   std::string(domains_table) + " WHERE lake = " + *quoted_lake,
                               std::string(operator_control_database));
    if (!domains_completed) {
        mark_failed();
        co_return platform_failure<void>(platform_error(domains_completed.error()));
    }
    auto domains = std::move(*domains_completed);
    if (!domains) {
        mark_failed();
        co_return platform_failure<void>(platform_error(domains.error()));
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
    std::map<std::string, json> previous_deployments;
    const auto old_rows = platform_store_->list_platform_mirror("deployment");
    if (!old_rows) {
        mark_failed();
        co_return platform_failure<void>(platform_error(old_rows.error()));
    }
    for (const PlatformMirrorRow& row : *old_rows) {
        previous_deployments[row.key] = row.value;
    }
    const std::vector<json> deployment_phases =
        new_deployment_phases(*deployments, previous_deployments);
    const std::string seen_at = now_iso8601();
    const auto ensure_rows = [&](const std::map<std::string, json>& rows) -> PlatformResult<void> {
        for (const auto& [key, row] : rows) {
            static_cast<void>(key);
            auto environment = platform_store_->ensure_platform_environment(
                text_of(row, "repository"), text_of(row, "environment"));
            if (!environment) {
                return std::unexpected(platform_error(environment.error()));
            }
        }
        return {};
    };
    const auto apply_rows = [&](std::string_view kind,
                                const std::map<std::string, json>& rows) -> PlatformResult<void> {
        for (const auto& [key, row] : rows) {
            auto mirrored =
                platform_store_->upsert_platform_mirror({std::string(kind), key, row, seen_at});
            if (!mirrored) {
                return std::unexpected(platform_error(mirrored.error()));
            }
        }
        return {};
    };
    if (auto result = ensure_rows(*latest_deployments); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    if (auto result = ensure_rows(*latest_environment_env); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    if (auto result = ensure_rows(*latest_domains); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    if (auto result = apply_rows("environment_env", *latest_environment_env); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    if (auto result = apply_rows("domain", *latest_domains); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    for (const json& row : deployment_phases) {
        const std::string key = deployment_key(row);
        const std::string phase = text_of(row, "phase");
        const auto repository = store_->get_repository(text_of(row, "repository"));
        if (!repository) {
            mark_failed();
            co_return platform_failure<void>(platform_error(repository.error()));
        }
        const audit::EventDraft event{
            .customer_id = repository->customer_id,
            .kind = "platform.deployment.phase",
            .principal = text_of(row, "requested_by"),
            .object = "deployment/" + key,
            .action = "deployment_phase",
            .outcome = "success",
            .request_id = new_uuid_v4(),
            .method = "zetta_mirror",
            .details = {{"repository_id", repository->id},
                        {"environment", text_of(row, "environment")},
                        {"phase", phase},
                        {"updated_at", text_of(row, "updated_at")}},
        };
        const audit::IdentityOptions options{config_.identity_seed, config_.instance_kek};
        const auto recorded = audit::record(*store_, event, options);
        if (!recorded) {
            mark_failed();
            co_return platform_failure<void>({recorded.error().code, recorded.error().message});
        }
    }
    if (auto result = apply_rows("deployment", *latest_deployments); !result) {
        mark_failed();
        co_return platform_failure<void>(result.error());
    }
    mark_succeeded();
    co_return PlatformResult<void>{};
}

PlatformResult<json> PlatformService::environment_document(const std::string& repository_id,
                                                           const std::string& environment) const {
    if (!platform_store_) {
        return std::unexpected(
            PlatformError{"store_unavailable", "the platform mirror is not configured"});
    }
    const auto local = platform_store_->get_platform_environment(repository_id, environment);
    if (!local) {
        return std::unexpected(platform_error(local.error()));
    }
    const auto deployment_rows = platform_store_->list_platform_mirror("deployment");
    const auto env_rows = platform_store_->list_platform_mirror("environment_env");
    const auto domain_rows = platform_store_->list_platform_mirror("domain");
    if (!deployment_rows || !env_rows || !domain_rows) {
        const StoreError error = !deployment_rows
                                     ? deployment_rows.error()
                                     : (!env_rows ? env_rows.error() : domain_rows.error());
        return std::unexpected(platform_error(error));
    }
    std::vector<json> deployments = deployment_values(*deployment_rows, repository_id, environment);
    json env = json::array();
    for (const PlatformMirrorRow& row : *env_rows) {
        if (text_of(row.value, "repository") == repository_id &&
            text_of(row.value, "environment") == environment && !bool_of(row.value, "deleted")) {
            env.push_back({{"key", text_of(row.value, "key")},
                           {"value", text_of(row.value, "value")},
                           {"updated_at", text_of(row.value, "updated_at")}});
        }
    }
    json domains = json::array();
    for (const PlatformMirrorRow& row : *domain_rows) {
        if (text_of(row.value, "repository") == repository_id &&
            text_of(row.value, "environment") == environment && !bool_of(row.value, "deleted")) {
            domains.push_back(row.value);
        }
    }
    json current = deployments.empty() ? json::object() : deployments.front();
    json output{{"id", local->id},
                {"repository_id", repository_id},
                {"name", environment},
                {"object_id", local->object_id},
                {"deployment", current},
                {"deployments", deployments},
                {"env", env},
                {"domains", domains}};
    const std::optional<std::string> stale = stale_since();
    if (stale) {
        output["stale_since"] = *stale;
    }
    return output;
}

PlatformResult<json> PlatformService::list_environments(const std::string& repository_id) const {
    if (!platform_store_) {
        return std::unexpected(
            PlatformError{"store_unavailable", "the platform mirror is not configured"});
    }
    const auto environments = platform_store_->list_platform_environments(repository_id);
    if (!environments) {
        return std::unexpected(platform_error(environments.error()));
    }
    json items = json::array();
    for (const PlatformEnvironment& environment : *environments) {
        auto document = environment_document(repository_id, environment.name);
        if (!document) {
            return std::unexpected(document.error());
        }
        const json deployment = document->at("deployment");
        items.push_back({{"id", environment.id},
                         {"repository_id", repository_id},
                         {"name", environment.name},
                         {"object_id", environment.object_id},
                         {"phase", text_of(deployment, "phase")},
                         {"change", text_of(deployment, "change")},
                         {"image", text_of(deployment, "image")},
                         {"url", nullable_text(text_of(deployment, "url"))},
                         {"domains", document->at("domains")},
                         {"updated_at", text_of(deployment, "updated_at")}});
    }
    json output{{"repository_id", repository_id}, {"items", items}};
    const std::optional<std::string> stale = stale_since();
    if (stale) {
        output["stale_since"] = *stale;
    }
    return output;
}

PlatformResult<json> PlatformService::get_environment(const std::string& repository_id,
                                                      const std::string& environment) const {
    return environment_document(repository_id, environment);
}

PlatformResult<json> PlatformService::list_deployments(const std::string& repository_id,
                                                       const std::string& environment,
                                                       int limit) const {
    if (!platform_store_) {
        return std::unexpected(
            PlatformError{"store_unavailable", "the platform mirror is not configured"});
    }
    const auto rows = platform_store_->list_platform_mirror("deployment");
    if (!rows) {
        return std::unexpected(platform_error(rows.error()));
    }
    std::vector<json> deployments = deployment_values(*rows, repository_id, environment);
    if (deployments.size() > static_cast<std::size_t>(limit)) {
        deployments.resize(static_cast<std::size_t>(limit));
    }
    json output{{"repository_id", repository_id}, {"items", deployments}};
    const std::optional<std::string> stale = stale_since();
    if (stale) {
        output["stale_since"] = *stale;
    }
    return output;
}

liquid::runtime::task<PlatformResult<json>>
PlatformService::environment_logs(std::string repository_id,
                                  std::string environment,
                                  std::string since,
                                  int limit,
                                  std::string direction) {
    if (!logs_) {
        co_return platform_failure<json>(
            {"store_unavailable", "the Loki-compatible log source is not configured"});
    }
    const auto document = environment_document(repository_id, environment);
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
    const std::optional<std::string> stale = stale_since();
    if (stale) {
        (*result)["stale_since"] = *stale;
    }
    co_return PlatformResult<json>(std::move(*result));
}

PlatformResult<void> PlatformService::require_writable() const {
    if (!control_ || !platform_store_) {
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
PlatformService::set_environment_env(std::string repository_id,
                                     std::string environment,
                                     std::string key,
                                     std::string value,
                                     bool deleted,
                                     std::string actor) {
    if (auto writable = require_writable(); !writable) {
        co_return platform_failure<void>(writable.error());
    }
    const auto local = platform_store_->get_platform_environment(repository_id, environment);
    if (!local) {
        co_return platform_failure<void>(platform_error(local.error()));
    }
    const std::string updated_at = now_iso8601();
    json row{{"lake", config_.lake},
             {"repository", repository_id},
             {"environment", environment},
             {"key", key},
             {"value", value},
             {"deleted", deleted},
             {"set_by", actor},
             {"updated_at", updated_at}};
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
    const auto mirrored = platform_store_->upsert_platform_mirror(
        {"environment_env", environment_env_key(row), row, updated_at});
    if (!mirrored) {
        co_return platform_failure<void>(platform_error(mirrored.error()));
    }
    co_return PlatformResult<void>{};
}

liquid::runtime::task<PlatformResult<void>>
PlatformService::change_domain(std::string repository_id,
                               std::string environment,
                               std::string host,
                               std::string tls,
                               bool deleted,
                               std::string actor) {
    if (auto writable = require_writable(); !writable) {
        co_return platform_failure<void>(writable.error());
    }
    const auto local = platform_store_->get_platform_environment(repository_id, environment);
    if (!local) {
        co_return platform_failure<void>(platform_error(local.error()));
    }
    if (deleted && tls.empty()) {
        const auto document = environment_document(repository_id, environment);
        if (document) {
            for (const json& domain : document->at("domains")) {
                if (text_of(domain, "host") == host) {
                    tls = text_of(domain, "tls");
                    break;
                }
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
             {"updated_at", updated_at}};
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
    const auto mirrored =
        platform_store_->upsert_platform_mirror({"domain", domain_key(row), row, updated_at});
    if (!mirrored) {
        co_return platform_failure<void>(platform_error(mirrored.error()));
    }
    co_return PlatformResult<void>{};
}

} // namespace lake::control
