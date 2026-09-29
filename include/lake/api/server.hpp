#pragma once
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "lake/api/registry.hpp"
#include "lake/transparency/checkpoint.hpp"

namespace lake::search {
class Service;
}

namespace lake::control {
class Store;
class PlatformService;
class WorkspaceService;
class AgentCaller;
class AgentTaskBus;
} // namespace lake::control
namespace lake::control::callers {
class TokenReviewer;
class JwtVerifier;
class CloudIdentityVerifier;
class FederationIssuerPolicy;
} // namespace lake::control::callers

// The embedded HTTP server, in Zetta's shape (zetta/api/server.hpp): Beast2 on
// the capy/corosio runtime, confined entirely to the adapter TU
// (src/api/http_server.cpp). No beast2 / http / Boost.JSON type appears here.
//
// Threading: the server's io_context runs on its own thread. The owning loop
// calls wait_for_work() to idle and drain() each tick. drain() runs scheduled
// owner-loop work such as timestamp-evidence renewal.
namespace lake::api {

struct ServerOptions {
    std::string listen_address = "127.0.0.1";
    unsigned short port = 4900; // 0 = pick an ephemeral port
    int worker_coroutines = 32;
    std::string node_name = "lake";
    // Public hostname used in OAuth callbacks, as the WebAuthn RP ID, and as
    // the HTTPS repository-command origin when external_origin is unset.
    std::string public_host;
    // Configured browser-facing HTTP(S) origin, including any non-default port.
    // It takes precedence over public_host. When both are empty, repository
    // terminal instructions are suppressed; request headers never supply it.
    std::string external_origin;
    // The control-plane store the capabilities call; null = none configured
    // (capabilities answer store_unavailable). Owned by the caller for the
    // server's lifetime.
    std::shared_ptr<control::Store> store;
    // Search index service backed by LAKE_STORE_ROOT. Null keeps search
    // unavailable, which is useful for a control-plane-only node.
    std::shared_ptr<search::Service> search;
    std::shared_ptr<control::PlatformService> platform;
    // Customer Zetta workspaces (docs/api/workspaces.md); null leaves Design, Train and Ship
    // unavailable.
    std::shared_ptr<control::WorkspaceService> workspaces;
    // LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false turns off every customer's opt-in (lake#234).
    bool allow_non_expiring_credentials = true;
    // Talks to agent services (lake#231); tests inject a double, main the HTTP caller.
    std::shared_ptr<control::AgentCaller> agent_caller;
    // The agent bus (docs/api/agent-bus.md): agent turns are published and followed on it.
    std::shared_ptr<control::AgentTaskBus> agent_bus;
    // Ship Apps wildcard and per-space quotas. Zero quotas mean no limit.
    std::string apps_wildcard;
    std::size_t apps_per_customer = 0;
    std::size_t app_replicas_per_customer = 0;
    // In-cluster Service URL used in build registry targets (LAKE_INTERNAL_URL).
    std::string internal_url;
    // The admin bearer token (LAKE_ADMIN_TOKEN). Empty = every capability except
    // server_info refuses with unauthorized.
    std::string admin_token;
    // Bearer presented to another Lake during asynchronous replication. The
    // executable defaults it to the admin bearer when no separate Secret is set.
    std::string replication_token;
    // A base64 32-byte Ed25519 seed. When empty lake creates its own identity at
    // server start.
    std::string identity_seed;
    // Customer-independent key-encryption key for the generated identity seed.
    std::string instance_kek;
    // Clerk publishable key (LAKE_CLERK_PUBLISHABLE_KEY). When it is a valid key, browser sign-in
    // and sign-up are Clerk's and lake's own identity pages leave the navigation; empty or `xxx`
    // keeps lake's own sign-in, as an on-prem install uses. See docs/api/clerk.md.
    std::string clerk_publishable_key;
    // Configured external attestation verifiers. Lake owns their invocation but
    // callers retain their lifetime for the server lifetime.
    control::callers::TokenReviewer* token_reviewer = nullptr;
    control::callers::JwtVerifier* jwt_verifier = nullptr;
    control::callers::CloudIdentityVerifier* cloud_identity_verifier = nullptr;
    control::callers::FederationIssuerPolicy* federation_issuer_policy = nullptr;
    // The operator-provided PersistentVolume mount. Customer stores live at
    // <root>/<customer id>/store and instance log objects at
    // <root>/<node name>/log. Empty disables data-plane storage.
    std::string store_root;
    // Transparency-log runtime configuration. The executable obtains this from
    // LAKE_LOG_* environment variables and defaults the deployed mode to
    // witnessed.
    transparency::LogConfiguration log_configuration;
    // RFC 3161 endpoint and trust roots used to timestamp checkpoint signatures.
    // Empty endpoint disables timestamp creation and scheduled renewal.
    std::string tsa_endpoint;
    std::string tsa_trust_pem;
    transparency::TimestampPolicy timestamp_policy;
    // Optional transport supplied by embedded users and integration tests.
    // The standalone server uses its outbound HTTP client when this is empty.
    transparency::TsaSender tsa_sender;
    MeteringConfig metering;
};

// Prefer the full configured origin. public_host is already the browser-facing
// hostname used by OAuth, so it supplies a safe HTTPS fallback when no origin
// was configured. The empty result keeps credential-bearing commands hidden.
inline std::string repository_external_origin(const ServerOptions& options) {
    if (!options.external_origin.empty()) {
        return options.external_origin;
    }
    if (!options.public_host.empty()) {
        return "https://" + options.public_host;
    }
    return {};
}

class ApiServer {
public:
    explicit ApiServer(ServerOptions options);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Bind + start serving on the io thread. False on failure (see reason()).
    bool start();
    void stop();
    [[nodiscard]] bool running() const;
    // The actual bound port (differs from options when port was 0).
    [[nodiscard]] unsigned short port() const;
    [[nodiscard]] const std::string& reason() const;
    // The GET patterns installed in the live router. Performance coverage derives its page set
    // from this list, then classifies HTML responses by their Server-Timing header.
    [[nodiscard]] std::vector<std::string> get_route_patterns() const;

    void drain();
    void wait_for_work(std::chrono::milliseconds max_idle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lake::api
