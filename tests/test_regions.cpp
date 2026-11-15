// SPDX-License-Identifier: MIT

// Regions (lake#757, docs/api/regions.md): the four environment variables, server_info's region
// fields, the closest region to a visitor's country, the apex host's preference cookie, the
// picker that keeps the path and query, and the apex's agent documents that name every region
// (lake#1110).
#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "lake/api/agent_docs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/regions.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using nlohmann::json;
using namespace lake::api;

// The value the operator sets for production: Europe first, then the United States.
constexpr std::string_view production_regions =
    R"([{"name":"swedencentral","label":"Europe","host":"eu.lake.example.com","latitude":"60.67488","longitude":"17.14127"},)"
    R"({"name":"eastus","label":"United States","host":"us.lake.example.com","latitude":"37.3719","longitude":"-79.8164"}])";

Regions production(std::string_view own = "swedencentral", std::string_view apex = "lake.example.com") {
    auto regions = regions_from_environment(own, "", production_regions, apex);
    EXPECT_TRUE(regions) << regions.error();
    return regions ? *regions : Regions{};
}

std::string error_of(std::string_view region,
                     std::string_view label,
                     std::string_view list,
                     std::string_view apex) {
    const auto regions = regions_from_environment(region, label, list, apex);
    EXPECT_FALSE(regions) << "accepted " << list;
    return regions ? std::string() : regions.error();
}

} // namespace

TEST(RegionsEnvironment, NoneSetMeansNoRegionAndNoPicker) {
    const auto regions = regions_from_environment("", "", "", "");
    ASSERT_TRUE(regions) << regions.error();
    EXPECT_FALSE(regions->configured());
    EXPECT_TRUE(regions->list.empty());
    EXPECT_TRUE(regions->apex_host.empty());
    EXPECT_EQ(region_json(&*regions), json(nullptr));
    EXPECT_EQ(regions_json(&*regions), json::array());
    EXPECT_EQ(region_json(nullptr), json(nullptr));
    EXPECT_EQ(regions_json(nullptr), json::array());
}

TEST(RegionsEnvironment, ReadsTheOperatorsValues) {
    const auto regions =
        regions_from_environment("swedencentral", "Europe", production_regions, "Lake.Live");
    ASSERT_TRUE(regions) << regions.error();
    EXPECT_TRUE(regions->configured());
    EXPECT_EQ(regions->name, "swedencentral");
    EXPECT_EQ(regions->label, "Europe");
    EXPECT_EQ(regions->apex_host, "lake.example.com");
    ASSERT_EQ(regions->list.size(), 2U);
    EXPECT_EQ(regions->list[0].host, "eu.lake.example.com");
    EXPECT_DOUBLE_EQ(regions->list[0].latitude, 60.67488);
    EXPECT_DOUBLE_EQ(regions->list[1].longitude, -79.8164);
    EXPECT_EQ(regions->list[1].label, "United States");
    EXPECT_EQ(region_json(&*regions), json({{"name", "swedencentral"}, {"label", "Europe"}}));
    EXPECT_EQ(
        regions_json(&*regions),
        json::array({{{"name", "swedencentral"}, {"label", "Europe"}, {"host", "eu.lake.example.com"}},
                     {{"name", "eastus"}, {"label", "United States"}, {"host", "us.lake.example.com"}}}));
}

TEST(RegionsEnvironment, TheLabelComesFromTheListWhenUnsetAndNumbersAreAccepted) {
    const auto regions = regions_from_environment(
        "eastus",
        "",
        R"([{"name":"eastus","label":"United States","host":"us.lake.example.com","latitude":37.3719,"longitude":-79.8164}])",
        "");
    ASSERT_TRUE(regions) << regions.error();
    EXPECT_EQ(regions->label, "United States");
    EXPECT_DOUBLE_EQ(regions->list[0].latitude, 37.3719);
    // A region alone, with no list, is reported with its name as the label when none is given.
    const auto alone = regions_from_environment("westus2", "", "", "");
    ASSERT_TRUE(alone) << alone.error();
    EXPECT_EQ(region_json(&*alone), json({{"name", "westus2"}, {"label", "westus2"}}));
}

TEST(RegionsEnvironment, InvalidValuesAreStartupErrorsNamingTheVariable) {
    EXPECT_EQ(error_of("", "", "[{", ""), "LAKE_REGIONS is not valid JSON");
    EXPECT_EQ(error_of("", "", R"({"name":"eu"})", ""),
              "LAKE_REGIONS must be a JSON array of regions");
    EXPECT_EQ(error_of("", "", R"(["eu"])", ""), "LAKE_REGIONS[0] must be an object");
    EXPECT_EQ(
        error_of("", "", R"([{"name":"eu","label":"Europe","latitude":"1","longitude":"2"}])", ""),
        "LAKE_REGIONS[0].host must be a non-empty string");
    EXPECT_EQ(
        error_of(
            "",
            "",
            R"([{"name":"eu","label":"Europe","host":"eu.lake.example.com/x","latitude":"1","longitude":"2"}])",
            ""),
        "LAKE_REGIONS[0].host must be a host name such as eu.lake.example.com, not \"eu.lake.example.com/x\"");
    EXPECT_EQ(
        error_of(
            "",
            "",
            R"([{"name":"eu","label":"Europe","host":"eu.lake.example.com","latitude":"north","longitude":"2"}])",
            ""),
        "LAKE_REGIONS[0].latitude must be a number, not \"north\"");
    EXPECT_EQ(
        error_of(
            "",
            "",
            R"([{"name":"eu","label":"Europe","host":"eu.lake.example.com","latitude":"1","longitude":"200"}])",
            ""),
        "LAKE_REGIONS[0].longitude must be between -180 and 180");
    EXPECT_EQ(
        error_of(
            "", "", R"([{"name":"eu","label":"Europe","host":"eu.lake.example.com","latitude":"1"}])", ""),
        "LAKE_REGIONS[0].longitude must be a number");
    EXPECT_EQ(
        error_of(
            "",
            "",
            R"([{"name":"eu","label":"A","host":"a.lake.example.com","latitude":"1","longitude":"2"},)"
            R"({"name":"eu","label":"B","host":"b.lake.example.com","latitude":"1","longitude":"2"}])",
            ""),
        "LAKE_REGIONS names the region \"eu\" twice");
    EXPECT_EQ(
        error_of(
            "",
            "",
            R"([{"name":"eu; Domain=x","label":"A","host":"a.lake.example.com","latitude":"1","longitude":"2"}])",
            ""),
        "LAKE_REGIONS[0].name must be letters, digits, hyphens and underscores, not \"eu; "
        "Domain=x\"");
    EXPECT_EQ(error_of("westus2", "", production_regions, ""),
              "LAKE_REGION \"westus2\" is not one of the regions in LAKE_REGIONS");
    EXPECT_EQ(error_of("", "", "", "lake.example.com"),
              "LAKE_APEX_HOST needs LAKE_REGIONS: the picker lists them");
    EXPECT_EQ(error_of("", "", production_regions, "lake.example.com/<x>"),
              "LAKE_APEX_HOST must be a host name such as lake.example.com");
}

TEST(RegionsClosest, CountryCentroidsAreISOAlpha2) {
    const auto sweden = country_centroid("SE");
    ASSERT_TRUE(sweden);
    EXPECT_NEAR(sweden->latitude, 60.13, 0.01);
    EXPECT_TRUE(country_centroid("us"));
    // Cloudflare's unknown and Tor values, and anything that is not a two-letter code.
    for (const std::string_view code : {"XX", "T1", "", "U", "USA", "XK", "AN"}) {
        EXPECT_FALSE(country_centroid(code)) << code;
    }
}

TEST(RegionsClosest, GreatCircleDistance) {
    // London to New York is about 5,570 km.
    EXPECT_NEAR(great_circle_km({51.5074, -0.1278}, {40.7128, -74.0060}), 5570, 15);
    EXPECT_NEAR(great_circle_km({10, 20}, {10, 20}), 0, 1e-9);
    // Antipodes: half the circumference.
    EXPECT_NEAR(great_circle_km({0, 0}, {0, 180}), 20015, 5);
}

TEST(RegionsClosest, PicksTheNearestRegionToTheCountry) {
    const Regions regions = production();
    const auto closest = [&](std::string_view country) {
        return closest_region(regions.list, country)->name;
    };
    for (const std::string_view country :
         {"SE", "DE", "FR", "GB", "ES", "IT", "PL", "NG", "IN", "JP"}) {
        EXPECT_EQ(closest(country), "swedencentral") << country;
    }
    for (const std::string_view country : {"US", "CA", "MX", "BR", "AR", "CO", "us"}) {
        EXPECT_EQ(closest(country), "eastus") << country;
    }
    // No country, or one not in the table: the first region.
    for (const std::string_view country : {"", "XX", "T1"}) {
        EXPECT_EQ(closest(country), "swedencentral") << country;
    }
    // The order of the list decides only the fallback, never the nearest.
    std::vector<Region> reversed{regions.list[1], regions.list[0]};
    EXPECT_EQ(closest_region(reversed, "DE")->name, "swedencentral");
    EXPECT_EQ(closest_region(reversed, "")->name, "eastus");
    EXPECT_EQ(closest_region({}, "DE"), nullptr);
}

TEST(RegionsPreference, TheCookieWinsOverTheCountry) {
    const Regions regions = production();
    EXPECT_EQ(preselected_region(regions, "eastus", "DE")->name, "eastus");
    EXPECT_EQ(preselected_region(regions, "swedencentral", "US")->name, "swedencentral");
    // A cookie naming no region of this deployment is ignored.
    EXPECT_EQ(preselected_region(regions, "westus2", "US")->name, "eastus");
    EXPECT_EQ(preselected_region(regions, "", "US")->name, "eastus");
    EXPECT_EQ(preselected_region(regions, "", "")->name, "swedencentral");
    EXPECT_EQ(region_preference_cookie("eastus", true),
              "lake_region=eastus; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax; Secure");
    EXPECT_EQ(region_preference_cookie("eastus", false),
              "lake_region=eastus; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax");
}

TEST(RegionsPreference, ContinuingKeepsThePathAndQuery) {
    const Regions regions = production();
    const Region& us = regions.list[1];
    EXPECT_EQ(region_continue_url(us, "/"), "https://us.lake.example.com/");
    EXPECT_EQ(region_continue_url(us, "/w/abc/design/projects?tab=runs&x=%20y"),
              "https://us.lake.example.com/w/abc/design/projects?tab=runs&x=%20y");
    // Anything that is not an origin-form path goes to the region's front page.
    for (const std::string_view target :
         {"", "https://evil.example/", "x", "/a b", "/a\r\nSet-Cookie: x"}) {
        EXPECT_EQ(region_continue_url(us, target), "https://us.lake.example.com/") << target;
    }
}

TEST(RegionsPicker, ListsEveryRegionPreselectsOneAndEscapes) {
    const Regions regions = production();
    const std::string html = web_ui_region_picker(regions, regions.list[1], "/w/a?x=1&y=\"2\"", "");
    EXPECT_NE(html.find("<h1>Choose a Region</h1>"), std::string::npos);
    EXPECT_NE(html.find("value=\"swedencentral\">"), std::string::npos);
    EXPECT_NE(html.find("value=\"eastus\" checked>"), std::string::npos);
    EXPECT_NE(html.find("eu.lake.example.com"), std::string::npos);
    EXPECT_NE(html.find("action=\"/w/a?x=1&amp;y=&quot;2&quot;\""), std::string::npos);
    EXPECT_EQ(html.find("class=\"error\""), std::string::npos);
    EXPECT_NE(web_ui_region_picker(regions, regions.list[0], "/", "Choose one.")
                  .find("<p class=\"error\">Choose one.</p>"),
              std::string::npos);
}

TEST(RegionsServerInfo, ReportsTheRegionThroughTheRegistry) {
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    lake::control::InMemoryStore store;
    ApiContext ctx;
    ctx.store = &store;
    // No region: null and [] beside every field older clients read.
    ApiResult result = registry.call("server_info", json::object(), ctx);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value["region"], json(nullptr));
    EXPECT_EQ(result.value["regions"], json::array());
    EXPECT_EQ(result.value["name"], "lake");
    const Regions regions = production("eastus");
    ctx.regions = &regions;
    result = registry.call("server_info", json::object(), ctx);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value["region"], json({{"name", "eastus"}, {"label", "United States"}}));
    ASSERT_EQ(result.value["regions"].size(), 2U);
    EXPECT_EQ(result.value["regions"][1]["host"], "us.lake.example.com");
    // The schema REST, OpenAPI and MCP publish names both fields.
    const auto capability = registry.find("server_info");
    ASSERT_NE(capability, nullptr);
    EXPECT_TRUE(capability->output_schema["properties"].contains("region"));
    EXPECT_TRUE(capability->output_schema["properties"].contains("regions"));
}

namespace {

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers; // names lowercased
    std::string body;
};

Response send(unsigned short port,
              std::string_view method,
              std::string_view target,
              std::string_view host,
              const std::map<std::string, std::string>& headers = {},
              std::string_view body = {}) {
    Response parsed;
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return parsed;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return parsed;
    }
    std::string wire = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\n";
    wire += "Host: " + std::string(host) + "\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers) {
        wire += name + ": " + value + "\r\n";
    }
    if (!body.empty() || method == "POST") {
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n" + std::string(body);
    for (std::size_t sent = 0; sent < wire.size();) {
        const ssize_t count = ::send(socket_fd, wire.data() + sent, wire.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return parsed;
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.size() < 12) {
        return parsed;
    }
    parsed.status = std::stoi(response.substr(9, 3));
    std::size_t line = response.find("\r\n") + 2;
    while (line < header_end) {
        const std::size_t end = response.find("\r\n", line);
        const std::size_t colon = response.find(':', line);
        if (colon < end) {
            std::string name = response.substr(line, colon - line);
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            parsed.headers[name] = response.substr(colon + 2, end - colon - 2);
        }
        line = end + 2;
    }
    parsed.body = response.substr(header_end + 4);
    return parsed;
}

bool checked(const Response& page, std::string_view region) {
    return page.body.find("value=\"" + std::string(region) + "\" checked>") != std::string::npos;
}

ServerOptions options_with(Regions regions) {
    ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.regions = std::move(regions);
    return options;
}

} // namespace

TEST(RegionsApex, EveryPathOnTheApexIsThePicker) {
    ApiServer server(options_with(production()));
    ASSERT_TRUE(server.start()) << server.reason();
    const unsigned short port = server.port();

    // The country preselects; any path, the API's included, is the picker; the port is ignored.
    for (const std::string_view target :
         {"/", "/w/abc/design?tab=runs", "/v1/server_info", "/mcp"}) {
        Response page = send(port, "GET", target, "lake.example.com", {{"CF-IPCountry", "US"}});
        ASSERT_EQ(page.status, 200) << target << " " << page.body;
        EXPECT_TRUE(page.headers["content-type"].starts_with("text/html")) << target;
        EXPECT_EQ(page.headers["cache-control"], "no-store");
        EXPECT_TRUE(checked(page, "eastus")) << target;
    }
    EXPECT_TRUE(checked(send(port, "GET", "/", "LAKE.live:443", {{"CF-IPCountry", "DE"}}),
                        "swedencentral"));
    // No country: the first region.
    EXPECT_TRUE(checked(send(port, "GET", "/", "lake.example.com"), "swedencentral"));
    // The region chosen before wins over the country.
    EXPECT_TRUE(
        checked(send(port,
                     "GET",
                     "/",
                     "lake.example.com",
                     {{"CF-IPCountry", "DE"}, {"Cookie", "theme=dark; lake_region=eastus"}}),
                "eastus"));
    // The form posts back to the same path and query.
    Response page = send(port, "GET", "/w/abc/design?tab=runs&x=1", "lake.example.com");
    EXPECT_NE(page.body.find("action=\"/w/abc/design?tab=runs&amp;x=1\""), std::string::npos);

    // The stylesheet and fonts the page links are served on the apex too.
    const auto stylesheet = page.body.find("href=\"/style-");
    ASSERT_NE(stylesheet, std::string::npos);
    const std::string css =
        page.body.substr(stylesheet + 6, page.body.find('"', stylesheet + 6) - stylesheet - 6);
    Response style = send(port, "GET", css, "lake.example.com");
    EXPECT_EQ(style.status, 200) << css;
    EXPECT_TRUE(style.headers["content-type"].starts_with("text/css"))
        << style.headers["content-type"];
    EXPECT_EQ(send(port, "GET", "/healthz", "lake.example.com").status, 200);

    // Continuing remembers the choice and keeps the path and query.
    Response chosen = send(
        port,
        "POST",
        "/w/abc/design?tab=runs&x=1",
        "lake.example.com",
        {{"Content-Type", "application/x-www-form-urlencoded"}, {"X-Forwarded-Proto", "https"}},
        "region=eastus");
    EXPECT_EQ(chosen.status, 302) << chosen.body;
    EXPECT_EQ(chosen.headers["location"], "https://us.lake.example.com/w/abc/design?tab=runs&x=1");
    EXPECT_EQ(chosen.headers["set-cookie"],
              "lake_region=eastus; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax; Secure");
    // A region that is not offered is refused with the page again, and nothing is remembered.
    Response refused = send(port,
                            "POST",
                            "/",
                            "lake.example.com",
                            {{"Content-Type", "application/x-www-form-urlencoded"}},
                            "region=westus2");
    EXPECT_EQ(refused.status, 400);
    EXPECT_EQ(refused.headers.count("set-cookie"), 0U);
    EXPECT_NE(refused.body.find("Choose one of the regions below."), std::string::npos);

    // The region's own host is the app and the API as before, with the region in server_info.
    Response info = send(port, "GET", "/v1/server_info", "eu.lake.example.com");
    ASSERT_EQ(info.status, 200) << info.body;
    const json value = json::parse(info.body)["value"];
    EXPECT_EQ(value["region"], json({{"name", "swedencentral"}, {"label", "Europe"}}));
    EXPECT_EQ(value["regions"].size(), 2U);
    server.stop();
}

TEST(RegionsApex, NoPickerWhenUnset) {
    // Nonprod: nothing set. lake.example.com is just another host, and server_info reports no region.
    ApiServer server(options_with(Regions{}));
    ASSERT_TRUE(server.start()) << server.reason();
    Response info = send(server.port(), "GET", "/v1/server_info", "lake.example.com");
    ASSERT_EQ(info.status, 200) << info.body;
    const json value = json::parse(info.body)["value"];
    EXPECT_EQ(value["region"], json(nullptr));
    EXPECT_EQ(value["regions"], json::array());
    Response page = send(server.port(), "GET", "/", "lake.example.com", {{"CF-IPCountry", "US"}});
    EXPECT_EQ(page.body.find("Choose a Region"), std::string::npos);
    server.stop();

    // Regions without an apex host: a region Lake that is not the picker.
    ApiServer region_only(options_with(production("eastus", "")));
    ASSERT_TRUE(region_only.start()) << region_only.reason();
    EXPECT_EQ(send(region_only.port(), "GET", "/", "lake.example.com").body.find("Choose a Region"),
              std::string::npos);
    Response region_info = send(region_only.port(), "GET", "/v1/server_info", "lake.example.com");
    ASSERT_EQ(region_info.status, 200) << region_info.body;
    EXPECT_EQ(json::parse(region_info.body)["value"]["region"]["name"], "eastus");
    region_only.stop();
}

namespace {

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

// The apex's agent documents (lake#1110) are built from LAKE_REGIONS: a third region appears
// with every link, and nothing names a host the list does not.
TEST(RegionsApexAgentDocuments, NameEveryRegionOfTheList) {
    const auto regions = regions_from_environment(
        "",
        "",
        R"([{"name":"swedencentral","label":"Europe","host":"eu.lake.example","latitude":60,"longitude":17},)"
        R"({"name":"eastus","label":"United States","host":"us.lake.example","latitude":37,"longitude":-79},)"
        R"({"name":"canadacentral","label":"Canada","host":"ca.lake.example","latitude":43,"longitude":-79}])",
        "lake.example");
    ASSERT_TRUE(regions) << regions.error();
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    const std::string origin = "https://lake.example";
    const std::string llms = agent_docs::apex_llms_txt(registry, *regions, origin);
    const std::string full = agent_docs::apex_llms_full_txt(registry, *regions, origin);
    const std::string skill = agent_docs::apex_skill_md(registry, *regions, origin);
    for (const std::string* document : {&llms, &full, &skill}) {
        EXPECT_TRUE(has(*document, "https://lake.example is not a Lake"));
        EXPECT_TRUE(has(*document,
                        "Europe (eu.lake.example), United States (us.lake.example) and Canada "
                        "(ca.lake.example)"));
        for (const std::string_view host :
             {"eu.lake.example", "us.lake.example", "ca.lake.example"}) {
            const std::string region = "https://" + std::string(host);
            for (const std::string_view path : {"/mcp`",
                                                "/llms.txt",
                                                "/llms-full.txt",
                                                "/skills/lake/SKILL.md",
                                                "/developer/mcp"}) {
                EXPECT_TRUE(has(*document, region + std::string(path))) << host << path;
            }
        }
        EXPECT_TRUE(has(*document, "### Canada (`canadacentral`): https://ca.lake.example"));
        EXPECT_TRUE(has(*document, "`lake-eu`, `lake-us` and `lake-ca`"));
        EXPECT_TRUE(
            has(*document, "claude mcp add --transport http lake-ca https://ca.lake.example/mcp"));
        // The apex is not an MCP server, an API or a token page itself.
        EXPECT_FALSE(has(*document, "https://lake.example/mcp"));
        EXPECT_FALSE(has(*document, "https://lake.example/v1/"));
        EXPECT_FALSE(has(*document, "https://lake.example/developer/mcp"));
        // No region host is written in the code: production's hosts appear only from the list.
        EXPECT_FALSE(has(*document, "eu.lake.example.com"));
        EXPECT_FALSE(has(*document, "us.lake.example.com"));
    }
    // Each still teaches the tools as a region's does.
    EXPECT_TRUE(has(llms, "`create_pipeline` (write)"));
    EXPECT_TRUE(has(llms, "## Worked flows"));
    EXPECT_TRUE(has(full, "Input schema:"));
    EXPECT_TRUE(skill.starts_with("---\nname: lake\n"));
    EXPECT_TRUE(has(skill, "## Which tools for which task"));
    EXPECT_TRUE(has(skill, "https://lake.example/skills/lake/SKILL.md"));
}

// On the apex host, /llms.txt, /llms-full.txt and the skill are the region-aware text, not the
// picker; every other path is still the picker, and a region's host keeps its own text.
TEST(RegionsApexAgentDocuments, TheApexServesThemAndARegionKeepsItsOwn) {
    ApiServer server(options_with(production()));
    ASSERT_TRUE(server.start()) << server.reason();
    const unsigned short port = server.port();

    Response llms = send(port, "GET", "/llms.txt", "lake.example.com", {{"CF-IPCountry", "US"}});
    ASSERT_EQ(llms.status, 200) << llms.body.substr(0, 300);
    EXPECT_EQ(llms.headers["content-type"], "text/plain; charset=utf-8");
    EXPECT_EQ(llms.headers["cache-control"], "public, max-age=300");
    EXPECT_TRUE(llms.body.starts_with("# Lake\n")) << llms.body.substr(0, 300);
    EXPECT_FALSE(has(llms.body, "<html"));
    EXPECT_TRUE(has(llms.body, "https://lake.example.com is not a Lake"));
    for (const std::string_view region : {"https://eu.lake.example.com", "https://us.lake.example.com"}) {
        for (const std::string_view path :
             {"/mcp`", "/llms.txt", "/llms-full.txt", "/skills/lake/SKILL.md"}) {
            EXPECT_TRUE(has(llms.body, std::string(region) + std::string(path))) << region << path;
        }
    }
    EXPECT_TRUE(has(llms.body, "(https://lake.example.com/skills/lake/SKILL.md)"));
    // The port and the case of the host do not matter, as for the picker.
    EXPECT_TRUE(has(send(port, "GET", "/llms.txt", "LAKE.live:443").body,
                    "https://lake.example.com is not a Lake"));

    Response full = send(port, "GET", "/llms-full.txt", "lake.example.com");
    ASSERT_EQ(full.status, 200);
    EXPECT_EQ(full.headers["content-type"], "text/plain; charset=utf-8");
    EXPECT_TRUE(full.body.starts_with("# Lake — every tool\n"));
    EXPECT_TRUE(has(full.body, "https://us.lake.example.com/mcp"));
    EXPECT_TRUE(has(full.body, "Input schema:"));

    for (const std::string_view path : {"/skills/lake/SKILL.md", "/developer/mcp/skill"}) {
        Response skill = send(port, "GET", path, "lake.example.com");
        ASSERT_EQ(skill.status, 200) << path;
        EXPECT_EQ(skill.headers["content-type"], "text/markdown; charset=utf-8") << path;
        EXPECT_EQ(skill.headers["content-disposition"], "attachment; filename=SKILL.md");
        EXPECT_TRUE(skill.body.starts_with("---\nname: lake\n")) << path;
        EXPECT_TRUE(has(skill.body, "https://eu.lake.example.com/mcp"));
        EXPECT_TRUE(has(skill.body, "https://us.lake.example.com/mcp"));
    }

    // Everything else on the apex is still the picker, including a POST to an agent path.
    for (const std::string_view target : {"/", "/mcp", "/skills/", "/llms.txt.bak"}) {
        Response page = send(port, "GET", target, "lake.example.com");
        EXPECT_TRUE(page.headers["content-type"].starts_with("text/html")) << target;
    }
    Response posted = send(port,
                           "POST",
                           "/llms.txt",
                           "lake.example.com",
                           {{"Content-Type", "application/x-www-form-urlencoded"}},
                           "region=eastus");
    EXPECT_EQ(posted.status, 302);
    EXPECT_EQ(posted.headers["location"], "https://us.lake.example.com/llms.txt");

    // A region's own host answers its own text, naming only itself.
    Response region = send(port, "GET", "/llms.txt", "eu.lake.example.com");
    ASSERT_EQ(region.status, 200);
    EXPECT_EQ(region.headers["content-type"], "text/plain; charset=utf-8");
    EXPECT_TRUE(has(region.body, "Lake's MCP server is `http://eu.lake.example.com/mcp`"));
    EXPECT_FALSE(has(region.body, "Choose a region first"));
    EXPECT_FALSE(has(region.body, "us.lake.example.com"));
    Response region_skill = send(port, "GET", "/skills/lake/SKILL.md", "eu.lake.example.com");
    EXPECT_EQ(region_skill.headers["content-type"], "text/markdown; charset=utf-8");
    EXPECT_FALSE(has(region_skill.body, "Choose a region first"));
    server.stop();
}

// Nonprod: no regions, so no apex; every host gets the Lake's own text, as before.
TEST(RegionsApexAgentDocuments, NoRegionsMeansTheUsualText) {
    ApiServer server(options_with(Regions{}));
    ASSERT_TRUE(server.start()) << server.reason();
    for (const std::string_view host : {"lake.example.com", "lake-dev.example.net"}) {
        Response llms = send(server.port(), "GET", "/llms.txt", host);
        ASSERT_EQ(llms.status, 200) << host;
        EXPECT_EQ(llms.headers["content-type"], "text/plain; charset=utf-8");
        EXPECT_TRUE(has(llms.body, "Lake's MCP server is `http://" + std::string(host) + "/mcp`"))
            << host;
        EXPECT_FALSE(has(llms.body, "Choose a region first")) << host;
        Response skill = send(server.port(), "GET", "/skills/lake/SKILL.md", host);
        EXPECT_FALSE(has(skill.body, "Choose a region first")) << host;
    }
    server.stop();
}

TEST(RegionsApex, ACouponCodeGoesToTheChosenRegion) {
    // An ad links to lake.example.com/?code=LAKE50 (lake#1156): the picker says the code goes along, its
    // form posts back to the same query, and continuing keeps it on the region's host, where the
    // web app keeps it through sign-up.
    ApiServer server(options_with(production()));
    ASSERT_TRUE(server.start()) << server.reason();
    const unsigned short port = server.port();
    Response page = send(port, "GET", "/?code=LAKE50", "lake.example.com", {{"CF-IPCountry", "US"}});
    ASSERT_EQ(page.status, 200) << page.body;
    EXPECT_NE(page.body.find("action=\"/?code=LAKE50\""), std::string::npos);
    EXPECT_NE(page.body.find("Your code <strong>LAKE50</strong> goes with you"), std::string::npos);
    Response chosen = send(port,
                           "POST",
                           "/?code=LAKE50",
                           "lake.example.com",
                           {{"Content-Type", "application/x-www-form-urlencoded"}},
                           "region=eastus");
    EXPECT_EQ(chosen.status, 302) << chosen.body;
    EXPECT_EQ(chosen.headers["location"], "https://us.lake.example.com/?code=LAKE50");
    // Among other parameters too.
    Response mixed = send(port,
                          "POST",
                          "/?utm_source=google&code=LAKE50",
                          "lake.example.com",
                          {{"Content-Type", "application/x-www-form-urlencoded"}},
                          "region=swedencentral");
    EXPECT_EQ(mixed.headers["location"], "https://eu.lake.example.com/?utm_source=google&code=LAKE50");
    server.stop();

    // A code that is not letters, digits and hyphens is not shown (the query is still carried).
    const Regions regions = production();
    const std::string odd =
        web_ui_region_picker(regions, regions.list[0], "/?code=%3Cscript%3E", "");
    EXPECT_EQ(odd.find("Your code"), std::string::npos);
    EXPECT_EQ(web_ui_region_picker(regions, regions.list[0], "/", "").find("Your code"),
              std::string::npos);
}
