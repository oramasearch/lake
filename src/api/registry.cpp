#include "lake/api/registry.hpp"
#include "lake/api/balance.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <print>
#include <set>
#include <thread>

#include "lake/api/audit_routing.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"
#include "lake/transparency/log.hpp"

#include <liquid/runtime.hpp>

namespace lake::api {
namespace {

// Capabilities that wait on a repository store run off the server's thread (lake#380), and so
// does the anonymous identity scan, whose first call after a start verifies the whole
// transparency log (lake#941). Their handlers reach shared state only through the control store,
// the repository store (one lane per store), search (which reaches stores only through the
// repository store), the transparency log and checkpoints, and the recorders, all of which lock.
bool runs_off_server_thread(const Capability& capability) {
    const std::string_view object = capability.object;
    return object.starts_with("repository") || object.find("drive") != std::string_view::npos ||
           object == "model" ||
           object == "dataset" || object == "workspace store move" ||
           // Removing a space's store waits on the space's lane (lake#380).
           object == "space store removal" ||
           capability.name == "replicate_repository" ||
           // Deleting a workspace closes its store, waiting on its lane.
           capability.name == "delete_workspace" ||
           capability.name == "transparency_identity_scan";
}

bool contains_database_diagnostic(std::string_view message) {
    std::string uppercase(message);
    std::ranges::transform(uppercase, uppercase.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return uppercase.contains("ERROR:") || uppercase.contains("DETAIL:");
}

std::string product_store_message(std::string_view code) {
    if (code == "conflict") {
        return "the requested change conflicts with existing data";
    }
    if (code == "invalid_input") {
        return "the request contains invalid data";
    }
    if (code == "store_unavailable") {
        return "the control-plane store is unavailable";
    }
    return "the control-plane store could not complete the request";
}

// A database constraint the caller's input can break, said as the rule and the field (lake#592).
// `field` is the input the rule is about when the caller's path does not already name one; the
// message is written with the field the caller sent, so create_drive's `repository_name` and
// create_repository's `name` each read as their own.
struct ConstraintRule {
    std::string_view constraint;
    std::string_view field;
    std::string_view rule; // follows "<field> "
};

constexpr std::array<ConstraintRule, 17> constraint_rules{{
    {"repository_name_format",
     "name",
     "is 1 to 256 lowercase letters, digits and hyphens, starting and ending with a letter or "
     "digit"},
    {"customer_name_format",
     "name",
     "is 1 to 256 lowercase letters, digits and hyphens, starting and ending with a letter or "
     "digit"},
    {"team_name_format",
     "name",
     "is 1 to 256 lowercase letters, digits and hyphens, starting and ending with a letter or "
     "digit"},
    {"teams_name_format",
     "name",
     "is 1 to 256 lowercase letters, digits and hyphens, starting and ending with a letter or "
     "digit"},
    {"service_name_format",
     "name",
     "is 1 to 31 lowercase letters, digits and hyphens, starting with a letter"},
    {"store_name_format",
     "name",
     "is 1 to 31 lowercase letters, digits and hyphens, starting with a letter"},
    {"app_name_format",
     "name",
     "is 1 to 31 lowercase letters, digits and hyphens, starting with a letter and not ending "
     "with a hyphen"},
    {"grants_role_check",
     "role",
     "is one of \"read\", \"write\", \"admin\", \"deploy\", \"review\", \"share\""},
    {"memberships_role_check", "role", "is one of \"owner\", \"admin\", \"member\""},
    {"invitations_role_check", "role", "is one of \"owner\", \"admin\", \"member\""},
    {"invitations_invitee_type_check", "invitee_type", "is one of \"email\", \"handle\""},
    {"grants_attestation_check",
     "attestation_digest",
     "is required: the digest of the signed statement that records this grant"},
    {"memberships_attestation_check",
     "attestation_digest",
     "is required: the digest of the signed statement that records this membership"},
    {"grants_principal_object_role_unique",
     "principal_taap_id",
     "already holds this role here: the item is already shared with that person in that role"},
    {"memberships_scope_principal_unique", "principal_id", "is already a member here"},
    {"invitations_expiry_check", "expires_in_seconds", "must be in the future"},
    {"audit_retention_floor_check", "retention_days", "is at least 400 days"},
}};

void log_database_diagnostic(std::string_view code, std::string_view message) {
    const nlohmann::json entry = {
        {"event", "api_store_error"}, {"code", code}, {"detail", message}};
    std::println(stderr, "{}", entry.dump());
}

constexpr std::array<ErrorCode, 37> documented_error_codes{{
    {"invalid_input", 400, "The input is at fault; `path` points at the field."},
    {"secret_required",
     400,
     "A value that looks like a secret was sent in plain text; store it as a Secret and pass its "
     "reference."},
    {"invalid_digest",
     400,
     "An archive or record does not match the digest it carries, so it was changed or damaged."},
    {"invalid_signature", 400, "A signature does not verify against its key."},
    {"unauthenticated",
     401,
     "No credential was accepted: `reason` says whether it was missing, invalid or expired, and "
     "`help` links to the page that makes a new key."},
    {"unauthorized", 403, "The credential is valid but does not grant this call on this object."},
    {"mfa_required", 403, "The account must finish multi-factor verification first."},
    {"identity_proof_required",
     403,
     "An identity document becomes an account's identity only with a fresh signature by its key: "
     "sign begin_identity_proof's challenge and send identity_proof."},
    {"forbidden",
     403,
     "The object is read-only for everyone, such as one of Orama's catalogue solutions; the "
     "message says why and what to do instead."},
    {"consent_required",
     403,
     "The workspace requires an approval before it reaches an outside source, which Lake cannot "
     "give."},
    {"not_found", 404, "The addressed object does not exist, or the caller cannot see it."},
    {"unknown_capability", 404, "No capability has this name."},
    {"method_not_allowed",
     405,
     "The capability exists but takes the other HTTP method: reads GET, actions POST."},
    {"payment_required",
     402,
     "The account's prepaid balance is past its overdraft limit, or it has not made its first "
     "top-up: nothing that costs money starts until funds are added. `details` gives the "
     "balance, the limit, how much to add and the Add funds link; reading, signing in, stopping "
     "and deleting stay open."},
    {"conflict", 409, "The change conflicts with existing state, such as a name already taken."},
    {"already_exists", 409, "A repository object or reference with this identity already exists."},
    {"locked", 409, "Another writer holds the repository reference; retry."},
    {"unrecoverable",
     409,
     "The request cannot succeed as asked, such as recovering a key only a lost device held."},
    {"not_approved",
     409,
     "The workspace action needs an approval it does not have yet: approve the plan or the "
     "decision first."},
    {"safe_mode_warning",
     409,
     "The database's safe mode warns on what the call would run: `details` names the database or "
     "connection, the level, the exact statements and a `confirmation`; repeat the same call with "
     "`safe_mode_confirmation` set to it to run them."},
    {"gate_blocked", 409, "A pipeline gate refused the run; the message names the failing check."},
    {"sign_in_required",
     409,
     "An MCP connection's server needs a person to sign in again, or its credential is missing or "
     "refused."},
    {"invalid_chain",
     409,
     "The audit log's hash chain is broken: an event is missing or was changed."},
    {"size_invalid", 413, "The request body is larger than the limit."},
    // 422, not 502: Cloudflare replaces a 502 or 504 body with its own page (lake#572), and the
    // engine refusing a call (a typo in SQL, a corrupt audio file) is not an outage.
    {"engine_error",
     422,
     "The workspace engine or the outside service refused the call; the message is its."},
    {"not_supported", 501, "This Lake or its workspace engine cannot do what was asked yet."},
    {"quota_exceeded",
     507,
     "The customer's storage quota is full; existing content stays readable."},
    {"store_error", 500, "The server failed; the message says what failed."},
    {"corrupt", 500, "Stored repository data failed an integrity check."},
    {"io", 500, "Repository storage could not be read or written."},
    {"crypto_error", 500, "A signing, hashing or certificate operation failed on the server."},
    {"store_unavailable",
     503,
     "The control-plane store, the workspace or another dependency is not reachable now."},
    {"workspace_not_ready", 503, "The workspace or service exists but is not ready yet."},
    {"audit_queue_full",
     503,
     "The workspace audit queue is full; inspect operation_result before retrying."},
    {"busy", 503, "The resource is busy; retry."},
    {"rate_limited",
     429,
     "A request limit was reached; retry when capacity or the authentication cooldown permits."},
    {"usage_limit_reached",
     429,
     "The account has used its monthly limit on this dimension (model tokens); an administrator "
     "raises or removes it with set_usage_limit."},
}};

// Other spellings of a documented code: the workspace engine's and liquid's.
std::string_view documented_spelling(std::string_view code) {
    if (code == "invalid_argument") {
        return "invalid_input";
    }
    if (code == "unsupported") {
        return "not_supported";
    }
    if (code == "unavailable" || code == "not_remote" || code == "not_configured") {
        return "store_unavailable";
    }
    if (code == "internal") {
        return "store_error";
    }
    return code;
}

} // namespace

namespace detail {
boost::capy::thread_pool& blocking_pool() {
    static boost::capy::thread_pool pool(32, "lake-store-");
    return pool;
}
std::atomic<int>& blocking_task_limit() {
    static std::atomic<int> limit{256};
    return limit;
}
std::atomic<int>& blocking_tasks() {
    static std::atomic<int> tasks{0};
    return tasks;
}
std::mutex& completion_gate() {
    static std::mutex gate;
    return gate;
}
namespace {
std::set<const void*>& closed_contexts() {
    static std::set<const void*> contexts;
    return contexts;
}
} // namespace
bool execution_context_closed(const void* context) {
    return closed_contexts().contains(context);
}
} // namespace detail

int wait_for_blocking_tasks(std::chrono::steady_clock::time_point deadline) {
    while (detail::blocking_tasks().load() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return detail::blocking_tasks().load();
}

void close_execution_context(const void* context) {
    std::lock_guard gate(detail::completion_gate());
    detail::closed_contexts().insert(context);
}

void forget_execution_context(const void* context) {
    std::lock_guard gate(detail::completion_gate());
    detail::closed_contexts().erase(context);
}

std::span<const ErrorCode> error_codes() {
    return documented_error_codes;
}

const ErrorCode* find_error_code(std::string_view code) {
    const auto found = std::ranges::find(documented_error_codes, code, &ErrorCode::code);
    return found == documented_error_codes.end() ? nullptr : &*found;
}

bool capability_writes(const Capability& capability) {
    // Signing creates a fresh credential even though only recipient read permission is needed.
    if (capability.name == "issue_network_tunnel_certificate") {
        return true;
    }
    return capability.kind == CapabilityKind::Action && capability.action != "read" &&
           capability.name != "workspace_query";
}

namespace {
bool named_with(const Capability& capability, std::initializer_list<std::string_view> prefixes) {
    return std::ranges::any_of(prefixes, [&](std::string_view prefix) {
        return std::string_view(capability.name).starts_with(prefix);
    });
}
} // namespace

bool capability_destroys(const Capability& capability) {
    return capability_writes(capability) && named_with(capability,
                                                       {"delete_",
                                                        "destroy_",
                                                        "remove_",
                                                        "revoke_",
                                                        "drop_",
                                                        "purge_",
                                                        "unroute_",
                                                        "scim_delete_"});
}

bool capability_idempotent(const Capability& capability) {
    if (!capability_writes(capability)) {
        return true;
    }
    // Each sets a value or removes something by name: a second identical call finds it done.
    return named_with(capability,
                      {"set_",
                       "unset_",
                       "upsert_",
                       "scim_upsert_",
                       "scim_patch_",
                       "delete_",
                       "destroy_",
                       "remove_",
                       "revoke_",
                       "drop_",
                       "purge_",
                       "settle_",
                       "unexpose_",
                       "update_"});
}

ApiResult ApiResult::success(nlohmann::json output) {
    ApiResult result;
    result.ok = true;
    result.value = std::move(output);
    return result;
}

ApiResult ApiResult::failure(std::string code, std::string message, std::string path) {
    code = std::string(documented_spelling(code));
    if (find_error_code(code) == nullptr) {
        // An undocumented code never reaches a caller (lake#450): it is named in the message.
        message = code.empty() ? message : code + ": " + message;
        code = "store_error";
    }
    const bool diagnostic = contains_database_diagnostic(message);
    if (diagnostic) {
        log_database_diagnostic(code, message);
    }
    // PostgreSQL names the constraint in its diagnostic and the in-memory store in its message;
    // either way the caller gets the rule and the field, never the constraint's name.
    const auto rule = std::ranges::find_if(constraint_rules, [&](const ConstraintRule& known) {
        return message.contains(known.constraint);
    });
    if (rule != constraint_rules.end()) {
        if (path.empty()) {
            path = "/" + std::string(rule->field);
        }
        const std::string field = path.substr(path.find_last_of('/') + 1);
        message = field + " " + std::string(rule->rule);
    } else if (diagnostic) {
        message = product_store_message(code);
    }
    ApiResult result;
    result.ok = false;
    result.error = ApiError{std::move(code), std::move(message), std::move(path)};
    return result;
}

nlohmann::json ApiResult::envelope() const {
    nlohmann::json envelope;
    envelope["ok"] = ok;
    if (ok) {
        envelope["value"] = value;
    } else {
        envelope["error"] = {
            {"code", error.code}, {"message", error.message}, {"path", error.path}};
        if (!error.reason.empty()) {
            envelope["error"]["reason"] = error.reason;
        }
        if (!error.help.empty()) {
            envelope["error"]["help"] = error.help;
        }
        if (!error.details.is_null()) {
            envelope["error"]["details"] = error.details;
        }
    }
    envelope["duration_ms"] = duration_ms;
    if (operation_result) {
        envelope["operation_result"] = *operation_result;
    }
    if (warnings.is_array() && !warnings.empty()) {
        envelope["warnings"] = warnings;
    }
    return envelope;
}

namespace {
// What an answer may carry beside its value or error (lake#1114): the account's balance warning.
nlohmann::json warnings_schema() {
    return {{"type", "array"},
            {"description",
             "Beside the answer: while the account's prepaid balance is low, negative or blocked, "
             "{code: balance_low | balance_negative | balance_blocked, message, customer_id, "
             "balance_micros, add_micros, add_funds_url}"},
            {"items",
             {{"type", "object"},
              {"properties", {{"code", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
              {"required", nlohmann::json::array({"code", "message"})}}}};
}
} // namespace

nlohmann::json error_envelope_schema() {
    nlohmann::json codes = nlohmann::json::array();
    for (const ErrorCode& code : documented_error_codes) {
        codes.push_back(std::string(code.code));
    }
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", false}}},
              {"error",
               {{"type", "object"},
                {"properties",
                 {{"code",
                   {{"type", "string"},
                    {"enum", std::move(codes)},
                    {"description", "what went wrong, one of the codes in docs/api/errors.md"}}},
                  {"message", {{"type", "string"}}},
                  {"path",
                   {{"type", "string"},
                    {"description",
                     "JSON pointer to the input field at fault; empty when the input is not"}}},
                  {"reason",
                   {{"type", "string"},
                    {"enum", {"missing", "invalid", "expired"}},
                    {"description", "unauthenticated only: why no credential was accepted"}}},
                  {"help",
                   {{"type", "string"},
                    {"description", "unauthenticated only: where to make a new key"}}},
                  {"details",
                   {{"type", "object"},
                    {"description",
                     "safe_mode_warning: {database or connection, mode, statements, confirmation}; "
                     "repeat the call with safe_mode_confirmation to run them. payment_required: "
                     "{customer_id, balance_micros, overdraft_micros, add_micros, add_funds_url, "
                     "reason}"}}}}},
                {"required", nlohmann::json::array({"code", "message"})}}},
              {"duration_ms", {{"type", "number"}}},
              {"operation_result",
               {{"type", "object"},
                {"description", "Original operation envelope when its subsequent audit failed."}}},
              {"warnings", warnings_schema()}}},
            {"required", nlohmann::json::array({"ok", "error", "duration_ms"})}};
}

nlohmann::json success_envelope_schema(const nlohmann::json& output_schema) {
    nlohmann::json value = output_schema.is_object() && !output_schema.empty()
                               ? output_schema
                               : nlohmann::json{{"type", "object"}};
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", true}}},
              {"value", value},
              {"duration_ms", {{"type", "number"}}},
              {"warnings", warnings_schema()}}},
            {"required", nlohmann::json::array({"ok", "value", "duration_ms"})}};
}

std::string_view to_string(CapabilityKind kind) {
    return kind == CapabilityKind::Read ? "read" : "action";
}

namespace {

bool matches_type(const nlohmann::json& value, const std::string& type) {
    if (type == "null")
        return value.is_null();
    if (type == "string")
        return value.is_string();
    if (type == "integer")
        return value.is_number_integer();
    if (type == "number")
        return value.is_number();
    if (type == "boolean")
        return value.is_boolean();
    if (type == "object")
        return value.is_object();
    if (type == "array")
        return value.is_array();
    return true;
}

std::string pointer_child(const std::string& parent, const std::string& key) {
    std::string path = parent + "/";
    for (const char character : key) {
        if (character == '~') {
            path += "~0";
        } else if (character == '/') {
            path += "~1";
        } else {
            path += character;
        }
    }
    return path;
}

ApiResult
validate_value(const nlohmann::json& schema, const nlohmann::json& value, const std::string& path) {
    const auto type = schema.find("type");
    if (type != schema.end()) {
        const bool matches =
            type->is_array()
                ? std::ranges::any_of(*type,
                                      [&](const auto& alternative) {
                                          return matches_type(
                                              value, alternative.template get<std::string>());
                                      })
                : matches_type(value, type->get<std::string>());
        if (!matches) {
            return ApiResult::failure("invalid_input", "expected type " + type->dump(), path);
        }
    }
    if (const auto allowed = schema.find("enum");
        allowed != schema.end() && allowed->is_array() &&
        std::ranges::find(*allowed, value) == allowed->end()) {
        return ApiResult::failure("invalid_input", "expected one of " + allowed->dump(), path);
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        const auto bound = [&](const char* keyword) -> std::optional<double> {
            const auto found = schema.find(keyword);
            if (found == schema.end() || !found->is_number()) {
                return std::nullopt;
            }
            return found->get<double>();
        };
        if (const auto minimum = bound("minimum"); minimum && number < *minimum) {
            return ApiResult::failure(
                "invalid_input", "expected at least " + schema.at("minimum").dump(), path);
        }
        if (const auto maximum = bound("maximum"); maximum && number > *maximum) {
            return ApiResult::failure(
                "invalid_input", "expected at most " + schema.at("maximum").dump(), path);
        }
        if (const auto minimum = bound("exclusiveMinimum"); minimum && number <= *minimum) {
            return ApiResult::failure("invalid_input",
                                      "expected more than " + schema.at("exclusiveMinimum").dump(),
                                      path);
        }
        if (const auto maximum = bound("exclusiveMaximum"); maximum && number >= *maximum) {
            return ApiResult::failure("invalid_input",
                                      "expected less than " + schema.at("exclusiveMaximum").dump(),
                                      path);
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            auto result = validate_value(
                schema.at("items"), value[index], pointer_child(path, std::to_string(index)));
            if (!result.ok) {
                return result;
            }
        }
    }
    if (!value.is_object()) {
        return ApiResult::success(nullptr);
    }
    const auto properties = schema.value("properties", nlohmann::json::object());
    for (const auto& required : schema.value("required", nlohmann::json::array())) {
        const std::string key = required.get<std::string>();
        if (!value.contains(key) || value[key].is_null()) {
            return ApiResult::failure(
                "invalid_input", key + " is required", pointer_child(path, key));
        }
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        const std::string child = pointer_child(path, field.key());
        if (!properties.contains(field.key())) {
            if (path.empty() ||
                schema.value("additionalProperties", nlohmann::json(true)) == false) {
                return ApiResult::failure(
                    "invalid_input", "unexpected field " + field.key(), child);
            }
            continue;
        }
        // Preserve the existing optional top-level null-as-absent input convention.
        if (path.empty() && field.value().is_null()) {
            continue;
        }
        auto result = validate_value(properties.at(field.key()), field.value(), child);
        if (!result.ok) {
            return result;
        }
    }
    return ApiResult::success(nullptr);
}

struct CallState {
    std::shared_ptr<const Capability> capability;
    ApiResult result;
    bool proceed = true;
    std::optional<control::AuthenticationAttempt> authentication;
};

ApiResult
authorize_app_token(const Capability& entry, const nlohmann::json& input, ApiContext& context) {
    const auto refused = [] {
        return ApiResult::failure("unauthorized", "app token does not grant this operation");
    };
    if (!context.authorized || context.credential_workload_id.empty() || !context.store) {
        return refused();
    }
    const auto app = context.store->get_web_app(context.credential_workload_id);
    if (!app || web_app_token_purpose(*app, context.credential_token_hash) !=
                    context.credential_kind.substr(4) ||
        (!context.path_workspace_id.empty() && app->workspace_id != context.path_workspace_id)) {
        return refused();
    }
    context.audit_customer_id = app->customer_id;
    context.audit_object = "app/" + app->id;
    if (context.credential_kind == "app_source") {
        if (app->desired == "deleted" || app->mode != "runtime" ||
            (entry.name != "repository_tree" && entry.name != "repository_file") ||
            input.value("id", "") != app->source_repository ||
            input.value("ref", "") != app->source_revision) {
            return refused();
        }
        const auto repository = context.store->get_repository(app->source_repository);
        if (!repository || repository->customer_id != app->customer_id) {
            return refused();
        }
    } else if (context.credential_kind == "app_zetta") {
        if (app->desired == "deleted" ||
            input.value("workspace_id", "") != app->workspace_id ||
            (entry.name != "workspace_query" && entry.name != "workspace_sql" &&
             entry.granted_by_scope != "workspace_query") ||
            input.value("customer_id", "") != app->customer_id ||
            app->bindings.value("zetta", nlohmann::json()) !=
                nlohmann::json{{"scopes", nlohmann::json::array({"read"})}}) {
            return refused();
        }
    } else if (context.credential_kind == "app_events") {
        if (entry.name != "publish_app_event" || input.value("app_id", "") != app->id ||
            input.value("workspace_id", "") != app->workspace_id ||
            (input.contains("customer_id") && input.value("customer_id", "") != app->customer_id)) {
            return refused();
        }
    } else {
        return refused();
    }
    return ApiResult::success(nullptr);
}

ApiResult
authorize_build_token(const Capability& entry, const nlohmann::json& input, ApiContext& context) {
    const auto refused = [] {
        return ApiResult::failure("unauthorized", "build token does not grant this operation");
    };
    if (!context.authorized || context.credential_workload_id.empty() || !context.store) {
        return refused();
    }
    const auto build = context.store->get_build(context.credential_workload_id);
    if (!build || build_token_purpose(*build, context.credential_token_hash) !=
                      context.credential_kind.substr(6) ||
        (!context.path_workspace_id.empty() && build->workspace_id != context.path_workspace_id)) {
        return refused();
    }
    context.audit_customer_id = build->customer_id;
    context.audit_object = "build/" + build->id;
    if (context.credential_kind == "build_source") {
        const std::string repository = input.value("repository", input.value("id", ""));
        const std::string revision = input.value("revision", input.value("ref", ""));
        if ((entry.name != "repository_tree" && entry.name != "repository_file") ||
            repository != build->repository || revision != build->revision) {
            return refused();
        }
        const auto stored_repository = context.store->get_repository(build->repository);
        if (!stored_repository || stored_repository->customer_id != build->customer_id) {
            return refused();
        }
    } else if (context.credential_kind == "build_events") {
        if (entry.name != "publish_build_event" || input.value("build_id", "") != build->id ||
            input.value("workspace_id", "") != build->workspace_id ||
            (input.contains("customer_id") &&
             input.value("customer_id", "") != build->customer_id)) {
            return refused();
        }
    } else {
        return refused();
    }
    return ApiResult::success(nullptr);
}

// A methodology run's credential (lake#655, docs/api/methodologies.md#runs) may call only the
// runner's two capabilities, and only for its own run; it acts as no person, so policy is not
// consulted. The handlers check the run's state.
ApiResult authorize_methodology_run_token(const Capability& entry,
                                          const nlohmann::json& input,
                                          ApiContext& context) {
    if (!context.authorized || context.credential_workload_id.empty() ||
        (entry.name != "methodology_run_input" && entry.name != "methodology_run_output" &&
         entry.name != "methodology_run_artifact") ||
        input.value("run_id", "") != context.credential_workload_id) {
        return ApiResult::failure("unauthorized",
                                  "a methodology run's credential reaches only its own run's "
                                  "input, artifact and output");
    }
    context.audit_customer_id = context.credential_customer_id;
    context.audit_object = "methodology_run/" + context.credential_workload_id;
    return ApiResult::success(nullptr);
}

ApiResult authorize_methodology_deployment_token(const Capability& entry,
                                                 const nlohmann::json& input,
                                                 ApiContext& context) {
    if (!context.authorized || context.credential_workload_id.empty() ||
        entry.name != "methodology_deployment_input" ||
        input.value("deployment_id", "") != context.credential_workload_id) {
        return ApiResult::failure("unauthorized",
                                  "a methodology deployment's credential reaches only its own "
                                  "deployment's input");
    }
    context.audit_customer_id = context.credential_customer_id;
    context.audit_object = "methodology_deployment/" + context.credential_workload_id;
    return ApiResult::success(nullptr);
}

// A browser key (lake#1152, docs/api/browser-keys.md) acts as no person, so policy is not
// consulted: what it may do is its binding. It may call search_index on its index, and predict on
// its model when it has one, in its workspace of its account, from one of its origins, within its
// calls a minute; anything else is refused before the handler runs.
ApiResult
authorize_browser_key(const Capability& entry, const nlohmann::json& input, ApiContext& context) {
    const control::BrowserKey& key = *context.browser_key;
    const auto refused = [](std::string message, std::string path = {}) {
        return ApiResult::failure("unauthorized", std::move(message), std::move(path));
    };
    context.audit_customer_id = key.customer_id;
    if (entry.name != "search_index" && entry.name != "predict") {
        return refused("a browser key may call only search_index and predict; " + entry.name +
                       " needs an API key");
    }
    if (entry.name == "predict" && key.model_id.empty()) {
        return refused("this browser key searches only; make one with model_id to ask a model");
    }
    // The website it is used from: browsers send Origin with every cross-site request and with
    // every POST, and a page cannot change it.
    if (context.caller_origin.empty()) {
        return refused("a browser key works only from the websites it lists, and this call names "
                       "no Origin");
    }
    const auto origin = control::browser_key_origin(context.caller_origin);
    if (!origin || std::ranges::find(key.origins, *origin) == key.origins.end()) {
        return refused("this browser key does not work from " + context.caller_origin +
                       "; it works from " + [&] {
                           std::string listed;
                           for (const auto& allowed : key.origins) {
                               listed += (listed.empty() ? "" : ", ") + allowed;
                           }
                           return listed;
                       }());
    }
    if (!input.is_object() || input.value("customer_id", std::string()) != key.customer_id) {
        return refused("this browser key belongs to account " + key.customer_id, "/customer_id");
    }
    if (input.value("workspace_id", std::string()) != key.workspace_id) {
        return refused("this browser key is bound to workspace " + key.workspace_id,
                       "/workspace_id");
    }
    if (entry.name == "search_index" && input.value("index_id", std::string()) != key.index_id) {
        return refused("this browser key searches index " + key.index_id + " only", "/index_id");
    }
    if (entry.name == "predict" && input.value("model_id", std::string()) != key.model_id) {
        return refused("this browser key asks model " + key.model_id + " only", "/model_id");
    }
    if (context.store == nullptr) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    const auto admitted = context.store->admit_browser_key_call(key.id);
    if (!admitted) {
        return ApiResult::failure(admitted.error().code == "not_found" ? "unauthorized"
                                                                       : "store_unavailable",
                                  admitted.error().message);
    }
    if (!admitted->allowed) {
        ApiResult limited = ApiResult::failure(
            "rate_limited",
            "this browser key has made its " + std::to_string(key.rate_limit_per_minute) +
                " calls this minute; retry in " + std::to_string(admitted->retry_after_seconds) +
                " s");
        return limited;
    }
    return ApiResult::success(nullptr);
}

const nlohmann::json& agent_task_input(const Capability& capability,
                                     const nlohmann::json& input,
                                     ApiContext& context,
                                     nlohmann::json& resolved) {
    // A task, launch or schedule credential names its workspace (lake#652 adds the launch and
    // the schedule, whose agents call the MCP connection capabilities without workspace_id).
    const std::string& session = context.credential_session_id;
    const bool task = context.credential_kind == "agent_task" && session.starts_with("task:");
    const bool launch = context.credential_kind == "agent_launch" && session.starts_with("launch:");
    const bool schedule =
        context.credential_kind == "agent_schedule" && session.starts_with("schedule:");
    if ((!task && !launch && !schedule) || !input.is_object() || input.contains("workspace_id") ||
        context.store == nullptr) {
        return input;
    }
    const auto properties = capability.input_schema.find("properties");
    if (properties == capability.input_schema.end() ||
        !properties->contains("workspace_id")) {
        return input;
    }
    const std::string id = session.substr(session.find(':') + 1);
    std::string workspace_id;
    if (task) {
        if (const auto found = context.store->get_agent_task(id)) {
            workspace_id = found->workspace_id;
        }
    } else if (launch) {
        if (const auto found = context.store->get_agent_launch(id)) {
            workspace_id = found->workspace_id;
        }
    } else if (const auto found = context.store->get_agent_cron_schedule(id)) {
        workspace_id = found->workspace_id;
    }
    if (workspace_id.empty()) {
        return input;
    }
    resolved = input;
    resolved["workspace_id"] = workspace_id;
    return resolved;
}

const nlohmann::json& workspace_path_input(const Capability& capability,
                                           const nlohmann::json& input,
                                           const ApiContext& context,
                                           nlohmann::json& resolved) {
    if (context.path_workspace_id.empty() || !input.is_object() || input.contains("workspace_id")) {
        return input;
    }
    const auto properties = capability.input_schema.find("properties");
    if (properties == capability.input_schema.end() || !properties->contains("workspace_id")) {
        return input;
    }
    resolved = input;
    resolved["workspace_id"] = context.path_workspace_id;
    return resolved;
}

// An input refusal that leads with the field it is about ("confirm must be the service name",
// "storage_gi: too large") points at that field when the handler gave no path (lake#450).
void point_at_named_field(ApiResult& result, const nlohmann::json& input) {
    if (result.ok || result.error.code != "invalid_input" || !result.error.path.empty() ||
        !input.is_object()) {
        return;
    }
    const std::string& message = result.error.message;
    for (const auto& [key, value] : input.items()) {
        if (message.size() > key.size() && message.starts_with(key) &&
            std::string_view(" :.,").contains(message[key.size()])) {
            result.error.path = pointer_child("", key);
            return;
        }
    }
}

// No credential was accepted (lake#450): 401, saying whether one was missing, invalid or expired,
// and where a person makes a new key.
ApiResult unauthenticated(const ApiContext& context) {
    const std::string reason =
        context.credential_rejection.empty() ? "missing" : context.credential_rejection;
    const std::string message =
        reason == "expired"   ? "the credential has expired; sign in again or make a new key"
        : reason == "invalid" ? "the credential is not valid (unknown, revoked or mistyped); sign "
                                "in again or make a new key"
                              : "a credential is required: send an API key as a bearer token";
    ApiResult result = ApiResult::failure("unauthenticated", message);
    result.error.reason = reason;
    result.error.help = context.request_origin + "/developer/mcp";
    return result;
}

// A session of an identity application's pool (lake#912) acts only through the capabilities that
// serve its user's own sign-in. For those it is the user, for this call; for every other one it is
// no credential, so its TAAP identity, which a Lake account may share, never meets a grant. Set
// afresh on every call, so a context reused for a second capability carries nothing over.
void admit_application_session(const Capability& entry, ApiContext& context) {
    if (!context.application_session) {
        return;
    }
    const control::Principal& session = *context.application_session;
    const bool admitted = entry.serves_application_pools;
    context.authorized = admitted;
    context.session_credential = admitted;
    context.acting_user_id = admitted ? session.user_id : std::string();
    context.acting_identity = admitted ? session.taap_id : std::string();
    context.credential_device_id = admitted ? session.device_id : std::string();
    context.credential_session_id = admitted ? session.session_id : std::string();
    context.credential_kind = admitted ? session.kind : std::string();
    context.credential_expires_at_unix =
        admitted ? session.expires_at_unix : std::optional<std::int64_t>{};
    context.pool_id = admitted ? session.pool_id : std::string("lake");
    context.scopes = admitted ? session.scopes : std::vector<std::string>{};
}

std::optional<ApiResult> authentication_failure(const Capability& entry, ApiContext& context) {
    admit_application_session(entry, context);
    // A bearer key lake rejected (unknown, revoked, expired, mistyped) is refused on every
    // capability a visitor may call: lake's own public reads (object `server`: `viewer`,
    // `server_info`, the Docs; lake#594) and the public reads of repositories and their content
    // (`allows_anonymous_public_read`, lake#775). A client whose key was revoked learns so instead
    // of reading a visitor's answer, such as an empty repository list. A browser's stale session
    // cookie is still a visitor, and the capabilities that verify their own bearer (SCIM
    // providers, sign-in, the agents' calls) are left to do so.
    const bool rejected_bearer = !context.authorized && !context.credential_rejection.empty() &&
                                 context.authentication_method == "bearer" &&
                                 (entry.object == "server" || entry.allows_anonymous_public_read);
    if (context.application_session && !entry.serves_application_pools && entry.requires_auth &&
        !entry.allows_anonymous_public_read) {
        return ApiResult::failure(
            "unauthorized",
            "an identity application's session reaches only its own sign-in (sessions, devices, "
            "tokens, MFA and passkeys); " +
                entry.name + " needs a Lake account's credential");
    } else if (((entry.requires_auth || entry.requires_credential) && !context.authorized &&
                !entry.allows_anonymous_public_read) ||
               rejected_bearer) {
        return unauthenticated(context);
    }
    return std::nullopt;
}

CallState prepare_call(std::shared_ptr<const Capability> capability,
                       const nlohmann::json& input,
                       ApiContext& context) {
    if (context.request_id.empty()) {
        context.request_id = control::new_uuid_v4();
    }
    context.audit_customer_id.clear();
    context.audit_kind.clear();
    context.audit_object.clear();
    context.audit_details = nlohmann::json::object();
    context.audit_workspace_id.clear();
    context.audit_validated_workspace = {};
    CallState state{.capability = std::move(capability)};
    if (context.authorized && context.credential_kind == "agent_task" &&
        context.credential_session_id.starts_with("task:") && context.store) {
        const auto renewed = context.store->renew_agent_task_lease(
            context.credential_session_id.substr(5), context.credential_token_hash);
        if (!renewed) {
            state.result = ApiResult::failure(renewed.error().code, renewed.error().message);
            state.proceed = false;
            return state;
        }
    }
    const Capability& entry = *state.capability;
    if (context.store &&
        (entry.name == "sign_up_password" || entry.name == "sign_in_password" ||
         entry.name == "verify_mfa" || entry.name == "recover_with_code" ||
         entry.name == "begin_email_recovery" || entry.name == "complete_recovery" ||
         entry.name == "enroll_mfa" || entry.name == "disable_mfa" ||
         (entry.name == "delete_account" && input.contains("code")))) {
        const auto gate = context.store->authentication_attempt(
            "", "", "", context.client_address.empty() ? "in-process" : context.client_address);
        if (!gate) {
            state.result = ApiResult::failure(gate.error().code, gate.error().message);
            state.proceed = false;
            return state;
        }
        state.authentication = *gate;
    }
    if (input.contains("workspace_id")) {
        context.workspace_id = input.at("workspace_id").is_string()
                                   ? input.at("workspace_id").get<std::string>()
                                   : std::string();
    }
    if (auto failure = authentication_failure(entry, context)) {
        state.result = std::move(*failure);
        state.proceed = false;
    } else if (ApiResult validation = validate_input(entry, input); !validation.ok) {
        state.result = validation;
        state.proceed = false;
    } else if (!context.path_workspace_id.empty() && input.contains("workspace_id") &&
               context.workspace_id != context.path_workspace_id) {
        state.result = ApiResult::failure(
            "invalid_input", "workspace_id must match the workspace URL", "/workspace_id");
        state.proceed = false;
    } else if (context.browser_key) {
        state.result = authorize_browser_key(entry, input, context);
        state.proceed = state.result.ok;
    } else if (context.credential_kind.starts_with("build_")) {
        state.result = authorize_build_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (context.credential_kind.starts_with("app_")) {
        state.result = authorize_app_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (context.credential_kind == "methodology_run") {
        state.result = authorize_methodology_run_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (context.credential_kind == "methodology_deployment") {
        state.result = authorize_methodology_deployment_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (entry.requires_auth && context.session_credential &&
               std::ranges::find(context.scopes, "*") == context.scopes.end() &&
               std::ranges::find(context.scopes, entry.name) == context.scopes.end() &&
               (entry.granted_by_scope.empty() ||
                std::ranges::find(context.scopes, entry.granted_by_scope) ==
                    context.scopes.end())) {
        state.result =
            ApiResult::failure("unauthorized", "credential does not grant " + entry.name);
        state.proceed = false;
    } else if (entry.requires_auth && context.authorized && !entry.authorizes_candidates) {
        if (entry.object.empty() || entry.action.empty() || !entry.resolve_object) {
            state.result = ApiResult::failure("store_error",
                                              "capability authorization metadata is incomplete");
            state.proceed = false;
        } else if (!context.store) {
            state.result = ApiResult::failure(
                "store_unavailable", "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
            state.proceed = false;
        } else {
            std::expected<std::string, ApiError> object_id = std::string{};
            if (context.acting_identity != "admin" || entry.resolves_operator_audit_target) {
                object_id = entry.resolve_object(input, context);
            }
            if (!object_id) {
                state.result = ApiResult::failure(
                    object_id.error().code, object_id.error().message, object_id.error().path);
                state.proceed = false;
            } else if (context.acting_identity != "admin") {
                auto decision = control::authorize(*context.store,
                                                   context.acting_identity,
                                                   *object_id,
                                                   entry.action,
                                                   context.authorization_cache);
                if (!decision) {
                    state.result =
                        ApiResult::failure(decision.error().code, decision.error().message);
                    state.proceed = false;
                } else if (!decision->allowed) {
                    state.result = ApiResult::failure("unauthorized",
                                                      "principal is not allowed to " +
                                                          entry.action + " " + entry.object);
                    state.proceed = false;
                }
            }
        }
    }
    if (state.proceed && input.contains("workspace_id")) {
        const auto workspace = context.store
            ? context.store->get_workspace(context.workspace_id)
            : control::Result<control::Workspace>(std::unexpect,
                  control::StoreError{"invalid_input", "workspace_id is unknown"});
        if (!workspace && workspace.error().code != "invalid_input" &&
            workspace.error().code != "not_found") {
            // The store could not answer; the id is not at fault.
            state.result = ApiResult::failure("store_unavailable",
                                              "the control-plane store did not answer");
            state.proceed = false;
        } else if (!workspace || (!context.path_workspace_id.empty() &&
                           context.workspace_id != context.path_workspace_id)) {
            state.result = ApiResult::failure("invalid_input",
                "workspace_id must name a workspace Zetta in this account", "/workspace_id");
            state.proceed = false;
        } else if (input.contains("customer_id") &&
                   input.at("customer_id") != workspace->customer_id) {
            // The workspace exists, so the space named beside it is the one at fault (lake#450).
            state.result = ApiResult::failure("invalid_input",
                                              "customer_id is not the account that owns workspace " +
                                                  context.workspace_id,
                                              "/customer_id");
            state.proceed = false;
        }
        if (state.proceed && context.credential_kind == "agent_task" &&
            context.credential_session_id.starts_with("task:")) {
            const auto task = context.store->get_agent_task(context.credential_session_id.substr(5));
            if (!task || task->workspace_id != context.workspace_id) {
                state.result = ApiResult::failure("unauthorized", "task credential belongs to another workspace");
                state.proceed = false;
            }
        }
        if (state.proceed) {
            context.audit_workspace_id = workspace->id;
            context.audit_validated_workspace = {workspace->id, workspace->customer_id};
        }
    }
    if (state.proceed && !context.path_workspace_id.empty() &&
        context.credential_kind == "agent_task" &&
        context.credential_session_id.starts_with("task:")) {
        const auto task = context.store->get_agent_task(context.credential_session_id.substr(5));
        if (!task || task->workspace_id != context.path_workspace_id) {
            state.result =
                ApiResult::failure("unauthorized", "task credential belongs to another workspace");
            state.proceed = false;
        }
    }
    return state;
}

// Accepted email-recovery requests do not prove identity and cannot clear a guessing budget.
void finish_authentication(CallState& state, const std::string& name, ApiContext& context) {
    if (!state.authentication)
        return;
    auto outcome = control::AuthenticationOutcome::neutral;
    if (state.proceed && name != "begin_email_recovery") {
        if (state.result.ok && name != "sign_up_password")
            outcome = control::AuthenticationOutcome::success;
        else if (state.result.error.code == "unauthenticated" ||
                 state.result.error.code == "unauthorized" ||
                 state.result.error.code == "not_found" ||
                 (name == "sign_up_password" && state.result.error.code == "conflict"))
            outcome = control::AuthenticationOutcome::failure;
    }
    const auto finished =
        context.store->authentication_attempt_finished(*state.authentication, outcome);
    if (!finished)
        state.result = ApiResult::failure(finished.error().code, finished.error().message);
}

// The agent a task, launch or schedule credential belongs to. Those credentials act as the person
// who started the agent, so the audit principal names the person; this names the agent (lake#445).
std::string acting_agent_of(const ApiContext& context) {
    const auto id_after = [&](std::string_view prefix) -> std::string {
        return context.credential_session_id.starts_with(prefix)
                   ? context.credential_session_id.substr(prefix.size())
                   : std::string();
    };
    if (context.credential_kind == "agent_task") {
        if (const std::string id = id_after("task:"); !id.empty()) {
            if (const auto task = context.store->get_agent_task(id)) {
                return task->agent;
            }
        }
    } else if (context.credential_kind == "agent_launch") {
        if (const std::string id = id_after("launch:"); !id.empty()) {
            if (const auto launch = context.store->get_agent_launch(id)) {
                return launch->agent_name.empty() ? launch->agent_id : launch->agent_name;
            }
        }
    } else if (context.credential_kind == "agent_schedule") {
        if (const std::string id = id_after("schedule:"); !id.empty()) {
            if (const auto schedule = context.store->get_agent_cron_schedule(id)) {
                return schedule->agent_name.empty() ? schedule->agent_id : schedule->agent_name;
            }
        }
    }
    return {};
}

// Counts a capability call in progress on the request's attribution, so finish_call can tell the
// outermost call from one a capability made through the registry.
class AttributionDepth {
public:
    explicit AttributionDepth(const ApiContext& context)
        : attribution_(context.request_attribution) {
        if (attribution_) {
            std::lock_guard lock(attribution_->mutex);
            ++attribution_->depth;
        }
    }
    ~AttributionDepth() {
        if (attribution_) {
            std::lock_guard lock(attribution_->mutex);
            --attribution_->depth;
        }
    }
    AttributionDepth(const AttributionDepth&) = delete;
    AttributionDepth& operator=(const AttributionDepth&) = delete;

private:
    std::shared_ptr<RequestAttribution> attribution_;
};

// The request's capability is its first outermost call; its workspace the first one a handled
// call acted on (lake#384). Nothing is looked up here.
void attribute_request(const ApiContext& context,
                       const std::string& name,
                       const std::string& acted) {
    if (!context.request_attribution) {
        return;
    }
    RequestAttribution& attribution = *context.request_attribution;
    std::lock_guard lock(attribution.mutex);
    if (attribution.depth <= 1 && attribution.capability.empty()) {
        attribution.capability = name;
    }
    if (attribution.workspace_id.empty() && !acted.empty()) {
        attribution.workspace_id = acted;
    }
}

// The account an event is about (lake#18), so the account's administrator finds it in the
// account's audit: the one a capability named itself, else the workspace the call acted on, the
// account (`customer_id`) or organization (`organization_id`) its input named, or the
// organization its event names. A name that is not an account's stays unattributed: the column is
// a reference, and a refused guess must not fail its own audit.
std::string audit_customer_of(const nlohmann::json& input, const ApiContext& context) {
    if (!context.audit_customer_id.empty()) {
        return context.audit_customer_id;
    }
    const auto text = [&input](const char* field) {
        return input.is_object() && input.contains(field) && input.at(field).is_string()
                   ? input.at(field).get<std::string>()
                   : std::string();
    };
    if (!context.audit_workspace_id.empty()) {
        if (context.audit_validated_workspace.first == context.audit_workspace_id) {
            return context.audit_validated_workspace.second;
        }
        if (const auto workspace = context.store->get_workspace(context.audit_workspace_id)) {
            return workspace->customer_id;
        }
    }
    if (const std::string customer_id = text("customer_id"); !customer_id.empty()) {
        if (const auto customer = context.store->get_customer(customer_id)) {
            return customer->id;
        }
    }
    std::string organization_id = text("organization_id");
    if (organization_id.empty() && context.audit_object.starts_with("organization/")) {
        organization_id = context.audit_object.substr(std::string_view("organization/").size());
    }
    if (!organization_id.empty()) {
        if (const auto customer = context.store->organization_customer(organization_id)) {
            return customer->id;
        }
    }
    return {};
}

ApiResult finish_call(const Capability* capability,
                      const std::string& name,
                      const nlohmann::json& input,
                      ApiResult result,
                      ApiContext& context,
                      bool handled) {
    // An anonymous caller refused by a capability that otherwise needs a credential (a private
    // repository read) is refused for want of one. An identity application's session presented
    // a credential that is valid but grants nothing here (lake#912), and stays `unauthorized`.
    if (capability != nullptr && capability->requires_auth && !context.authorized &&
        !context.application_session && !result.ok && result.error.code == "unauthorized") {
        const double duration = result.duration_ms;
        result = unauthenticated(context);
        result.duration_ms = duration;
    }
    if (!context.store) {
        attribute_request(context, name, {});
        return result;
    }
    const std::string kind = !context.audit_kind.empty()
                                 ? context.audit_kind
                                 : (name == "set_audit_retention" ? "retention" : "capability");
    nlohmann::json details = context.audit_details;
    details["error_code"] = result.ok ? "" : result.error.code;
    if (const std::string agent = acting_agent_of(context); !agent.empty()) {
        details["acting_agent"] = agent;
    }
    const audit::EventDraft event{
        .customer_id = audit_customer_of(input, context),
        .kind = kind,
        .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
        .object = context.audit_object.empty() ? "capability/" + name : context.audit_object,
        .action = name,
        .outcome = result.ok ? "success"
                             : (result.error.code == "unauthorized" ||
                                        result.error.code == "unauthenticated"
                                    ? "denied"
                                    : "failure"),
        .request_id = context.request_id,
        .method = context.authentication_method,
        .device = context.device,
        .details = std::move(details),
    };
    // The workspace the call acted on, looked up once for the audit route and the request's
    // series (lake#384) when either needs it.
    const std::string acted =
        context.audit_recorder != nullptr || context.request_attribution
            ? acted_workspace(capability, name, input, context, event.outcome, handled)
            : std::string();
    attribute_request(context, name, acted);
    // Decision 52: events about a workspace's data go to that workspace's audit, queued without
    // I/O; the rest are signed into the central chain now.
    const AuditRoute route = audit_route(name, acted, context);
    auto recorded = record_audit(context, event, route);
    if (!recorded) {
        auto failure = ApiResult::failure(
            recorded.error().code == "audit_queue_full" ? "audit_queue_full" : "store_error",
            "could not record audit event: " + recorded.error().message);
        failure.operation_result = result.envelope();
        return failure;
    }
    if (!*recorded) {
        return result;
    }
    if (result.ok && kind == "dynamic_secret" && context.transparency_log) {
        auto appended = context.transparency_log->append_audit_event(**recorded);
        if (!appended) {
            return ApiResult::failure("store_error",
                                      "could not append audit event to transparency log: " +
                                          appended.error().message);
        }
    }
    return result;
}

} // namespace

std::string acting_agent(const ApiContext& context) {
    return context.store == nullptr ? std::string() : acting_agent_of(context);
}

SchemaAudience schema_audience(const ApiContext& context) {
    return context.credential_kind == "agent_task" ? SchemaAudience::AgentTask
                                                   : SchemaAudience::Caller;
}

nlohmann::json audience_input_schema(const Capability& capability, SchemaAudience audience) {
    nlohmann::json schema = capability.input_schema;
    if (audience != SchemaAudience::AgentTask || !schema.is_object()) {
        return schema;
    }
    const auto required = schema.find("required");
    if (required == schema.end() || !required->is_array()) {
        return schema;
    }
    const auto workspace = std::ranges::find(*required, "workspace_id");
    if (workspace != required->end()) {
        required->erase(workspace);
    }
    return schema;
}

ApiResult validate_input(const Capability& capability, const nlohmann::json& input) {
    if (!input.is_object())
        return ApiResult::failure("invalid_input", "input must be a JSON object");
    return validate_value(capability.input_schema, input, "");
}

std::expected<nlohmann::json, ApiError> canonical_input(const Capability& capability,
                                                        const nlohmann::json& input) {
    if (capability.input_aliases.empty() || !input.is_object()) {
        return input;
    }
    nlohmann::json canonical = input;
    for (const auto& [alias, name] : capability.input_aliases) {
        const auto found = canonical.find(alias);
        if (found == canonical.end()) {
            continue;
        }
        if (const auto current = canonical.find(name);
            current != canonical.end() && !current->is_null() && *current != *found) {
            return std::unexpected(
                ApiError{"invalid_input",
                         alias + " is the deprecated name of " + name + "; send only " + name,
                         pointer_child("", alias)});
        }
        nlohmann::json value = std::move(*found);
        canonical.erase(alias);
        canonical[name] = std::move(value);
    }
    return canonical;
}

bool Registry::add(Capability capability) {
    if (capability.name.empty() || find(capability.name) || capability.max_body_bytes == 0 ||
        capability.max_body_bytes > maximum_capability_body_bytes)
        return false;
    // A deprecated alias is listed in the schema, so OpenAPI and MCP show it (docs/api/naming.md).
    for (const auto& [alias, name] : capability.input_aliases) {
        auto& properties = capability.input_schema["properties"];
        if (!properties.contains(name) || properties.contains(alias)) {
            return false;
        }
        nlohmann::json property = properties.at(name);
        property["deprecated"] = true;
        property["description"] = "Deprecated: the old name of `" + name + "`; send `" + name +
                                  "`. Accepted while callers move.";
        properties[alias] = std::move(property);
    }
    // The one list of capabilities that start paid work (lake#1114) marks them, engine
    // capabilities registered from a schema included.
    if (starts_paid_work(capability.name)) {
        capability.starts_paid_work = true;
    }
    entries_.push_back(std::make_shared<const Capability>(std::move(capability)));
    return true;
}

std::shared_ptr<const Capability> Registry::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry->name == name)
            return entry;
    }
    return nullptr;
}

std::vector<std::shared_ptr<const Capability>> Registry::list() const {
    return entries_;
}

namespace {
// Attributes the workspace store requests a synchronous handler sends on this thread to its HTTP
// request's Server-Timing (lake#527); a call without one leaves any enclosing scope in place.
class StoreRequestScope {
public:
    explicit StoreRequestScope(const ApiContext& context) {
        if (context.workspace_call_timing) {
            scope_.emplace(&context.workspace_call_timing->store);
        }
    }

private:
    std::optional<control::ObjectRequestScope> scope_;
};

// Counts the Registry calls a context is inside, so only the outermost answer is warned.
class DepthGuard {
public:
    explicit DepthGuard(ApiContext& context) : context_(context) {
        ++context_.registry_depth;
    }
    ~DepthGuard() {
        --context_.registry_depth;
    }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

private:
    ApiContext& context_;
};

// The balance gate (lake#1114, docs/api/billing.md#the-gate): a capability that starts paid work
// is refused 402 payment_required, before its handler, while the account it charges is blocked.
// One place for every capability: the handlers never check.
void gate_paid_work(CallState& state,
                    const Capability& capability,
                    const nlohmann::json& input,
                    ApiContext& context) {
    if (!state.proceed || !capability.starts_paid_work || !context.balance) {
        return;
    }
    if (auto refused = balance_refusal(capability, input, context)) {
        state.result = std::move(*refused);
        state.proceed = false;
    }
}

// The balance warning on the outermost answer while the account is low, negative or blocked.
void warn_about_balance(CallState& state, const nlohmann::json& input, ApiContext& context) {
    if (!context.balance || context.registry_depth != 1 ||
        (!state.proceed && state.result.error.code != "payment_required")) {
        return;
    }
    nlohmann::json warning = balance_warning(input, context);
    if (!warning.is_null()) {
        state.result.warnings = nlohmann::json::array({std::move(warning)});
    }
}
} // namespace

ApiResult
Registry::call(const std::string& name, const nlohmann::json& input, ApiContext& context) const {
    context.registry = this;
    const AttributionDepth depth(context);
    const auto capability = find(name);
    if (!capability)
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    const auto canonical = canonical_input(*capability, input);
    if (!canonical) {
        return finish_call(capability.get(),
                           name,
                           input,
                           ApiResult::failure(canonical.error().code,
                                              canonical.error().message,
                                              canonical.error().path),
                           context,
                           false);
    }
    nlohmann::json workspace_resolved_input;
    const nlohmann::json& workspace_input =
        workspace_path_input(*capability, *canonical, context, workspace_resolved_input);
    nlohmann::json task_resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, workspace_input, context, task_resolved_input);

    const DepthGuard registry_depth(context);
    CallState state = prepare_call(capability, call_input, context);
    gate_paid_work(state, *capability, call_input, context);
    if (state.proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->handler) {
                const StoreRequestScope scope(context);
                state.result = capability->handler(call_input, context);
            } else {
                state.result =
                    ApiResult::failure("store_error", "capability requires asynchronous dispatch");
            }
        } catch (const std::exception& error) {
            state.result = ApiResult::failure("store_error", error.what());
        }
        point_at_named_field(state.result, call_input);
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    finish_authentication(state, name, context);
    warn_about_balance(state, call_input, context);
    return finish_call(
        capability.get(), name, call_input, std::move(state.result), context, state.proceed);
}

ApiResult Registry::authenticate(const std::string& name, ApiContext& context) const {
    const auto capability = find(name);
    if (!capability) {
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    if (auto failure = authentication_failure(*capability, context)) {
        return refuse(name, std::move(*failure), {{"input", "unread"}, {"body_bytes", 0}}, context);
    }
    return ApiResult::success(nlohmann::json::object());
}

ApiResult Registry::refuse(const std::string& name,
                           ApiResult failure,
                           const nlohmann::json& details,
                           ApiContext& context) const {
    context.registry = this;
    const AttributionDepth depth(context);
    const auto capability = find(name);
    if (!capability) {
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    if (context.request_id.empty()) {
        context.request_id = control::new_uuid_v4();
    }
    context.audit_customer_id.clear();
    context.audit_kind.clear();
    context.audit_object.clear();
    context.audit_workspace_id.clear();
    context.audit_validated_workspace = {};
    context.audit_details = details.is_object() ? details : nlohmann::json::object();
    const nlohmann::json no_input = nlohmann::json::object();
    return finish_call(capability.get(), name, no_input, std::move(failure), context, false);
}

liquid::runtime::task<ApiResult> Registry::call_async(const std::string& name,
                                                      const nlohmann::json& input,
                                                      ApiContext& context) const {
    context.registry = this;
    const AttributionDepth depth(context);
    const auto capability = find(name);
    if (!capability) {
        co_return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    const auto canonical = canonical_input(*capability, input);
    if (!canonical) {
        co_return finish_call(capability.get(),
                              name,
                              input,
                              ApiResult::failure(canonical.error().code,
                                                 canonical.error().message,
                                                 canonical.error().path),
                              context,
                              false);
    }
    nlohmann::json workspace_resolved_input;
    const nlohmann::json& workspace_input =
        workspace_path_input(*capability, *canonical, context, workspace_resolved_input);
    nlohmann::json task_resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, workspace_input, context, task_resolved_input);

    const DepthGuard registry_depth(context);
    CallState state = prepare_call(capability, call_input, context);
    gate_paid_work(state, *capability, call_input, context);
    if (state.proceed) {
        // A long poll (`wait_ms`) parks on the bus: it gives its HTTP worker back so parked
        // callers never fill the short-request workers (lake#534). Only a capability declared a
        // long poll does; a caller's `wait_ms` alone never decides it.
        const double wait_ms =
            call_input.is_object() && call_input.contains("wait_ms") &&
                    call_input.at("wait_ms").is_number()
                ? call_input.at("wait_ms").get<double>()
                : static_cast<double>(capability->default_wait_ms);
        if (capability->long_poll && context.leave_worker && wait_ms > 0 &&
            !context.leave_worker(context.acting_identity)) {
            co_return finish_call(capability.get(), name, call_input,
                                  ApiResult::failure("rate_limited",
                                                     "too many long polls are waiting; retry"),
                                  context, false);
        }
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->async_handler) {
                auto completed = co_await capability->async_handler(call_input, context);
                if (!completed) {
                    state.result = ApiResult::failure("store_error", completed.error().message);
                } else {
                    state.result = std::move(*completed);
                }
            } else if (capability->handler && runs_off_server_thread(*capability)) {
                const CapabilityHandler& handler = capability->handler;
                auto completed = co_await run_blocking([&handler, &call_input, &context] {
                    const StoreRequestScope scope(context);
                    return handler(call_input, context);
                });
                state.result = completed ? std::move(*completed)
                               : completed.error().code == liquid::errc::busy
                                   ? ApiResult::failure("busy", completed.error().message)
                                   : ApiResult::failure("store_error", completed.error().message);
            } else if (capability->handler) {
                state.result = capability->handler(call_input, context);
            } else {
                state.result = ApiResult::failure("store_error", "capability has no handler");
            }
        } catch (const std::exception& error) {
            state.result = ApiResult::failure("store_error", error.what());
        }
        point_at_named_field(state.result, call_input);
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    finish_authentication(state, name, context);
    warn_about_balance(state, call_input, context);
    co_return finish_call(
        capability.get(), name, call_input, std::move(state.result), context, state.proceed);
}

} // namespace lake::api
