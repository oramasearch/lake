#include <chrono>
#include <csignal>
#include <cstdlib>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "lake/api/server.hpp"
#include "lake/version.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int usage() {
    std::println("lake {} — the hosted liquid service. liquid is the tool; lake is where it collects.",
                 lake::version_string());
    std::println("");
    std::println("usage: lake [--listen HOST:PORT] [--node NAME]");
    std::println("       lake version");
    std::println("");
    std::println("  --listen HOST:PORT  HTTP bind (default 127.0.0.1:4900)");
    std::println("  --node NAME         this node's display name (default: lake)");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    lake::api::ServerOptions opts;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "version" || a == "--version") { std::println("lake {}", lake::version_string()); return 0; }
        if (a == "--help" || a == "-h") return usage();
        if (a == "--listen" && i + 1 < args.size()) {
            const std::string_view v = args[++i];
            const auto colon = v.rfind(':');
            if (colon == std::string_view::npos) return usage();
            opts.listen_address = std::string(v.substr(0, colon));
            opts.port = static_cast<unsigned short>(std::atoi(std::string(v.substr(colon + 1)).c_str()));
            continue;
        }
        if (a == "--node" && i + 1 < args.size()) { opts.node_name = args[++i]; continue; }
        return usage();
    }

    lake::api::ApiServer server(opts);
    if (!server.start()) {
        std::println(stderr, "lake: cannot start: {}", server.reason());
        return 1;
    }
    std::println("lake {} — http://{}:{}/  (node {})", lake::version_string(), opts.listen_address,
                 server.port(), opts.node_name);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        server.wait_for_work(std::chrono::milliseconds(100));
        server.drain();
    }
    server.stop();
    return 0;
}
