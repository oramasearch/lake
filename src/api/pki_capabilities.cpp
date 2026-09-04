#include "lake/api/capabilities.hpp"

#include "lake/control/callers.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/store.hpp"

#include <array>
#include <charconv>
#include <format>
#include <tuple>

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
    for (const auto& [name, title, profile] :
         std::array<std::tuple<std::string, std::string, std::string>, 2>{
             {{"issue_client_certificate", "Issue client certificate", "client"},
              {"issue_device_certificate", "Issue device certificate", "device"}}}) {
        registry.add(Capability{
            .name = name,
            .title = title,
            .description = "Issue a short-lived " + profile + " certificate.",
            .input_schema = {{"type", "object"},
                             {"properties",
                              {{"customer_id", {{"type", "string"}}},
                               {"subject", {{"type", "string"}}},
                               {"lifetime_seconds", {{"type", "integer"}}}}},
                             {"required", json::array({"customer_id", "subject"})},
                             {"additionalProperties", false}},
            .output_schema = svid_schema(),
            .kind = CapabilityKind::Action,
            .object = "customer",
            .action = "admin",
            .resolve_object = customer_object,
            .handler = [profile](const json& input, ApiContext& context) {
                if (!context.pki || !context.store) {
                    return no_pki();
                }
                auto certificate =
                    context.pki->issue_x509_certificate(*context.store,
                                                        input.at("customer_id").get<std::string>(),
                                                        input.at("subject").get<std::string>(),
                                                        context.instance_kek,
                                                        input.value("lifetime_seconds", 3600));
                if (!certificate) {
                    return from_pki_error(certificate.error());
                }
                context.audit_customer_id = input.at("customer_id").get<std::string>();
                context.audit_kind = "certificate";
                context.audit_object = profile + "/" + input.at("subject").get<std::string>();
                return ApiResult::success({{"certificate_pem", certificate->certificate_pem},
                                           {"private_key_pem", certificate->private_key_pem},
                                           {"chain_pem", certificate->chain_pem}});
            }});
    }
    for (const auto& [name, title, host] :
         std::array<std::tuple<std::string, std::string, bool>, 2>{
             {{"issue_ssh_user_certificate", "Issue SSH user certificate", false},
              {"issue_ssh_host_certificate", "Issue SSH host certificate", true}}}) {
        registry.add(Capability{
            .name = name,
            .title = title,
            .description = "Issue a short-lived OpenSSH certificate for an Ed25519 public key.",
            .input_schema =
                {{"type", "object"},
                 {"properties",
                  {{"customer_id", {{"type", "string"}}},
                   {"public_key", {{"type", "string"}}},
                   {"key_id", {{"type", "string"}}},
                   {"principals",
                    {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}}},
                   {"lifetime_seconds", {{"type", "integer"}}}}},
                 {"required", json::array({"customer_id", "public_key", "key_id", "principals"})},
                 {"additionalProperties", false}},
            .output_schema = {{"type", "object"},
                              {"properties",
                               {{"certificate", {{"type", "string"}}},
                                {"ca_public_key", {{"type", "string"}}},
                                {"serial", {{"type", "integer"}}}}}},
            .kind = CapabilityKind::Action,
            .object = "customer",
            .action = "admin",
            .resolve_object = customer_object,
            .handler = [host](const json& input, ApiContext& context) {
                if (!context.pki || !context.store) {
                    return no_pki();
                }
                std::vector<std::string> principals;
                for (const json& principal : input.at("principals")) {
                    if (!principal.is_string()) {
                        return ApiResult::failure(
                            "invalid_input", "SSH principals must be strings", "/principals");
                    }
                    principals.push_back(principal.get<std::string>());
                }
                auto certificate =
                    context.pki->issue_ssh_certificate(*context.store,
                                                       input.at("customer_id").get<std::string>(),
                                                       input.at("public_key").get<std::string>(),
                                                       input.at("key_id").get<std::string>(),
                                                       principals,
                                                       host,
                                                       context.instance_kek,
                                                       input.value("lifetime_seconds", 3600));
                if (!certificate) {
                    return from_pki_error(certificate.error());
                }
                context.audit_customer_id = input.at("customer_id").get<std::string>();
                context.audit_kind = "certificate";
                context.audit_object = std::string(host ? "ssh-host/" : "ssh-user/") +
                                       input.at("key_id").get<std::string>();
                return ApiResult::success({{"certificate", certificate->certificate},
                                           {"ca_public_key", certificate->ca_public_key},
                                           {"serial", certificate->serial}});
            }});
    }
    registry.add(
        Capability{.name = "acme_new_account",
                   .title = "Create ACME account",
                   .description = "Register an RFC 8555 account key for one customer.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"customer_id", {{"type", "string"}}},
                                      {"x", {{"type", "string"}}},
                                      {"y", {{"type", "string"}}}}},
                                    {"required", json::array({"customer_id", "x", "y"})},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"},
                                     {"properties", {{"account_id", {{"type", "string"}}}}}},
                   .kind = CapabilityKind::Action,
                   .transport = CapabilityTransport::Acme,
                   .requires_auth = false,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.pki) {
                           return no_pki();
                       }
                       auto account = context.pki->create_acme_account(
                           input.at("customer_id").get<std::string>(),
                           {input.at("x").get<std::string>(), input.at("y").get<std::string>()});
                       if (!account) {
                           return from_pki_error(account.error());
                       }
                       context.audit_customer_id = account->customer_id;
                       context.audit_kind = "certificate";
                       context.audit_object = "acme-account/" + account->id;
                       return ApiResult::success({{"account_id", account->id}});
                   }});
    registry.add(Capability{
        .name = "acme_new_order",
        .title = "Create ACME order",
        .description = "Create an RFC 8555 DNS identifier order.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"account_id", {{"type", "string"}}},
                           {"identifier", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "account_id", "identifier"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {}}},
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::Acme,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            auto order = context.pki->create_acme_order(*context.store,
                                                        input.at("customer_id").get<std::string>(),
                                                        input.at("account_id").get<std::string>(),
                                                        input.at("identifier").get<std::string>(),
                                                        context.instance_kek);
            if (!order) {
                return from_pki_error(order.error());
            }
            auto authorization = context.pki->acme_authorization(
                order->customer_id, order->account_id, order->authorization_id);
            if (!authorization) {
                return from_pki_error(authorization.error());
            }
            context.audit_customer_id = order->customer_id;
            context.audit_kind = "certificate";
            context.audit_object = "acme-order/" + order->id;
            return ApiResult::success({{"order_id", order->id},
                                       {"authorization_id", order->authorization_id},
                                       {"token", authorization->token},
                                       {"status", order->status}});
        }});
    const auto add_acme_lookup = [&registry](std::string name, bool authorization_lookup) {
        registry.add(Capability{
            .name = std::move(name),
            .title = authorization_lookup ? "Read ACME authorization" : "Read ACME order",
            .description = "Return current RFC 8555 state.",
            .input_schema =
                {{"type", "object"},
                 {"properties",
                  {{"customer_id", {{"type", "string"}}},
                   {"account_id", {{"type", "string"}}},
                   {authorization_lookup ? "authorization_id" : "order_id", {{"type", "string"}}}}},
                 {"required",
                  json::array({"customer_id",
                               "account_id",
                               authorization_lookup ? "authorization_id" : "order_id"})},
                 {"additionalProperties", false}},
            .output_schema = {{"type", "object"}, {"properties", {}}},
            .kind = CapabilityKind::Read,
            .transport = CapabilityTransport::Acme,
            .requires_auth = false,
            .handler = [authorization_lookup](const json& input, ApiContext& context) {
                if (!context.pki) {
                    return no_pki();
                }
                const std::string customer_id = input.at("customer_id").get<std::string>();
                const std::string account_id = input.at("account_id").get<std::string>();
                context.audit_customer_id = customer_id;
                context.audit_kind = "certificate";
                if (authorization_lookup) {
                    auto authorization = context.pki->acme_authorization(
                        customer_id, account_id, input.at("authorization_id").get<std::string>());
                    if (!authorization) {
                        return from_pki_error(authorization.error());
                    }
                    context.audit_object = "acme-authorization/" + authorization->id;
                    return ApiResult::success({{"authorization_id", authorization->id},
                                               {"identifier", authorization->identifier},
                                               {"token", authorization->token},
                                               {"status", authorization->status}});
                }
                auto order = context.pki->acme_order(
                    customer_id, account_id, input.at("order_id").get<std::string>());
                if (!order) {
                    return from_pki_error(order.error());
                }
                context.audit_object = "acme-order/" + order->id;
                return ApiResult::success({{"order_id", order->id},
                                           {"authorization_id", order->authorization_id},
                                           {"identifier", order->identifier},
                                           {"status", order->status},
                                           {"has_certificate", !order->certificate_pem.empty()}});
            }});
    };
    add_acme_lookup("acme_order", false);
    add_acme_lookup("acme_authorization", true);
    registry.add(Capability{
        .name = "acme_http01_challenge",
        .title = "Accept ACME HTTP-01 challenge",
        .description = "Validate the account-bound HTTP-01 response and ready the order.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"account_id", {{"type", "string"}}},
                           {"authorization_id", {{"type", "string"}}}}},
                         {"required",
                          json::array({"customer_id", "account_id", "authorization_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {}}},
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::Acme,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto authorization =
                context.pki->authorize_acme_order(input.at("customer_id").get<std::string>(),
                                                  input.at("account_id").get<std::string>(),
                                                  input.at("authorization_id").get<std::string>());
            if (!authorization) {
                return from_pki_error(authorization.error());
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "certificate";
            context.audit_object = "acme-authorization/" + authorization->id;
            return ApiResult::success(
                {{"authorization_id", authorization->id}, {"status", authorization->status}});
        }});
    registry.add(Capability{
        .name = "acme_finalize_order",
        .title = "Finalize ACME order",
        .description = "Verify a DER CSR and issue the ordered certificate.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"account_id", {{"type", "string"}}},
                           {"order_id", {{"type", "string"}}},
                           {"csr", {{"type", "string"}}}}},
                         {"required",
                          json::array({"customer_id", "account_id", "order_id", "csr"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {}}},
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::Acme,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            auto order =
                context.pki->finalize_acme_order(*context.store,
                                                 input.at("customer_id").get<std::string>(),
                                                 input.at("account_id").get<std::string>(),
                                                 input.at("order_id").get<std::string>(),
                                                 input.at("csr").get<std::string>(),
                                                 context.instance_kek);
            if (!order) {
                return from_pki_error(order.error());
            }
            context.audit_customer_id = order->customer_id;
            context.audit_kind = "certificate";
            context.audit_object = "acme-order/" + order->id;
            return ApiResult::success({{"order_id", order->id}, {"status", order->status}});
        }});
    registry.add(Capability{
        .name = "acme_certificate",
        .title = "Download ACME certificate",
        .description = "Return the PEM certificate chain for a valid order.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"account_id", {{"type", "string"}}},
                           {"order_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "account_id", "order_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"certificate_pem", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .transport = CapabilityTransport::Acme,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto order = context.pki->acme_order(input.at("customer_id").get<std::string>(),
                                                 input.at("account_id").get<std::string>(),
                                                 input.at("order_id").get<std::string>());
            if (!order) {
                return from_pki_error(order.error());
            }
            if (order->status != "valid" || order->certificate_pem.empty()) {
                return ApiResult::failure("invalid_input", "ACME order is not valid");
            }
            context.audit_customer_id = order->customer_id;
            context.audit_kind = "certificate";
            context.audit_object = "acme-order/" + order->id;
            return ApiResult::success({{"certificate_pem", order->certificate_pem}});
        }});
    registry.add(Capability{
        .name = "acme_http01_response",
        .title = "Serve ACME HTTP-01 response",
        .description = "Return the account-key authorization for an active challenge token.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"token", {{"type", "string"}}}}},
                         {"required", json::array({"token"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"key_authorization", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .transport = CapabilityTransport::Acme,
        .requires_auth = false,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            auto response = context.pki->acme_http01_response(input.at("token").get<std::string>());
            if (!response) {
                return from_pki_error(response.error());
            }
            context.audit_kind = "certificate";
            context.audit_object = "acme-http-01/" + input.at("token").get<std::string>();
            return ApiResult::success({{"key_authorization", *response}});
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
        .name = "exchange_jwt_attestation",
        .title = "Exchange OIDC or JWT attestation",
        .description = "Exchange a registered OIDC or JWT federation assertion.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"organization_id", {{"type", "string"}}},
                           {"token", {{"type", "string"}}},
                           {"workload_taap_id", {{"type", "string"}}}}},
                         {"required",
                          json::array(
                              {"customer_id", "organization_id", "token", "workload_taap_id"})},
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
            auto credential =
                context.callers->exchange_jwt(input.at("organization_id").get<std::string>(),
                                              input.at("token").get<std::string>(),
                                              input.at("workload_taap_id").get<std::string>());
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
        .name = "exchange_cloud_attestation",
        .title = "Exchange cloud instance attestation",
        .description = "Exchange AWS or Azure instance identity evidence.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"provider", {{"type", "string"}}},
                           {"document", {{"type", "string"}}},
                           {"workload_taap_id", {{"type", "string"}}}}},
                         {"required",
                          json::array({"customer_id", "provider", "document", "workload_taap_id"})},
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
            auto credential =
                context.callers->exchange_cloud(input.at("provider").get<std::string>(),
                                                input.at("document").get<std::string>(),
                                                input.at("workload_taap_id").get<std::string>());
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
