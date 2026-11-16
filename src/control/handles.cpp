// SPDX-License-Identifier: MIT

#include "lake/control/handles.hpp"

#include <array>
#include <random>
#include <regex>
#include <string_view>

namespace lake::control {
namespace {

constexpr std::array<std::string_view, 48> adjectives = {
    "swift",  "quiet", "bright", "calm",   "bold",   "clear",  "deep",   "early",  "fair",   "gentle",
    "grand",  "happy", "keen",   "light",  "lucky",  "merry",  "noble",  "plain",  "proud",  "quick",
    "rapid",  "rare",  "ready",  "royal",  "sharp",  "silent", "simple", "smart",  "solid",  "steady",
    "still",  "sunny", "tidy",   "true",   "vivid",  "warm",   "wild",   "wise",   "young",  "zesty",
    "amber",  "azure", "coral",  "golden", "ivory",  "jade",   "olive",  "silver"};
constexpr std::array<std::string_view, 48> nouns = {
    "falcon", "otter",  "heron",  "lynx",   "maple",  "cedar",  "river",  "harbor", "meadow", "summit",
    "comet",  "ember",  "glacier", "island", "lantern", "marble", "nebula", "orchid", "pebble", "quartz",
    "raven",  "saffron", "tundra", "valley", "willow", "zephyr", "anchor", "beacon", "canyon", "delta",
    "estuary", "fjord", "geyser", "hollow", "inlet",  "juniper", "kestrel", "lagoon", "mesa",   "oasis",
    "prairie", "ridge", "sequoia", "thicket", "upland", "vista",  "wren",   "yarrow"};

const std::regex& handle_pattern() {
    static const std::regex pattern("^[a-z0-9]([a-z0-9-]{0,254}[a-z0-9])?$");
    return pattern;
}

}  // namespace

bool valid_handle(const std::string& name) {
    return !name.empty() && name.size() <= 256 && std::regex_match(name, handle_pattern());
}

std::string handle_rule(std::string_view field) {
    return std::string(field) +
           " is 1 to 256 lowercase letters, digits and hyphens, starting and ending with a letter "
           "or digit";
}

std::string generate_handle() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> adjective(0, adjectives.size() - 1);
    std::uniform_int_distribution<std::size_t> noun(0, nouns.size() - 1);
    std::uniform_int_distribution<int> number(1, 999);
    return std::string(adjectives[adjective(rng)]) + "-" + std::string(nouns[noun(rng)]) + "-" +
           std::to_string(number(rng));
}

}  // namespace lake::control
