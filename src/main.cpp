#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stop_token>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "lake/api/billing.hpp"
#include "lake/api/server.hpp"
#include "lake/api/tunnel_clients.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/methodologies.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/search/index.hpp"
#include "lake/thread.hpp"
#include "lake/version.hpp"

namespace {

std::atomic<bool> stop_requested{false};
void on_signal(int) {
    stop_requested.store(true);
}

std::string env_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::move(fallback);
}

std::uint64_t env_u64(const char* name, std::uint64_t fallback = 0) {
    const std::string value = env_or(name, std::to_string(fallback));
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        std::println(stderr, "lake: {} must be an unsigned integer", name);
        std::exit(2);
    }
    return parsed;
}

int usage() {
    std::println("lake {}", lake::version_string());
    std::println("  lake [--listen HOST:PORT] [--node NAME] [--control-db URI] "
                 "[--store-root PATH] [--external-origin ORIGIN]");
    std::println("       [--admin-token TOKEN] [--identity-seed BASE64] "
                 "[--instance-kek VALUE] [--tsa-url URL] [--tunnel-clients-dir PATH] "
                 "[--memory-store]");
    std::println("  lake version");
    std::println("");
    std::println("  --control-db   Postgres URI of the control plane (default "
                 "$LAKE_CONTROL_DB_URI)");
    std::println("  --store-root   customer and instance data root (default $LAKE_STORE_ROOT)");
    std::println("  --external-origin HTTP(S) origin for repository commands "
                 "(default $LAKE_EXTERNAL_ORIGIN, then https://$LAKE_PUBLIC_HOST)");
    std::println("  --admin-token  bearer for /v1 capabilities and MCP tools "
                 "(default $LAKE_ADMIN_TOKEN)");
    std::println("  LAKE_REPLICATION_TOKEN bearer presented to peer lake instances "
                 "(default $LAKE_ADMIN_TOKEN)");
    std::println("  LAKE_MAX_REGISTRY_BLOB_BYTES caps streamed OCI blobs (default 4294967296)");
    std::println("  --identity-seed base64 Ed25519 seed for lake's TAAP identity "
                 "(default $LAKE_IDENTITY_SEED)");
    std::println("  --instance-kek key-encryption key for a generated identity "
                 "(default $LAKE_INSTANCE_KEK)");
    std::println("  --clerk-publishable-key Clerk key; browser sign-in goes through Clerk "
                 "(default $LAKE_CLERK_PUBLISHABLE_KEY)");
    std::println("  LAKE_OPENAI_API_KEY OpenAI key for speech-to-text and the model gateway "
                 "(/openai/v1); LAKE_TRANSCRIBE_MODEL the speech model "
                 "(default gpt-4o-transcribe)");
    std::println("  LAKE_MODEL_PROVIDER_URL the model gateway's OpenAI-compatible provider "
                 "(default https://api.openai.com/v1)");
    std::println("  LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1 lets MCP connections reach loopback and "
                 "private addresses (local development only)");
    std::println("  LAKE_STORE_ALLOW_PRIVATE_ADDRESSES=1 lets a store's bucket endpoint be a "
                 "loopback or private address (local development only)");
    std::println(
        "  LAKE_BILLING=on turns billing on (Orama Cloud); unset or off, as on an enterprise "
        "install, billing is absent. LAKE_BILLING_INSTRUCTIONS tells accounts how to pay");
    std::println("  LAKE_STRIPE_SECRET_KEY, LAKE_STRIPE_WEBHOOK_SECRET Stripe's keys (lake's own "
                 "platform credentials); either unset, invoices are paid outside lake");
    std::println("  LAKE_LOG_MODE air-gapped, witnessed (default), or anchored when "
                 "LAKE_STORE_ROOT is set");
    std::println("  LAKE_LOG_WITNESSES JSON witness endpoint/public_key records; "
                 "LAKE_LOG_WITNESS_THRESHOLD selects their quorum");
    std::println("  LAKE_LOG_AGGREGATOR endpoint required by anchored mode");
    std::println("  --tsa-url      RFC 3161 endpoint (default $LAKE_TSA_URL)");
    std::println("  $LAKE_TSA_TRUST_CERT contains optional PEM trust roots for timestamp tokens");
    std::println(
        "  $LAKE_TSA_RENEWAL_SECONDS and $LAKE_TSA_RENEW_BEFORE_SECONDS set renewal policy");
    std::println("  LAKE_OPERATOR_ZETTA_URL and LAKE_OPERATOR_ZETTA_TOKEN enable platform sync");
    std::println("  LAKE_INTERNAL_URL is lake's in-cluster Service URL for agents and builds");
    std::println("  LAKE_LOGS_URL and optional LAKE_LOGS_TOKEN enable environment logs");
    std::println("  LAKE_PLATFORM_SYNC_SECONDS sets the platform poll interval (default 15)");
    std::println(
        "  LAKE_SERVICE_DIR enables managed services and workspaces (the operator's mount)");
    std::println(
        "  LAKE_SERVICES_PER_CUSTOMER caps live services per customer (default 0: no limit)");
    std::println("  LAKE_APPS_WILDCARD is the required parent domain for Ship app hosts");
    std::println("  LAKE_APPS_RUNTIME_IMAGE is the base image for source-backed Ship App builds");
    std::println("  LAKE_METHODOLOGY_RUNTIME_IMAGE is the oramasearch/methodology-runtime image "
                 "workspace methodologies are built from and run with");
    std::println("  LAKE_MCP_RUNNER_IMAGE is the oramasearch/lake-mcp-runner image a stdio MCP "
                 "connection's App runs");
    std::println(
        "  LAKE_APPS_RESERVED_NAMES adds comma-separated names that Ship apps cannot use");
    std::println("  LAKE_APPS_PER_CUSTOMER caps live apps per account (default 0: no limit)");
    std::println(
        "  LAKE_APP_REPLICAS_PER_CUSTOMER caps desired app replicas (default 0: no limit)");
    std::println(
        "  LAKE_AGENT_ENDPOINTS lists the Agent Library agents this lake runs (key=url,...)");
    std::println("  LAKE_BASE_AGENT_MODEL is the base-agent checkpoint inside a workspace");
    std::println("  LAKE_AGENT_MODEL is the frontier model Python agents run on (default gpt-4o)");
    std::println(
        "  LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false refuses never-expiring keys and tokens");
    std::println("  LAKE_STORE_OWNER_WAIT_SECONDS bounds the wait for a predecessor to release "
                 "the store root (default 90)");
    std::println("  LAKE_WORKSPACE_STORE_LEASE_SECONDS is a workspace store's bucket lease "
                 "(default 30; renewed every third)");
    std::println("  LAKE_SEARCH_MEMORY_BYTES bounds the workspace search indexes kept loaded "
                 "(default 268435456; idle ones are evicted and loaded again on use)");
    std::println("  LAKE_DRAIN_SECONDS bounds the wait for in-flight requests on SIGTERM "
                 "(default 20)");
    std::println("  LAKE_HTTP_WORKERS requests handled at once (default 32); "
                 "LAKE_HTTP_WORKER_WAIT_SECONDS a request's wait for one before 503 (default 10)");
    std::println("  LAKE_HTTP_MAX_CONNECTIONS open connections before new ones get 503 "
                 "(default 4096)");
    std::println("  LAKE_HTTP_IDLE_TIMEOUT_SECONDS closes idle keep-alive connections (default 75, "
                 "above the proxy's); LAKE_HTTP_HEADER_TIMEOUT_SECONDS bounds a request header "
                 "(default 10)");
    std::println("  LAKE_HTTP_IO_TIMEOUT_SECONDS bounds a body read or response write that makes "
                 "no progress (default 60)");
    std::println("  LAKE_HTTP_MIN_TRANSFER_RATE bytes per second a client must move while it holds "
                 "a worker, after 10 s (default 1024; 0 turns it off)");
    std::println("  LAKE_HTTP_MAX_LONG_REQUESTS long polls and pulls at once (default 512); "
                 "LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER per caller (default 64)");
    std::println("  LAKE_WORKSPACE_STORE_PREOPEN=false opens workspace stores on first use only "
                 "(default true: opened in the background from start)");
    std::println("  --tunnel-clients-dir the tunnel client downloads (default "
                 "$LAKE_TUNNEL_CLIENTS_DIR, then /usr/share/lake/tunnel-clients)");
    std::println(
        "  LAKE_REGION, LAKE_REGION_LABEL this Lake's region; LAKE_REGIONS the deployment's "
        "regions as JSON [{{name,label,host,latitude,longitude}}]");
    std::println("  LAKE_APEX_HOST the host whose every request gets the region picker");
    std::println("  --memory-store an in-memory control plane for local demos and tests");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    lake::api::ServerOptions opts;
    std::string control_db = env_or("LAKE_CONTROL_DB_URI", "");
    opts.admin_token = env_or("LAKE_ADMIN_TOKEN", "");
    opts.replication_token = env_or("LAKE_REPLICATION_TOKEN", "");
    opts.max_registry_blob_bytes =
        env_u64("LAKE_MAX_REGISTRY_BLOB_BYTES", opts.max_registry_blob_bytes);
    opts.identity_seed = env_or("LAKE_IDENTITY_SEED", "");
    opts.instance_kek = env_or("LAKE_INSTANCE_KEK", "");
    opts.clerk_publishable_key = env_or("LAKE_CLERK_PUBLISHABLE_KEY", "");
    // The cluster delivery key the operator publishes (a public key, not a secret): the only one
    // mint_sealed_secret seals a user's secret to (lake#625).
    opts.sealing_public_key = env_or("LAKE_SEALING_PUBLIC_KEY", "");
    // Speech-to-text (docs/api/transcripts.md): without a key, transcribe answers not_supported.
    opts.openai_api_key = env_or("LAKE_OPENAI_API_KEY", "");
    opts.model_provider_url = env_or("LAKE_MODEL_PROVIDER_URL", "");
    opts.transcribe_model = env_or("LAKE_TRANSCRIBE_MODEL", "");
    // MCP connections reach only public addresses unless this is set, for local development
    // against an MCP server on the same machine (docs/api/mcp-connections.md#the-client).
    opts.mcp_allow_private_addresses = env_or("LAKE_MCP_ALLOW_PRIVATE_ADDRESSES", "") == "1";
    opts.store_allow_private_addresses = env_or("LAKE_STORE_ALLOW_PRIVATE_ADDRESSES", "") == "1";
    // Billing (docs/api/billing.md): on only where LAKE_BILLING=on, as on Orama Cloud; an
    // enterprise install leaves it unset and has no billing capability or page.
    opts.billing = lake::api::billing_enabled(env_or("LAKE_BILLING", ""));
    opts.billing_instructions = env_or("LAKE_BILLING_INSTRUCTIONS", "");
    // Stripe (decision 57, lake#628): lake's own platform credentials, environment variables of
    // the hosting platform (Railway; spec.env with a secretKeyRef on the dev cluster), never a
    // user's secret store. Read once here and never logged; a new key takes a restart.
    opts.stripe_keys = {env_or("LAKE_STRIPE_SECRET_KEY", ""), env_or("LAKE_STRIPE_WEBHOOK_SECRET", "")};
    opts.store_root = env_or("LAKE_STORE_ROOT", "");
    opts.store_owner_wait = std::chrono::seconds{env_u64("LAKE_STORE_OWNER_WAIT_SECONDS", 90)};
    opts.drain_timeout = std::chrono::seconds{env_u64("LAKE_DRAIN_SECONDS", 20)};
    // lake#534: connections apart from request workers; zero is refused at start.
    opts.worker_coroutines = static_cast<int>(std::min<std::uint64_t>(
        env_u64("LAKE_HTTP_WORKERS", static_cast<std::uint64_t>(opts.worker_coroutines)), 65536));
    opts.max_connections = static_cast<std::size_t>(
        env_u64("LAKE_HTTP_MAX_CONNECTIONS", opts.max_connections));
    opts.idle_timeout = std::chrono::seconds{env_u64("LAKE_HTTP_IDLE_TIMEOUT_SECONDS", 75)};
    opts.io_timeout = std::chrono::seconds{env_u64("LAKE_HTTP_IO_TIMEOUT_SECONDS", 60)};
    opts.min_transfer_rate =
        static_cast<std::size_t>(env_u64("LAKE_HTTP_MIN_TRANSFER_RATE", opts.min_transfer_rate));
    opts.max_long_requests =
        static_cast<std::size_t>(env_u64("LAKE_HTTP_MAX_LONG_REQUESTS", opts.max_long_requests));
    opts.max_long_requests_per_caller = static_cast<std::size_t>(
        env_u64("LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER", opts.max_long_requests_per_caller));
    opts.header_timeout = std::chrono::seconds{env_u64("LAKE_HTTP_HEADER_TIMEOUT_SECONDS", 10)};
    opts.worker_wait = std::chrono::seconds{env_u64("LAKE_HTTP_WORKER_WAIT_SECONDS", 10)};
    // Every connection is a descriptor: raise the soft limit to the hard one and keep the
    // connection cap inside it, with room for PostgreSQL, stores and outbound calls.
    const std::size_t wanted_connections = opts.max_connections;
    const auto open_file_budget = lake::api::fit_connections_to_open_files(wanted_connections);
    opts.max_connections = open_file_budget.max_connections;
    if (opts.max_connections < wanted_connections) {
        std::println(stderr,
                     "lake: LAKE_HTTP_MAX_CONNECTIONS {} lowered to {} to fit the open-file limit {}",
                     wanted_connections, opts.max_connections, open_file_budget.open_files);
    }
    opts.preopen_workspace_stores = env_or("LAKE_WORKSPACE_STORE_PREOPEN", "true") != "false";
    opts.workspace_store_lease_ttl =
        std::chrono::seconds{std::max<std::uint64_t>(3, env_u64("LAKE_WORKSPACE_STORE_LEASE_SECONDS", 30))};
    opts.search_memory_bytes = static_cast<std::size_t>(
        std::max<std::uint64_t>(1024ULL * 1024ULL, env_u64("LAKE_SEARCH_MEMORY_BYTES", 256ULL * 1024ULL * 1024ULL)));
    opts.public_host = env_or("LAKE_PUBLIC_HOST", "");
    opts.external_origin = env_or("LAKE_EXTERNAL_ORIGIN", "");
    opts.apps_wildcard = env_or("LAKE_APPS_WILDCARD", "");
    opts.apps_runtime_image = env_or("LAKE_APPS_RUNTIME_IMAGE", "");
    opts.methodology_runtime_image = env_or("LAKE_METHODOLOGY_RUNTIME_IMAGE", "");
    opts.mcp_runner_image = env_or("LAKE_MCP_RUNNER_IMAGE", "");
    // The tunnel client downloads (lake#745): where the image puts them unless told otherwise.
    opts.tunnel_clients_dir = env_or("LAKE_TUNNEL_CLIENTS_DIR",
                                     std::string(lake::api::default_tunnel_clients_dir));
    // Regions (lake#757, docs/api/regions.md): the operator sets these only where a region is
    // configured; unset, Lake has no region and serves no picker. Invalid values stop startup.
    {
        auto regions = lake::api::regions_from_environment(env_or("LAKE_REGION", ""),
                                                           env_or("LAKE_REGION_LABEL", ""),
                                                           env_or("LAKE_REGIONS", ""),
                                                           env_or("LAKE_APEX_HOST", ""));
        if (!regions) {
            std::println(stderr, "lake: {}", regions.error());
            return 2;
        }
        opts.regions = std::move(*regions);
    }
    opts.apps_reserved_names = env_or("LAKE_APPS_RESERVED_NAMES", "");
    opts.apps_per_customer = env_u64("LAKE_APPS_PER_CUSTOMER");
    opts.app_replicas_per_customer = env_u64("LAKE_APP_REPLICAS_PER_CUSTOMER");
    opts.internal_url = env_or("LAKE_INTERNAL_URL", "");
    const std::string operator_zetta_url = env_or("LAKE_OPERATOR_ZETTA_URL", "");
    const std::string operator_zetta_token = env_or("LAKE_OPERATOR_ZETTA_TOKEN", "");
    const std::string logs_url = env_or("LAKE_LOGS_URL", "");
    const std::string logs_token = env_or("LAKE_LOGS_TOKEN", "");
    const std::uint64_t platform_sync_seconds = env_u64("LAKE_PLATFORM_SYNC_SECONDS", 15);
    opts.tsa_endpoint = env_or("LAKE_TSA_URL", "");
    opts.tsa_trust_pem = env_or("LAKE_TSA_TRUST_CERT", "");
    opts.timestamp_policy.renewal_interval =
        std::chrono::seconds{env_u64("LAKE_TSA_RENEWAL_SECONDS", 31'536'000)};
    opts.timestamp_policy.renew_before_expiry =
        std::chrono::seconds{env_u64("LAKE_TSA_RENEW_BEFORE_SECONDS", 2'592'000)};
    opts.metering.currency = env_or("LAKE_PRICE_CURRENCY", "USD");
    opts.metering.storage_quota_bytes = env_u64("LAKE_STORAGE_QUOTA_BYTES");
    opts.metering.storage_gib_hour_micros = env_u64("LAKE_PRICE_STORAGE_GIB_HOUR_MICROS");
    opts.metering.transfer_in_gib_micros = env_u64("LAKE_PRICE_TRANSFER_IN_GIB_MICROS");
    opts.metering.transfer_out_gib_micros = env_u64("LAKE_PRICE_TRANSFER_OUT_GIB_MICROS");
    opts.metering.compute_second_micros = env_u64("LAKE_PRICE_COMPUTE_SECOND_MICROS");
    if (!opts.store_root.empty()) {
        const auto log_configuration = lake::transparency::log_configuration_from_environment(
            env_or("LAKE_LOG_MODE", "witnessed"),
            env_or("LAKE_LOG_WITNESSES", "[]"),
            env_u64("LAKE_LOG_WITNESS_THRESHOLD"),
            env_or("LAKE_LOG_AGGREGATOR", ""));
        if (!log_configuration) {
            std::println(stderr,
                         "lake: invalid transparency log configuration: {}",
                         log_configuration.error().message);
            return 2;
        }
        opts.log_configuration = *log_configuration;
    }
    bool memory_store = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::println(stderr, "lake: {} needs a value", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "version" || arg == "--version") {
            std::println("lake {}", lake::version_string());
            return 0;
        }
        if (arg == "--help" || arg == "-h")
            return usage() == 2 ? 0 : 0;
        if (arg == "--listen") {
            const std::string spec = value("--listen");
            const auto colon = spec.rfind(':');
            if (colon == std::string::npos) {
                std::println(stderr, "lake: --listen wants HOST:PORT");
                return 2;
            }
            opts.listen_address = spec.substr(0, colon);
            opts.port = static_cast<unsigned short>(std::atoi(spec.c_str() + colon + 1));
            continue;
        }
        if (arg == "--node") {
            opts.node_name = value("--node");
            continue;
        }
        if (arg == "--external-origin") {
            opts.external_origin = value("--external-origin");
            continue;
        }
        if (arg == "--control-db") {
            control_db = value("--control-db");
            continue;
        }
        if (arg == "--store-root") {
            opts.store_root = value("--store-root");
            continue;
        }
        if (arg == "--admin-token") {
            opts.admin_token = value("--admin-token");
            continue;
        }
        if (arg == "--identity-seed") {
            opts.identity_seed = value("--identity-seed");
            continue;
        }
        if (arg == "--clerk-publishable-key") {
            opts.clerk_publishable_key = value("--clerk-publishable-key");
            continue;
        }
        if (arg == "--instance-kek") {
            opts.instance_kek = value("--instance-kek");
            continue;
        }
        if (arg == "--tunnel-clients-dir") {
            opts.tunnel_clients_dir = value("--tunnel-clients-dir");
            continue;
        }
        if (arg == "--tsa-url") {
            opts.tsa_endpoint = value("--tsa-url");
            continue;
        }
        if (arg == "--memory-store") {
            memory_store = true;
            continue;
        }
        std::println(stderr, "lake: unknown argument {}", arg);
        return usage();
    }

    if (opts.replication_token.empty()) {
        opts.replication_token = opts.admin_token;
    }
    if (operator_zetta_url.empty() != operator_zetta_token.empty()) {
        std::println(stderr,
                     "lake: LAKE_OPERATOR_ZETTA_URL and LAKE_OPERATOR_ZETTA_TOKEN must be set "
                     "together");
        return 2;
    }
    if (!operator_zetta_url.empty() && opts.public_host.empty()) {
        std::println(
            stderr, "lake: LAKE_PUBLIC_HOST is required when the operator Zetta client is enabled");
        return 2;
    }
    if (platform_sync_seconds == 0) {
        std::println(stderr, "lake: LAKE_PLATFORM_SYNC_SECONDS must be greater than zero");
        return 2;
    }

    std::string store_kind = "none";
    if (memory_store) {
        opts.store = std::make_shared<lake::control::InMemoryStore>();
        store_kind = "memory";
    } else if (!control_db.empty()) {
        opts.store = lake::control::make_postgres_store(control_db, opts.instance_kek);
        store_kind = "postgres";
    }
    if (!opts.store_root.empty()) {
        opts.search = std::make_shared<lake::search::Service>(opts.store_root);
    }
    const bool platform_transport_required = !operator_zetta_url.empty() || !logs_url.empty();
    if (platform_transport_required) {
        lake::control::install_zetta_control_transport();
    }
    if (!operator_zetta_url.empty() || !logs_url.empty()) {
        std::shared_ptr<lake::control::ZettaControl> control;
        if (!operator_zetta_url.empty()) {
            control = std::make_shared<lake::control::ZettaControlClient>(operator_zetta_url,
                                                                          operator_zetta_token);
        }
        std::shared_ptr<lake::control::LokiSource> logs;
        if (!logs_url.empty()) {
            logs = std::make_shared<lake::control::LokiHttpSource>(logs_url, logs_token);
        }
        opts.platform = std::make_shared<lake::control::PlatformService>(
            opts.store,
            std::move(control),
            std::move(logs),
            lake::control::PlatformConfig{opts.public_host,
                                          std::chrono::seconds{platform_sync_seconds},
                                          opts.identity_seed,
                                          opts.instance_kek});
    }

    // Managed services and the workspace (docs/control-plane/services.md). The operator mounts
    // service connections at LAKE_SERVICE_DIR when the Lake enables services; without it the
    // Services, Design, Train and Ship pages say the feature is unavailable.
    lake::control::WorkspaceConfig workspace_config{env_or("LAKE_SERVICE_DIR", "")};
    workspace_config.agent_endpoints =
        lake::control::parse_agent_endpoints(env_or("LAKE_AGENT_ENDPOINTS", ""));
    auto leases = lake::control::parse_agent_leases(env_or("LAKE_AGENT_LEASE_SECONDS", ""));
    if (!leases) {
        std::println(stderr, "lake: LAKE_AGENT_LEASE_SECONDS: {}", leases.error());
        return 2;
    }
    workspace_config.agent_lease_seconds = std::move(*leases);
    if (const std::string model = env_or("LAKE_BASE_AGENT_MODEL", ""); !model.empty()) {
        workspace_config.base_agent_model = model;
    }
    if (const std::string model = env_or("LAKE_AGENT_MODEL", ""); !model.empty()) {
        workspace_config.frontier_model = model;
    }
    workspace_config.internal_url = opts.internal_url;
    workspace_config.agent_service_token = env_or("LAKE_AGENT_SERVICE_TOKEN", "");
    workspace_config.agent_bus_tokens =
        lake::control::read_agent_bus_tokens(env_or("LAKE_AGENT_BUS_DIR", ""));
    if (const std::string limit = env_or("LAKE_SERVICES_PER_CUSTOMER", ""); !limit.empty()) {
        std::size_t parsed = 0;
        const auto [end, error] =
            std::from_chars(limit.data(), limit.data() + limit.size(), parsed);
        if (error != std::errc() || end != limit.data() + limit.size()) {
            std::println(stderr, "lake: LAKE_SERVICES_PER_CUSTOMER must be a whole number");
            return 2;
        }
        workspace_config.max_per_customer = parsed;
    }
    opts.allow_non_expiring_credentials =
        env_or("LAKE_ALLOW_NON_EXPIRING_CREDENTIALS", "true") != "false";
    const bool workspaces_configured = workspace_config.configured() && opts.store != nullptr;
    // The agent bus (docs/api/agent-bus.md): lake's own Zetta's pub/sub, or memory for a demo lake.
    std::vector<std::string> bus_agents;
    for (const auto& [agent, endpoint] : workspace_config.agent_endpoints) {
        bus_agents.push_back(agent);
    }
    const std::string bus_url = env_or("LAKE_DATA_PLANE_URL", "");
    const std::string bus_token = env_or("LAKE_DATA_PLANE_TOKEN", "");
    const bool app_bus_configured = opts.store != nullptr && !opts.apps_wildcard.empty();
    if ((workspaces_configured && !bus_agents.empty()) || app_bus_configured) {
        std::shared_ptr<lake::control::AgentBus> bus;
        if (!bus_url.empty() && !bus_token.empty()) {
            bus = std::make_shared<lake::control::ZettaAgentBus>(bus_url, bus_token);
        } else if (memory_store) {
            bus = std::make_shared<lake::control::MemoryAgentBus>();
        }
        if (bus) {
            opts.agent_bus = std::make_shared<lake::control::AgentTaskBus>(
                opts.store, std::move(bus), std::move(bus_agents));
        }
    }
    if (workspaces_configured) {
        if (!platform_transport_required) {
            lake::control::install_zetta_control_transport();
        }
        opts.workspaces = std::make_shared<lake::control::WorkspaceService>(
            std::move(workspace_config),
            opts.store,
            std::make_shared<lake::control::ZettaWorkspaceCaller>());
        opts.agent_caller = std::make_shared<lake::control::HttpAgentCaller>();
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    lake::api::ApiServer server(opts);
    if (!server.start()) {
        std::println(stderr, "lake: cannot start: {}", server.reason());
        if (platform_transport_required || workspaces_configured) {
            lake::control::shutdown_zetta_control_transport();
        }
        return 1;
    }
    std::println(
        "{}",
        nlohmann::json{
            {"event", "startup"},
            {"version", lake::version_string()},
            {"revision", lake::build_revision_string()},
            {"listen", "http://" + opts.listen_address + ":" + std::to_string(server.port())},
            {"node", opts.node_name},
            {"store", store_kind},
            {"store_root_configured", opts.store_root.empty() ? "no" : "yes"},
            {"store_owner_wait_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.store_owner_wait).count()},
            {"drain_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.drain_timeout).count()},
            {"http_workers", opts.worker_coroutines},
            {"http_max_connections", opts.max_connections},
            {"http_idle_timeout_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.idle_timeout).count()},
            {"http_header_timeout_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.header_timeout).count()},
            {"http_worker_wait_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.worker_wait).count()},
            {"http_io_timeout_seconds",
             std::chrono::duration_cast<std::chrono::seconds>(opts.io_timeout).count()},
            {"http_min_transfer_rate", opts.min_transfer_rate},
            {"http_max_long_requests", opts.max_long_requests},
            {"http_max_long_requests_per_caller", opts.max_long_requests_per_caller},
            {"open_files_limit", open_file_budget.open_files},
            {"log_mode", lake::transparency::to_string(opts.log_configuration.mode)},
            {"tsa_configured", opts.tsa_endpoint.empty() ? "no" : "yes"},
            {"admin_token_set", opts.admin_token.empty() ? "no" : "yes"},
            {"replication_token_set", opts.replication_token.empty() ? "no" : "yes"},
            {"operator_zetta_configured", operator_zetta_url.empty() ? "no" : "yes"},
            {"logs_configured", logs_url.empty() ? "no" : "yes"},
            {"region",
             opts.regions.name.empty() ? nlohmann::json(nullptr)
                                       : nlohmann::json(opts.regions.name)},
            {"apex_host",
             opts.regions.apex_host.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(opts.regions.apex_host)},
            {"agent_bus", opts.agent_bus ? (bus_url.empty() ? "memory" : "zetta") : "no"},
            {"api", "/v1/openapi.json"},
            {"mcp", "POST /mcp"}}
            .dump());
    // Recover lost dispatch and expire overdue tasks. Workspace event publication applies
    // progress directly; this loop does not read an event topic on the bus.
    std::stop_source bus_stop;
    lake::Thread bus_loop;
    if (opts.agent_bus) {
        bus_loop = lake::Thread([bus = opts.agent_bus,
                                 workspaces = opts.workspaces,
                                 stop = bus_stop.get_token()] {
            // A pause the stop request ends, so SIGTERM never waits out a sleep (lake#536).
            const auto pause = [&stop](std::chrono::seconds duration) {
                std::mutex mutex;
                std::condition_variable_any woken;
                std::unique_lock lock(mutex);
                woken.wait_for(lock, stop, duration, [] { return false; });
            };
            bool started = false;
            while (!stop.stop_requested()) {
                if (!started) {
                    auto created = liquid::runtime::block_on(bus->start());
                    started = created && created->has_value();
                    if (!started) {
                        std::println(
                            stderr,
                            "{}",
                            nlohmann::json{
                                {"event", "agent_bus_unavailable"},
                                {"error", created ? created->error().message : "transport"}}
                                .dump());
                        pause(std::chrono::seconds(5));
                        continue;
                    }
                }
                auto pumped =
                    liquid::runtime::block_on(bus->pump(std::chrono::seconds(5), workspaces.get()));
                if (!pumped || !pumped->has_value()) {
                    pause(std::chrono::seconds(2));
                } else if (**pumped == 0) {
                    pause(std::chrono::seconds(5));
                }
            }
        });
    }
    // The owner loop below can be inside a long drain() tick when SIGTERM arrives; this watcher
    // begins the drain at once, so the workspace stores are handed off without waiting for it
    // (lake#536).
    std::stop_source watcher_stop;
    lake::Thread signal_watcher([&server, stop = watcher_stop.get_token()] {
        while (!stop.stop_requested() && !stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stop_requested.load()) {
            server.begin_drain();
        }
    });
    auto next_app_cleanup = std::chrono::steady_clock::now();
    int exit_code = 0;
    while (!stop_requested.load()) {
        // The store root stayed held past LAKE_STORE_OWNER_WAIT_SECONDS, or did not open: exit
        // so the kubelet restarts the container and the wait starts over (lake#479).
        if (const std::string fatal = server.fatal_error(); !fatal.empty()) {
            std::println(stderr, "lake: cannot start: {}", fatal);
            exit_code = 1;
            break;
        }
        if (std::chrono::steady_clock::now() >= next_app_cleanup) {
            if (app_bus_configured) {
                const auto purged = opts.store->purge_deleted_web_apps();
                if (!purged) {
                    std::println(stderr, "lake: could not remove acknowledged app deletions");
                }
            }
            const auto swept = opts.store->sweep_web_app_authorization_codes();
            if (!swept) {
                std::println(stderr, "lake: could not sweep expired app authorization codes");
            }
            // A launch the operator never reported on is withdrawn with a reason (lake#442), so
            // it neither waits forever nor starts long after the person asked for it.
            const auto expired = opts.store->expire_unstarted_agent_launches(
                lake::control::agent_launch_start_timeout_seconds,
                std::string(lake::control::agent_launch_start_timeout_reason));
            if (!expired) {
                std::println(stderr, "lake: could not expire unstarted agent launches");
            } else if (*expired > 0) {
                std::println(stderr,
                             "{}",
                             nlohmann::json{{"event", "agent_launches_expired"},
                                            {"count", *expired}}
                                 .dump());
            }
            // A methodology run the operator never reported on, its image build included, is
            // withdrawn the same way (lake#655).
            const auto expired_runs = opts.store->expire_unstarted_methodology_runs(
                lake::control::methodology_run_start_timeout_seconds,
                std::string(lake::control::methodology_run_start_timeout_reason));
            if (!expired_runs) {
                std::println(stderr, "lake: could not expire unstarted methodology runs");
            } else if (*expired_runs > 0) {
                std::println(
                    stderr,
                    "{}",
                    nlohmann::json{{"event", "methodology_runs_expired"}, {"count", *expired_runs}}
                        .dump());
            }
            next_app_cleanup = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    // SIGTERM: stop taking new work and hand the workspace stores off at once (lake#536), before
    // anything else winds down; then drain in-flight requests, close the stores and release the
    // store root (ApiServer::stop, lake#479).
    watcher_stop.request_stop();
    signal_watcher.join();
    server.begin_drain();
    bus_stop.request_stop();
    server.stop();
    // The bus loop needs nothing the server holds; joined after it, so a pump in progress does
    // not hold up the store root's release.
    if (bus_loop.joinable()) {
        bus_loop.join();
    }
    if (platform_transport_required || workspaces_configured) {
        lake::control::shutdown_zetta_control_transport();
    }
    return exit_code;
}
