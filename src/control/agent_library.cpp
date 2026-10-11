#include "lake/control/agent_library.hpp"

#include <charconv>

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

std::expected<std::map<std::string, int>, std::string> parse_agent_leases(std::string_view text) {
    std::map<std::string, int> leases;
    if (text.ends_with(',')) {
        return std::unexpected("expected a library agent key=seconds pair");
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto pair = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        const auto equals = pair.find('=');
        const auto key = pair.substr(0, equals);
        if (equals == std::string_view::npos || !library_agent(key)) {
            return std::unexpected("expected a library agent key=seconds pair");
        }
        const auto value = pair.substr(equals + 1);
        int seconds = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (error != std::errc() || end != value.data() + value.size() || seconds < 60 ||
            seconds > 3600) {
            return std::unexpected("agent lease seconds must be an integer from 60 to 3600");
        }
        if (!leases.emplace(std::string(key), seconds).second) {
            return std::unexpected("duplicate agent lease configuration");
        }
    }
    return leases;
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
            status = "Not running on this Lake";
        }
        agents.push_back({{"key", std::string(agent.key)},
                          {"name", std::string(agent.name)},
                          {"purpose", std::string(agent.purpose)},
                          {"used_in", std::string(agent.used_in)},
                          {"runs_as", std::string(agent.runs_as)},
                          {"chat", agent.chat},
                          {"internal", agent.internal},
                          {"deployed", deployed},
                          {"status", status}});
    }
    return {{"agents", std::move(agents)}};
}

nlohmann::json talk_to_agents(const nlohmann::json& library) {
    nlohmann::json agents =
        nlohmann::json::array({{{"id", "agent-ask-orama"}, {"name", "Ask Orama"}}});
    if (!library.is_object()) {
        return agents;
    }
    for (const nlohmann::json& agent : library.value("agents", nlohmann::json::array())) {
        const std::string key = agent.value("key", std::string());
        // The row's own flag, and the library's, so an older answer cannot list internal tooling.
        const LibraryAgent* known = library_agent(key);
        if (key != "agent-ask-orama" && agent.value("chat", false) &&
            agent.value("deployed", false) && !agent.value("internal", false) &&
            !(known != nullptr && known->internal)) {
            agents.push_back({{"id", key}, {"name", agent.value("name", key)}});
        }
    }
    return agents;
}

std::string agent_display_name(std::string_view key) {
    const LibraryAgent* agent = library_agent(key);
    return std::string(agent != nullptr ? agent->name : key);
}

} // namespace lake::control
