#include "lake/control/agent_library.hpp"

namespace lake::control {

const LibraryAgent* library_agent(std::string_view key) {
    for (const LibraryAgent& agent : agent_library) {
        if (agent.key == key) {
            return &agent;
        }
    }
    return nullptr;
}

std::map<std::string, std::string> parse_agent_endpoints(std::string_view text) {
    std::map<std::string, std::string> endpoints;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view pair = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0 || equals + 1 == pair.size()) {
            continue;
        }
        const std::string_view key = pair.substr(0, equals);
        if (library_agent(key) == nullptr) {
            continue;
        }
        endpoints[std::string(key)] = std::string(pair.substr(equals + 1));
    }
    return endpoints;
}

nlohmann::json agent_library_json(const std::map<std::string, std::string>& endpoints) {
    nlohmann::json agents = nlohmann::json::array();
    for (const LibraryAgent& agent : agent_library) {
        const bool deployed = endpoints.contains(std::string(agent.key));
        std::string status;
        if (agent.runs_as == "offline") {
            status = "Offline tool";
        } else if (deployed) {
            status = "Available";
        } else {
            status = "Not running on this lake";
        }
        agents.push_back({{"key", std::string(agent.key)},
                          {"name", std::string(agent.name)},
                          {"purpose", std::string(agent.purpose)},
                          {"used_in", std::string(agent.used_in)},
                          {"runs_as", std::string(agent.runs_as)},
                          {"chat", agent.chat},
                          {"deployed", deployed},
                          {"status", status}});
    }
    return {{"agents", std::move(agents)}};
}

} // namespace lake::control
