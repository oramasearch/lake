#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "lake/api/drive.hpp"
#include "lake/api/web_ui.hpp"

// What lake still renders on the server (docs/ui/web-app.md): the stylesheet and fonts the hosted
// identity pages link, a handler failure's page, and Drive's previews. The console is the web app,
// tested in frontend/ and by test_web_app.cpp.

namespace {

std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

} // namespace

TEST(WebUi, EscapesHtml) {
    EXPECT_EQ(lake::api::web_ui_escape("<a href=\"x\">&'"),
              "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
}

TEST(WebUi, EmbedsEveryDesignSystemFont) {
    for (const std::string_view name : lake::api::web_ui_font_names()) {
        const std::string_view font = lake::api::web_ui_font(name);
        ASSERT_GE(font.size(), 4);
        EXPECT_EQ(font.substr(0, 4), "wOF2");
    }
    EXPECT_TRUE(lake::api::web_ui_font("unknown.woff2").empty());
}

TEST(WebUi, ContentHashedAssetUrlsUseTheFirstSha256HexDigits) {
    EXPECT_EQ(lake::api::web_ui_hashed_asset_url("/style.css", "abc"),
              "/style-ba7816bf8f01cfea.css");
    EXPECT_EQ(lake::api::web_ui_hashed_asset_url("/fonts/example.woff2", "abc"),
              "/fonts/example-ba7816bf8f01cfea.woff2");
    EXPECT_NE(lake::api::web_ui_hashed_asset_url("/style.css", "first stylesheet"),
              lake::api::web_ui_hashed_asset_url("/style.css", "second stylesheet"));
}

TEST(WebUi, StylesheetUsesContentHashedFontUrls) {
    const std::string_view css = lake::api::web_ui_stylesheet();
    for (const std::string_view name : lake::api::web_ui_font_names()) {
        const std::string_view url = lake::api::web_ui_font_url(name);
        EXPECT_FALSE(url.empty()) << name;
        EXPECT_NE(css.find(url), std::string_view::npos) << name;
        EXPECT_EQ(css.find("url('/fonts/" + std::string(name) + "')"), std::string_view::npos)
            << name;
    }
}

TEST(WebUi, CompatibilityAssetRoutesUseNoCache) {
    EXPECT_EQ(lake::api::web_ui_asset_cache_control("/style.css"), "no-cache");
    EXPECT_EQ(lake::api::web_ui_asset_cache_control(lake::api::web_ui_stylesheet_url()),
              "public, max-age=31536000, immutable");
    for (const std::string_view name : lake::api::web_ui_font_names()) {
        const std::string plain_url = "/fonts/" + std::string(name);
        EXPECT_EQ(lake::api::web_ui_asset_cache_control(plain_url), "no-cache") << name;
        EXPECT_EQ(lake::api::web_ui_asset_cache_control(lake::api::web_ui_font_url(name)),
                  "public, max-age=31536000, immutable")
            << name;
    }
}

TEST(WebUi, StylesheetDefinesTheGovernedComponentSet) {
    const std::string_view css = lake::api::web_ui_stylesheet();
    for (const std::string_view selector : {".app-shell",
                                            ".sidebar",
                                            ".topbar",
                                            ".panel",
                                            ".card",
                                            ".badge",
                                            ".status-success",
                                            ".tabs",
                                            ".pagination",
                                            ".breadcrumbs",
                                            ".toast",
                                            ".diff-entry",
                                            "th[aria-sort]",
                                            "prefers-reduced-motion"}) {
        EXPECT_NE(css.find(selector), std::string::npos) << selector;
    }
    EXPECT_NE(css.find("font-family: var(--lake-font-sans)"), std::string::npos);
    EXPECT_NE(css.find("color: var(--lake-text-primary)"), std::string::npos);
}

TEST(DrivePreview, DefaultRegistryRendersEveryFormat) {
    auto registry = lake::api::DriveFormatRegistry::defaults();
    const std::vector<std::pair<std::string, std::string>> fixtures{
        {"note.txt", "plain"},
        {"README.md", "# Heading\n- item"},
        {"image.png", "PNG"},
        {"rows.csv", "name,value\none,1\n"},
        {"rows.json", R"([{"name":"one","value":1}])"},
        {"paper.pdf", "%PDF-1.7\n/Title (Paper)\n/Type /Page\n"},
        {"clip.mp4", "video"},
        {"sound.mp3", "audio"},
    };
    for (const auto& [path, contents] : fixtures) {
        const lake::api::RepositoryFile file{path, "blob", contents, contents.size()};
        const auto preview = registry.preview(file);
        ASSERT_TRUE(preview) << path << ": " << preview.error();
        EXPECT_FALSE(preview->html.empty()) << path;
    }
    const lake::api::RepositoryFile unsupported{"archive.zip", "blob", "zip", 3};
    const auto fallback = registry.preview(unsupported);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback->format, "download");
    EXPECT_NE(fallback->html.find("No preview is available"), std::string::npos);
    const lake::api::RepositoryFile hostile{"README.md", "blob", "# <script>alert(1)</script>", 27};
    const auto markdown = registry.preview(hostile);
    ASSERT_TRUE(markdown);
    EXPECT_EQ(markdown->html.find("<script>"), std::string::npos);
    EXPECT_NE(markdown->html.find("&lt;script&gt;"), std::string::npos);
    EXPECT_TRUE(registry.add(".custom", [](const lake::api::RepositoryFile&) {
        return lake::api::DrivePreview{"custom", "text/plain", "<p>custom</p>"};
    }));
    EXPECT_FALSE(registry.add(".custom", [](const lake::api::RepositoryFile&) {
        return lake::api::DrivePreview{"custom", "text/plain", "<p>duplicate</p>"};
    }));
}

TEST(DrivePreview, TextDiffPreservesCommonAndChangedLines) {
    const lake::api::RepositoryFile left{"note.txt", "left", "same\nold\ntail\n", 14};
    const lake::api::RepositoryFile right{"note.txt", "right", "same\nnew\ntail\n", 14};
    const lake::api::DriveVersionDiff difference = lake::api::drive_text_diff(left, right);
    ASSERT_EQ(difference.lines.size(), 5u);
    EXPECT_EQ(difference.lines[0].kind, "context");
    EXPECT_EQ(difference.lines[1].kind, "removed");
    EXPECT_EQ(difference.lines[1].text, "old");
    EXPECT_EQ(difference.lines[2].kind, "added");
    EXPECT_EQ(difference.lines[2].text, "new");
    EXPECT_EQ(difference.lines[3].kind, "context");
    EXPECT_EQ(difference.lines[4].kind, "context");
}

TEST(WebUi, HostedPagesLinkTheSharedStylesheetOnce) {
    const lake::control::Application application{"app-1",
                                                 "customer-1",
                                                 "pool-1",
                                                 "Example",
                                                 {},
                                                 {},
                                                 "",
                                                 nlohmann::json::object(),
                                                 "created",
                                                 "updated"};
    const std::string link = "<link rel=\"stylesheet\" href=\"" +
                             std::string(lake::api::web_ui_stylesheet_url()) + "\">";
    for (const std::string& html : {lake::api::web_ui_application_sign_in(application, ""),
                                    lake::api::web_ui_application_sign_up(application, ""),
                                    lake::api::web_ui_application_mfa(application, ""),
                                    lake::api::web_ui_application_passkey_sign_in(application, ""),
                                    lake::api::web_ui_error("500", "failed")}) {
        EXPECT_EQ(occurrences(html, link), 1u) << html;
    }
}

TEST(WebUi, AFailurePageEscapesWhatItSays) {
    const std::string html = lake::api::web_ui_error("<500>", "<script>bad()</script>");
    EXPECT_TRUE(html.starts_with("<!doctype html>"));
    EXPECT_NE(html.find("&lt;script&gt;bad()&lt;/script&gt;"), std::string::npos);
    EXPECT_EQ(html.find("<script>bad()"), std::string::npos);
    EXPECT_NE(html.find("We Could Not Load This Page"), std::string::npos);
}
