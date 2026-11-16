#include "lake/api/capabilities.hpp"

#include "lake/control/callers.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <set>
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

std::expected<std::string, ApiError> certificate_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    auto certificate = context.store->get_pki_certificate(input.at("certificate_id"));
    if (!certificate) {
        return std::unexpected(
            ApiError{certificate.error().code, certificate.error().message, "/certificate_id"});
    }
    json normalized = {{"customer_id", certificate->customer_id}};
    context.audit_customer_id = certificate->customer_id;
    context.audit_kind = "certificate";
    context.audit_object = "certificate/" + certificate->id;
    context.audit_details = {{"certificate_id", certificate->id}};
    return customer_object(normalized, context);
}

std::expected<std::string, ApiError> acme_challenge_object(const json& input, ApiContext&) {
    return "acme-http-01/" + input.at("token").get<std::string>();
}

ApiResult no_pki() {
    return ApiResult::failure("store_unavailable", "PKI service is not configured");
}

ApiResult no_callers() {
    return ApiResult::failure("store_unavailable", "caller attestation service is not configured");
}

ApiResult caller_credential_result(const json& input,
                                   const control::callers::Credential& credential,
                                   ApiContext& context) {
    context.audit_customer_id = input.at("customer_id").get<std::string>();
    context.audit_kind = "authentication";
    context.audit_object = "workload/" + credential.workload_taap_id;
    auto record =
        context.store->create_pki_caller(control::NewPkiCallerRecord{context.audit_customer_id,
                                                                     credential.workload_taap_id,
                                                                     credential.attestation_type,
                                                                     credential.issuer,
                                                                     credential.subject,
                                                                     credential.attributes,
                                                                     credential.expires_at});
    if (!record) {
        return ApiResult::failure(record.error().code, record.error().message);
    }
    return ApiResult::success({{"caller_id", record->id},
                               {"credential", credential.value},
                               {"expires_at", credential.expires_at}});
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

json certificate_record_json(const control::PkiCertificateRecord& value) {
    return {{"id", value.id},
            {"customer_id", value.customer_id},
            {"profile", value.profile},
            {"subject", value.subject},
            {"serial", value.serial},
            {"certificate_pem", value.public_value},
            {"expires_at", value.expires_at},
            {"issued_at", value.issued_at},
            {"renewed_from_id", value.renewed_from_id},
            {"revoked_at", value.revoked_at}};
}

std::string expiry_after(int lifetime_seconds) {
    const auto expiry = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now() + std::chrono::seconds(lifetime_seconds));
    return std::format("{:%FT%TZ}", expiry);
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
                ApiError{"invalid_input", "share value must be hexadecimal", "/share"});
        }
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::expected<control::pki::ShamirShare, ApiError> share_of(const json& input) {
    if (!input.is_object() || !input.contains("index") || !input.contains("value") ||
        !input.at("index").is_number_integer() || !input.at("value").is_string()) {
        return std::unexpected(
            ApiError{"invalid_input", "a recovery share needs index and value", "/share"});
    }
    auto bytes = bytes_of_hex(input.at("value").get<std::string>());
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return control::pki::ShamirShare{input.at("index").get<int>(), std::move(*bytes)};
}

// A person's own credential: a share of a recovery ceremony goes to, and comes from, only the
// participant signed in as themselves (lake#934), never the operator, an agent or a workload.
std::expected<void, ApiResult> recovery_participant(const ApiContext& context) {
    static const std::set<std::string> person_kinds{"browser", "api_token", "static", "taap"};
    if (context.operator_credential || context.acting_user_id.empty() ||
        context.acting_identity.empty() || !person_kinds.contains(context.credential_kind)) {
        return std::unexpected(ApiResult::failure(
            "forbidden", "only a recovery participant signed in as themselves may hold a share"));
    }
    return {};
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
        .name = "issue_agent_certificate",
        .title = "Issue agent certificate",
        .description = "Issue a short-lived X.509 certificate to an agent TAAP Ed25519 key.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"public_key", {{"type", "string"}}},
                           {"lifetime_seconds", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "taap_id", "public_key"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            const int lifetime_seconds = input.value("lifetime_seconds", 300);
            auto certificate =
                context.pki->issue_agent_certificate(*context.store,
                                                     input.at("customer_id").get<std::string>(),
                                                     input.at("taap_id").get<std::string>(),
                                                     input.at("public_key").get<std::string>(),
                                                     context.instance_kek,
                                                     lifetime_seconds);
            if (!certificate) {
                return from_pki_error(certificate.error());
            }
            auto stored = context.store->create_pki_certificate(
                {input.at("customer_id").get<std::string>(),
                 "agent",
                 input.at("taap_id").get<std::string>(),
                 control::identity_crypto::sha256_hex(certificate->certificate_pem),
                 certificate->certificate_pem,
                 expiry_after(lifetime_seconds),
                 {}});
            if (!stored) {
                return ApiResult::failure(stored.error().code, stored.error().message);
            }
            context.audit_customer_id = stored->customer_id;
            context.audit_kind = "certificate";
            context.audit_object = "certificate/" + stored->id;
            context.audit_details = {{"certificate_id", stored->id}, {"operation", "issue"}};
            json output = certificate_record_json(*stored);
            output["chain_pem"] = certificate->chain_pem;
            return ApiResult::success(std::move(output));
        }});
    registry.add(Capability{
        .name = "rotate_agent_certificate",
        .title = "Rotate agent certificate",
        .description = "Issue a replacement certificate for an agent TAAP key.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"certificate_id", {{"type", "string"}}},
                           {"public_key", {{"type", "string"}}},
                           {"lifetime_seconds", {{"type", "integer"}}}}},
                         {"required", json::array({"certificate_id", "public_key"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = certificate_object,
        .handler = [](const json& input, ApiContext& context) {
            auto previous = context.store->get_pki_certificate(input.at("certificate_id"));
            if (!previous) {
                return ApiResult::failure(previous.error().code, previous.error().message);
            }
            if (previous->profile != "agent") {
                return ApiResult::failure("invalid_input",
                                          "certificate is not an agent certificate");
            }
            const int lifetime_seconds = input.value("lifetime_seconds", 300);
            auto certificate =
                context.pki->issue_agent_certificate(*context.store,
                                                     previous->customer_id,
                                                     previous->subject,
                                                     input.at("public_key").get<std::string>(),
                                                     context.instance_kek,
                                                     lifetime_seconds);
            if (!certificate) {
                return from_pki_error(certificate.error());
            }
            auto stored = context.store->create_pki_certificate(
                {previous->customer_id,
                 "agent",
                 previous->subject,
                 control::identity_crypto::sha256_hex(certificate->certificate_pem),
                 certificate->certificate_pem,
                 expiry_after(lifetime_seconds),
                 previous->id});
            if (!stored) {
                return ApiResult::failure(stored.error().code, stored.error().message);
            }
            context.audit_object = "certificate/" + stored->id;
            context.audit_details = {{"certificate_id", stored->id},
                                     {"operation", "rotate"},
                                     {"renewed_from_id", previous->id}};
            json output = certificate_record_json(*stored);
            output["chain_pem"] = certificate->chain_pem;
            return ApiResult::success(std::move(output));
        }});
    registry.add(Capability{
        .name = "revoke_agent_certificate",
        .title = "Revoke agent certificate",
        .description = "Revoke an issued agent certificate immediately.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"certificate_id", {{"type", "string"}}}}},
                         {"required", json::array({"certificate_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = certificate_object,
        .handler = [](const json& input, ApiContext& context) {
            auto revoked = context.store->revoke_pki_certificate(input.at("certificate_id"));
            if (!revoked) {
                return ApiResult::failure(revoked.error().code, revoked.error().message);
            }
            context.audit_details["operation"] = "revoke";
            return ApiResult::success(certificate_record_json(*revoked));
        }});
    registry.add(Capability{
        .name = "check_agent_certificate",
        .title = "Check agent certificate",
        .description = "Reject revoked or expired agent certificates at the API boundary.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"certificate_id", {{"type", "string"}}}}},
                         {"required", json::array({"certificate_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = certificate_object,
        .handler = [](const json& input, ApiContext& context) {
            auto certificate = context.store->get_pki_certificate(input.at("certificate_id"));
            if (!certificate) {
                return ApiResult::failure(certificate.error().code, certificate.error().message);
            }
            context.audit_details["operation"] = "check";
            if (!certificate->revoked_at.empty()) {
                return ApiResult::failure("unauthorized", "certificate is revoked");
            }
            if (certificate->expires_at <= expiry_after(0)) {
                return ApiResult::failure("unauthorized", "certificate is expired");
            }
            return ApiResult::success({{"valid", true}, {"certificate_id", certificate->id}});
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
            .description =
                profile == "client"
                    ? "Issue a short-lived TLS client certificate from the account's authority, so a "
                      "program can prove who it is to a server that trusts it."
                    : "Issue a short-lived device certificate from the account's authority, so a "
                      "machine can prove which device it is.",
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
            .description =
                host ? std::string("Sign a server's Ed25519 SSH host key with the account's SSH "
                                   "authority, so clients that trust the authority can connect "
                                   "without a host-key prompt; short-lived.")
                     : std::string("Sign a person's or workload's Ed25519 SSH key with the account's "
                                   "SSH authority, so it can log in as the listed principals; "
                                   "short-lived."),
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
    registry.add(Capability{
        .name = "create_acme_external_account_binding",
        .title = "Create ACME account binding",
        .description = "Issue an RFC 8555 External Account Binding (key id and HMAC key) that "
                       "lets one ACME client, such as cert-manager, enroll on this customer's "
                       "certificate authority. Customer administrators only; the key is shown "
                       "once.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"key_id", {{"type", "string"}}},
                            {"hmac_key", {{"type", "string"}}},
                            {"directory", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string customer_id = input.at("customer_id");
            auto binding =
                lake::control::pki::new_external_account_binding(context.instance_kek, customer_id);
            if (!binding) {
                return from_pki_error(binding.error());
            }
            context.audit_customer_id = customer_id;
            context.audit_kind = "certificate";
            context.audit_object = "acme-binding/" + binding->key_id;
            return ApiResult::success({{"key_id", binding->key_id},
                                       {"hmac_key", binding->hmac_key},
                                       {"directory", "/acme/" + customer_id + "/directory"}});
        }});
    registry.add(
        Capability{.name = "acme_new_account",
                   .title = "Create ACME account",
                   .description = "Register an RFC 8555 account key for one customer, bound by "
                                  "an External Account Binding its administrator issued "
                                  "(create_acme_external_account_binding).",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"customer_id", {{"type", "string"}}},
                                      {"x", {{"type", "string"}}},
                                      {"y", {{"type", "string"}}},
                                      {"external_account_binding", {{"type", "string"}}},
                                      {"url", {{"type", "string"}}}}},
                                    {"required", json::array({"customer_id", "x", "y"})},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"},
                                     {"properties", {{"account_id", {{"type", "string"}}}}}},
                   .kind = CapabilityKind::Action,
                   .transport = CapabilityTransport::Acme,
                   .requires_auth = false,
                   .object = "customer",
                   .action = "admin",
                   .resolve_object = customer_object,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.pki || !context.store) {
                           return no_pki();
                       }
                       if (!context.store->get_customer(input.at("customer_id").get<std::string>())) {
                           return ApiResult::failure("not_found", "no such customer");
                       }
                       // Only an administrator of the customer enrolls a client on it
                       // (lake#919): the account's key must be bound by a key they were given.
                       const lake::control::pki::AcmeJwk jwk{input.at("x").get<std::string>(),
                                                             input.at("y").get<std::string>()};
                       if (auto bound = lake::control::pki::verify_external_account_binding(
                               context.instance_kek,
                               input.at("customer_id").get<std::string>(),
                               input.value("external_account_binding", ""),
                               input.value("url", ""),
                               jwk);
                           !bound) {
                           return ApiResult::failure("unauthorized", bound.error().message);
                       }
                       auto account = context.pki->create_acme_account(
                           *context.store,
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
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
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
            .description =
                authorization_lookup
                    ? std::string("Read an ACME authorization: whether the HTTP-01 challenge for "
                                  "the domain has been validated yet (RFC 8555).")
                    : std::string("Read an ACME certificate order: its status and, once it is "
                                  "valid, the certificate to download (RFC 8555)."),
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
            .object = "customer",
            .action = "read",
            .resolve_object = customer_object,
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
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki) {
                return no_pki();
            }
            // Proof of control (lake#919): the HTTP-01 response Lake fetched from the identifier
            // itself must be this challenge's key authorization before the order is ready.
            const auto pending =
                context.pki->acme_authorization(input.at("customer_id").get<std::string>(),
                                                input.at("account_id").get<std::string>(),
                                                input.at("authorization_id").get<std::string>());
            if (!pending) {
                return from_pki_error(pending.error());
            }
            if (pending->status != "valid") {
                std::string observed = context.acme_http01_body.value_or("");
                while (!observed.empty() &&
                       (observed.back() == '\n' || observed.back() == '\r' ||
                        observed.back() == ' ' || observed.back() == '\t')) {
                    observed.pop_back();
                }
                if (!context.acme_http01_body || observed != pending->key_authorization) {
                    return ApiResult::failure(
                        "incorrect_response",
                        "http://" + pending->identifier + "/.well-known/acme-challenge/" +
                            pending->token + " did not answer this challenge's key authorization");
                }
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
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
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
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
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
        .object = "certificate",
        .action = "read",
        .resolve_object = acme_challenge_object,
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
                           {{"caller_id", {{"type", "string"}}},
                            {"credential", {{"type", "string"}}},
                            {"expires_at", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.callers || !context.store) {
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
            return caller_credential_result(input, *credential, context);
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
                           {{"caller_id", {{"type", "string"}}},
                            {"credential", {{"type", "string"}}},
                            {"expires_at", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.callers || !context.store) {
                return no_callers();
            }
            auto credential =
                context.callers->exchange_jwt(input.at("organization_id").get<std::string>(),
                                              input.at("token").get<std::string>(),
                                              input.at("workload_taap_id").get<std::string>());
            if (!credential) {
                return from_caller_error(credential.error());
            }
            return caller_credential_result(input, *credential, context);
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
                           {{"caller_id", {{"type", "string"}}},
                            {"credential", {{"type", "string"}}},
                            {"expires_at", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.callers || !context.store) {
                return no_callers();
            }
            auto credential =
                context.callers->exchange_cloud(input.at("provider").get<std::string>(),
                                                input.at("document").get<std::string>(),
                                                input.at("workload_taap_id").get<std::string>());
            if (!credential) {
                return from_caller_error(credential.error());
            }
            return caller_credential_result(input, *credential, context);
        }});
    registry.add(Capability{
        .name = "begin_root_recovery",
        .title = "Begin root recovery rehearsal",
        .description = "Begin a Shamir recovery ceremony among distinct participants, each of "
                       "whom claims only their own share.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"threshold", {{"type", "integer"}, {"minimum", 2}}},
                           {"participants",
                            {{"type", "array"},
                             {"items", {{"type", "string"}}},
                             {"minItems", 2},
                             {"maxItems", 255},
                             {"uniqueItems", true}}}}},
                         {"required", json::array({"customer_id", "threshold", "participants"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"ceremony_id", {{"type", "string"}}},
                            {"threshold", {{"type", "integer"}}},
                            {"share_count", {{"type", "integer"}}},
                            {"participants", {{"type", "array"}}},
                            {"expires_at", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            const auto object = customer_object(input, context);
            if (!object) {
                return ApiResult::failure(
                    object.error().code, object.error().message, object.error().path);
            }
            // Every participant is a TAAP identity holding admin on the customer now; each is
            // checked again, as the caller, when they claim and present their share.
            std::vector<std::string> participants;
            for (const json& value : input.at("participants")) {
                const std::string principal = value.is_string() ? value.get<std::string>() : "";
                if (!principal.starts_with("taap:")) {
                    return ApiResult::failure(
                        "invalid_input", "each participant is a TAAP identity", "/participants");
                }
                const auto decision = control::authorize(
                    *context.store, principal, *object, "admin", context.authorization_cache);
                if (!decision) {
                    return ApiResult::failure(decision.error().code, decision.error().message);
                }
                if (!decision->allowed) {
                    return ApiResult::failure("invalid_input",
                                              "participant " + principal +
                                                  " does not hold admin on the customer",
                                              "/participants");
                }
                participants.push_back(principal);
            }
            const int threshold = input.at("threshold").get<int>();
            if (threshold < 2 || threshold > static_cast<int>(participants.size())) {
                return ApiResult::failure(
                    "invalid_input",
                    "threshold must be at least 2 and at most the number of participants",
                    "/threshold");
            }
            auto ceremony = context.pki->begin_recovery(*context.store,
                                                        input.at("customer_id").get<std::string>(),
                                                        context.instance_kek,
                                                        threshold,
                                                        participants);
            if (!ceremony) {
                return from_pki_error(ceremony.error());
            }
            json listed = json::array();
            for (std::size_t index = 0; index < ceremony->participants.size(); ++index) {
                listed.push_back({{"index", static_cast<int>(index) + 1},
                                  {"principal", ceremony->participants[index]}});
            }
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "recovery";
            context.audit_object = "root-recovery/" + ceremony->id;
            context.audit_details["threshold"] = ceremony->threshold;
            context.audit_details["participants"] = ceremony->participants;
            return ApiResult::success({{"ceremony_id", ceremony->id},
                                       {"threshold", ceremony->threshold},
                                       {"share_count", static_cast<int>(listed.size())},
                                       {"participants", std::move(listed)},
                                       {"expires_at", ceremony->expires_at}});
        }});
    registry.add(Capability{
        .name = "claim_root_recovery_share",
        .title = "Claim my root recovery share",
        .description = "Issue the caller's own share of a root recovery ceremony, once.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"ceremony_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "ceremony_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"ceremony_id", {{"type", "string"}}},
                            {"index", {{"type", "integer"}}},
                            {"value", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            if (auto person = recovery_participant(context); !person) {
                return person.error();
            }
            const std::string ceremony_id = input.at("ceremony_id").get<std::string>();
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "recovery";
            context.audit_object = "root-recovery/" + ceremony_id;
            auto share =
                context.pki->claim_recovery_share(*context.store,
                                                  input.at("customer_id").get<std::string>(),
                                                  context.instance_kek,
                                                  ceremony_id,
                                                  context.acting_identity);
            if (!share) {
                return from_pki_error(share.error());
            }
            context.audit_details["share_index"] = share->index;
            return ApiResult::success({{"ceremony_id", ceremony_id},
                                       {"index", share->index},
                                       {"value", hex_of(share->value)}});
        }});
    registry.add(Capability{
        .name = "complete_root_recovery",
        .title = "Present my root recovery share",
        .description = "Present the caller's own share; the ceremony is recovered once a quorum "
                       "of distinct participants has presented theirs.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"ceremony_id", {{"type", "string"}}},
                           {"share",
                            {{"type", "object"},
                             {"properties",
                              {{"index", {{"type", "integer"}}}, {"value", {{"type", "string"}}}}},
                             {"required", json::array({"index", "value"})},
                             {"additionalProperties", false}}}}},
                         {"required", json::array({"customer_id", "ceremony_id", "share"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"recovered", {{"type", "boolean"}}},
                            {"submitted", {{"type", "integer"}}},
                            {"threshold", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.pki || !context.store) {
                return no_pki();
            }
            if (auto person = recovery_participant(context); !person) {
                return person.error();
            }
            auto share = share_of(input.at("share"));
            if (!share) {
                return ApiResult::failure(
                    share.error().code, share.error().message, share.error().path);
            }
            const std::string ceremony_id = input.at("ceremony_id").get<std::string>();
            context.audit_customer_id = input.at("customer_id").get<std::string>();
            context.audit_kind = "recovery";
            context.audit_object = "root-recovery/" + ceremony_id;
            context.audit_details["share_index"] = share->index;
            auto submitted =
                context.pki->submit_recovery_share(*context.store,
                                                   input.at("customer_id").get<std::string>(),
                                                   context.instance_kek,
                                                   ceremony_id,
                                                   context.acting_identity,
                                                   *share);
            if (!submitted) {
                return from_pki_error(submitted.error());
            }
            return ApiResult::success({{"recovered", submitted->recovered},
                                       {"submitted", submitted->submitted},
                                       {"threshold", submitted->threshold}});
        }});
}

} // namespace lake::api
