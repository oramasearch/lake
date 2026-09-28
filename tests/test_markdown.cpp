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

TEST(Docs, EveryGuideRendersWithContentsAndTheNextGuide) {
    const auto& pages = lake::api::web_ui_guide();
    ASSERT_GE(pages.size(), 7U);
    EXPECT_EQ(pages.front().slug, "index");
    EXPECT_EQ(pages.front().title, "Getting started");
    for (const auto& page : pages) {
        const auto html = lake::api::web_ui_docs(page.slug, true);
        ASSERT_TRUE(html) << page.slug;
        EXPECT_NE(html->find("docs-contents"), std::string::npos);
        EXPECT_NE(html->find("aria-current=\"page\">" + std::string(page.title)), std::string::npos)
            << page.slug;
    }
    const auto first = lake::api::web_ui_docs("", false);
    ASSERT_TRUE(first);
    EXPECT_NE(first->find("Next: <a href=\"/docs/concepts\">Concepts</a>"), std::string::npos);
    EXPECT_NE(first->find("href=\"/docs\""), std::string::npos) << "Docs is in the navigation";
    EXPECT_FALSE(lake::api::web_ui_docs("nope", true));
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

TEST(WebUi, HomeStartsANewcomerOnTheDocsAndTheirData) {
    const std::string home = lake::api::web_ui_home(true);
    EXPECT_NE(home.find("href=\"/docs\">Getting started</a>"), std::string::npos);
    EXPECT_NE(home.find("href=\"/train/data\">Load your data</a>"), std::string::npos);
    EXPECT_EQ(home.find("requires access to its source"), std::string::npos);
}

TEST(WebUi, AskOramaOpensBesideThePageAndKnowsWhichPageItIs) {
    // UX review 2026-09-27 (lake#258): Ask Orama left the page and lost which pipeline it was on.
    lake::api::WebUiViewer viewer{true};
    viewer.path = "/design/pipeline";
    viewer.query = "customer_id=c-1&project=retention&graph=churn";
    const std::string page = lake::api::web_ui_docs("", viewer).value();
    EXPECT_NE(page.find("<details class=\"ask-dock\">"), std::string::npos);
    EXPECT_NE(page.find("<iframe src=\"/ask?embed=1&amp;from=%2Fdesign%2Fpipeline%3Fcustomer_id%3Dc-1%"
                        "26project%3Dretention%26graph%3Dchurn\""),
              std::string::npos)
        << "the panel is told the whole page, query included";

    lake::api::AskPage ask;
    ask.embed = true;
    ask.customer_id = "c-1";
    ask.conversation_id = "conv-1";
    ask.from = "/design/pipeline?project=retention&graph=churn";
    const std::string embedded = lake::api::web_ui_ask(ask, true);
    EXPECT_NE(embedded.find("<body class=\"ask-embedded\">"), std::string::npos);
    EXPECT_EQ(embedded.find("primary-nav"), std::string::npos) << "no site frame in the panel";
    EXPECT_NE(embedded.find("name=\"embed\" value=\"1\""), std::string::npos)
        << "its forms stay in the panel";
    EXPECT_NE(embedded.find("target=\"_top\">/design/pipeline</a>"), std::string::npos);
}

TEST(WebUi, ARunSaysWhatItReadAndWhenNothingWasTrained) {
    // lake#251, lake#253: a structural run and an unread connect step are said on the run.
    lake::api::WorkspacePage page;
    page.section = "run";
    page.customer_id = "c-1";
    page.subject = "r-1";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_run"] = {
        {"status", "succeeded"},
        {"logs",
         nlohmann::json::array(
             {{{"level", "warn"},
               {"message", "structural: this pipeline is not churn-shaped, so its steps ran "
                           "without training or scoring anything"}},
              {{"level", "info"},
               {"message", "connect: Load usage was not read: this run trained on datasets.a"}}})}};
    page.data["get_run_report"] = {
        {"dataset", {{"source", "datasets.a"}, {"not_used", nlohmann::json::array({"'plan'"})}}}};
    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<strong>Nothing was trained.</strong>"), std::string::npos);
    EXPECT_NE(html.find("Load usage was not read: this run trained on datasets.a"),
              std::string::npos);
    EXPECT_NE(html.find("<dt>trained on</dt><dd>datasets.a</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>columns not used</dt>"), std::string::npos);

    page.section = "project";
    page.subject = "retention";
    page.data = {{"get_project", {{"name", "retention"}, {"graphs", nlohmann::json::array()}}},
                 {"list_datasets",
                  {{"datasets",
                    nlohmann::json::array({{{"name", "accounts"}, {"database", "datasets"}}})}}}};
    const std::string project = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(project.find("<select name=\"dataset\">"), std::string::npos)
        << "Build a Pipeline can start from the person's own data";
    EXPECT_NE(project.find("<option value=\"datasets.accounts\">accounts</option>"),
              std::string::npos);
}

TEST(WebUi, AnAgentsMarkdownReplyRendersAndStaysEscaped) {
    // UX review 2026-09-27 (2): Ask Orama answered with a markdown table shown as raw pipes.
    lake::api::AskPage ask;
    ask.customer_id = "c-1";
    ask.conversation_id = "conv-1";
    ask.messages = nlohmann::json::array(
        {{{"role", "user"}, {"content", "**not bold** <b>me</b>"}},
         {{"role", "assistant"},
          {"content", "You have **2** models:\n\n| Model | AUC |\n|---|---:|\n| churn | 0.95 |\n\n"
                      "<script>alert(1)</script>"}}});
    const std::string page = lake::api::web_ui_ask(ask, true);
    EXPECT_NE(page.find("<table>"), std::string::npos);
    EXPECT_NE(page.find("<td>0.95</td>"), std::string::npos);
    EXPECT_NE(page.find("<strong>2</strong>"), std::string::npos);
    EXPECT_EQ(page.find("<script>alert"), std::string::npos) << "a reply cannot inject HTML";
    EXPECT_NE(page.find("**not bold** &lt;b&gt;me&lt;/b&gt;"), std::string::npos)
        << "the person's own words are shown as typed";
}

TEST(WebUi, TheLatestScanIsTheRecommendationAndEarlierOnesFold) {
    // lake#287: every scan added its Opportunities to the last one's, so ideas piled up.
    lake::api::WorkspacePage page;
    page.section = "discover";
    page.customer_id = "c-1";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    const auto row = [](std::string id, std::string task, std::string at, std::string status,
                        std::string intent) {
        return nlohmann::json{{"id", id}, {"task_id", task}, {"created_at", at},
                              {"status", status}, {"intent", intent}, {"solution", "S-1"},
                              {"methodology", "classifier"}, {"why", "because"},
                              {"detail", nlohmann::json::object()}};
    };
    page.data["list_opportunities"] = {{"opportunities", nlohmann::json::array({
        row("o-3", "scan-2", "2026-09-27T19:00:00Z", "new", "Predict churn and rank accounts"),
        row("o-4", "scan-2", "2026-09-27T19:00:00Z", "not_yet", "Forecast revenue"),
        row("o-1", "scan-1", "2026-09-27T12:00:00Z", "new", "Rank accounts for outreach"),
        row("o-2", "scan-1", "2026-09-27T12:00:00Z", "project", "Score leads"),
        row("o-5", "scan-1", "2026-09-27T12:00:00Z", "not_yet", "Old reason")})}};
    const std::string html = lake::api::web_ui_workspace(page, true);
    const auto recommended = html.find("<h2>Recommended</h2>");
    const auto earlier = html.find("<summary>Earlier scans (1)</summary>");
    ASSERT_NE(recommended, std::string::npos);
    ASSERT_NE(earlier, std::string::npos);
    const auto newest = html.find("Predict churn and rank accounts");
    const auto older = html.find("Rank accounts for outreach");
    EXPECT_TRUE(recommended < newest && newest < earlier) << "the latest scan leads";
    EXPECT_GT(older, earlier) << "an untouched earlier idea folds under Earlier scans";
    EXPECT_NE(html.find("value=\"o-1\""), std::string::npos) << "it can still be picked";
    EXPECT_NE(html.find("Score leads"), std::string::npos) << "what a person acted on stays";
    EXPECT_NE(html.find("Forecast revenue"), std::string::npos);
    EXPECT_EQ(html.find("Old reason"), std::string::npos) << "Not Yet is the latest scan's";
}
