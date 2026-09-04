#include "lake/api/capabilities.hpp"

#include "lake/control/callers.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/store.hpp"

#include <charconv>
#include <format>

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_pki_error(const control::pki::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult from_caller_error(const control::callers::Error& error) {
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

ApiResult no_pki() {
    return ApiResult::failure("store_unavailable", "PKI service is not configured");
}

ApiResult no_callers() {
    return ApiResult::failure("store_unavailable", "caller attestation service is not configured");
}

json authority_schema() {
    return {{"type", "object"},
            {"properties",
             {{"root_certificate_pem", {{"type", "string"}}},
              {"intermediate_chain_pem", {{"type", "string"}}}}}};
}

json svid_schema() {
    return {{"type", "object"},
            {"properties",
             {{"certificate_pem", {{"type", "string"}}},
              {"private_key_pem", {{"type", "string"}}},
              {"chain_pem", {{"type", "string"}}}}}};
}

std::string hex_of(const std::vector<std::byte>& bytes) {
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const std::byte value : bytes) {
        output += std::format("{:02x}", std::to_integer<unsigned char>(value));
    }
    return output;
}

std::expected<std::vector<std::byte>, ApiError> bytes_of_hex(const std::string& input) {
    if (input.empty() || input.size() % 2 != 0) {
        return std::unexpected(ApiError{"invalid_input", "share value must be hexadecimal", ""});
    }
    std::vector<std::byte> output;
    output.reserve(input.size() / 2);
    for (std::size_t index = 0; index < input.size(); index += 2) {
        unsigned value = 0;
        const auto [end, error] =
            std::from_chars(input.data() + index, input.data() + index + 2, value, 16);
        if (error != std::errc{} || end != input.data() + index + 2) {
            return std::unexpected(
                ApiError{"invalid_input", "share value must be hexadecimal", "/shares"});
        }
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::expected<std::vector<control::pki::ShamirShare>, ApiError> shares_of(const json& input) {
    std::vector<control::pki::ShamirShare> shares;
    for (const json& value : input) {
        if (!value.is_object() || !value.contains("index") || !value.contains("value") ||
            !value.at("index").is_number_integer() || !value.at("value").is_string()) {
            return std::unexpected(
                ApiError{"invalid_input", "each recovery share needs index and value", "/shares"});
        }
        auto bytes = bytes_of_hex(value.at("value").get<std::string>());
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        shares.push_back({value.at("index").get<int>(), std::move(*bytes)});
    }
    return shares;
}

} // namespace

void register_pki_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_pki_authority",
        .title = "Create customer certificate authority",
        .description = "Create a private root and online intermediate for one customer.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = authority_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto authority = context.pki->create_authority(
                *context.store, input.at("customer_id").get<std::string>(), context.instance_kek);
            if (!authority) {
                return from_pki_error(authority.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "certificate";
            context.audit_object = "customer/" + input.at("customer_id").get<std::string>();
            return ApiResult::success(
                {{"root_certificate_pem", authority->root_certificate_pem},
                 {"intermediate_chain_pem", authority->intermediate_chain_pem}});
        }});
    registry.add(Capability{
        .name = "issue_x509_svid",
        .title = "Issue SPIFFE X.509 SVID",
        .description = "Issue a short-lived SPIFFE X.509 workload credential.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"spiffe_id", {{"type", "string"}}},
                           {"lifetime_seconds", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "spiffe_id"})},
                         {"additionalProperties", false}},
        .output_schema = svid_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto svid = context.pki->issue_x509_svid(*context.store,
                                                     input.at("customer_id").get<std::string>(),
                                                     input.at("spiffe_id").get<std::string>(),
                                                     context.instance_kek,
                                                     input.value("lifetime_seconds", 3600));
            if (!svid) {
                return from_pki_error(svid.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "certificate";
            context.audit_object = "x509_svid/" + input.at("spiffe_id").get<std::string>();
            return ApiResult::success({{"certificate_pem", svid->certificate_pem},
                                       {"private_key_pem", svid->private_key_pem},
                                       {"chain_pem", svid->chain_pem}});
        }});
    registry.add(Capability{
        .name = "issue_jwt_svid",
        .title = "Issue SPIFFE JWT SVID",
        .description = "Issue a short-lived JWT workload credential bound to a SPIFFE identity.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"spiffe_id", {{"type", "string"}}},
                           {"audience", {{"type", "string"}}},
                           {"lifetime_seconds", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "spiffe_id", "audience"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"token", {{"type", "string"}}},
                            {"issuer", {{"type", "string"}}},
                            {"subject", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto svid = context.pki->issue_jwt_svid(*context.store,
                                                    input.at("customer_id").get<std::string>(),
                                                    input.at("spiffe_id").get<std::string>(),
                                                    input.at("audience").get<std::string>(),
                                                    context.instance_kek,
                                                    input.value("lifetime_seconds", 900));
            if (!svid) {
                return from_pki_error(svid.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "certificate";
            context.audit_object = "jwt_svid/" + input.at("spiffe_id").get<std::string>();
            return ApiResult::success(
                {{"token", svid->token}, {"issuer", svid->issuer}, {"subject", svid->subject}});
        }});
    registry.add(Capability{
        .name = "exchange_kubernetes_attestation",
        .title = "Exchange Kubernetes attestation",
        .description = "Exchange a projected service-account token through TokenReview.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_id", {{"type", "string"}}},
               {"organization_id", {{"type", "string"}}},
               {"token", {{"type", "string"}}},
               {"workload_taap_id", {{"type", "string"}}},
               {"audiences", {{"type", "array"}}}}},
             {"required",
              json::array(
                  {"customer_id", "organization_id", "token", "workload_taap_id", "audiences"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"credential", {{"type", "string"}}},
                            {"expires_at", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.callers) {
                return no_callers();
            }
            std::vector<std::string> audiences =
                input.at("audiences").get<std::vector<std::string>>();
            auto credential = context.callers->exchange_kubernetes(
                input.at("organization_id").get<std::string>(),
                input.at("token").get<std::string>(),
                input.at("workload_taap_id").get<std::string>(),
                audiences);
            if (!credential) {
                return from_caller_error(credential.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "authentication";
            context.audit_object = "workload/" + credential->workload_taap_id;
            return ApiResult::success(
                {{"credential", credential->value}, {"expires_at", credential->expires_at}});
        }});
    registry.add(Capability{
        .name = "begin_root_recovery",
        .title = "Begin root recovery rehearsal",
        .description = "Create Shamir recovery shares for a configured customer quorum.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"threshold", {{"type", "integer"}}},
                           {"share_count", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "threshold", "share_count"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"ceremony_id", {{"type", "string"}}},
                            {"threshold", {{"type", "integer"}}},
                            {"shares", {{"type", "array"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            auto ceremony = context.pki->begin_recovery(*context.store,
                                                        input.at("customer_id").get<std::string>(),
                                                        context.instance_kek,
                                                        input.at("threshold").get<int>(),
                                                        input.at("share_count").get<int>());
            if (!ceremony) {
                return from_pki_error(ceremony.error());
            }
            json shares = json::array();
            for (const control::pki::ShamirShare& share : ceremony->shares) {
                shares.push_back({{"index", share.index}, {"value", hex_of(share.value)}});
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "recovery";
            context.audit_object = "root-recovery/" + ceremony->id;
            return ApiResult::success({{"ceremony_id", ceremony->id},
                                       {"threshold", ceremony->threshold},
                                       {"shares", std::move(shares)}});
        }});
    registry.add(Capability{
        .name = "complete_root_recovery",
        .title = "Complete root recovery rehearsal",
        .description = "Verify a Shamir quorum restores the customer root.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"ceremony_id", {{"type", "string"}}},
                           {"threshold", {{"type", "integer"}}},
                           {"shares", {{"type", "array"}}}}},
                         {"required",
                          json::array({"customer_id", "ceremony_id", "threshold", "shares"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"recovered", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            auto shares = shares_of(input.at("shares"));
            if (!shares) {
                return ApiResult::failure(
                    shares.error().code, shares.error().message, shares.error().path);
            }
            control::pki::RecoveryCeremony ceremony{
                input.at("ceremony_id").get<std::string>(), input.at("threshold").get<int>(), {}};
            auto recovered = context.pki->recover_root(*context.store,
                                                       input.at("customer_id").get<std::string>(),
                                                       context.instance_kek,
                                                       ceremony,
                                                       *shares);
            if (!recovered) {
                return from_pki_error(recovered.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "recovery";
            context.audit_object = "root-recovery/" + ceremony.id;
            return ApiResult::success({{"recovered", true}});
        }});
}

} // namespace lake::api
