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
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
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

std::uint64_t env_u64(const char* name) {
    const std::string value = env_or(name, "0");
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
                 "[--store-root PATH]");
    std::println("       [--admin-token TOKEN] [--identity-seed BASE64] "
                 "[--instance-kek VALUE] [--memory-store]");
    std::println("  lake version");
    std::println("");
    std::println("  --control-db   Postgres URI of the control plane (default "
                 "$LAKE_CONTROL_DB_URI)");
    std::println("  --store-root   customer and instance data root (default $LAKE_STORE_ROOT)");
    std::println("  --admin-token  bearer for /v1 capabilities and MCP tools "
                 "(default $LAKE_ADMIN_TOKEN)");
    std::println("  --identity-seed base64 Ed25519 seed for lake's TAAP identity "
                 "(default $LAKE_IDENTITY_SEED)");
    std::println("  --instance-kek key-encryption key for a generated identity "
                 "(default $LAKE_INSTANCE_KEK)");
    std::println("  LAKE_LOG_MODE air-gapped, witnessed (default), or anchored when "
                 "LAKE_STORE_ROOT is set");
    std::println("  LAKE_LOG_WITNESSES JSON witness endpoint/public_key records; "
                 "LAKE_LOG_WITNESS_THRESHOLD selects their quorum");
    std::println("  LAKE_LOG_AGGREGATOR endpoint required by anchored mode");
    std::println("  --memory-store an in-memory control plane for local demos and tests");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    lake::api::ServerOptions opts;
    std::string control_db = env_or("LAKE_CONTROL_DB_URI", "");
    opts.admin_token = env_or("LAKE_ADMIN_TOKEN", "");
    opts.identity_seed = env_or("LAKE_IDENTITY_SEED", "");
    opts.instance_kek = env_or("LAKE_INSTANCE_KEK", "");
    opts.store_root = env_or("LAKE_STORE_ROOT", "");
    opts.public_host = env_or("LAKE_PUBLIC_HOST", "");
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
        if (arg == "--instance-kek") {
            opts.instance_kek = value("--instance-kek");
            continue;
        }
        if (arg == "--memory-store") {
            memory_store = true;
            continue;
        }
        std::println(stderr, "lake: unknown argument {}", arg);
        return usage();
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

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    lake::api::ApiServer server(opts);
    if (!server.start()) {
        std::println(stderr, "lake: cannot start: {}", server.reason());
        return 1;
    }
    std::println(
        "{}",
        nlohmann::json{
            {"event", "startup"},
            {"version", lake::version_string()},
            {"listen", "http://" + opts.listen_address + ":" + std::to_string(server.port())},
            {"node", opts.node_name},
            {"store", store_kind},
            {"store_root_configured", opts.store_root.empty() ? "no" : "yes"},
            {"log_mode", lake::transparency::to_string(opts.log_configuration.mode)},
            {"admin_token_set", opts.admin_token.empty() ? "no" : "yes"},
            {"api", "/v1/openapi.json"},
            {"mcp", "POST /mcp"}}
            .dump());
    while (!stop_requested.load()) {
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    server.stop();
    return 0;
}
