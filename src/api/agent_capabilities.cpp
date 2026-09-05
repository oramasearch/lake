#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "lake/control/agents.hpp"
#include "lake/control/store.hpp"
#include "lake/transparency/log.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_agent_error(const control::agents::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult from_transparency_error(const transparency::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    auto object =
        context.store->find_policy_object("customer", input.at("customer_id").get<std::string>());
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<std::vector<std::byte>, ApiError> decode_base64url(std::string input,
                                                                 std::string path) {
    if (input.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "base64url value must not be empty", std::move(path)});
    }
    if (!std::ranges::all_of(input, [](unsigned char value) {
            return std::isalnum(value) != 0 || value == '-' || value == '_';
        })) {
        return std::unexpected(
            ApiError{"invalid_input", "base64url value is invalid", std::move(path)});
    }
    const std::size_t remainder = input.size() % 4;
    if (remainder == 1) {
        return std::unexpected(
            ApiError{"invalid_input", "base64url value is invalid", std::move(path)});
    }
    std::ranges::replace(input, '-', '+');
    std::ranges::replace(input, '_', '/');
    if (remainder != 0) {
        input.append(4 - remainder, '=');
    }
    if (input.size() / 4 > static_cast<std::size_t>(std::numeric_limits<int>::max() / 3)) {
        return std::unexpected(
            ApiError{"invalid_input", "base64url value is too large", std::move(path)});
    }
    std::vector<std::byte> output((input.size() / 4) * 3);
    const int decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                        reinterpret_cast<const unsigned char*>(input.data()),
                                        static_cast<int>(input.size()));
    if (decoded < 0) {
        return std::unexpected(
            ApiError{"invalid_input", "base64url value is invalid", std::move(path)});
    }
    std::size_t padding = 0;
    if (input.ends_with('=')) {
        ++padding;
    }
    if (input.ends_with("==")) {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(decoded) - padding);
    return output;
}

json session_output(const control::AgentSession& session) {
    return {{"agent_session_id", session.id},
            {"attestation", session.attestation},
            {"dispatcher_taap_id", session.dispatcher_taap_id},
            {"durable_agent_taap_id", session.durable_agent_taap_id},
            {"expires_at", session.expires_at},
            {"issued_at", session.issued_at},
            {"public_key", session.public_key},
            {"scopes", session.scopes},
            {"session_taap_id", session.session_taap_id},
            {"transparency_index", session.transparency_index},
            {"transparency_root", session.transparency_root}};
}

json output_schema() {
    return {{"type", "object"}};
}

void set_audit_fields(ApiContext& context,
                      const std::string& customer_id,
                      const std::string& object,
                      const std::string& operation) {
    context.audit_customer_id = customer_id;
    context.audit_kind = "agent_session";
    context.audit_object = object;
    context.audit_details = {{"operation", operation}};
}

} // namespace

void register_agent_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "mint_agent_session_key",
        .title = "Mint agent session key",
        .description =
            "Mint a short-lived signing key delegated to one durable TAAP agent identity.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_id", {{"type", "string"}}},
               {"dispatcher_taap_id", {{"type", "string"}}},
               {"durable_agent_taap_id", {{"type", "string"}}},
               {"lifetime_seconds", {{"type", "integer"}}},
               {"scopes", {{"type", "array"}}}}},
             {"required",
              json::array(
                  {"customer_id", "dispatcher_taap_id", "durable_agent_taap_id", "scopes"})},
             {"additionalProperties", false}},
        .output_schema = output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string customer_id = input.at("customer_id").get<std::string>();
            const std::string durable_agent = input.at("durable_agent_taap_id").get<std::string>();
            set_audit_fields(context, customer_id, "agent/" + durable_agent, "mint");
            if (!context.agents) {
                return ApiResult::failure("store_unavailable",
                                          "agent session service is not configured");
            }
            if (!context.transparency_log) {
                return ApiResult::failure(
                    "store_unavailable",
                    "no tiled transparency log is configured (LAKE_STORE_ROOT)");
            }
            std::vector<std::string> scopes;
            for (std::size_t index = 0; index < input.at("scopes").size(); ++index) {
                if (!input.at("scopes")[index].is_string()) {
                    return ApiResult::failure("invalid_input",
                                              "agent session scope must be a string",
                                              "/scopes/" + std::to_string(index));
                }
                scopes.push_back(input.at("scopes")[index].get<std::string>());
            }
            const int lifetime = input.value("lifetime_seconds", 900);
            auto minted = context.agents->mint(
                *context.store,
                {.customer_id = customer_id,
                 .durable_agent_taap_id = durable_agent,
                 .dispatcher_taap_id = input.at("dispatcher_taap_id").get<std::string>(),
                 .scopes = std::move(scopes),
                 .lifetime_seconds = lifetime},
                {.configured_seed = context.identity_seed, .instance_kek = context.instance_kek});
            if (!minted) {
                return from_agent_error(minted.error());
            }
            auto appended = context.transparency_log->append({minted->record.attestation});
            if (!appended) {
                return from_transparency_error(appended.error());
            }
            minted->record.transparency_index = appended->first_index;
            minted->record.transparency_root = appended->root_hash;
            auto stored = context.store->create_agent_session(minted->record);
            if (!stored) {
                return from_store_error(stored.error());
            }
            json output = session_output(*stored);
            output["private_key"] = minted->private_key;
            context.audit_object = "agent_session/" + stored->id;
            context.audit_details = {{"durable_agent_taap_id", durable_agent},
                                     {"expires_at", stored->expires_at},
                                     {"operation", "mint"},
                                     {"session_taap_id", stored->session_taap_id},
                                     {"transparency_index", stored->transparency_index}};
            return ApiResult::success(std::move(output));
        }});
    registry.add(Capability{
        .name = "verify_agent_session_drip",
        .title = "Verify agent session drip",
        .description =
            "Verify a signed drip and attribute it to the durable delegated agent identity.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"agent_session_id", {{"type", "string"}}},
                           {"body", {{"type", "string"}}},
                           {"customer_id", {{"type", "string"}}},
                           {"signature", {{"type", "string"}}}}},
                         {"required",
                          json::array({"agent_session_id", "body", "customer_id", "signature"})},
                         {"additionalProperties", false}},
        .output_schema = output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string customer_id = input.at("customer_id").get<std::string>();
            const std::string session_id = input.at("agent_session_id").get<std::string>();
            set_audit_fields(context, customer_id, "agent_session/" + session_id, "verify_drip");
            if (!context.agents) {
                return ApiResult::failure("store_unavailable",
                                          "agent session service is not configured");
            }
            auto session = context.store->get_agent_session(customer_id, session_id);
            if (!session) {
                return from_store_error(session.error());
            }
            auto body = decode_base64url(input.at("body").get<std::string>(), "/body");
            if (!body) {
                return ApiResult::failure(
                    body.error().code, body.error().message, body.error().path);
            }
            auto signature =
                decode_base64url(input.at("signature").get<std::string>(), "/signature");
            if (!signature) {
                return ApiResult::failure(
                    signature.error().code, signature.error().message, signature.error().path);
            }
            auto verified = context.agents->verify_drip(*session, *body, *signature);
            if (!verified) {
                return from_agent_error(verified.error());
            }
            context.audit_details = {{"durable_agent_taap_id", verified->durable_agent_taap_id},
                                     {"operation", "verify_drip"},
                                     {"session_taap_id", verified->session_taap_id}};
            json output = session_output(*session);
            output["verified"] = true;
            return ApiResult::success(std::move(output));
        }});
}

} // namespace lake::api
