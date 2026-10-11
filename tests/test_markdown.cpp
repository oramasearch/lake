#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lake/api/agent_docs.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/markdown.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/rest.hpp"
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

// The guides speak of Lake and the workspace, never of the engine underneath it (lake#601, as the
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
         "| **Service** | A database, stream or engine Lake runs for you: Postgres, Zetta,",
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
                << page.slug << " names Zetta to a person; say Lake or the workspace: " << line;
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

// The product's name is Lake, with a capital L (decision 59, lake#642). Prose never names it
// lowercase; technical names keep their form: code spans and blocks (`claude mcp add lake`), a
// hostname or dotted name (lake.live, lake.oramadev.net), a path or link (/skills/lake/SKILL.md,
// oramasearch/lake), an identifier (lake_tok_, lake-theme, LAKE_KEY), the ciphertext prefix
// lake:v1 and an issue reference (lake#642).
static std::vector<std::string> lowercase_product_names(std::string_view prose) {
    const auto inside_name = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '/' ||
               c == '\\';
    };
    std::vector<std::string> found;
    for (std::size_t at = prose.find("lake"); at != std::string_view::npos;
         at = prose.find("lake", at + 4)) {
        const char before = at == 0 ? ' ' : prose[at - 1];
        const std::string_view after = prose.substr(at + 4, 2);
        if (inside_name(before) || before == '.' || before == '@' || before == '$' ||
            (!after.empty() && (inside_name(after[0]) || after[0] == '#')) ||
            (after.size() == 2 && after[0] == '.' &&
             std::isalpha(static_cast<unsigned char>(after[1]))) ||
            after == ":v") {
            continue;
        }
        const std::size_t from = at < 40 ? 0 : at - 40;
        found.emplace_back(prose.substr(from, 100));
    }
    return found;
}

TEST(Docs, LowercaseProductNamesAreFoundOnlyInProse) {
    EXPECT_EQ(lowercase_product_names("Open lake in the browser.").size(), 1U);
    EXPECT_EQ(lowercase_product_names("Ask your lake's administrator.").size(), 1U);
    EXPECT_EQ(lowercase_product_names("on this lake.").size(), 1U);
    EXPECT_EQ(lowercase_product_names("lake checks it").size(), 1U);
    EXPECT_TRUE(lowercase_product_names("Lake checks it, and the Lake Console shows it.").empty());
    EXPECT_TRUE(lowercase_product_names("Open https://lake.oramadev.net or lake.live.").empty());
    EXPECT_TRUE(lowercase_product_names("[skill](/skills/lake/SKILL.md) oramasearch/lake").empty());
    EXPECT_TRUE(
        lowercase_product_names("lake_tok_K, lake-theme, LAKE_KEY, lake#642, lake:v1:k").empty());
    EXPECT_TRUE(
        lowercase_product_names(prose_of("Run `claude mcp add lake`.\n```\nlake\n```")).empty());
}

TEST(Docs, GuidesCallTheProductLake) {
    for (const auto& page : lake::api::web_ui_guide()) {
        for (const std::string& near : lowercase_product_names(prose_of(page.markdown))) {
            ADD_FAILURE() << page.slug << " names the product \"lake\"; say Lake: " << near;
        }
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

// Every capability, billing's included, is listed under a group that names it on purpose: a new
// capability no group claims fails here instead of falling into Repositories (lake#642).
TEST(Docs, EveryCapabilityHasADeliberateGroup) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::register_billing_capabilities(registry);
    std::vector<std::string> ungrouped;
    for (const auto& capability : registry.list()) {
        if (lake::api::agent_docs::group_of(capability->name) == lake::api::agent_docs::ungrouped) {
            ungrouped.push_back(capability->name);
        }
    }
    EXPECT_TRUE(ungrouped.empty()) << "no group claims: " << [&] {
        std::string names;
        for (const auto& name : ungrouped) {
            names += name + " ";
        }
        return names;
    }();
    const auto groups = lake::api::agent_docs::tool_groups(registry);
    const auto group = [&](std::string_view title) {
        const auto found = std::ranges::find(groups, title, &lake::api::agent_docs::Group::title);
        EXPECT_NE(found, groups.end()) << title;
        return found == groups.end() ? std::set<std::string>{} : [&] {
            std::set<std::string> names;
            for (const auto& tool : found->tools) {
                names.insert(tool.name);
            }
            return names;
        }();
    };
    const auto drives = group("Drives");
    for (const char* name : {"create_drive",
                             "list_drives",
                             "search_drives",
                             "get_drive_entry",
                             "begin_drive_upload",
                             "finish_drive_upload",
                             "load_drive_dataset"}) {
        EXPECT_TRUE(drives.contains(name)) << name;
    }
    const auto billing = group("Usage and billing");
    for (const char* name : {"usage_over_time",
                             "usage_limits",
                             "set_usage_limit",
                             "price_list",
                             "set_price_list",
                             "bill_preview",
                             "list_invoices",
                             "get_invoice"}) {
        EXPECT_TRUE(billing.contains(name)) << name;
    }
    const auto repositories = group("Repositories");
    for (const char* name : {"list_issues", "create_issue", "update_issue", "issue_assignees"}) {
        EXPECT_TRUE(repositories.contains(name)) << name;
    }
    EXPECT_TRUE(group("Transcripts").contains("transcribe"));
    EXPECT_TRUE(group("Solutions").contains("duplicate_solution"));
    EXPECT_TRUE(group("Search").contains("search"));
}

namespace {

// Where a text a person reads says "space" or names the product "lake" in prose: one line each, with
// what is around it. Code spans and blocks are left out (`customer_id`, `remove_space_store`,
// `claude mcp add lake`), and so are workspace, namespace and whitespace, which are other words.
std::vector<std::string> wording_faults(std::string_view text) {
    static const std::regex space(R"re(\bspaces?\b)re", std::regex::icase);
    const std::string prose = prose_of(text);
    std::vector<std::string> faults;
    for (auto it = std::sregex_iterator(prose.begin(), prose.end(), space);
         it != std::sregex_iterator();
         ++it) {
        const auto at = static_cast<std::size_t>(it->position());
        faults.push_back("says \"" + it->str() + "\": " +
                         prose.substr(at < 60 ? 0 : at - 60, 120));
    }
    for (const std::string& near : lowercase_product_names(prose)) {
        faults.push_back("names the product \"lake\": " + near);
    }
    return faults;
}

// Every description, summary and title in a JSON document (OpenAPI, or MCP's tools), with where it
// is, so a fault names the tool or route and the field.
void collect_descriptions(const nlohmann::json& value,
                          const std::string& where,
                          std::vector<std::pair<std::string, std::string>>& out) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if ((key == "description" || key == "summary" || key == "title") && child.is_string()) {
                out.emplace_back(where + "/" + key, child.get<std::string>());
            } else {
                collect_descriptions(child, where + "/" + key, out);
            }
        }
    } else if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            const std::string name = value[index].is_object() && value[index].contains("name") &&
                                             value[index]["name"].is_string()
                                         ? value[index]["name"].get<std::string>()
                                         : std::to_string(index);
            collect_descriptions(value[index], where + "/" + name, out);
        }
    }
}

lake::api::Registry every_capability() {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::register_billing_capabilities(registry);
    return registry;
}

} // namespace

TEST(Docs, WordingFaultsAreFoundOnlyInProse) {
    EXPECT_EQ(wording_faults("read the audits of this space's live workspaces").size(), 1U);
    EXPECT_EQ(wording_faults("List the Spaces you can see").size(), 1U);
    EXPECT_EQ(wording_faults("the handle of their personal space, on this lake.").size(), 2U);
    EXPECT_TRUE(wording_faults("A workspace in a namespace, whitespace trimmed.").empty());
    EXPECT_TRUE(wording_faults("`remove_space_store` names `customer_id`; Lake answers.").empty());
    EXPECT_TRUE(wording_faults("See https://lake.example/llms.txt and LAKE_STORE_ROOT.").empty());
}

// Every word a person or an agent reads about Lake's tools says Lake and never "space"
// (decision 59, lake#642, lake#647): all of llms.txt, llms-full.txt and the skill, every
// description, summary and title in OpenAPI (both audiences) and in MCP's tools/list (both
// audiences), the registry's descriptions and input schemas included. Account, workspace or
// organization is said instead, whichever is true.
TEST(Docs, AgentDocsSayLakeAndNeverSpace) {
    const lake::api::Registry registry = every_capability();
    const std::string origin = "https://lake.example";
    // The skill's frontmatter names it `lake`, an identifier; its body is read.
    const std::string skill = lake::api::agent_docs::skill_md(registry, origin);
    const std::size_t body = skill.find("\n---\n", 4);
    ASSERT_NE(body, std::string::npos);
    for (const auto& [name, text] :
         std::vector<std::pair<std::string, std::string>>{
             {"llms.txt", lake::api::agent_docs::llms_txt(registry, origin)},
             {"llms-full.txt", lake::api::agent_docs::llms_full_txt(registry, origin)},
             {"SKILL.md", skill.substr(body + 5)}}) {
        for (const std::string& fault : wording_faults(text)) {
            ADD_FAILURE() << name << " " << fault;
        }
    }
    for (const auto audience :
         {lake::api::SchemaAudience::Caller, lake::api::SchemaAudience::AgentTask}) {
        std::vector<std::pair<std::string, std::string>> texts;
        collect_descriptions(lake::api::openapi_document(registry, "test", audience), "openapi", texts);
        collect_descriptions(lake::api::mcp_tools(registry, audience), "tools/list", texts);
        ASSERT_GT(texts.size(), registry.list().size());
        for (const auto& [where, text] : texts) {
            for (const std::string& fault : wording_faults(text)) {
                ADD_FAILURE() << where << " " << fault;
            }
        }
    }
}

// llms.txt, llms-full.txt and the skill keep their own sections: what Lake does, the flows, every
// tool with its input schema, and every group in the skill (lake#642).
TEST(Docs, AgentDocsKeepTheirSections) {
    const lake::api::Registry registry = every_capability();
    const std::string origin = "https://lake.example";
    const std::string own = lake::api::agent_docs::llms_txt(registry, origin);
    for (const auto& group : lake::api::agent_docs::tool_groups(registry)) {
        for (const std::string& fault : wording_faults(group.summary)) {
            ADD_FAILURE() << group.title << " " << fault;
        }
    }
    EXPECT_NE(own.find("## What Lake Does"), std::string::npos);
    for (const char* flow :
         {"### Search everything", "### A drive of each kind", "### See usage and the bill"}) {
        EXPECT_NE(own.find(flow), std::string::npos) << flow;
    }
    // llms-full.txt carries every tool with its input schema; the skill names every group but
    // Server, and its frontmatter keeps the skill's name.
    const std::string full = lake::api::agent_docs::llms_full_txt(registry, origin);
    for (const auto& capability : registry.list()) {
        if (capability->transport != lake::api::CapabilityTransport::Json) {
            continue;
        }
        const std::size_t at = full.find("### `" + capability->name + "` — ");
        ASSERT_NE(at, std::string::npos) << capability->name;
        EXPECT_NE(full.find("Input schema:", at), std::string::npos) << capability->name;
    }
    const std::string skill = lake::api::agent_docs::skill_md(registry, origin);
    EXPECT_TRUE(skill.starts_with("---\nname: lake\n"));
    EXPECT_NE(skill.find("\n# Lake\n"), std::string::npos);
    for (const auto& group : lake::api::agent_docs::tool_groups(registry)) {
        if (group.title != "Server") {
            EXPECT_NE(skill.find("- **" + group.title + "**"), std::string::npos) << group.title;
        }
    }
}
