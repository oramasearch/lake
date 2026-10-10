#include <algorithm>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "lake/api/markdown.hpp"
#include "lake/api/web_ui.hpp"

using lake::api::markdown_to_html;

TEST(Markdown, RendersTheGuidesBlocks) {
    const std::string html = markdown_to_html(
        "# Getting started\n\nlake is **where** you *train*, with `code`.\n\n## Load data\n\n"
        "- one\n- two\n  continued\n\n1. first\n2. second\n\n```\nSELECT <1>\n```\n\n"
        "| Term | What |\n|---|---|\n| **Space** | an area |\n\n> A note\n\n"
        "[Train](/docs/train) and [site](https://orama.com).");
    EXPECT_NE(html.find("<h1 id=\"getting-started\">Getting started</h1>"), std::string::npos);
    EXPECT_NE(html.find("<h2 id=\"load-data\">"), std::string::npos);
    EXPECT_NE(html.find("<strong>where</strong>"), std::string::npos);
    EXPECT_NE(html.find("<em>train</em>"), std::string::npos);
    EXPECT_NE(html.find("<code>code</code>"), std::string::npos);
    EXPECT_NE(html.find("<ul><li>one</li><li>two continued</li></ul>"), std::string::npos);
    EXPECT_NE(html.find("<ol><li>first</li><li>second</li></ol>"), std::string::npos);
    EXPECT_NE(html.find("<pre><code>SELECT &lt;1&gt;\n</code></pre>"), std::string::npos);
    EXPECT_NE(html.find("<th>Term</th>"), std::string::npos);
    EXPECT_NE(html.find("<td><strong>Space</strong></td>"), std::string::npos);
    EXPECT_NE(html.find("class=\"panel notice\"><p>A note</p>"), std::string::npos);
    EXPECT_NE(html.find("<a href=\"/docs/train\">Train</a>"), std::string::npos);
    EXPECT_NE(html.find("<a href=\"https://orama.com\">site</a>"), std::string::npos);
}

TEST(Markdown, NeverPassesHtmlOrUnsafeLinksThrough) {
    const std::string html = markdown_to_html(
        "<script>alert(1)</script> [x](javascript:alert(1)) [y](//evil.example) `<b>`");
    EXPECT_EQ(html.find("<script>"), std::string::npos);
    EXPECT_NE(html.find("&lt;script&gt;"), std::string::npos);
    EXPECT_EQ(html.find("javascript:"), std::string::npos);
    EXPECT_EQ(html.find("evil.example"), std::string::npos);
    EXPECT_NE(html.find("<code>&lt;b&gt;</code>"), std::string::npos);
}

TEST(Docs, EveryGuideHasItsTitleAndRenders) {
    const auto& pages = lake::api::web_ui_guide();
    ASSERT_GE(pages.size(), 30U);
    EXPECT_EQ(pages.front().slug, "index");
    EXPECT_EQ(pages.front().title, "Getting Started");
    for (const auto& page : pages) {
        EXPECT_FALSE(page.title.empty()) << page.slug;
        EXPECT_TRUE(page.markdown.starts_with("# " + std::string(page.title))) << page.slug;
        EXPECT_NE(markdown_to_html(page.markdown).find("<h1 id="), std::string::npos) << page.slug;
    }
}

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"

TEST(Workspaces, TheWorkspaceOpenApiIsWrittenForCustomers) {
    // UX review 2026-09-27 (lake#257): engine issue numbers and permission notes leaked.
    const nlohmann::json document = {
        {"paths",
         {{"/v1/upload_dataset",
           {{"post",
             {{"description",
               "Load a csv file's TEXT into a table. (#1008) Requires orchestration:add_dataset."},
              {"summary", "Upload (#1008) a dataset"}}}}},
          {"/v1/list_datasets",
           {{"get", {{"description", "Requires nothing special. The datasets."}}}}}}}};
    const nlohmann::json shown = lake::api::workspace_openapi(document, "https://lake.test");
    const std::string upload =
        shown["paths"]["/v1/upload_dataset"]["post"]["description"].get<std::string>();
    EXPECT_EQ(upload, "Load a csv file's TEXT into a table.");
    EXPECT_EQ(shown["paths"]["/v1/upload_dataset"]["post"]["summary"], "Upload a dataset");
    EXPECT_EQ(shown["paths"]["/v1/list_datasets"]["get"]["description"],
              "Requires nothing special. The datasets.")
        << "only a permission note (with a colon) is removed";
}

namespace {

// The guide's text a reader sees as prose: code blocks and inline code spans removed.
std::string prose_of(std::string_view markdown) {
    std::string out;
    bool block = false;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        const std::size_t end = std::min(markdown.find('\n', start), markdown.size());
        const std::string_view line = markdown.substr(start, end - start);
        if (line.starts_with("```")) {
            block = !block;
        } else if (!block) {
            bool span = false;
            for (const char c : line) {
                if (c == '`') {
                    span = !span;
                } else if (!span) {
                    out += c;
                }
            }
            out += '\n';
        }
        if (end == markdown.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace

// lake#561: the Docs navigation groups pages by section. Every page file is embedded, once, under a
// section, and a section's pages are adjacent, so the navigation never repeats a section.
TEST(Docs, EveryGuideFileIsEmbeddedOnceUnderASection) {
    const auto& pages = lake::api::web_ui_guide();
    std::set<std::string> slugs;
    std::set<std::string> finished;
    std::string current;
    for (const auto& page : pages) {
        EXPECT_TRUE(slugs.insert(std::string(page.slug)).second) << "twice: " << page.slug;
        ASSERT_FALSE(page.section.empty()) << page.slug;
        if (page.section != current) {
            EXPECT_FALSE(finished.contains(std::string(page.section)))
                << page.section << " is split around " << page.slug;
            if (!current.empty()) {
                finished.insert(current);
            }
            current = std::string(page.section);
        }
    }
    EXPECT_EQ(pages.front().section, "Get Started");
    const std::filesystem::path root =
        std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "docs" / "guide";
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".md" && entry.path().filename() != "README.md") {
            EXPECT_TRUE(slugs.contains(entry.path().stem().string()))
                << entry.path().filename() << " is not in cmake/embed_guide.cmake's GUIDE_SECTIONS";
        }
    }
}

// Every link from one guide to another names a page that exists, and a #anchor names one of that
// page's headings as the renderer anchors them.
TEST(Docs, LinksBetweenGuidesResolve) {
    const auto& pages = lake::api::web_ui_guide();
    std::map<std::string, std::string> html;
    for (const auto& page : pages) {
        html[std::string(page.slug)] = markdown_to_html(page.markdown);
    }
    const std::regex link(R"re(href="/docs(?:/([a-z0-9-]+))?(?:#([a-z0-9-]+))?")re");
    for (const auto& page : pages) {
        const std::string& text = html[std::string(page.slug)];
        for (auto it = std::sregex_iterator(text.begin(), text.end(), link);
             it != std::sregex_iterator(); ++it) {
            const std::string slug = (*it)[1].matched ? (*it)[1].str() : "index";
            ASSERT_TRUE(html.contains(slug)) << page.slug << " links /docs/" << slug;
            if ((*it)[2].matched) {
                EXPECT_NE(html[slug].find("id=\"" + (*it)[2].str() + "\""), std::string::npos)
                    << page.slug << " links /docs/" << slug << "#" << (*it)[2].str();
            }
        }
    }
}

// The product speaks of workspaces, never of a "space" or a "personal space" (Steve, 2026-10-10),
// and never uses the banned word. Code spans and blocks may name fields such as `customer_id`.
TEST(Docs, GuidesSpeakOfWorkspaces) {
    const std::regex space(R"re(\bspaces?\b)re", std::regex::icase);
    const std::regex banned(R"re(\bseams?\b)re", std::regex::icase);
    for (const auto& page : lake::api::web_ui_guide()) {
        const std::string prose = prose_of(page.markdown);
        std::smatch found;
        EXPECT_FALSE(std::regex_search(prose, found, space))
            << page.slug << " says \"" << found.str() << "\" near: "
            << prose.substr(static_cast<std::size_t>(std::max<long>(0, found.position() - 40)), 100);
        EXPECT_FALSE(std::regex_search(prose, banned)) << page.slug;
    }
}
