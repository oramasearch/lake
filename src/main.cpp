#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "lake/api/server.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/search/index.hpp"
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
                 "[--instance-kek VALUE] [--tsa-url URL] [--memory-store]");
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
    std::println("  --identity-seed base64 Ed25519 seed for lake's TAAP identity "
                 "(default $LAKE_IDENTITY_SEED)");
    std::println("  --instance-kek key-encryption key for a generated identity "
                 "(default $LAKE_INSTANCE_KEK)");
    std::println("  --clerk-publishable-key Clerk key; browser sign-in goes through Clerk "
                 "(default $LAKE_CLERK_PUBLISHABLE_KEY)");
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
    std::println("  LAKE_LOGS_URL and optional LAKE_LOGS_TOKEN enable environment logs");
    std::println("  LAKE_PLATFORM_SYNC_SECONDS sets the platform poll interval (default 15)");
    std::println(
        "  LAKE_SERVICE_DIR enables managed services and workspaces (the operator's mount)");
    std::println(
        "  LAKE_SERVICES_PER_CUSTOMER caps live services per customer (default 0: no limit)");
    std::println(
        "  LAKE_AGENT_ENDPOINTS lists the Agent Library agents this lake runs (key=url,...)");
    std::println("  LAKE_BASE_AGENT_MODEL is the base-agent checkpoint inside a workspace");
    std::println("  LAKE_AGENT_MODEL is the frontier model Python agents run on (default gpt-4o)");
    std::println(
        "  LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false refuses never-expiring keys and tokens");
    std::println("  --memory-store an in-memory control plane for local demos and tests");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    lake::api::ServerOptions opts;
    std::string control_db = env_or("LAKE_CONTROL_DB_URI", "");
    opts.admin_token = env_or("LAKE_ADMIN_TOKEN", "");
    opts.replication_token = env_or("LAKE_REPLICATION_TOKEN", "");
    opts.identity_seed = env_or("LAKE_IDENTITY_SEED", "");
    opts.instance_kek = env_or("LAKE_INSTANCE_KEK", "");
    opts.clerk_publishable_key = env_or("LAKE_CLERK_PUBLISHABLE_KEY", "");
    opts.store_root = env_or("LAKE_STORE_ROOT", "");
    opts.public_host = env_or("LAKE_PUBLIC_HOST", "");
    opts.external_origin = env_or("LAKE_EXTERNAL_ORIGIN", "");
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
    if (!operator_zetta_url.empty()) {
        auto control = std::make_shared<lake::control::ZettaControlClient>(operator_zetta_url,
                                                                           operator_zetta_token);
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
    if (const std::string model = env_or("LAKE_BASE_AGENT_MODEL", ""); !model.empty()) {
        workspace_config.base_agent_model = model;
    }
    if (const std::string model = env_or("LAKE_AGENT_MODEL", ""); !model.empty()) {
        workspace_config.frontier_model = model;
    }
    workspace_config.internal_url = env_or("LAKE_INTERNAL_URL", "");
    workspace_config.agent_service_token = env_or("LAKE_AGENT_SERVICE_TOKEN", "");
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
            {"log_mode", lake::transparency::to_string(opts.log_configuration.mode)},
            {"tsa_configured", opts.tsa_endpoint.empty() ? "no" : "yes"},
            {"admin_token_set", opts.admin_token.empty() ? "no" : "yes"},
            {"replication_token_set", opts.replication_token.empty() ? "no" : "yes"},
            {"operator_zetta_configured", operator_zetta_url.empty() ? "no" : "yes"},
            {"logs_configured", logs_url.empty() ? "no" : "yes"},
            {"api", "/v1/openapi.json"},
            {"mcp", "POST /mcp"}}
            .dump());
    while (!stop_requested.load()) {
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    server.stop();
    if (platform_transport_required || workspaces_configured) {
        lake::control::shutdown_zetta_control_transport();
    }
    return 0;
}
