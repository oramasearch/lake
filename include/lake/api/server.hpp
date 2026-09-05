#pragma once
#include <chrono>
#include <memory>
#include <string>

#include "lake/api/registry.hpp"
#include "lake/transparency/checkpoint.hpp"

namespace lake::search {
class Service;
}

namespace lake::control {
class Store;
}
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
// calls wait_for_work() to idle and drain() each tick (no-ops until lake has a
// node to marshal onto — the API is kept so the shape matches Zetta from day
// one).
namespace lake::api {

struct ServerOptions {
    std::string listen_address = "127.0.0.1";
    unsigned short port = 4900; // 0 = pick an ephemeral port
    int worker_coroutines = 32;
    std::string node_name = "lake";
    // Public hostname used in OAuth callbacks and as the WebAuthn RP ID.
    std::string public_host;
    // The control-plane store the capabilities call; null = none configured
    // (capabilities answer store_unavailable). Owned by the caller for the
    // server's lifetime.
    std::shared_ptr<control::Store> store;
    // Search index service backed by LAKE_STORE_ROOT. Null keeps search
    // unavailable, which is useful for a control-plane-only node.
    std::shared_ptr<search::Service> search;
    // The admin bearer token (LAKE_ADMIN_TOKEN). Empty = every capability except
    // server_info refuses with unauthorized.
    std::string admin_token;
    // A base64 32-byte Ed25519 seed. When empty lake creates its own identity at
    // server start.
    std::string identity_seed;
    // Customer-independent key-encryption key for the generated identity seed.
    std::string instance_kek;
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
    MeteringConfig metering;
};

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

    void drain();
    void wait_for_work(std::chrono::milliseconds max_idle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lake::api
