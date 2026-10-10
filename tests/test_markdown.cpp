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
    ASSERT_GE(pages.size(), 7U);
    EXPECT_EQ(pages.front().slug, "index");
    EXPECT_EQ(pages.front().title, "Getting started");
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
