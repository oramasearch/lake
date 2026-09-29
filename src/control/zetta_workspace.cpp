#include "lake/control/zetta_workspace.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

#include <zetta/connect/http_transport.hpp>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> workspace_failure(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

std::string percent_encode(std::string_view input) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
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

} // namespace

std::string workspace_slug(std::string_view customer_id) {
    return managed_service_slug("ws", customer_id);
}

nlohmann::json workspace_json(const Workspace& workspace) {
    return {{"id", workspace.id}, {"customer_id", workspace.customer_id},
            {"name", workspace.name}, {"environment", workspace.environment},
            {"desired", workspace.desired}, {"generation", workspace.generation},
            {"phase", workspace.phase}, {"observed_generation", workspace.observed_generation},
            {"message", workspace.message}, {"namespace", workspace.namespace_name},
            {"zetta_service_id", workspace.zetta_service_id},
            {"created_at", workspace.created_at}, {"updated_at", workspace.updated_at}};
}

nlohmann::json service_json(const ManagedService& service) {
    return {{"id", service.id},
            {"customer_id", service.customer_id},
            {"workspace_id", service.workspace_id},
            {"kind", service.kind},
            {"name", service.name},
            {"slug", service.slug},
            {"default", service.is_default},
            {"instances", service.instances},
            {"storage_gi", service.storage_gi},
            {"desired", service.desired},
            {"generation", service.generation},
            {"phase", service.phase},
            {"message", service.message},
            {"endpoint", service.endpoint},
            {"observed_generation", service.observed_generation},
            {"created_at", service.created_at}};
}

nlohmann::json store_json(const ManagedStore& store) {
    return {{"id", store.id},
            {"customer_id", store.customer_id},
            {"type", store.type},
            {"provider", store.provider},
            {"name", store.name},
            {"slug", store.slug},
            {"size_gi", store.size_gi},
            {"region", store.region},
            {"public", store.is_public},
            {"desired", store.desired},
            {"generation", store.generation},
            {"phase", store.phase},
            {"message", store.message},
            {"endpoint", store.endpoint},
            {"observed_generation", store.observed_generation},
            {"created_at", store.created_at}};
}

nlohmann::json WorkspaceState::to_json() const {
    return {{"workspace", slug},
            {"service_id", service_id},
            {"workspace_id", workspace_id},
            {"requested", requested},
            {"phase", phase},
            {"message", message},
            {"ready", ready()}};
}

std::map<std::string, std::string> read_agent_bus_tokens(const std::string& directory) {
    std::map<std::string, std::string> tokens;
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error)) {
        return tokens;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        // Kubernetes mounts a Secret through ..data symlinks; the keys are the plain names.
        if (name.starts_with('.') || !entry.is_regular_file(error)) {
            continue;
        }
        std::ifstream file(entry.path());
        std::string token((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.pop_back();
        }
        if (!token.empty()) {
            tokens.emplace(name, std::move(token));
        }
    }
    return tokens;
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>> ZettaWorkspaceCaller::call(
    std::string url, std::string token, std::string capability, bool read, nlohmann::json input) {
    const zetta::connect::AsyncHttpTransport& transport =
        zetta::connect::default_async_http_transport();
    if (!transport) {
        co_return workspace_failure<nlohmann::json>("store_unavailable",
                                                    "Zetta HTTP transport is not installed");
    }
    while (url.ends_with('/')) {
        url.pop_back();
    }
    zetta::connect::HttpRequest request;
    request.url = url + "/v1/" + capability;
    request.headers = {{"Accept", "application/json"}, {"Authorization", "Bearer " + token}};
    request.secret_headers = {"Authorization"};
    // A read that hangs must not hold a page for a minute (zetta#1007: get_run_report never
    // answers); actions such as compile_intent may legitimately take longer.
    request.timeout_seconds = read ? 15 : 60;
    if (capability == "receive_events" || capability == "browse_topic") {
        input["wait_ms"] = std::clamp(input.value("wait_ms", 0LL), 0LL, 30'000LL);
        request.timeout_seconds = 40;
    }
    if (read) {
        request.method = "GET";
        std::string query;
        for (const auto& [key, value] : input.items()) {
            if (value.is_null()) {
                continue;
            }
            query += query.empty() ? '?' : '&';
            query += percent_encode(key) + "=" +
                     percent_encode(value.is_string() ? value.get<std::string>() : value.dump());
        }
        request.url += query;
    } else {
        request.method = "POST";
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = input.dump();
    }
    auto [transport_error, response] = co_await transport(request);
    if (transport_error || !response.error.empty()) {
        co_return workspace_failure<nlohmann::json>(
            "store_unavailable",
            "the workspace did not answer " + capability +
                (response.error.empty() ? std::string() : ": " + response.error));
    }
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        co_return workspace_failure<nlohmann::json>(
            "store_error",
            "the workspace returned HTTP " + std::to_string(response.status) + " without JSON");
    }
    // Zetta serves its OpenAPI document bare, not in the envelope (docs/api/workspaces.md).
    if (capability == "openapi.json" && body.contains("openapi")) {
        co_return ZettaControlResult<nlohmann::json>(body);
    }
    if (!body.value("ok", false)) {
        const nlohmann::json error = body.value("error", nlohmann::json::object());
        co_return workspace_failure<nlohmann::json>(
            error.value("code", std::string("store_error")),
            error.value("message", "the workspace refused " + capability));
    }
    co_return ZettaControlResult<nlohmann::json>(body.value("value", nlohmann::json::object()));
}

WorkspaceService::WorkspaceService(WorkspaceConfig config,
                                   std::shared_ptr<Store> store,
                                   std::shared_ptr<WorkspaceCaller> caller)
    : config_(std::move(config)), store_(std::move(store)), caller_(std::move(caller)) {}

Result<std::vector<ManagedService>> WorkspaceService::services(const std::string& customer_id) {
    return store_->list_services(customer_id);
}

Result<ManagedService> WorkspaceService::service(const std::string& customer_id,
                                                 const std::string& service_id) {
    Result<ManagedService> found = store_->get_service(service_id);
    if (found && found->customer_id != customer_id) {
        return std::unexpected(StoreError{"not_found", "no service " + service_id});
    }
    return found;
}

Result<ManagedService> WorkspaceService::create_service(NewManagedService service) {
    if (!service.workspace_id.empty()) {
        const auto workspace = store_->get_workspace(service.workspace_id);
        if (!workspace || workspace->customer_id != service.customer_id || workspace->desired != "running") {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must name a running workspace in this space"});
        }
    }
    service.max_per_customer = config_.max_per_customer;
    return store_->create_service(service);
}

Result<ManagedService> WorkspaceService::workspace(const std::string& customer_id,
                                                   const std::string& workspace_id) {
    if (workspace_id.empty() || workspace_slug(workspace_id).empty()) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta"});
    }
    const auto row = store_->get_workspace(workspace_id);
    if (!row || row->customer_id != customer_id || row->desired == "deleted") {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is not a workspace in this space"});
    }
    if (row->desired != "running" || row->zetta_service_id.empty()) {
        return std::unexpected(StoreError{"workspace_not_ready", "workspace_id is not running"});
    }
    auto found = service(customer_id, row->zetta_service_id);
    if (!found || found->kind != "zetta" || found->desired != "present") {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is not a workspace Zetta in this space"});
    }
    return found;
}

Result<ManagedService> WorkspaceService::delete_service(const std::string& customer_id,
                                                        const std::string& service_id) {
    const Result<ManagedService> found = service(customer_id, service_id);
    if (!found) {
        return found;
    }
    if (!found->workspace_id.empty()) {
        const auto workspace = store_->get_workspace(found->workspace_id);
        if (workspace && workspace->zetta_service_id == service_id) {
            return std::unexpected(StoreError{"invalid_input", "use delete_workspace for the primary workspace Zetta"});
        }
    }
    return store_->withdraw_service(service_id);
}

std::optional<nlohmann::json> WorkspaceService::connection(const ManagedService& service) const {
    if (service.slug.empty() || config_.service_dir.empty()) {
        return std::nullopt;
    }
    std::ifstream input(std::filesystem::path(config_.service_dir) / (service.slug + ".json"));
    if (!input) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

Result<std::vector<ManagedStore>> WorkspaceService::stores(const std::string& customer_id) {
    return store_->list_stores(customer_id);
}

Result<ManagedStore> WorkspaceService::store(const std::string& customer_id,
                                             const std::string& store_id) {
    Result<ManagedStore> found = store_->get_store(store_id);
    if (found && found->customer_id != customer_id) {
        return std::unexpected(StoreError{"not_found", "no store " + store_id});
    }
    return found;
}

Result<ManagedStore> WorkspaceService::create_store(NewManagedStore store) {
    const StoreProvider* provider = find_store_provider(store.type, store.provider);
    if (provider == nullptr) {
        return std::unexpected(StoreError{
            "invalid_input", "there is no " + store.provider + " " + store.type + " store"});
    }
    if (!provider->available) {
        return std::unexpected(StoreError{"invalid_input",
                                          std::string(provider->label) +
                                              " is not available on this lake yet: " +
                                              std::string(provider->unavailable_reason)});
    }
    return store_->create_store(store);
}

Result<ManagedStore> WorkspaceService::delete_store(const std::string& customer_id,
                                                    const std::string& store_id) {
    const Result<ManagedStore> found = store(customer_id, store_id);
    if (!found) {
        return found;
    }
    return store_->withdraw_store(store_id);
}

std::optional<nlohmann::json> WorkspaceService::store_connection(const ManagedStore& store) const {
    ManagedService named;
    named.slug = store.slug;
    return connection(named);
}

Result<std::optional<ManagedService>>
WorkspaceService::default_service(const std::string& customer_id) {
    const Result<std::vector<ManagedService>> listed = store_->list_services(customer_id);
    if (!listed) {
        return std::unexpected(listed.error());
    }
    for (const ManagedService& service : *listed) {
        if (service.is_default && service.desired == "present") {
            return std::optional<ManagedService>(service);
        }
    }
    return std::optional<ManagedService>();
}

WorkspaceState WorkspaceService::state_of(const std::string& customer_id,
                                          const std::optional<ManagedService>& service) const {
    WorkspaceState state;
    state.slug = workspace_slug(customer_id);
    if (!service) {
        return state;
    }
    state.slug = service->slug;
    state.service_id = service->id;
    state.workspace_id = service->workspace_id;
    state.requested = true;
    state.phase = service->phase;
    state.message = service->message;
    state.endpoint = service->endpoint;
    const std::optional<nlohmann::json> connection = this->connection(*service);
    state.credential = connection && !connection->value("token", std::string()).empty();
    if (state.endpoint.empty() && connection) {
        state.endpoint = connection->value("endpoint", std::string());
    }
    return state;
}

namespace {

template <class T> ZettaControlResult<T> from_store(const StoreError& error) {
    return workspace_failure<T>(error.code, error.message);
}

} // namespace

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::state(std::string customer_id) {
    if (workspace_slug(customer_id).empty()) {
        co_return workspace_failure<WorkspaceState>("invalid_input", "customer id is not a UUID");
    }
    const Result<std::optional<ManagedService>> found = default_service(customer_id);
    if (!found) {
        co_return from_store<WorkspaceState>(found.error());
    }
    co_return state_of(customer_id, *found);
}

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::state(std::string customer_id, std::string workspace_id) {
    const auto found = workspace(customer_id, workspace_id);
    if (!found) {
        co_return from_store<WorkspaceState>(found.error());
    }
    co_return state_of(customer_id, *found);
}

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::request(std::string customer_id, std::string requested_by) {
    if (workspace_slug(customer_id).empty()) {
        co_return workspace_failure<WorkspaceState>("invalid_input", "customer id is not a UUID");
    }
    const auto rows = store_->list_workspaces(customer_id);
    if (!rows) {
        co_return from_store<WorkspaceState>(rows.error());
    }
    for (const Workspace& row : *rows) {
        if (row.name == "workspace" && row.desired != "deleted") {
            const auto found = store_->get_service(row.zetta_service_id);
            if (!found) {
                co_return from_store<WorkspaceState>(found.error());
            }
            co_return state_of(customer_id, *found);
        }
    }
    const auto created = store_->create_workspace({customer_id, "workspace", "prod", requested_by});
    if (!created) {
        co_return from_store<WorkspaceState>(created.error());
    }
    const auto service = store_->get_service(created->zetta_service_id);
    if (!service) {
        co_return from_store<WorkspaceState>(service.error());
    }
    co_return state_of(customer_id, *service);
}

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::withdraw(std::string customer_id) {
    const Result<std::optional<ManagedService>> found = default_service(customer_id);
    if (!found) {
        co_return from_store<WorkspaceState>(found.error());
    }
    if (!*found) {
        co_return workspace_failure<WorkspaceState>("not_found", "there is no workspace to delete");
    }
    WorkspacePatch patch;
    patch.desired = "deleted";
    const auto withdrawn = store_->update_workspace((*found)->workspace_id, patch);
    if (!withdrawn) {
        co_return from_store<WorkspaceState>(withdrawn.error());
    }
    WorkspaceState state = state_of(customer_id, std::nullopt);
    state.slug = (*found)->slug;
    state.service_id = withdrawn->zetta_service_id;
    state.workspace_id = withdrawn->id;
    co_return state;
}

void WorkspaceService::invalidate_registry(const std::string& customer_id,
                                           const std::string& workspace_id) {
    std::lock_guard lock(registry_mutex_);
    ++registry_epoch_;
    std::erase_if(registry_cache_, [&](const auto& row) {
        return row.second.customer_id == customer_id && row.second.workspace_id == workspace_id;
    });
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>> WorkspaceService::call(
    std::string customer_id, std::string workspace_id, std::string capability,
    bool read, nlohmann::json input,
    std::function<void(std::chrono::nanoseconds)> observe, bool cache_definitions) {
    const auto started = std::chrono::steady_clock::now();
    const auto found = workspace(customer_id, workspace_id);
    if (!found) {
        co_return from_store<nlohmann::json>(found.error());
    }
    const WorkspaceState workspace = state_of(customer_id, *found);
    if (!workspace.ready()) {
        co_return workspace_failure<nlohmann::json>("workspace_not_ready",
                                                    workspace.requested
                                                        ? "the workspace is still being prepared"
                                                        : "start the workspace first");
    }
    const auto credential = connection(*found);
    const std::string token = credential ? credential->value("token", std::string()) : std::string();
    if (token.empty()) {
        co_return workspace_failure<nlohmann::json>("workspace_not_ready",
                                                    "the workspace credential is not available");
    }
    // Deployment catalogues and project/pipeline definitions change on writes, not every
    // render. Data, connections and execution state are never cached. Recheck access and
    // readiness before every hit; successful actions invalidate definitions below.
    const bool catalogue = read &&
        ((input.empty() && (capability == "list_methodologies" || capability == "list_solutions" ||
                            capability == "list_dataset_generators")) ||
         (cache_definitions && (capability == "get_project" || capability == "get_graph")));
    std::string cache_key;
    std::uint64_t cache_epoch = 0;
    if (catalogue) {
        cache_key = nlohmann::json::array({customer_id, workspace_id, found->id,
            found->generation, workspace.endpoint, token, capability, input}).dump();
        std::lock_guard lock(registry_mutex_);
        cache_epoch = registry_epoch_;
        const auto cached = registry_cache_.find(cache_key);
        if (cached != registry_cache_.end() &&
            cached->second.expires > std::chrono::steady_clock::now()) {
            co_return ZettaControlResult<nlohmann::json>(cached->second.value);
        }
    }
    auto called = co_await caller_->call(
        workspace.endpoint, token, std::move(capability), read, std::move(input));
    if (observe) {
        observe(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started));
    }
    if (!called) {
        co_return workspace_failure<nlohmann::json>("store_unavailable", called.error().message);
    }
    if (!read && *called) {
        invalidate_registry(customer_id, workspace_id);
    }
    if (catalogue && *called) {
        std::lock_guard lock(registry_mutex_);
        // Bound retained credentials and catalogue memory. Expiry also detects an engine
        // deployment that keeps the same service address and credentials.
        if (registry_cache_.size() >= 128) {
            registry_cache_.clear();
        }
        if (cache_epoch == registry_epoch_) {
            registry_cache_.insert_or_assign(cache_key, RegistryEntry{
                **called, std::chrono::steady_clock::now() + std::chrono::seconds(5),
                customer_id, workspace_id});
        }
    }
    co_return std::move(*called);
}

} // namespace lake::control
