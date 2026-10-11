// The metering contract (lake#658, docs/api/metering.md#what-is-metered-where): everything that
// consumes a resource for a customer is metered into its workspace's usage, and every dimension
// that is metered is on the price list. These tests fail when a capability, a workload kind or a
// store type lands without a meter, or a billed dimension without a published rate.

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/store.hpp"

namespace {

using nlohmann::json;

std::string source(const std::string& path) {
    std::ifstream in(std::string(LAKE_TEST_SOURCE_DIR) + "/" + path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::set<std::string> billed_dimensions() {
    std::set<std::string> names;
    for (const auto& dimension : lake::api::usage_dimensions()) {
        if (dimension.priced) {
            names.insert(std::string(dimension.name));
        }
    }
    return names;
}

// A capability whose name says it starts, sizes, runs, serves, moves or stores something.
bool looks_like_it_consumes(const std::string& name) {
    static const std::regex resource(
        "^(create|launch|start|deploy|scale|run|train|upload|append|import|load|begin|finish|"
        "transcribe|predict|score|search|download|read|resolve|push|pull|replicate|copy|"
        "set_workspace_storage|set_service|set_app|stage|commit|record|publish)(_.*)?$");
    return std::regex_match(name, resource);
}

} // namespace

TEST(UsageContract, EveryBilledDimensionHasARateOnThePublishedList) {
    const std::string billing = source("docs/api/billing.md");
    const auto section = billing.find("### Publish the price list");
    ASSERT_NE(section, std::string::npos);
    const auto start = billing.find("{\"currency\"", section);
    ASSERT_NE(start, std::string::npos);
    const auto end = billing.find('\n', start);
    const json list = json::parse(billing.substr(start, end - start), nullptr, false);
    ASSERT_FALSE(list.is_discarded()) << "the published payload is JSON";
    std::set<std::string> rated;
    for (const json& price : list.at("prices")) {
        rated.insert(price.at("dimension").get<std::string>());
    }
    EXPECT_EQ(rated, billed_dimensions())
        << "every billed dimension, and only those, has a rate in docs/api/billing.md";

    // The web app's list editor offers the same rates.
    const std::string web = source("frontend/src/pages/billing/billing.ts");
    for (const std::string& name : billed_dimensions()) {
        EXPECT_NE(web.find("dimension: \"" + name + "\""), std::string::npos)
            << name << " has no LIST_RATES entry in billing.ts";
    }
}

TEST(UsageContract, EveryWorkloadKindTheSchemaAllowsIsMetered) {
    const std::string schema = source("db/schema.sql");
    const auto check = schema.rfind("workload_resources_object_kind_check");
    ASSERT_NE(check, std::string::npos);
    const auto open = schema.find("IN (", check);
    const auto close = schema.find(')', open);
    std::set<std::string> allowed;
    const std::regex quoted("'([a-z_]+)'");
    const std::string list = schema.substr(open, close - open);
    for (std::sregex_iterator it(list.begin(), list.end(), quoted), done; it != done; ++it) {
        allowed.insert((*it)[1].str());
    }
    std::set<std::string> metered;
    for (const auto kind : lake::api::metered_workload_kinds()) {
        metered.insert(std::string(kind));
    }
    EXPECT_EQ(allowed, metered) << "a workload kind the operator can report is charged";
}

TEST(UsageContract, EveryOfferedStoreTypeIsMetered) {
    for (const auto& provider : lake::control::store_providers) {
        if (!provider.available) {
            continue;
        }
        bool metered = false;
        for (const auto type : lake::api::metered_store_types()) {
            metered = metered || type == provider.type;
        }
        EXPECT_TRUE(metered) << provider.type << "/" << provider.provider
                             << " is offered but its bytes are not metered";
    }
}

TEST(UsageContract, EveryCapabilityThatConsumesAResourceHasAMeter) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    std::set<std::string> registered;
    for (const auto& capability : registry.list()) {
        registered.insert(capability->name);
    }
    const std::set<std::string> billed = billed_dimensions();
    std::set<std::string> classified;
    for (const auto& meter : lake::api::capability_meters()) {
        const std::string name(meter.capability);
        EXPECT_TRUE(registered.contains(name)) << name << " is metered but not registered";
        EXPECT_TRUE(classified.insert(name).second) << name << " is listed twice";
        EXPECT_FALSE(meter.dimensions.empty()) << name;
        for (const auto dimension : meter.dimensions) {
            EXPECT_TRUE(billed.contains(std::string(dimension)))
                << name << " meters " << dimension << ", which is not billed";
        }
        EXPECT_FALSE(meter.how.empty()) << name;
    }
    for (const auto& [capability, why] : lake::api::calls_only_capabilities()) {
        const std::string name(capability);
        EXPECT_TRUE(registered.contains(name)) << name << " is classified but not registered";
        EXPECT_TRUE(classified.insert(name).second) << name << " is listed twice";
        EXPECT_FALSE(why.empty()) << name;
    }
    std::string missing;
    for (const std::string& name : registered) {
        if (looks_like_it_consumes(name) && !classified.contains(name)) {
            missing += " " + name;
        }
    }
    EXPECT_TRUE(missing.empty())
        << "classify each in capability_meters() or calls_only_capabilities() "
           "(src/api/usage_recorder.cpp):"
        << missing;
}
