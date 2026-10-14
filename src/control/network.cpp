#include "network_internal.hpp"

#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace lake::control::network_detail {
namespace {
using nlohmann::json;

std::optional<std::string> optional_text(const json& value, const char* name) {
    return value.contains(name) && !value.at(name).is_null()
               ? std::optional(value.at(name).get<std::string>())
               : std::nullopt;
}

NetworkMetadata metadata(const json& value) {
    NetworkMetadata result;
    result.id = value.at("id");
    result.customer_id = value.at("customer_id");
    result.workspace_id = value.at("workspace_id");
    result.generation = value.at("generation");
    result.created_by = optional_text(value, "created_by");
    result.created_at = value.at("created_at");
    result.updated_at = value.at("updated_at");
    if (value.contains("status") && !value.at("status").is_null()) {
        const auto& status = value.at("status");
        result.status = NetworkStatus{.phase = status.at("phase"),
                                      .message = status.at("message"),
                                      .observed_generation = status.at("observed_generation"),
                                      .updated_at = status.at("updated_at"),
                                      .credential_secret = status.value("credential_secret", ""),
                                      .endpoint = status.value("endpoint", ""),
                                      .relay_ca_pem = status.value("relay_ca_pem", "")};
    }
    return result;
}

Result<void> invalid(std::string message) {
    return std::unexpected(StoreError{"invalid_input", std::move(message)});
}

bool valid_port(const json& port) {
    return port.is_number_integer() && port >= 1 && port <= 65535;
}

bool valid_cidr(const std::string& value) {
    const auto slash = value.find('/');
    const std::string address = value.substr(0, slash);
    const int family = address.find(':') == std::string::npos ? AF_INET : AF_INET6;
    const int bytes = family == AF_INET ? 4 : 16;
    int prefix = bytes * 8;
    if (slash != std::string::npos) {
        const auto* begin = value.data() + slash + 1;
        const auto* end = value.data() + value.size();
        auto parsed = std::from_chars(begin, end, prefix);
        if (parsed.ec != std::errc{} || parsed.ptr != end || prefix < 0 || prefix > bytes * 8) {
            return false;
        }
    }
    std::array<unsigned char, 16> packed{};
    if (inet_pton(family, address.c_str(), packed.data()) != 1) {
        return false;
    }
    for (int bit = prefix; bit < bytes * 8; ++bit) {
        if ((packed[bit / 8] & (1 << (7 - bit % 8))) != 0) {
            return false;
        }
    }
    return true;
}

Result<void>
workload(const json& spec,
         const char* kind,
         const char* id,
         bool source,
         const std::function<Result<void>(const std::string&, const std::string&)>& check) {
    if (spec.at(kind).is_null() || spec.at(id).is_null()) {
        return invalid("a workload needs both its kind and id");
    }
    const std::string type = spec.at(kind);
    const std::string key = spec.at(id);
    if (type != "app" && type != "service" && !(source && (type == "agent" || type == "tunnel"))) {
        return invalid("unknown network workload kind");
    }
    if (!uuid(key)) {
        return invalid("a network workload id must be a UUID");
    }
    return check(type, key);
}
} // namespace

bool uuid(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? value[i] != '-' : !std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

Result<void> validate_scope(const NetworkScope& scope) {
    if (!uuid(scope.customer_id) || !uuid(scope.workspace_id)) {
        return invalid("network customer_id and workspace_id must be UUIDs");
    }
    return {};
}

// Input instants are UTC RFC 3339, with optional fractional seconds. Dates and all components
// are checked before conversion, so an invalid calendar day never rolls into another month.
Result<double> instant(const std::string& value) {
    std::tm parts{};
    std::istringstream input(value);
    input >> std::get_time(&parts, "%Y-%m-%dT%H:%M:%S");
    const std::chrono::year_month_day day{
        std::chrono::year{parts.tm_year + 1900},
        std::chrono::month{static_cast<unsigned>(parts.tm_mon + 1)},
        std::chrono::day{static_cast<unsigned>(parts.tm_mday)}};
    if (input.fail() || value.size() < 20 || value[10] != 'T' || !day.ok() || parts.tm_hour > 23 ||
        parts.tm_min > 59 || parts.tm_sec > 59) {
        return std::unexpected(
            StoreError{"invalid_input", "expires_at must be a UTC RFC 3339 instant"});
    }
    double fraction = 0;
    if (input.peek() == '.') {
        input.get();
        double place = 0.1;
        int digits = 0;
        while (input.peek() >= '0' && input.peek() <= '9') {
            fraction += (input.get() - '0') * place;
            place /= 10;
            ++digits;
        }
        if (digits == 0 || digits > 6) {
            return std::unexpected(
                StoreError{"invalid_input", "expires_at supports up to six fractional digits"});
        }
    }
    if (input.get() != 'Z' || input.peek() != std::char_traits<char>::eof()) {
        return std::unexpected(StoreError{"invalid_input", "expires_at must use UTC Z"});
    }
    return std::chrono::duration<double>(std::chrono::sys_days(day).time_since_epoch()).count() +
           parts.tm_hour * 3600 + parts.tm_min * 60 + parts.tm_sec + fraction;
}

Result<void>
validate(const Table& table,
         const json& spec,
         const std::function<Result<void>(const std::string&, const std::string&)>& check) {
    if (spec.contains("ports")) {
        if (table.name == std::string_view("network_egress_rules") && spec.at("ports").empty()) {
            return invalid("egress needs at least one port");
        }
        for (const auto& port : spec.at("ports")) {
            if (!valid_port(port)) {
                return invalid("ports must be between 1 and 65535");
            }
        }
    }
    if (spec.contains("source_kind")) {
        const bool has_kind = !spec.at("source_kind").is_null();
        const bool has_id = !spec.at("source_id").is_null();
        if (has_kind != has_id) {
            return invalid("source_kind and source_id must both be set or both be absent");
        }
        if (has_kind) {
            if (table.name == std::string_view("network_egress_rules") &&
                spec.at("source_kind") == "tunnel") {
                return invalid("egress sources are apps, services or agents");
            }
            if (auto result = workload(spec, "source_kind", "source_id", true, check); !result) {
                return result;
            }
        }
    }
    if (spec.contains("target_kind")) {
        if (auto result = workload(spec, "target_kind", "target_id", false, check); !result) {
            return result;
        }
    }
    if (spec.contains("credential_scope") && !spec.at("credential_scope").is_null()) {
        const auto& scope = spec.at("credential_scope");
        if (!scope.is_object() || !scope.contains("access") ||
            (scope.at("access") != "read" && scope.at("access") != "write") ||
            (scope.contains("schema") &&
             (!scope.at("schema").is_string() || scope.at("schema").get<std::string>().empty()))) {
            return invalid("credential_scope needs access read or write and an optional schema");
        }
        for (auto it = scope.begin(); it != scope.end(); ++it) {
            if (it.key() != "access" && it.key() != "schema") {
                return invalid("credential_scope contains an unknown field");
            }
        }
    }
    if (spec.contains("cidr")) {
        if (spec.at("cidr").is_null() == spec.at("dns_name").is_null()) {
            return invalid("exactly one of cidr and dns_name must be set");
        }
        if (!spec.at("cidr").is_null() && !valid_cidr(spec.at("cidr"))) {
            return invalid("cidr must be a network address with no host bits");
        }
        if (!spec.at("dns_name").is_null()) {
            const std::string name = spec.at("dns_name");
            if (name.empty() || name.size() > 253 ||
                name.find_first_of(" \t\r\n/") != std::string::npos) {
                return invalid("dns_name must be a nonempty DNS name");
            }
        }
    }
    if (table.tunnel) {
        if (!uuid(spec.at("user_id").get<std::string>()) || !valid_port(spec.at("port"))) {
            return invalid("a tunnel needs a user UUID and a port between 1 and 65535");
        }
        auto expires = instant(spec.at("expires_at"));
        if (!expires) {
            return std::unexpected(expires.error());
        }
        const double now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (*expires <= now) {
            return invalid("expires_at must be in the future");
        }
    }
    if (spec.contains("exports")) {
        if (spec.at("name").get<std::string>().empty() ||
            spec.at("remote").get<std::string>().empty() || !spec.at("exports").is_array() ||
            !spec.at("imports").is_array()) {
            return invalid("a peer needs a name, remote address, exports array and imports array");
        }
        for (const auto& entry : spec.at("exports")) {
            if (!entry.is_object() || !entry.contains("target_kind") ||
                !entry.at("target_kind").is_string() || !entry.contains("target_id") ||
                !entry.at("target_id").is_string() || !entry.contains("port") ||
                !valid_port(entry.at("port"))) {
                return invalid("each export needs target_kind, target_id and port");
            }
            if (auto result = workload(entry, "target_kind", "target_id", false, check); !result) {
                return result;
            }
        }
        for (const auto& entry : spec.at("imports")) {
            if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string() ||
                entry.at("name").get<std::string>().empty() || !entry.contains("port") ||
                !valid_port(entry.at("port"))) {
                return invalid("each import needs a name and port");
            }
        }
    }
    return {};
}

Result<audit::Identity>
prepare_audit(Store& store, const NetworkActor& actor, const audit::IdentityOptions& options) {
    if (actor.principal.empty() || actor.request_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "network writes need a principal and request_id"});
    }
    if (!actor.user_id.empty()) {
        if (!uuid(actor.user_id)) {
            return std::unexpected(StoreError{"invalid_input", "actor user_id must be a UUID"});
        }
        auto user = store.get_user(actor.user_id);
        if (!user || user->taap_id != actor.principal) {
            return std::unexpected(
                StoreError{"invalid_input", "actor must name the user's TAAP identity"});
        }
    }
    auto signer = audit::identity(store, options);
    if (!signer) {
        return std::unexpected(StoreError{signer.error().code, signer.error().message});
    }
    return *signer;
}

Result<audit::Event> sign_change(Store& store,
                                 const audit::Identity& signer,
                                 const NetworkActor& actor,
                                 const NetworkScope& scope,
                                 const Table& table,
                                 const std::string& id,
                                 const std::string& operation,
                                 std::int64_t generation,
                                 const std::string& previous,
                                 const audit::IdentityOptions& options) {
    const audit::EventDraft draft{.customer_id = scope.customer_id,
                                  .kind = "control_plane",
                                  .principal = actor.principal,
                                  .object = std::string(table.object_kind) + "/" + id,
                                  .action = operation + "_" + table.object_kind,
                                  .outcome = "success",
                                  .request_id = actor.request_id,
                                  .details = {{"workspace_id", scope.workspace_id},
                                              {"user_id", actor.user_id},
                                              {"generation", generation}}};
    auto event = audit::sign_pending(store, signer, audit::pending(draft), previous, options);
    if (!event) {
        return std::unexpected(StoreError{event.error().code, event.error().message});
    }
    return *event;
}

json metadata_json(const NetworkMetadata& row) {
    return {{"id", row.id},
            {"customer_id", row.customer_id},
            {"workspace_id", row.workspace_id},
            {"generation", row.generation},
            {"created_by", row.created_by ? json(*row.created_by) : json(nullptr)},
            {"created_at", row.created_at},
            {"updated_at", row.updated_at}};
}

template <> const Table& table<NetworkConnection>() {
    static const Table value{
        "network_connections",
        "network_connection_status",
        "connection_id",
        "network_connection",
        {"source_kind", "source_id", "target_kind", "target_id", "ports", "credential_scope"},
        false};
    return value;
}

json encode(const NetworkConnectionSpec& spec) {
    return {{"source_kind", spec.source_kind},
            {"source_id", spec.source_id},
            {"target_kind", spec.target_kind},
            {"target_id", spec.target_id},
            {"ports", spec.ports},
            {"credential_scope", spec.credential_scope}};
}

template <> NetworkConnection decode<NetworkConnection>(const json& value) {
    NetworkConnection result;
    static_cast<NetworkMetadata&>(result) = metadata(value);
    result.spec.source_kind = value.at("source_kind").get<std::string>();
    result.spec.source_id = value.at("source_id").get<std::string>();
    result.spec.target_kind = value.at("target_kind").get<std::string>();
    result.spec.target_id = value.at("target_id").get<std::string>();
    result.spec.ports = value.at("ports").get<std::vector<int>>();
    result.spec.credential_scope = value.at("credential_scope").get<nlohmann::json>();
    result.desired = value.at("desired");
    return result;
}

template <> const Table& table<NetworkEgressRule>() {
    static const Table value{"network_egress_rules",
                             "network_egress_rule_status",
                             "rule_id",
                             "network_egress_rule",
                             {"source_kind", "source_id", "cidr", "dns_name", "ports"},
                             false};
    return value;
}

json encode(const NetworkEgressRuleSpec& spec) {
    return {{"source_kind", spec.source_kind ? json(*spec.source_kind) : json(nullptr)},
            {"source_id", spec.source_id ? json(*spec.source_id) : json(nullptr)},
            {"cidr", spec.cidr ? json(*spec.cidr) : json(nullptr)},
            {"dns_name", spec.dns_name ? json(*spec.dns_name) : json(nullptr)},
            {"ports", spec.ports}};
}

template <> NetworkEgressRule decode<NetworkEgressRule>(const json& value) {
    NetworkEgressRule result;
    static_cast<NetworkMetadata&>(result) = metadata(value);
    result.spec.source_kind = optional_text(value, "source_kind");
    result.spec.source_id = optional_text(value, "source_id");
    result.spec.cidr = optional_text(value, "cidr");
    result.spec.dns_name = optional_text(value, "dns_name");
    result.spec.ports = value.at("ports").get<std::vector<int>>();
    result.desired = value.at("desired");
    return result;
}

template <> const Table& table<NetworkTunnelGrant>() {
    static const Table value{
        "network_tunnel_grants",
        "network_tunnel_grant_status",
        "grant_id",
        "network_tunnel_grant",
        {"user_id", "target_kind", "target_id", "port", "expires_at", "client_ca_pem"},
        true};
    return value;
}

json encode(const NetworkTunnelGrantSpec& spec) {
    return {{"user_id", spec.user_id},
            {"target_kind", spec.target_kind},
            {"target_id", spec.target_id},
            {"port", spec.port},
            {"expires_at", spec.expires_at},
            {"client_ca_pem", spec.client_ca_pem}};
}

template <> NetworkTunnelGrant decode<NetworkTunnelGrant>(const json& value) {
    NetworkTunnelGrant result;
    static_cast<NetworkMetadata&>(result) = metadata(value);
    result.spec.user_id = value.at("user_id").get<std::string>();
    result.spec.target_kind = value.at("target_kind").get<std::string>();
    result.spec.target_id = value.at("target_id").get<std::string>();
    result.spec.port = value.at("port").get<int>();
    result.spec.expires_at = value.at("expires_at").get<std::string>();
    result.spec.client_ca_pem = value.value("client_ca_pem", "");
    result.revoked_at = optional_text(value, "revoked_at");
    return result;
}

template <> const Table& table<NetworkPeer>() {
    static const Table value{"network_peers",
                             "network_peer_status",
                             "peer_id",
                             "network_peer",
                             {"name", "remote", "exports", "imports"},
                             false};
    return value;
}

json encode(const NetworkPeerSpec& spec) {
    return {{"name", spec.name},
            {"remote", spec.remote},
            {"exports", spec.exports},
            {"imports", spec.imports}};
}

template <> NetworkPeer decode<NetworkPeer>(const json& value) {
    NetworkPeer result;
    static_cast<NetworkMetadata&>(result) = metadata(value);
    result.spec.name = value.at("name").get<std::string>();
    result.spec.remote = value.at("remote").get<std::string>();
    result.spec.exports = value.at("exports").get<nlohmann::json>();
    result.spec.imports = value.at("imports").get<nlohmann::json>();
    result.desired = value.at("desired");
    return result;
}
} // namespace lake::control::network_detail

namespace lake::control {

std::optional<std::chrono::system_clock::time_point>
network_tunnel_grant_expiry(const NetworkTunnelGrant& grant) {
    const auto expires = network_detail::instant(grant.spec.expires_at);
    if (!expires) {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(*expires)));
}

bool network_tunnel_grant_active(const NetworkTunnelGrant& grant,
                                 std::chrono::system_clock::time_point now) {
    if (grant.revoked_at) {
        return false;
    }
    const auto expires = network_tunnel_grant_expiry(grant);
    if (!expires) {
        return false;
    }
    return *expires > now;
}

} // namespace lake::control
