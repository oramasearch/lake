#pragma once
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "lake/api/billing.hpp"
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
class ObjectTransport;
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
//
// Store hand-off (lake#479): start() binds and serves at once. When a store root is configured,
// a separate thread takes the root's owner lock (waiting, bounded, for a predecessor to release
// it) and opens the customer stores; until then every request except probes, metrics and static
// assets waits for that open. begin_drain() (at SIGTERM) stops taking new work and releases each
// workspace store's lease once its admitted requests are done; stop() drains in-flight requests,
// closes the stores and releases the owner lock. docs/deployment/store-volume.md has the contract.
namespace lake::api {

class PaymentProviders; // lake/api/billing.hpp

struct ServerOptions {
    std::string listen_address = "127.0.0.1";
    unsigned short port = 4900; // 0 = pick an ephemeral port
    // Connections and request workers are apart (lake#534, docs/serving/http.md, "Connections and
    // workers"): a request holds one of `worker_coroutines` only while its route runs
    // (LAKE_HTTP_WORKERS), and waits up to `worker_wait` for one before it is answered 503
    // (LAKE_HTTP_WORKER_WAIT_SECONDS). An idle or still-arriving connection holds none.
    int worker_coroutines = 32;
    std::chrono::milliseconds worker_wait{std::chrono::seconds(10)};
    // Open connections, idle ones included; a connection above it is answered 503 and closed
    // (LAKE_HTTP_MAX_CONNECTIONS).
    std::size_t max_connections = 4096;
    // A keep-alive connection waiting this long for its next request is closed
    // (LAKE_HTTP_IDLE_TIMEOUT_SECONDS). It must outlast the idle timeout of a proxy's pooled
    // upstream connections (nginx: 60 s).
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(75)};
    // One read of a request body or one write of a response that makes no progress for this long
    // ends the connection (LAKE_HTTP_IO_TIMEOUT_SECONDS).
    std::chrono::milliseconds io_timeout{std::chrono::seconds(60)};
    // While a request holds a worker, the time it waits on its client may not exceed 10 s plus a
    // second per this many bytes moved (LAKE_HTTP_MIN_TRANSFER_RATE, bytes per second; 0: off).
    std::size_t min_transfer_rate = 1024;
    // Requests that left their worker (long polls, pulls), in all and per caller; past either a
    // request is answered 429 (LAKE_HTTP_MAX_LONG_REQUESTS, LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER).
    std::size_t max_long_requests = 512;
    std::size_t max_long_requests_per_caller = 64;
    // A request header must arrive whole within this of its first byte, or of the accept for a
    // connection's first request; otherwise the connection is closed
    // (LAKE_HTTP_HEADER_TIMEOUT_SECONDS).
    std::chrono::milliseconds header_timeout{std::chrono::seconds(10)};
    int listen_backlog = 1024;
    std::string node_name = "lake";
    // Maximum completed OCI blob size. The HTTP parser admits this much so registry upload
    // handlers can stream it; every other request body retains the 64 MiB adapter limit.
    std::uint64_t max_registry_blob_bytes = std::uint64_t{4} * 1024 * 1024 * 1024;
    // OCI uploads (staged or claimed by a completion) untouched for this long are removed by the
    // maintenance thread (lake#383 phase B), checked every ten minutes or every max age, whichever
    // is shorter.
    std::chrono::seconds registry_upload_max_age{std::chrono::hours(24)};
    // An image pull whose client takes no bytes for this long is ended, releasing its stream slot
    // (lake#383 phase B).
    std::chrono::milliseconds registry_stream_idle_timeout{std::chrono::seconds(60)};
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
    // Raw HTTP to each workspace Zetta's object routes, where a switched workspace's liquid store
    // lives (lake#380, docs/architecture/workspace-store.md). Null with `workspaces` set uses
    // Zetta's installed HTTP client; tests inject a fake object server.
    std::shared_ptr<control::ObjectTransport> object_transport;
    // How long a workspace store's bucket lease lasts without a renewal; lake renews it every
    // third of this (LAKE_WORKSPACE_STORE_LEASE_SECONDS).
    std::chrono::milliseconds workspace_store_lease_ttl{std::chrono::seconds(30)};
    // The bound on loaded workspace search indexes (LAKE_SEARCH_MEMORY_BYTES, lake#381).
    std::size_t search_memory_bytes = 256ULL * 1024ULL * 1024ULL;
    // LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false turns off every customer's opt-in (lake#234).
    bool allow_non_expiring_credentials = true;
    // Talks to agent services (lake#231); tests inject a double, main the HTTP caller.
    std::shared_ptr<control::AgentCaller> agent_caller;
    // The agent bus (docs/api/agent-bus.md): agent turns are published and followed on it.
    std::shared_ptr<control::AgentTaskBus> agent_bus;
    // Ship Apps wildcard, reserved names and per-space quotas. Zero quotas mean no limit.
    std::string apps_wildcard;
    // Runtime base image for source-backed Ship App builds (LAKE_APPS_RUNTIME_IMAGE).
    std::string apps_runtime_image;
    std::string apps_reserved_names;
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
    // The cluster's sealed-secret delivery key (LAKE_SEALING_PUBLIC_KEY, unpadded base64url X25519,
    // set by the operator from Lake status.sealing). When set, mint_sealed_secret seals only to it
    // (lake#625, docs/api/secrets.md#projections).
    std::string sealing_public_key;
    // Speech-to-text (docs/api/transcripts.md, lake#561): OpenAI's key (LAKE_OPENAI_API_KEY) and
    // the transcription model (LAKE_TRANSCRIBE_MODEL, empty: gpt-4o-transcribe). With no key and
    // no injected `transcriber`, transcribe and audio transcripts answer not_supported.
    std::string openai_api_key;
    std::string transcribe_model;
    // An injected transcriber (tests); empty: OpenAI over the server's outbound client when
    // `openai_api_key` is set.
    Transcriber transcriber;
    // The model gateway (docs/api/model-gateway.md, lake#248): agents' model calls go to
    // `model_provider_url` (LAKE_MODEL_PROVIDER_URL, empty: OpenAI's API) with `openai_api_key`,
    // Orama's own key. `model_provider` is an injected provider (tests); empty: that one over the
    // server's outbound client when `openai_api_key` is set, else model calls answer
    // not_supported.
    std::string model_provider_url;
    ModelProvider model_provider;
    // Configured external attestation verifiers. Lake owns their invocation but
    // callers retain their lifetime for the server lifetime.
    control::callers::TokenReviewer* token_reviewer = nullptr;
    control::callers::JwtVerifier* jwt_verifier = nullptr;
    control::callers::CloudIdentityVerifier* cloud_identity_verifier = nullptr;
    control::callers::FederationIssuerPolicy* federation_issuer_policy = nullptr;
    // Billing (lake#564, decision 56, docs/api/billing.md): on only where LAKE_BILLING=on (Orama
    // Cloud). Off, as on an enterprise install, the billing capabilities are not registered, so
    // REST, OpenAPI, MCP, the web app's billing pages and the billing Docs guide are absent; usage
    // tracking is unaffected. `payment_providers` says how invoices are paid (tests inject one);
    // null: Stripe with `stripe_keys` (LAKE_STRIPE_SECRET_KEY, LAKE_STRIPE_WEBHOOK_SECRET: lake's
    // own platform credentials, environment variables of the hosting platform, never a user's
    // secret store; decision 57, lake#628), else the manual provider with `billing_instructions`
    // (LAKE_BILLING_INSTRUCTIONS). `stripe_api_base` is Stripe's API origin (a fake's in tests).
    bool billing = false;
    std::shared_ptr<PaymentProviders> payment_providers;
    StripeKeys stripe_keys;
    std::string billing_instructions;
    std::string stripe_api_base = "https://api.stripe.com";
    // The operator-provided PersistentVolume mount. Customer stores live at
    // <root>/<customer id>/store and instance log objects at
    // <root>/<node name>/log. Empty disables data-plane storage.
    std::string store_root;
    // How long a starting server waits for another process to release the store root, and how
    // long each customer-store open waits for its liquid owner lock
    // (LAKE_STORE_OWNER_WAIT_SECONDS). It must outlast the predecessor's drain: the Kubernetes
    // preStop delay plus drain_timeout. See docs/deployment/store-volume.md (lake#479).
    std::chrono::milliseconds store_owner_wait{std::chrono::seconds(90)};
    // How long stop() waits for in-flight requests before it closes the listener and the stores
    // (LAKE_DRAIN_SECONDS), counted from begin_drain().
    std::chrono::milliseconds drain_timeout{std::chrono::seconds(20)};
    // Open every running switched workspace's store in the background from start, so the first
    // request after a rollout does not pay for the open (lake#536, LAKE_WORKSPACE_STORE_PREOPEN).
    bool preopen_workspace_stores = true;
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
    // Repository transfer is counted in memory and written to each workspace Zetta by the owner
    // loop at this interval, and once more on stop (docs/api/metering.md, lake#348). Zero writes
    // on every drain() tick. At most `metering_max_pending` counters are held.
    std::chrono::milliseconds metering_flush_interval{std::chrono::seconds(5)};
    std::size_t metering_max_pending = 4096;
    // Events about a workspace's data are queued in memory and appended to that workspace's audit
    // by the owner loop at this interval, at once when half of `audit_max_pending` is held, and
    // once more on stop (docs/api/audit.md#workspace-audit, decision 52). Zero writes on every
    // drain() tick.
    std::chrono::milliseconds audit_flush_interval{std::chrono::seconds(1)};
    std::size_t audit_max_pending = 65536;
    // Requests routed to a workspace are counted in memory and written to that workspace Zetta's
    // `lake_requests` series by the owner loop at this interval, at once when half of
    // `request_series_max_pending` is held, and once more on stop (docs/serving/telemetry.md,
    // lake#384). Zero writes on every drain() tick.
    std::chrono::milliseconds request_series_flush_interval{std::chrono::seconds(10)};
    std::size_t request_series_max_pending = 16384;
    // Usage over time (lake#564, docs/api/metering.md#usage-over-time): calls, speech-to-text
    // seconds and model tokens are held in memory and written to each workspace Zetta's
    // `lake_usage` series by the owner loop at this interval, at once when half of
    // `usage_max_pending` is held, and once more on stop. Every `usage_sample_interval` the store
    // root's owner samples each workspace's stored bytes and the instance and volume time of its
    // services, stores and apps (only the owner, so one lake counts them once). Zero runs on every
    // drain() tick.
    std::chrono::milliseconds usage_flush_interval{std::chrono::seconds(10)};
    std::size_t usage_max_pending = 8192;
    std::chrono::milliseconds usage_sample_interval{std::chrono::minutes(15)};
    // Dynamic-secret leases that are due (expired, or revoked and not yet settled) are settled in
    // their workspaces by the owner loop at this interval (docs/api/secrets.md, lake#362). It
    // reads the control store's due index, never every workspace. Zero settles on every drain().
    std::chrono::milliseconds secret_lease_expiry_interval{std::chrono::seconds(10)};
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

// lake#534: raises the soft open-file limit to the hard limit and returns it, with the
// connections that fit in it: `wanted`, at most the limit less a reserve of 512 descriptors (half
// the limit when that is smaller) for PostgreSQL, store files and outbound connections.
struct OpenFileBudget {
    std::uint64_t open_files = 0;
    std::size_t max_connections = 0;
};
OpenFileBudget fit_connections_to_open_files(std::size_t wanted);

class ApiServer {
public:
    explicit ApiServer(ServerOptions options);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Bind + start serving on the io thread. False on failure (see reason()). A server is started
    // once: after stop() it refuses, and a restart is a new ApiServer.
    bool start();
    // The drain's first step, called at SIGTERM before the owning loop winds down (lake#536):
    // stops taking new work and hands each workspace store to the successor as soon as its
    // admitted requests are done. stop() calls it when it has not run, then finishes the drain.
    void begin_drain();
    void stop();
    [[nodiscard]] bool running() const;
    // The actual bound port (differs from options when port was 0).
    [[nodiscard]] unsigned short port() const;
    [[nodiscard]] const std::string& reason() const;
    // Non-empty once the server cannot continue: the store root stayed held past
    // store_owner_wait, or could not be opened. The owning loop exits non-zero with it.
    [[nodiscard]] std::string fatal_error() const;
    // True once this process owns the store root and has opened it (or has no store root).
    [[nodiscard]] bool store_root_open() const;
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
