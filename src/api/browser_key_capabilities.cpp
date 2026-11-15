// Browser keys (lake#1152, docs/api/browser-keys.md): the key a website's own page holds to search
// one published index, and ask one published model, of one workspace.
//
// - create_browser_key: makes one, bound to a workspace, an index and optionally a model, and to
//   the website origins it may be called from; its secret is answered once. A signed-in person, an
//   API token, an agent or an MCP client with write on the account may make it.
// - list_browser_keys: the account's keys (never their secrets), revoked ones too.
// - revoke_browser_key: ends one at once.
//
// The key acts as no person: what it may do is its binding, which Registry::call checks on every
// call it makes (registry.cpp, authorize_browser_key), with its origins against `Origin` and its
// calls a minute. Its hash and binding are a control-plane row (`browser_keys`), like
// every other credential; nothing about it is in a workspace Zetta.

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {

namespace {

using json = nlohmann::json;

ApiResult store_failure(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable",
                              "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

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

json nullable(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

std::string state_of(const control::BrowserKey& key) {
    if (!key.revoked_at.empty()) {
        return "revoked";
    }
    if (!key.expires_at.empty()) {
        // RFC 3339 UTC text compares in time order.
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (key.expires_at <= std::format("{:%Y-%m-%dT%H:%M:%SZ}", now)) {
            return "expired";
        }
    }
    return "active";
}

} // namespace

json browser_key_json(const control::BrowserKey& key) {
    json capabilities = json::array({"search_index"});
    if (!key.model_id.empty()) {
        capabilities.push_back("predict");
    }
    return {{"id", key.id},
            {"name", key.name},
            {"customer_id", key.customer_id},
            {"workspace_id", key.workspace_id},
            {"index_id", key.index_id},
            {"model_id", nullable(key.model_id)},
            {"origins", key.origins},
            {"capabilities", std::move(capabilities)},
            {"rate_limit_per_minute", key.rate_limit_per_minute},
            {"state", state_of(key)},
            {"created_by", key.created_by_taap_id},
            {"created_at", key.created_at},
            {"expires_at", nullable(key.expires_at)},
            {"revoked_at", nullable(key.revoked_at)},
            {"last_used_at", nullable(key.last_used_at)}};
}

json browser_key_schema() {
    const json text{{"type", "string"}};
    const json maybe_text{{"type", json::array({"string", "null"})}};
    return {{"type", "object"},
            {"properties",
             {{"id", text},
              {"name", text},
              {"customer_id", text},
              {"workspace_id", text},
              {"index_id", text},
              {"model_id", maybe_text},
              {"origins", {{"type", "array"}, {"items", text}}},
              {"capabilities", {{"type", "array"}, {"items", text}}},
              {"rate_limit_per_minute", {{"type", "integer"}}},
              {"state", {{"type", "string"}, {"enum", {"active", "revoked", "expired"}}}},
              {"created_by", text},
              {"created_at", text},
              {"expires_at", maybe_text},
              {"revoked_at", maybe_text},
              {"last_used_at", maybe_text}}},
            {"required",
             json::array({"id",
                          "customer_id",
                          "workspace_id",
                          "index_id",
                          "origins",
                          "capabilities",
                          "rate_limit_per_minute",
                          "state"})}};
}

std::expected<std::vector<std::string>, ApiResult> browser_key_origins(const json& origins) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < origins.size(); ++i) {
        const auto origin = control::browser_key_origin(origins[i].get<std::string>());
        if (!origin) {
            return std::unexpected(ApiResult::failure(
                "invalid_input", origin.error(), "/origins/" + std::to_string(i)));
        }
        if (std::ranges::find(out, *origin) == out.end()) {
            out.push_back(*origin);
        }
    }
    if (out.empty()) {
        return std::unexpected(ApiResult::failure(
            "invalid_input",
            "origins: name the website's origin, such as https://www.example.com",
            "/origins"));
    }
    return out;
}

liquid::runtime::task<ApiResult> create_browser_key(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return no_store();
    }
    if (!context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string index_id = input.at("index_id");
    const std::string model_id = input.value("model_id", std::string());
    auto origins = browser_key_origins(input.at("origins"));
    if (!origins) {
        co_return std::move(origins.error());
    }
    // The index and the model must exist in that workspace: the key is bound to what is there.
    ApiContext bound = context;
    bound.workspace_id = workspace_id;
    auto index = control::settled(
        co_await call_workspace(bound, customer_id, "get_index", true, {{"index_id", index_id}}));
    if (!index) {
        co_return workspace_failure(index.error(), {{"index_id", index_id}});
    }
    json model = nullptr;
    if (!model_id.empty()) {
        auto found = control::settled(co_await call_workspace(
            bound, customer_id, "get_model", true, {{"model_id", model_id}}));
        if (!found) {
            co_return ApiResult::failure("not_found",
                                         "no model " + model_id + " in workspace " + workspace_id +
                                             " (use its id from Ship → Models)",
                                         "/model_id");
        }
        model = std::move(*found);
    }
    control::NewBrowserKey key{
        .customer_id = customer_id,
        .workspace_id = workspace_id,
        .index_id = index_id,
        .model_id = model_id,
        .name = input.value("name", std::string()),
        .origins = std::move(*origins),
        .rate_limit_per_minute =
            input.value("rate_limit_per_minute", control::browser_key_default_rate_limit),
        .expires_in_seconds = input.value("expires_in_seconds", std::int64_t{0}),
        .created_by_user_id = context.acting_user_id,
        .created_by_taap_id = context.acting_identity,
    };
    if (key.name.empty()) {
        key.name = "website " + key.origins.front();
    }
    const auto issued = context.store->create_browser_key(key);
    if (!issued) {
        co_return store_failure(issued.error());
    }
    context.audit_object = "browser_key/" + issued->key.id;
    context.audit_details["browser_key_id"] = issued->key.id;
    context.audit_details["index_id"] = index_id;
    if (!model_id.empty()) {
        context.audit_details["model_id"] = model_id;
    }
    context.audit_details["origins"] = issued->key.origins;
    json answer{{"key", browser_key_json(issued->key)}, {"secret", issued->secret}};
    // Whether search and predict answer yet: an unpublished index or model is refused when called.
    answer["index_published"] = index->is_object() && index->value("exposed", false);
    if (!model.is_null()) {
        answer["model_published"] = model.is_object() && model.value("exposed", false);
    }
    co_return ApiResult::success(std::move(answer));
}

void register_browser_key_capabilities(Registry& registry) {
    const json id_text{{"type", "string"}, {"minLength", 1}};
    registry.add(Capability{
        .name = "create_browser_key",
        .title = "Create a browser key",
        .description =
            "Make the key a website's own page holds (lake#1152). It is bound to one search index "
            "(`index_id`) and optionally one model (`model_id`) of one workspace, may call only "
            "search_index (and predict when a model is named) on them, and only from the website "
            "`origins` listed (https://host[:port]; http:// for localhost while trying it): Lake "
            "checks each call's Origin header and its CORS answers only those origins. It belongs "
            "to the workspace, not to your sign-in: it lives until revoke_browser_key or the "
            "optional `expires_in_seconds`, and signing out leaves it working. It may make "
            "`rate_limit_per_minute` calls a minute (600 unless set, at most 6000). Every search "
            "and answer is billed to the account. The `secret` is answered once; Lake keeps only "
            "its hash. A signed-in person, an API token, an agent or an MCP client with write on "
            "the account may make one.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_id", id_text},
               {"workspace_id", id_text},
               {"index_id",
                {{"type", "string"},
                 {"minLength", 1},
                 {"description", "the search index the key searches (list_indexes)"}}},
               {"model_id",
                {{"type", "string"},
                 {"minLength", 1},
                 {"description", "a model the key may also ask with predict (list_models)"}}},
               {"origins",
                {{"type", "array"},
                 {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 300}}},
                 {"minItems", 1},
                 {"maxItems", control::browser_key_max_origins},
                 {"description",
                  "the website origins that may call with the key, such as "
                  "https://www.example.com"}}},
               {"name", {{"type", "string"}, {"maxLength", 200}}},
               {"expires_in_seconds",
                {{"type", "integer"},
                 {"minimum", 60},
                 {"maximum", 10LL * 366 * 24 * 3600},
                 {"description", "when the key stops working; left out, it lives until revoked"}}},
               {"rate_limit_per_minute",
                {{"type", "integer"},
                 {"minimum", 1},
                 {"maximum", control::browser_key_max_rate_limit},
                 {"description", "calls a minute the key may make (default 600)"}}}}},
             {"required", json::array({"customer_id", "workspace_id", "index_id", "origins"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"key", browser_key_schema()},
                            {"secret", {{"type", "string"}}},
                            {"index_published", {{"type", "boolean"}}},
                            {"model_published", {{"type", "boolean"}}}}},
                          {"required", json::array({"key", "secret"})}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = create_browser_key,
    });
    registry.add(Capability{
        .name = "list_browser_keys",
        .title = "List browser keys",
        .description =
            "The account's browser keys (create_browser_key), newest first, revoked and expired "
            "ones too: each key's binding (workspace, index, model), origins, calls a minute, "
            "state, who made it and when it was last used. Never a secret. `workspace_id` and "
            "`index_id` narrow the list.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_id", id_text}, {"workspace_id", id_text}, {"index_id", id_text}}},
             {"required", json::array({"customer_id"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"keys", {{"type", "array"}, {"items", browser_key_schema()}}}}},
                          {"required", json::array({"keys"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // A list authorizes its own candidates (every list_ capability does): here, read on the
        // one account it names.
        .authorizes_candidates = true,
        .handler =
            [](const json& input, ApiContext& context) {
                if (!context.store) {
                    return no_store();
                }
                if (context.acting_identity != "admin") {
                    const auto object = customer_object(input, context);
                    if (!object) {
                        return ApiResult::failure(
                            object.error().code, object.error().message, object.error().path);
                    }
                    const auto decision = control::authorize(*context.store,
                                                             context.acting_identity,
                                                             *object,
                                                             "read",
                                                             context.authorization_cache);
                    if (!decision) {
                        return ApiResult::failure(decision.error().code, decision.error().message);
                    }
                    if (!decision->allowed) {
                        return ApiResult::failure("unauthorized",
                                                  "principal is not allowed to read customer");
                    }
                }
                const auto keys = context.store->list_browser_keys(
                    input.at("customer_id"), input.value("workspace_id", std::string()));
                if (!keys) {
                    return store_failure(keys.error());
                }
                const std::string index_id = input.value("index_id", std::string());
                json listed = json::array();
                for (const auto& key : *keys) {
                    if (index_id.empty() || key.index_id == index_id) {
                        listed.push_back(browser_key_json(key));
                    }
                }
                return ApiResult::success({{"keys", std::move(listed)}});
            },
    });
    registry.add(Capability{
        .name = "revoke_browser_key",
        .title = "Revoke a browser key",
        .description = "End one of the account's browser keys at once: every call with it is "
                       "refused from then on, on every replica. Revoking a revoked key changes "
                       "nothing.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", id_text}, {"id", id_text}}},
                         {"required", json::array({"customer_id", "id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"key", browser_key_schema()}}},
                          {"required", json::array({"key"})}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler =
            [](const json& input, ApiContext& context) {
                if (!context.store) {
                    return no_store();
                }
                const auto revoked = context.store->revoke_browser_key(
                    input.at("customer_id"), input.at("id"), context.acting_identity);
                if (!revoked) {
                    ApiResult failure = store_failure(revoked.error());
                    if (failure.error.code == "not_found") {
                        failure.error.path = "/id";
                    }
                    return failure;
                }
                context.audit_object = "browser_key/" + revoked->id;
                context.audit_details["browser_key_id"] = revoked->id;
                return ApiResult::success({{"key", browser_key_json(*revoked)}});
            },
    });
}

} // namespace lake::api
