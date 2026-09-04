#include "lake/api/capabilities.hpp"

#include "lake/control/callers.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/store.hpp"

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
            auto authority =
                context.pki->create_authority(input.at("customer_id").get<std::string>());
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
            auto svid = context.pki->issue_x509_svid(input.at("customer_id").get<std::string>(),
                                                     input.at("spiffe_id").get<std::string>(),
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
            auto svid = context.pki->issue_jwt_svid(input.at("customer_id").get<std::string>(),
                                                    input.at("spiffe_id").get<std::string>(),
                                                    input.at("audience").get<std::string>(),
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
}

} // namespace lake::api
