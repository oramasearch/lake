#include <gtest/gtest.h>

#include "lake/api/drive.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/version.hpp"

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

void expect_theme_scripts(std::string_view html, std::size_t page_scripts = 0) {
    EXPECT_NE(html.find("id=\"theme-preference\""), std::string_view::npos);
    EXPECT_NE(html.find("id=\"theme-toggle-script\""), std::string_view::npos);
    EXPECT_EQ(occurrences(html, "<script"), 2 + page_scripts);
}

} // namespace

TEST(WebUi, EscapesHtml) {
    EXPECT_EQ(lake::api::web_ui_escape("<a href=\"x\">&'"),
              "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
}

TEST(WebUi, MarkdownEscapesRawHtmlAndAddsStableHeadingAnchors) {
    const std::string html = lake::api::web_ui_markdown(
        "# Overview\n\nUse `<safe>` here.\n\n## Details\n\n## Details\n\n<script>bad()</script>");
    EXPECT_NE(html.find("id=\"overview\""), std::string::npos);
    EXPECT_NE(html.find("id=\"details\""), std::string::npos);
    EXPECT_NE(html.find("id=\"details-2\""), std::string::npos);
    EXPECT_NE(html.find("<code>&lt;safe&gt;</code>"), std::string::npos);
    EXPECT_NE(html.find("&lt;script&gt;bad()&lt;/script&gt;"), std::string::npos);
    EXPECT_EQ(html.find("<script>bad()</script>"), std::string::npos);
}

TEST(WebUi, EmbedsEveryDesignSystemFont) {
    for (const std::string_view name : {"ibm-plex-sans-400.woff2",
                                        "ibm-plex-sans-500.woff2",
                                        "ibm-plex-sans-600.woff2",
                                        "ibm-plex-mono-400.woff2",
                                        "ibm-plex-mono-500.woff2"}) {
        const std::string_view font = lake::api::web_ui_font(name);
        ASSERT_GE(font.size(), 4);
        EXPECT_EQ(font.substr(0, 4), "wOF2");
    }
    EXPECT_TRUE(lake::api::web_ui_font("unknown.woff2").empty());
}

TEST(WebUi, ApprovedIconsAreDecorativeInlineSvg) {
    for (const std::string_view name : {"lake",
                                        "home",
                                        "repository",
                                        "search",
                                        "customers",
                                        "organization",
                                        "metering",
                                        "secrets",
                                        "audit",
                                        "identity",
                                        "api",
                                        "theme"}) {
        const std::string icon = lake::api::web_ui_icon(name);
        EXPECT_NE(icon.find("<svg"), std::string::npos) << name;
        EXPECT_NE(icon.find("aria-hidden=\"true\""), std::string::npos) << name;
        EXPECT_EQ(icon.find("<script"), std::string::npos) << name;
    }
    EXPECT_TRUE(lake::api::web_ui_icon("unknown").empty());
}

TEST(WebUi, StylesheetDefinesTheGovernedComponentSet) {
    const std::string css = lake::api::web_ui_stylesheet();
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
    };
    for (const auto& [path, contents] : fixtures) {
        const lake::api::RepositoryFile file{path, "blob", contents, contents.size()};
        const auto preview = registry.preview(file);
        ASSERT_TRUE(preview) << path << ": " << preview.error();
        EXPECT_FALSE(preview->html.empty()) << path;
    }
    const lake::api::RepositoryFile unsupported{"archive.zip", "blob", "zip", 3};
    EXPECT_FALSE(registry.preview(unsupported));
    EXPECT_TRUE(registry.add(".custom", [](const lake::api::RepositoryFile&) {
        return lake::api::DrivePreview{"custom", "text/plain", "<p>custom</p>"};
    }));
    EXPECT_FALSE(registry.add(".custom", [](const lake::api::RepositoryFile&) {
        return lake::api::DrivePreview{"custom", "text/plain", "<p>duplicate</p>"};
    }));
}

TEST(WebUi, HelloIsASelfContainedDocument) {
    const auto html = lake::api::web_ui_hello("node-1", lake::version_string());
    EXPECT_TRUE(html.starts_with("<!doctype html>"));
    EXPECT_NE(html.find("hello from lake"), std::string::npos);
    EXPECT_NE(html.find("node-1"), std::string::npos);
    EXPECT_NE(html.find("/style.css"), std::string::npos);
    EXPECT_NE(html.find("class=\"skip-link\""), std::string::npos);
    EXPECT_NE(html.find("<aside class=\"sidebar\""), std::string::npos);
    EXPECT_NE(html.find("<nav class=\"primary-nav\" aria-label=\"Primary\""), std::string::npos);
    EXPECT_NE(html.find("<header class=\"topbar\""), std::string::npos);
    EXPECT_NE(html.find("<main id=\"main-content\""), std::string::npos);
    expect_theme_scripts(html);
}

TEST(WebUi, HelloEscapesNodeName) {
    const auto html = lake::api::web_ui_hello("<b>evil</b>", "0.0.1");
    EXPECT_EQ(html.find("<b>evil</b>"), std::string::npos);
    EXPECT_NE(html.find("&lt;b&gt;evil&lt;/b&gt;"), std::string::npos);
}

TEST(WebUi, NavigationAndSession) {
    const std::string out = lake::api::web_ui_hello("n", "0.0.1", false);
    EXPECT_NE(out.find("href=\"/customers\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/repositories\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/organizations\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/metering\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/sign-in\""), std::string::npos);
    const std::string in = lake::api::web_ui_hello("n", "0.0.1", true);
    EXPECT_NE(in.find("action=\"/logout\""), std::string::npos);
    EXPECT_NE(in.find("aria-current=\"page\""), std::string::npos);
    expect_theme_scripts(in);
}

TEST(WebUi, MeteringRendersQuotaWarningAndHourlyUsage) {
    const nlohmann::json usage{{"customer_id", "customer<&>"},
                               {"currency", "USD"},
                               {"quota_bytes", 10},
                               {"over_quota", true},
                               {"warning", "Storage quota exceeded"},
                               {"cost_micros", 7},
                               {"items",
                                nlohmann::json::array({{{"hour", "2026-09-04T00:00:00Z"},
                                                        {"stored_bytes", 10},
                                                        {"transfer_in_bytes", 2},
                                                        {"transfer_out_bytes", 3},
                                                        {"compute_seconds", 4},
                                                        {"cost_micros", 7}}})}};
    const std::string html = lake::api::web_ui_metering(usage);
    EXPECT_NE(html.find("Storage quota exceeded"), std::string::npos);
    EXPECT_NE(html.find("2026-09-04T00:00:00Z"), std::string::npos);
    EXPECT_NE(html.find("customer&lt;&amp;&gt;"), std::string::npos);
    EXPECT_EQ(html.find("customer<&>"), std::string::npos);
}

TEST(WebUi, OrganizationAndPolicyPagesEscapeTheirData) {
    const lake::control::Organization organization{
        "o1", "taap:1:ed25519:org", "<Orama>", "2026-09-04T00:00:00Z", "2026-09-04T00:00:00Z"};
    const std::string organizations = lake::api::web_ui_organizations({organization});
    EXPECT_NE(organizations.find("&lt;Orama&gt;"), std::string::npos);
    EXPECT_NE(organizations.find("/teams?organization_id=o1"), std::string::npos);
    EXPECT_NE(organizations.find("/organization/access?id=o1"), std::string::npos);
    expect_theme_scripts(organizations);

    const lake::control::Team team{
        "t1", "o1", "reviewers", "Reviewers", "2026-09-04T00:00:00Z", "2026-09-04T00:00:00Z"};
    const std::string teams = lake::api::web_ui_teams("o1", {team});
    EXPECT_NE(teams.find("/team/access?id=t1"), std::string::npos);

    const lake::control::PolicyObject object{"p1", "team", "t1", "", "created", "updated", true};
    const lake::control::EffectiveAccessEntry entry{
        "taap:1:ed25519:user", "p0", "admin", {"read", "write"}, true};
    const std::string access = lake::api::web_ui_effective_access(object, {entry});
    EXPECT_NE(access.find("inherited from"), std::string::npos);
    EXPECT_NE(access.find("read, write"), std::string::npos);
    expect_theme_scripts(access);
}

TEST(WebUi, SsoPageEscapesProviderAndProvisioningValues) {
    const lake::control::SsoProvider provider{"p1",
                                              "o1",
                                              "lake",
                                              "<provider>",
                                              "oidc",
                                              "https://id.example",
                                              "",
                                              "",
                                              "client",
                                              {},
                                              true,
                                              true,
                                              "tested",
                                              "created",
                                              "updated"};
    const lake::control::FederationIssuer issuer{
        "i1", "o1", "https://id.example", "email", "created"};
    const lake::control::ProvisioningLog log{
        "l1", "p1", "user", "deactivate", "<external>", "success", {}, "created"};
    const std::string page = lake::api::web_ui_sso("o1", {provider}, {issuer}, {log});
    EXPECT_NE(page.find("/v1/register_sso_provider"), std::string::npos);
    EXPECT_NE(page.find("&lt;provider&gt;"), std::string::npos);
    EXPECT_NE(page.find("&lt;external&gt;"), std::string::npos);
    expect_theme_scripts(page);
}

TEST(WebUi, SearchEscapesHitsAndOffersTheQueryForm) {
    const lake::search::Hit hit{"customer", "repository", "<path>", "change", "<snippet>", 1.0};
    const lake::search::Query query{.text = "needle", .history = true};
    const lake::search::QueryResult result{
        .mode = lake::search::QueryMode::Scan, .scanned_changes = 12, .items = {hit}};
    const std::string page = lake::api::web_ui_search(query, result);
    EXPECT_NE(page.find("action=\"/search\""), std::string::npos);
    EXPECT_NE(page.find("&lt;path&gt;"), std::string::npos);
    EXPECT_NE(page.find("&lt;snippet&gt;"), std::string::npos);
    EXPECT_NE(page.find("scan"), std::string::npos);
    EXPECT_NE(page.find("12 changes examined"), std::string::npos);
    EXPECT_EQ(page.find("<path>"), std::string::npos);
}

TEST(WebUi, IdentityFormsUseCapabilityBackedActions) {
    const std::string sign_up = lake::api::web_ui_sign_up("bad <identity>");
    EXPECT_NE(sign_up.find("action=\"/identity/sign-up\""), std::string::npos);
    EXPECT_NE(sign_up.find("bad &lt;identity&gt;"), std::string::npos);
    expect_theme_scripts(sign_up);

    const std::string sign_in = lake::api::web_ui_sign_in("");
    EXPECT_NE(sign_in.find("action=\"/identity/sign-in\""), std::string::npos);
    EXPECT_NE(sign_in.find("action=\"/identity/oauth\""), std::string::npos);
    expect_theme_scripts(sign_in);

    const std::string verify = lake::api::web_ui_mfa("", "", {}, false);
    EXPECT_NE(verify.find("action=\"/identity/mfa/verify\""), std::string::npos);
    const std::string enroll = lake::api::web_ui_mfa("", "otpauth://secret<&", {"recover<&"}, true);
    EXPECT_NE(enroll.find("action=\"/identity/mfa/enroll\""), std::string::npos);
    EXPECT_NE(enroll.find("action=\"/identity/mfa/disable\""), std::string::npos);
    EXPECT_NE(enroll.find("otpauth://secret&lt;&amp;"), std::string::npos);
    EXPECT_NE(enroll.find("recover&lt;&amp;"), std::string::npos);
    expect_theme_scripts(enroll);
}

TEST(WebUi, PasskeyPagesCarryOnlyTheirDocumentedScript) {
    const std::string registration = lake::api::web_ui_passkeys("");
    EXPECT_NE(registration.find("navigator.credentials.create"), std::string::npos);
    expect_theme_scripts(registration, 1);
    const std::string assertion = lake::api::web_ui_passkey_sign_in("");
    EXPECT_NE(assertion.find("navigator.credentials.get"), std::string::npos);
    expect_theme_scripts(assertion, 1);
    EXPECT_EQ(assertion.find("document.cookie"), std::string::npos);
}

TEST(WebUi, SessionsDevicesAndTokensEscapeStoredValues) {
    lake::control::Session browser{"s1",
                                   "u1",
                                   "d1",
                                   "lake",
                                   "browser",
                                   "<session>",
                                   "",
                                   "2026-10-01T00:00:00Z",
                                   "",
                                   "2026-09-04T00:00:00Z",
                                   "",
                                   {"*"}};
    lake::control::Device device{"d1", "u1", "device<&", "", "2026-09-04T00:00:00Z", ""};
    const std::string sessions = lake::api::web_ui_sessions({browser}, {device}, "");
    EXPECT_NE(sessions.find("&lt;session&gt;"), std::string::npos);
    EXPECT_NE(sessions.find("device&lt;&amp;"), std::string::npos);
    EXPECT_NE(sessions.find("action=\"/identity/session/revoke\""), std::string::npos);
    EXPECT_NE(sessions.find("action=\"/identity/device/revoke\""), std::string::npos);

    lake::control::Session token{"t1",
                                 "u1",
                                 "",
                                 "lake",
                                 "api_token",
                                 "token<&",
                                 "",
                                 "2026-10-01T00:00:00Z",
                                 "",
                                 "2026-09-04T00:00:00Z",
                                 "",
                                 {"read"}};
    const std::string tokens = lake::api::web_ui_tokens({token}, "secret<&", "");
    EXPECT_NE(tokens.find("token&lt;&amp;"), std::string::npos);
    EXPECT_NE(tokens.find("secret&lt;&amp;"), std::string::npos);
    EXPECT_NE(tokens.find("action=\"/identity/token/create\""), std::string::npos);
    expect_theme_scripts(tokens);
}

TEST(WebUi, RepositoriesListEscapesAndLinks) {
    lake::control::Customer ada{
        "c1", "individual", "u1", "", "ada", "2026-09-02T00:00:00Z", "2026-09-02T00:00:00Z"};
    lake::control::Repository repo{"r1",
                                   "c1",
                                   "liquid",
                                   "<b>the tool</b>",
                                   "public",
                                   "",
                                   "",
                                   "2026-09-02T00:00:00Z",
                                   "2026-09-02T00:00:00Z"};
    const std::string out = lake::api::web_ui_repositories({repo}, {{"c1", ada}});
    EXPECT_NE(out.find("ada/liquid"), std::string::npos);
    EXPECT_NE(out.find("href=\"/repository?id=r1\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/repositories\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(out.find("href=\"/search\" aria-current=\"page\""), std::string::npos);
    EXPECT_NE(out.find("&lt;b&gt;the tool&lt;/b&gt;"), std::string::npos);
    EXPECT_EQ(out.find("<b>the tool</b>"), std::string::npos);
    EXPECT_NE(lake::api::web_ui_repositories({}, {}).find("No repositories yet"),
              std::string::npos);
    const std::string page = lake::api::web_ui_repository(repo, &ada);
    EXPECT_NE(page.find("<h1>ada/liquid</h1>"), std::string::npos);
    EXPECT_NE(lake::api::web_ui_login("bad token").find("bad token"), std::string::npos);

    repo.deleted_at = "2026-09-03T00:00:00Z";
    repo.retention_until = "2026-10-03T00:00:00Z";
    EXPECT_NE(lake::api::web_ui_repository(repo, &ada).find("Tombstoned."), std::string::npos);

    const lake::api::RepositoryHomeData data{
        .refs = {{"main", "blake3:123456789012345678901234567890"}},
        .tree = {{"README.md", "blob", "blob-1", 0644, "node-1", 48, 32},
                 {"src/example.cpp", "blob", "blob-2", 0644, "node-2", 96, 64},
                 {"src", "tree", "", 0755}},
        .changes = {{"blake3:abcdef012345678901234567890",
                     {},
                     "taap:1:ed25519:author",
                     "Explain the repository",
                     "2026-09-04T00:00:00Z"}},
        .readme = lake::api::RepositoryFile{"README.md", "blob-1", "# Lake\n\nSafe <text>.", 26},
        .access = {{"taap:1:ed25519:reader", "", "read", {"read"}, false}},
    };
    const std::string home = lake::api::web_ui_repository(repo, &ada, data);
    EXPECT_NE(home.find("aria-label=\"Repository\""), std::string::npos);
    EXPECT_NE(home.find("Repository root"), std::string::npos);
    EXPECT_NE(home.find("Recent changes"), std::string::npos);
    EXPECT_NE(home.find("id=\"lake\""), std::string::npos);
    EXPECT_NE(home.find("Safe &lt;text&gt;"), std::string::npos);
    EXPECT_NE(home.find("Clone and push"), std::string::npos);
    EXPECT_NE(home.find("C++"), std::string::npos);
    EXPECT_NE(home.find("Markdown"), std::string::npos);
    EXPECT_NE(home.find("Effective access"), std::string::npos);
}

TEST(WebUi, RepositoryContentAndChangePagesRenderSafely) {
    const lake::control::Repository repository{
        "r1", "c1", "liquid", "", "public", "", "", "2026-09-02T00:00:00Z", "2026-09-02T00:00:00Z"};
    const lake::api::RepositoryTreeEntry tree_entry{.path = "<readme>.md",
                                                    .kind = "blob",
                                                    .blob = "blob-1",
                                                    .mode = 0644,
                                                    .size = 8,
                                                    .last_change = "change-1",
                                                    .last_message = "Add README",
                                                    .last_author = "author",
                                                    .last_changed_at = "2026-09-04"};
    const std::string tree =
        lake::api::web_ui_repository_tree(repository, "main", {tree_entry}, false);
    EXPECT_NE(tree.find("&lt;readme&gt;.md"), std::string::npos);
    EXPECT_NE(tree.find("href=\"/file?repository_id=r1"), std::string::npos);
    EXPECT_NE(tree.find("Last change"), std::string::npos);
    EXPECT_NE(tree.find("Add README"), std::string::npos);
    EXPECT_EQ(tree.find("<table>"), std::string::npos);
    expect_theme_scripts(tree);

    const lake::api::RepositoryFile file{"<readme>.md", "blob-1", "<source>", 8};
    const std::string contents = lake::api::web_ui_repository_file(repository, file, false);
    EXPECT_NE(contents.find("&lt;source&gt;"), std::string::npos);
    EXPECT_EQ(contents.find("<source>"), std::string::npos);

    const lake::api::RepositoryFile source{
        "src/main.cpp", "blake3:source", "int main() {\n    return 0;\n}\n", 29};
    const std::vector<lake::api::RepositoryBlameLine> blame{
        {1, "blake3:change-one", "taap:1:author", "2026-09-04"},
        {2, "blake3:change-two", "taap:1:reviewer", "2026-09-05"},
        {3, "blake3:change-one", "taap:1:author", "2026-09-04"},
    };
    const std::string source_page =
        lake::api::web_ui_repository_file(repository, "main", source, blame, false);
    EXPECT_NE(source_page.find("class=\"source-view syntax-highlighted\""), std::string::npos);
    EXPECT_NE(source_page.find("class=\"syntax-keyword\">int</span>"), std::string::npos);
    EXPECT_NE(source_page.find("id=\"L2\""), std::string::npos);
    EXPECT_NE(source_page.find("data-line-range=\"L2-L2\""), std::string::npos);
    EXPECT_NE(source_page.find("taap:1:reviewer"), std::string::npos);
    EXPECT_NE(source_page.find("/file/raw?repository_id=r1"), std::string::npos);
    EXPECT_NE(source_page.find("/file/download?repository_id=r1"), std::string::npos);
    EXPECT_NE(source_page.find("id=\"file-range-script\""), std::string::npos);
    expect_theme_scripts(source_page, 1);

    const lake::api::RepositoryChange change{
        "blake3:change-identifier-0123456789",
        {"parent-1"},
        "taap:1:ed25519:author",
        "<message>\n\nReview **carefully**. <script>x</script>",
        "2026-09-04T00:00:00Z"};
    const std::string log = lake::api::web_ui_repository_log(repository, {change}, false);
    EXPECT_NE(log.find("href=\"/change?repository_id=r1&amp;id=blake3%3Achange-identifier-"
                       "0123456789\""),
              std::string::npos);
    EXPECT_NE(log.find("&lt;message&gt;"), std::string::npos);

    const lake::api::RepositoryDiffEntry difference{
        .path = "<readme>.md",
        .old_path = "old-readme.md",
        .kind = "blob",
        .changes = {"moved", "modified", "conflicted"},
        .old_mode = 0644,
        .new_mode = 0755,
        .old_blob = "old-blob",
        .new_blob = "new-blob",
        .line_hunks =
            std::vector<lake::api::RepositoryLineHunk>{{2, 1, 2, 1, {"<old>"}, {"<new>"}}},
        .old_conflicts = {"left"},
        .new_conflicts = {"left", "right"},
    };
    const lake::control::ForgeRecord review_record{
        "review-1",
        "r1",
        "review",
        "created",
        "updated",
        {{"change_id", change.id}, {"state", "approved"}, {"reviewers", {"taap:1:reviewer"}}}};
    const lake::control::ForgeRecord comment_record{
        "comment-1",
        "r1",
        "review_comment",
        "created",
        "updated",
        {{"review_id", "review-1"},
         {"body", "Looks **good**. <img src=x>"},
         {"anchor", {{"change", change.id}, {"path", "<readme>.md"}, {"line", 2}, {"column", 0}}}}};
    const lake::api::RepositoryArtifact check{
        .id = "result-long-identifier-0123456789",
        .action = "test",
        .exit_code = 0,
        .attestations = {{"attestation-1", "profile", "subject", "issuer", true, ""}},
    };
    const lake::api::ChangeReviewData review_data{
        .reviews = {review_record},
        .comments = {comment_record},
        .checks = {.items = {check}, .done = true},
    };
    const std::string change_page =
        lake::api::web_ui_change(repository, change, {difference}, review_data, false);
    EXPECT_NE(change_page.find("&lt;message&gt;"), std::string::npos);
    EXPECT_NE(change_page.find("&lt;readme&gt;.md"), std::string::npos);
    EXPECT_NE(change_page.find("@@ -2,1 +2,1 @@"), std::string::npos);
    EXPECT_NE(change_page.find("-&lt;old&gt;"), std::string::npos);
    EXPECT_NE(change_page.find("+&lt;new&gt;"), std::string::npos);
    EXPECT_NE(change_page.find("Conflict values"), std::string::npos);
    EXPECT_NE(change_page.find("Side by side"), std::string::npos);
    EXPECT_NE(change_page.find("data-full-identifier=\"blake3:change-identifier-0123456789\""),
              std::string::npos);
    EXPECT_NE(change_page.find("Copy identifier"), std::string::npos);
    EXPECT_NE(change_page.find("approved"), std::string::npos);
    EXPECT_NE(change_page.find("Looks <strong>good</strong>. &lt;img src=x&gt;"),
              std::string::npos);
    EXPECT_NE(change_page.find("1 attestations"), std::string::npos);
    EXPECT_EQ(change_page.find("<script>x</script>"), std::string::npos);
    EXPECT_NE(change_page.find("id=\"change-review-script\""), std::string::npos);
    expect_theme_scripts(change_page, 1);

    const lake::api::RepositoryDiffEntry reformatting{
        .path = "readme.md",
        .kind = "blob",
        .changes = {"modified"},
        .line_hunks = std::vector<lake::api::RepositoryLineHunk>{},
    };
    const std::string empty_change =
        lake::api::web_ui_change(repository, change, {reformatting}, false);
    EXPECT_NE(empty_change.find("No changes to display."), std::string::npos);
    EXPECT_EQ(empty_change.find("<code>readme.md</code>"), std::string::npos);

    const lake::api::RepositoryConflict conflict{"<config>", {"left", "right"}};
    const std::string conflicts =
        lake::api::web_ui_repository_conflicts(repository, {conflict}, false);
    EXPECT_NE(conflicts.find("&lt;config&gt;"), std::string::npos);

    const lake::api::RepositoryArtifact artifact{
        .id = "result-1<&",
        .action = "action-1",
        .exit_code = 0,
        .outputs = {{"build/<app>", 0755, "blob-1"}},
        .timing = {.queued_ms = 1, .sandbox_ms = 2, .exec_ms = 3, .hash_ms = 4, .store_ms = 5},
        .cache_status = "hit",
        .worker = "worker-1",
        .observed_at = "2026-09-04T00:00:00Z",
        .attestations =
            {{"attestation-1",
              "slsa_provenance/1",
              "subject-1",
              "taap:1:ed25519:builder",
              true,
              ""},
             {"attestation-2",
              "reproducibility/1",
              "subject-2",
              "taap:1:ed25519:builder",
              false,
              "signature mismatch"},
             {"attestation-3", "sbom/1", "subject-3", "taap:1:ed25519:builder", true, ""}},
        .trace = lake::api::RepositoryArtifactTrace{"trace-1", 12, 2, 1, 1, 1},
    };
    const lake::api::RepositoryArtifactPage page{{artifact}, "next<&", false};
    const lake::api::RepositoryArtifactFilter filter{
        .action = "action-1", .output = "build/app", .limit = 1};
    const std::string artifacts = lake::api::web_ui_artifacts(repository, page, filter, false);
    EXPECT_NE(artifacts.find("Build results"), std::string::npos);
    EXPECT_NE(artifacts.find("href=\"/artifact?repository_id=r1&amp;id=result-1%3C%26\""),
              std::string::npos);
    EXPECT_NE(artifacts.find("Next page"), std::string::npos);
    EXPECT_NE(artifacts.find("value=\"build/app\""), std::string::npos);

    const std::string artifact_page = lake::api::web_ui_artifact(repository, artifact, false);
    EXPECT_NE(artifact_page.find("result-1&lt;&amp;"), std::string::npos);
    EXPECT_NE(artifact_page.find("build/&lt;app&gt;"), std::string::npos);
    EXPECT_NE(artifact_page.find("slsa_provenance/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("reproducibility/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("sbom/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("unverified: signature mismatch"), std::string::npos);
    EXPECT_NE(artifact_page.find("trace-1"), std::string::npos);
    expect_theme_scripts(artifact_page);
}

TEST(WebUi, IssueTrackerRendersFiltersDiscussionAndLinkedChangesSafely) {
    const lake::control::Repository repository{
        "r1", "c1", "forge", "description", "public", "", "", "created", "updated"};
    const lake::control::ForgeRecord open_issue{
        "issue-long-identifier-0123456789",
        "r1",
        "issue",
        "2026-09-05",
        "2026-09-05",
        {{"title", "Fix <parser>"},
         {"body", "## Reproduction\n\nUse `lake`. <script>x</script>"},
         {"state", "open"},
         {"labels", {"bug", "priority"}},
         {"assignees", {"taap:1:alice"}},
         {"references", {"blake3:change-0123456789"}}}};
    const lake::control::ForgeRecord closed_issue{
        "issue-2", "r1", "issue", "created", "updated", {{"title", "Closed"}, {"state", "closed"}}};
    const std::string list =
        lake::api::web_ui_issues(repository, {open_issue, closed_issue}, "open", "bug", "", false);
    EXPECT_NE(list.find("Fix &lt;parser&gt;"), std::string::npos);
    EXPECT_NE(list.find("name=\"state\""), std::string::npos);
    EXPECT_NE(list.find("value=\"open\" selected"), std::string::npos);
    EXPECT_NE(list.find("taap:1:alice"), std::string::npos);
    EXPECT_EQ(list.find(">Closed</a>"), std::string::npos);
    expect_theme_scripts(list);

    const lake::control::ForgeRecord comment{
        "comment-1",
        "r1",
        "issue_comment",
        "2026-09-05",
        "2026-09-05",
        {{"issue_id", open_issue.id}, {"body", "Confirmed with **tests**. <iframe>x</iframe>"}}};
    const std::string detail = lake::api::web_ui_issue(repository, open_issue, {comment}, false);
    EXPECT_NE(detail.find("id=\"reproduction\""), std::string::npos);
    EXPECT_NE(detail.find("Use <code>lake</code>. &lt;script&gt;x&lt;/script&gt;"),
              std::string::npos);
    EXPECT_NE(detail.find("Confirmed with <strong>tests</strong>. &lt;iframe&gt;x&lt;/iframe&gt;"),
              std::string::npos);
    EXPECT_NE(detail.find("Linked changes"), std::string::npos);
    EXPECT_NE(detail.find("blake3:chang…3456789"), std::string::npos);
    EXPECT_EQ(detail.find("<script>x</script>"), std::string::npos);
    expect_theme_scripts(detail);
}

TEST(WebUi, ReleaseWebhookAndProtectionPagesRenderForgeEvidenceSafely) {
    const lake::control::Repository repository{
        "r1", "c1", "forge", "description", "private", "", "", "created", "updated"};
    const lake::control::ForgeRecord release{
        "release-1",
        "r1",
        "release",
        "created",
        "updated",
        {{"tag", "v1.0 <stable>"},
         {"change_id", "blake3:change-0123456789"},
         {"notes", "## Notes\n\nShips **forge pages**. <script>x</script>"},
         {"artifacts",
          {{{"id", "artifact-1"},
            {"action", "build"},
            {"outputs", 2},
            {"attestations", {{{"id", "attestation-1"}, {"profile", "slsa"}}}}}}}}};
    const std::string releases = lake::api::web_ui_releases(repository, {release}, true);
    EXPECT_NE(releases.find("v1.0 &lt;stable&gt;"), std::string::npos);
    EXPECT_NE(releases.find("Ships <strong>forge pages</strong>. &lt;script&gt;x&lt;/script&gt;"),
              std::string::npos);
    EXPECT_NE(releases.find("2 outputs"), std::string::npos);
    EXPECT_NE(releases.find("1 attestations"), std::string::npos);
    expect_theme_scripts(releases);

    const lake::control::ForgeRecord webhook{"webhook-1",
                                             "r1",
                                             "webhook",
                                             "created",
                                             "updated",
                                             {{"url", "https://example.test/<hook>"},
                                              {"events", {"review", "release"}},
                                              {"secret", "x"}}};
    const lake::control::ForgeRecord delivery{"delivery-1",
                                              "r1",
                                              "webhook_delivery",
                                              "created",
                                              "updated",
                                              {{"webhook_id", "webhook-1"},
                                               {"event", "review"},
                                               {"attempt", 2},
                                               {"status", 503},
                                               {"error", "connection <failed>"},
                                               {"delivered", false}}};
    const std::string webhooks =
        lake::api::web_ui_webhooks(repository, {webhook}, {delivery}, true);
    EXPECT_NE(webhooks.find("https://example.test/&lt;hook&gt;"), std::string::npos);
    EXPECT_NE(webhooks.find("attempt 2 · status 503"), std::string::npos);
    EXPECT_NE(webhooks.find("connection &lt;failed&gt;"), std::string::npos);
    EXPECT_NE(webhooks.find("action=\"/webhooks/retry\""), std::string::npos);
    EXPECT_EQ(webhooks.find("name=\"secret\""), std::string::npos);
    expect_theme_scripts(webhooks);

    const lake::control::ForgeRecord rule{"rule-1",
                                          "r1",
                                          "branch_protection",
                                          "created",
                                          "updated",
                                          {{"ref", "main"},
                                           {"required_review", true},
                                           {"required_verification", true},
                                           {"fast_forward_only", true},
                                           {"allowed_pushers", {"taap:1:maintainer"}},
                                           {"policy_object_id", "policy-1"}}};
    const lake::audit::Event refused{"event-1",
                                     "2026-09-05",
                                     {.customer_id = "c1",
                                      .kind = "forge.branch_protection.refused",
                                      .principal = "taap:1:outsider",
                                      .object = "repository/r1/ref/main",
                                      .action = "reference_update",
                                      .outcome = "denied",
                                      .details = {{"repository", "r1"},
                                                  {"reference", "main"},
                                                  {"caller", "taap:1:outsider"},
                                                  {"rule", "required_review"},
                                                  {"reason", "review <required>"}}},
                                     "previous",
                                     "signer",
                                     "signature",
                                     "digest"};
    const std::string protection =
        lake::api::web_ui_branch_protection(repository, {rule}, {refused}, true);
    EXPECT_NE(protection.find("fast-forward only"), std::string::npos);
    EXPECT_NE(protection.find("taap:1:maintainer"), std::string::npos);
    EXPECT_NE(protection.find("review &lt;required&gt;"), std::string::npos);
    EXPECT_NE(protection.find("Refused updates"), std::string::npos);
    expect_theme_scripts(protection);
}

TEST(WebUi, ForgeStatesAndFocusOrderRemainAccessible) {
    const std::string error = lake::api::web_ui_error("not_<found>", "Missing <repository>", true);
    EXPECT_NE(error.find("role=\"alert\""), std::string::npos);
    EXPECT_NE(error.find("not_&lt;found&gt;"), std::string::npos);
    EXPECT_NE(error.find("Missing &lt;repository&gt;"), std::string::npos);
    EXPECT_NE(error.find("Back to repositories"), std::string::npos);
    expect_theme_scripts(error);

    const std::string loading = lake::api::web_ui_loading("Change <review>", false);
    EXPECT_NE(loading.find("role=\"status\""), std::string::npos);
    EXPECT_NE(loading.find("aria-live=\"polite\""), std::string::npos);
    EXPECT_NE(loading.find("aria-busy=\"true\""), std::string::npos);
    EXPECT_NE(loading.find("Change &lt;review&gt;"), std::string::npos);
    expect_theme_scripts(loading);

    const lake::control::Repository repository{
        "r1", "c1", "forge", "description", "public", "", "", "created", "updated"};
    const std::string empty_issues = lake::api::web_ui_issues(repository, {}, "", "", "", false);
    EXPECT_NE(empty_issues.find("No issues match these filters."), std::string::npos);
    const std::string empty_releases = lake::api::web_ui_releases(repository, {}, false);
    EXPECT_NE(empty_releases.find("No releases have been published."), std::string::npos);
    const std::string empty_webhooks = lake::api::web_ui_webhooks(repository, {}, {}, false);
    EXPECT_NE(empty_webhooks.find("No webhooks are configured."), std::string::npos);
    const std::string empty_protection =
        lake::api::web_ui_branch_protection(repository, {}, {}, false);
    EXPECT_NE(empty_protection.find("No branch protection rules."), std::string::npos);
    EXPECT_NE(empty_protection.find("No protected updates have been refused."), std::string::npos);

    const std::size_t skip = empty_issues.find("class=\"skip-link\"");
    const std::size_t navigation = empty_issues.find("class=\"sidebar\"");
    const std::size_t topbar = empty_issues.find("class=\"topbar\"");
    const std::size_t main = empty_issues.find("id=\"main-content\"");
    const std::size_t filters = empty_issues.find("class=\"filters\"");
    ASSERT_NE(skip, std::string::npos);
    ASSERT_NE(navigation, std::string::npos);
    ASSERT_NE(topbar, std::string::npos);
    ASSERT_NE(main, std::string::npos);
    ASSERT_NE(filters, std::string::npos);
    EXPECT_LT(skip, navigation);
    EXPECT_LT(navigation, topbar);
    EXPECT_LT(topbar, main);
    EXPECT_LT(main, filters);
}

TEST(WebUi, AuditPageFiltersAndEscapesEvents) {
    lake::audit::Event event{
        .id = "event-1",
        .occurred_at = "2026-09-04T00:00:00.000000Z",
        .draft = {.kind = "capability",
                  .principal = "<admin>",
                  .object = "capability/list_users",
                  .action = "list_users",
                  .outcome = "success"},
        .digest = "abcdef",
    };
    lake::audit::Filter filter{.principal = "<admin>", .outcome = "success"};
    const std::string page = lake::api::web_ui_audit(
        {event},
        filter,
        {{"mode", "witnessed"},
         {"witness_threshold", 2},
         {"witnesses", nlohmann::json::array({{{"endpoint", "https://witness.example"}}})}});
    EXPECT_NE(page.find("action=\"/audit\""), std::string::npos);
    EXPECT_NE(page.find("name=\"principal\""), std::string::npos);
    EXPECT_NE(page.find("&lt;admin&gt;"), std::string::npos);
    EXPECT_EQ(page.find("<admin>"), std::string::npos);
    EXPECT_NE(page.find("capability/list_users"), std::string::npos);
    EXPECT_NE(page.find("GET /v1/verify_audit"), std::string::npos);
    EXPECT_NE(page.find("Effective mode: <code>witnessed</code>"), std::string::npos);
}

TEST(WebUi, TransparencyPageRendersVerifiedIdentityMatches) {
    const nlohmann::json scan{
        {"duration_ms", 12.5},
        {"root_hash", "abcdef<&"},
        {"tiles_scanned", 98},
        {"tree_size", 100000},
        {"matches",
         nlohmann::json::array({nlohmann::json{{"index", 42},
                                               {"profile", "authorship/1<&"},
                                               {"issuer", "taap:1:ed25519:issuer<&"},
                                               {"subject", "blake3:subject<&"}}})}};
    const std::string page = lake::api::web_ui_transparency("taap:1:ed25519:person<&", scan);
    EXPECT_NE(page.find("action=\"/transparency\""), std::string::npos);
    EXPECT_NE(page.find("100000 entries"), std::string::npos);
    EXPECT_NE(page.find("98"), std::string::npos);
    EXPECT_NE(page.find("authorship/1&lt;&amp;"), std::string::npos);
    EXPECT_NE(page.find("person&lt;&amp;"), std::string::npos);
    EXPECT_EQ(page.find("person<&"), std::string::npos);
    expect_theme_scripts(page);
}

TEST(WebUi, SecretPagesEscapeDataAndDoNotRenderSecretValues) {
    const lake::control::SecretMount mount{"mount-1",
                                           "customer-1",
                                           "organization-1",
                                           "production<&",
                                           "credentials<&",
                                           "created",
                                           "updated"};
    const std::string mounts = lake::api::web_ui_secret_mounts({mount});
    EXPECT_NE(mounts.find("production&lt;&amp;"), std::string::npos);
    EXPECT_NE(mounts.find("credentials&lt;&amp;"), std::string::npos);
    EXPECT_NE(mounts.find("/secrets?mount_id=mount-1"), std::string::npos);
    EXPECT_NE(mounts.find("/encryption-keys?mount_id=mount-1"), std::string::npos);
    expect_theme_scripts(mounts);

    lake::control::SecretMetadata metadata{
        "mount-1",
        "database<&",
        2,
        {{"owner", "payments<&"}},
        {{1, "created-1", "deleted<&", ""}, {2, "created-2", "", ""}},
        "created",
        "updated"};
    const std::string secrets = lake::api::web_ui_secrets(mount, "database<&", &metadata);
    EXPECT_NE(secrets.find("database&lt;&amp;"), std::string::npos);
    EXPECT_NE(secrets.find("payments&lt;&amp;"), std::string::npos);
    EXPECT_NE(secrets.find("deleted&lt;&amp;"), std::string::npos);
    EXPECT_NE(secrets.find("Inspect version metadata"), std::string::npos);
    EXPECT_EQ(secrets.find("secret-value"), std::string::npos);
    expect_theme_scripts(secrets);

    const lake::control::EncryptionKey key{
        "key-1", "mount-1", "orders<&", "parent<&", 3, "created", "updated"};
    const std::string keys = lake::api::web_ui_encryption_keys(mount, {key});
    EXPECT_NE(keys.find("orders&lt;&amp;"), std::string::npos);
    EXPECT_NE(keys.find("parent&lt;&amp;"), std::string::npos);
    EXPECT_NE(keys.find("Key material is never returned"), std::string::npos);
    expect_theme_scripts(keys);
}
