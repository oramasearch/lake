#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>

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

// The guides speak of lake and the workspace, never of the engine underneath it (lake#601, as the
// pages do since lake#583). Zetta stays only where a person picks it by name: it is a kind on
// Ship -> Services, so the Services guide and the glossary's Service entry name it, and each says
// once what it is. Every other line naming it fails. Code spans and blocks (a `zetta` kind key in
// a request) are left out, as above.
TEST(Docs, GuidesNameZettaOnlyWhereAPersonChoosesIt) {
    struct Allowed {
        std::string_view slug;
        std::string_view line; // a fragment of the one prose line allowed to name Zetta
        std::string_view reason;
    };
    static constexpr std::array<Allowed, 6> allowed{{
        {"concepts",
         "| **Service** | A database, stream or engine lake runs for you: Postgres, Zetta,",
         "the glossary lists the Service kinds a person can create, Zetta among them, and says "
         "what Zetta is"},
        {"services",
         "on its own cluster: Zetta, Postgres, Kafka,",
         "the Services guide opens with the kinds of service a person can create"},
        {"services",
         "| **Zetta** | Orama's data and AI engine.",
         "the Services glossary says once what the Zetta kind is"},
        {"services",
         "Choose the **Kind** (Zetta is first)",
         "the Kind menu on Ship -> Services lists Zetta first, by that name"},
        {"services",
         "Once a service (other than Zetta) is Ready",
         "a Zetta service has no connection dialog; the step names the kind a person picked"},
        {"services",
         "A Zetta service's admin token is never shown.",
         "what a person who created a Zetta service does not get"},
    }};
    const std::regex engine(R"re(\bzetta\b)re", std::regex::icase);
    std::set<std::pair<std::string, std::string>> used;
    for (const auto& page : lake::api::web_ui_guide()) {
        const std::string prose = prose_of(page.markdown);
        std::size_t start = 0;
        while (start < prose.size()) {
            const std::size_t end = std::min(prose.find('\n', start), prose.size());
            const std::string line = prose.substr(start, end - start);
            start = end + 1;
            if (!std::regex_search(line, engine)) {
                continue;
            }
            const auto entry = std::ranges::find_if(allowed, [&](const Allowed& item) {
                return item.slug == page.slug && line.find(item.line) != std::string::npos;
            });
            EXPECT_NE(entry, allowed.end())
                << page.slug << " names Zetta to a person; say lake or the workspace: " << line;
            if (entry != allowed.end()) {
                used.emplace(std::string(entry->slug), std::string(entry->line));
            }
        }
    }
    for (const Allowed& item : allowed) {
        EXPECT_TRUE(used.contains({std::string(item.slug), std::string(item.line)}))
            << "an allowed line is gone; remove its entry: " << item.slug << ": " << item.line;
    }
}

// A person runs an AI training experiment, as an experiment tracker says it, never a "study"
// (Steve, 2026-10-10, lake#561). The engine's names (`study-events.*`, ExperimentsAgent's `study`
// field) appear only in code spans and blocks, which the prose leaves out.
TEST(Docs, GuidesSpeakOfExperiments) {
    const std::regex study(R"re(\bstud(y|ies)\b)re", std::regex::icase);
    for (const auto& page : lake::api::web_ui_guide()) {
        const std::string prose = prose_of(page.markdown);
        std::smatch found;
        EXPECT_FALSE(std::regex_search(prose, found, study))
            << page.slug << " says \"" << found.str() << "\" near: "
            << prose.substr(static_cast<std::size_t>(std::max<long>(0, found.position() - 40)),
                            100);
    }
}
