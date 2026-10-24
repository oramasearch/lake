// The web app's serving rules (docs/ui/web-app.md): which requests are answered with index.html,
// and how the embedded files are cached.
#include "lake/api/web_app.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

using lake::api::web_app_asset;
using lake::api::web_app_cache_control;
using lake::api::web_app_index;
using lake::api::web_app_serves;

namespace {
constexpr std::string_view browser = "text/html,application/xhtml+xml,*/*;q=0.8";
}

TEST(WebApp, BrowserPagesAreTheApp) {
    for (const std::string_view path :
         {"/", "/repositories", "/search", "/drive", "/w/3f9a1c2e-0000-0000-0000-000000000000/design/projects",
          "/design/projects", "/share/abc", "/models/acme/bert", "/sign-in", "/no/such/page"}) {
        EXPECT_TRUE(web_app_serves("GET", path, browser, false)) << path;
    }
    EXPECT_TRUE(web_app_serves("HEAD", "/repositories", browser, false));
}

TEST(WebApp, ServerRoutesAreNeverTheApp) {
    for (const std::string_view path :
         {"/v1/viewer", "/v1/openapi.json", "/mcp", "/ui/index-abc.js", "/fonts/ibm-plex-sans-400.woff2",
          "/apps/1/sign-in", "/apps/authorize", "/file/raw", "/file/download", "/drive/download",
          "/exports/download", "/share/abc/download", "/llms.txt", "/llms-full.txt",
          "/developer/mcp/skill", "/skills/lake/SKILL.md", "/style.css", "/healthz", "/metrics",
          "/models/acme/bert/resolve/main/config.json", "/w/3f9a1c2e/v1/viewer", "/w/3f9a1c2e/mcp",
          "/workspaces/c1/zetta/v1/openapi.json", "/.well-known/acme-challenge/x"}) {
        EXPECT_FALSE(web_app_serves("GET", path, browser, false)) << path;
    }
}

TEST(WebApp, OnlyBrowserNavigationsAreTheApp) {
    // npm, pip, git and API clients ask for JSON or anything; a single-segment npm package stays
    // the npm route's.
    EXPECT_FALSE(web_app_serves("GET", "/left-pad", "application/json", false));
    EXPECT_FALSE(web_app_serves("GET", "/left-pad", "*/*", false));
    EXPECT_FALSE(web_app_serves("GET", "/repositories", "", false));
    EXPECT_FALSE(web_app_serves("POST", "/repositories", browser, false));
}

TEST(WebApp, HostedSignInPagesOwnCustomDomains) {
    for (const std::string_view path : {"/sign-in", "/sign-up", "/mfa", "/passkey-sign-in"}) {
        EXPECT_FALSE(web_app_serves("GET", path, browser, true)) << path;
        EXPECT_TRUE(web_app_serves("GET", path, browser, false)) << path;
    }
    EXPECT_TRUE(web_app_serves("GET", "/repositories", browser, true));
}

TEST(WebApp, HashedFilesAreImmutableAndTheIndexIsNot) {
    EXPECT_EQ(web_app_cache_control("/ui/index-abc.js"), "public, max-age=31536000, immutable");
    EXPECT_EQ(web_app_cache_control("/index.html"), "no-cache");
}

TEST(WebApp, TheBuiltAppIsEmbedded) {
    ASSERT_FALSE(web_app_index().empty());
    EXPECT_NE(web_app_index().find("<div id=\"root\">"), std::string_view::npos);
    EXPECT_EQ(web_app_asset("/index.html")->content_type, "text/html; charset=utf-8");
    EXPECT_EQ(web_app_asset("/no-such-file"), nullptr);
}

namespace {

std::string read_source(const char* relative) {
    std::ifstream in(std::string(LAKE_TEST_SOURCE_DIR) + "/" + relative);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string collapsed(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += c;
    }
    return out;
}

} // namespace

// docs/ui/front-door.md's Landing copy quotes the homepage word for word: every bold phrase is in
// the page's source (frontend/src/pages/home/HomePage.tsx, and the invitation sentence in
// frontend/src/lib/clerkSignIn.ts), as its Verify section checks by hand.
TEST(WebApp, FrontDoorDocQuotesTheHomePage) {
    const std::string doc = read_source("docs/ui/front-door.md");
    const auto start = doc.find("## Landing copy");
    ASSERT_NE(start, std::string::npos);
    const auto end = doc.find("\n## ", start + 1);
    const std::string section = doc.substr(start, end == std::string::npos ? end : end - start);
    const std::string source = collapsed(read_source("frontend/src/pages/home/HomePage.tsx") +
                                         read_source("frontend/src/lib/clerkSignIn.ts"));
    ASSERT_FALSE(source.empty());
    std::size_t quoted = 0;
    for (std::size_t at = section.find("**"); at != std::string::npos;) {
        const auto close = section.find("**", at + 2);
        if (close == std::string::npos) {
            break;
        }
        const std::string phrase = collapsed(section.substr(at + 2, close - at - 2));
        EXPECT_NE(source.find(phrase), std::string::npos) << phrase;
        ++quoted;
        at = section.find("**", close + 2);
    }
    EXPECT_GE(quoted, 10U) << "the Landing copy section quotes the homepage";
}
