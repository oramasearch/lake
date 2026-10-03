#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

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

TEST(WebUi, WorkspacePathOwnsNavigationAndEveryFormCarriesTheSelection) {
    const std::string id = "01234567-89ab-4cde-8fab-0123456789ab";
    lake::api::WebUiViewer viewer = true;
    viewer.path = "/w/" + id + "/train/stream";
    viewer.query = "topic=orders&zetta=workspace";
    viewer.customer_id = "space-one";
    viewer.workspace_id = id;
    viewer.spaces = {{"space-one", "My space"}};
    viewer.workspaces = {{id, "Primary"}};
    const auto html = lake::api::web_ui_shell("Workspace navigation", "",
        "<a href=\"/ship/apps?customer_id=space-one\">Apps</a>"
        "<form method=\"post\" action=\"/train/dataset/delete\"><button>Delete</button></form>", viewer);
    EXPECT_NE(html.find("href=\"/w/" + id + "/train/streams\" aria-current=\"page\""), std::string::npos);
    EXPECT_NE(html.find("href=\"/w/" + id + "/ship/apps?customer_id=space-one\""), std::string::npos);
    EXPECT_NE(html.find("action=\"/w/" + id + "/train/dataset/delete\""), std::string::npos);
    EXPECT_NE(html.find("type=\"hidden\" name=\"workspace_id\" value=\"" + id + "\""), std::string::npos);
    EXPECT_NE(html.find("name=\"page\" value=\"/train/stream\""), std::string::npos);
    EXPECT_NE(html.find("name=\"page_query\" value=\"topic=orders&amp;zetta=workspace\""), std::string::npos);
    EXPECT_NE(html.find("value=\"" + id + "\" selected>Primary"), std::string::npos);
    EXPECT_EQ(occurrences(html, "<select name=\"workspace_id\""), 1);
    EXPECT_NE(html.find("New workspace</a>"), std::string::npos);
    EXPECT_EQ(lake::api::web_ui_workspace_document(html, id), html);
    EXPECT_EQ(lake::api::workspace_local_path(viewer.path), "/train/stream");
    EXPECT_EQ(lake::api::workspace_path_id(viewer.path), id);
    EXPECT_TRUE(lake::api::workspace_path_id("/w/not-an-id/train/data").empty());
}

// At 390 px the Space and Workspace selectors shrank to a few letters because they shared one
// nowrap row with the actions (lake#452). The picker now has its own full-width row at phone
// width, two equal columns and the Switch button, and the trail keeps a row of its own.
TEST(WebUi, PhoneWidthGivesTheWorkspacePickerItsOwnRow) {
    const std::string id = "01234567-89ab-4cde-8fab-0123456789ab";
    lake::api::WebUiViewer viewer = true;
    viewer.path = "/w/" + id + "/train/data";
    viewer.customer_id = "space-one";
    viewer.workspace_id = id;
    viewer.spaces = {{"space-one", "Personal space"}};
    viewer.workspaces = {{id, "workspace"}};
    const std::string html = lake::api::web_ui_shell(
        "Data", std::vector<lake::api::WebUiCrumb>{{"Train", "/train/data", {}}, {"Data", "", {}}},
        "<p>content</p>", viewer);
    const std::size_t context = html.find("<div class=\"topbar-context\">");
    const std::size_t picker = html.find("<form method=\"get\" action=\"/workspace/select\" "
                                         "class=\"filters workspace-picker\">");
    const std::size_t trail = html.find("<nav class=\"breadcrumbs topbar-trail\"");
    const std::size_t actions = html.find("<div class=\"topbar-actions\">");
    ASSERT_NE(context, std::string::npos);
    ASSERT_NE(picker, std::string::npos);
    ASSERT_NE(trail, std::string::npos);
    EXPECT_LT(context, picker);
    EXPECT_LT(picker, trail);
    EXPECT_LT(trail, actions) << "picker and trail live in the context, before the actions";
    EXPECT_NE(html.find("selected>Personal space</option>"), std::string::npos);

    const std::string_view css = lake::api::web_ui_stylesheet();
    const std::size_t phone = css.find("@media (max-width: 40rem)");
    ASSERT_NE(phone, std::string::npos);
    const std::string_view block = css.substr(phone, css.find("\n}\n", phone) - phone);
    const auto rule = [&](std::string_view selector) {
        const std::size_t at = block.find(std::string(selector) + " {");
        return at == std::string_view::npos
                   ? std::string_view()
                   : block.substr(at, block.find('}', at) - at);
    };
    EXPECT_NE(rule("  .topbar").find("flex-wrap: wrap"), std::string_view::npos) << block;
    EXPECT_NE(rule("  .topbar").find("position: static"), std::string_view::npos);
    EXPECT_NE(rule("  .topbar-context").find("flex: 1 1 100%"), std::string_view::npos);
    EXPECT_NE(rule("  .topbar-context").find("white-space: normal"), std::string_view::npos);
    EXPECT_NE(rule("  .workspace-picker").find(
                  "grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) auto"),
              std::string_view::npos);
    // Only design tokens: the phone rules introduce no raw lengths or colours.
    for (const std::string_view selector : {"  .topbar", "  .topbar-context", "  .workspace-picker",
                                            "  .topbar-trail"}) {
        const std::string_view body = rule(selector);
        EXPECT_EQ(body.find("px"), std::string_view::npos) << selector;
        EXPECT_EQ(body.find('#'), std::string_view::npos) << selector;
    }
}

TEST(WebUi, WorkspaceRewritePreservesScriptsValuesAndExplicitSelections) {
    const std::string id = "01234567-89ab-4cde-8fab-0123456789ab";
    const std::string prefix = "/w/" + id;
    const std::string other = "/w/abcdef01-2345-4678-9abc-def012345678/train/runs";
    const std::string input =
        "<script src=\"/train/client.js\">location='/train/runs?state=all#runs';</script>"
        "<a href='/ship/models#models'>Models</a>"
        "<a href=\"" + other + "\">Other workspace</a>"
        "<a href=\"/workspaces\">Workspaces</a>"
        "<a href=\"/training\">Unrelated</a>"
        "<a href=\"https://example.test/train/runs\">External</a>"
        "<input type=\"hidden\" value=\"/train/runs\">"
        "<form action='/train/run'><button>Run</button></form>"
        "<form action=\"/workspace/select\"><select name=\"workspace_id\">"
        "<option value=\"other\">Other</option></select></form>"
        "<form action=\"/train/run\"><input name=\"workspace_id\" value=\"other\"></form>"
        "<script>location=\"/design/projects\";</script>"
        "<input value='/ship/models'>";
    const std::string expected =
        "<script src=\"" + prefix + "/train/client.js\">location='" + prefix +
        "/train/runs?state=all#runs';</script>"
        "<a href='" + prefix + "/ship/models#models'>Models</a>"
        "<a href=\"" + other + "\">Other workspace</a>"
        "<a href=\"/workspaces\">Workspaces</a>"
        "<a href=\"/training\">Unrelated</a>"
        "<a href=\"https://example.test/train/runs\">External</a>"
        "<input type=\"hidden\" value=\"/train/runs\">"
        "<form action='" + prefix + "/train/run'>"
        "<input type=\"hidden\" name=\"workspace_id\" value=\"" + id + "\">"
        "<button>Run</button></form>"
        "<form action=\"/workspace/select\"><select name=\"workspace_id\">"
        "<option value=\"other\">Other</option></select></form>"
        "<form action=\"" + prefix + "/train/run\">"
        "<input name=\"workspace_id\" value=\"other\"></form>"
        "<script>location=\"" + prefix + "/design/projects\";</script>"
        "<input value='/ship/models'>";
    EXPECT_EQ(lake::api::web_ui_workspace_document(input, id), expected);
    EXPECT_EQ(lake::api::web_ui_workspace_document(expected, id), expected);
    EXPECT_EQ(lake::api::web_ui_workspace_document(input, "invalid"), input);
    EXPECT_EQ(lake::api::web_ui_workspace_document("<form", id), "<form");
    EXPECT_EQ(lake::api::web_ui_workspace_document("<form>unfinished", id), "<form>unfinished");
}

TEST(WebUi, WorkspaceRewriteHandlesLargeListsWithoutDuplicatingFormFields) {
    const std::string id = "01234567-89ab-4cde-8fab-0123456789ab";
    std::string input = "<script>const page='/train/runs';</script>";
    for (int row = 0; row < 2000; ++row) {
        input += "<a href=\"/train/run?id=" + std::to_string(row) + "\">Run</a>";
        input += "<form action=\"/train/run/delete\"><button>Delete</button></form>";
    }
    const std::string result = lake::api::web_ui_workspace_document(input, id);
    EXPECT_EQ(occurrences(result, "href=\"/w/" + id + "/train/run?id="), 2000);
    EXPECT_EQ(occurrences(result, "action=\"/w/" + id + "/train/run/delete\""), 2000);
    EXPECT_EQ(occurrences(result, "name=\"workspace_id\""), 2000);
    EXPECT_EQ(lake::api::web_ui_workspace_document(result, id), result);
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
                                        "streams",
                                        "theme"}) {
        const std::string icon = lake::api::web_ui_icon(name);
        EXPECT_NE(icon.find("<svg"), std::string::npos) << name;
        EXPECT_NE(icon.find("aria-hidden=\"true\""), std::string::npos) << name;
        EXPECT_EQ(icon.find("<script"), std::string::npos) << name;
    }
    EXPECT_TRUE(lake::api::web_ui_icon("unknown").empty());
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

TEST(WebUi, DrivePagesExposeBrowseMutationHistoryAndConflictControls) {
    const lake::control::Repository repository{
        "r1", "c1", "lake", "", "private", "", "", "created", "updated"};
    const std::vector<lake::api::RepositoryTreeEntry> entries{
        {.path = "docs", .kind = "tree", .mode = 0755, .last_changed_at = "2026-09-04"},
        {.path = "README.md",
         .kind = "blob",
         .blob = "digest",
         .mode = 0644,
         .size = 1536,
         .last_changed_at = "2026-09-05"}};
    const lake::api::DriveFolderOptions list_options{"list", "modified", true};
    const std::string list =
        lake::api::web_ui_drive_folder(repository, "main", "", entries, list_options, true);
    EXPECT_NE(list.find("Name</th><th>Size</th><th>Modified"), std::string::npos);
    EXPECT_NE(list.find("action=\"/drive/folder\""), std::string::npos);
    EXPECT_NE(list.find("action=\"/drive/upload\""), std::string::npos);
    EXPECT_NE(list.find("enctype=\"multipart/form-data\""), std::string::npos);
    EXPECT_NE(list.find("action=\"/drive/rename\""), std::string::npos);
    EXPECT_NE(list.find("action=\"/drive/move\""), std::string::npos);
    EXPECT_NE(list.find("action=\"/drive/delete\""), std::string::npos);
    EXPECT_NE(list.find("1.5 KiB"), std::string::npos);
    expect_theme_scripts(list, 1);

    const std::string nested =
        lake::api::web_ui_drive_folder(repository, "main", "docs/guides", {}, list_options, true);
    EXPECT_NE(nested.find("aria-label=\"Breadcrumb\""), std::string::npos);
    EXPECT_NE(nested.find("path=docs"), std::string::npos);

    const lake::api::DriveFolderOptions grid_options{"grid", "name", false};
    const std::string grid =
        lake::api::web_ui_drive_folder(repository, "main", "", entries, grid_options, false);
    EXPECT_NE(grid.find("class=\"drive-grid\""), std::string::npos);
    EXPECT_EQ(grid.find("/drive/upload"), std::string::npos);
    expect_theme_scripts(grid);

    const lake::api::RepositoryFile archive{"archive.zip", "archive-digest", "zip", 3};
    const auto fallback = lake::api::DriveFormatRegistry::defaults().preview(archive);
    ASSERT_TRUE(fallback);
    const std::string preview =
        lake::api::web_ui_drive_preview(repository, "main", archive, *fallback, true);
    EXPECT_NE(preview.find("archive-digest"), std::string::npos);
    EXPECT_NE(preview.find("3 B"), std::string::npos);
    EXPECT_NE(preview.find("download>Download"), std::string::npos);

    const std::vector<nlohmann::json> history{
        {{"id", "new"},
         {"new_blob", "new-blob"},
         {"changes", {"modified", "conflicted"}},
         {"new_conflicts", {"left-value", "right-value"}}},
        {{"id", "old"}, {"new_blob", "old-blob"}, {"changes", {"added"}}}};
    const std::string versions =
        lake::api::web_ui_drive_history(repository, "main", "README.md", history, true);
    EXPECT_NE(versions.find("action=\"/drive/history/diff\""), std::string::npos);
    EXPECT_NE(versions.find("action=\"/drive/history/restore\""), std::string::npos);
    EXPECT_NE(versions.find("Conflict records:"), std::string::npos);
    EXPECT_NE(versions.find("left-value"), std::string::npos);
    EXPECT_NE(versions.find("right-value"), std::string::npos);

    const lake::api::DriveVersionDiff difference = lake::api::drive_text_diff(
        {"README.md", "old-blob", "old\n", 4}, {"README.md", "new-blob", "new\n", 4});
    const std::string diff = lake::api::web_ui_drive_version_diff(
        repository, "review", "README.md", "old", "new", difference, true);
    EXPECT_NE(diff.find("ref=review"), std::string::npos);
    EXPECT_NE(diff.find("class=\"diff-remove\""), std::string::npos);
    EXPECT_NE(diff.find("class=\"diff-add\""), std::string::npos);

    const lake::api::RepositoryConflict conflict_value{
        .id = "conflict-1",
        .base = "base-value",
        .left = "old-blob",
        .right = "new-blob",
        .region = {.path = "README.md", .bytes = lake::api::RepositoryConflictByteRange{4, 3}},
        .kind = "suspicious-merge",
        .explanation = "Both sides changed the same greeting.",
        .proposal = "proposal-value",
        .reasons = {"The merged wording needs review."},
    };
    const std::string conflict = lake::api::web_ui_drive_conflict(
        repository, "main", conflict_value, {difference.left, difference.right}, difference, true);
    EXPECT_NE(conflict.find("Two versions of README.md"), std::string::npos);
    EXPECT_NE(conflict.find("Suspicious merge proposal"), std::string::npos);
    EXPECT_NE(conflict.find("Both sides changed the same greeting."), std::string::npos);
    EXPECT_NE(conflict.find("The merged wording needs review."), std::string::npos);
    EXPECT_NE(conflict.find("What Differs"), std::string::npos);
    EXPECT_NE(conflict.find("action=\"/drive/conflict/resolve\""), std::string::npos);
    EXPECT_NE(conflict.find("value=\"accept_proposal\""), std::string::npos);
    expect_theme_scripts(conflict);
}

TEST(WebUi, DriveSharingAnswersAccessExpiryAndRevocationQuestions) {
    const lake::control::Repository repository{
        "r1", "c1", "lake", "", "private", "", "", "created", "updated"};
    const lake::control::EffectiveAccessEntry access{
        "taap:1:ed25519:person<&>", "path-1", "write", {"read", "write"}, true};
    const nlohmann::json link{{"id", "link-1"},
                              {"role", "read"},
                              {"expires_at", "2030-01-01T00:00:00Z"},
                              {"active", true},
                              {"revoked_at", nullptr}};
    const std::string share =
        lake::api::web_ui_drive_share(repository, "main", "docs", {access}, {link}, "", "");
    EXPECT_NE(share.find("Who Can Access This Folder Now"), std::string::npos);
    EXPECT_NE(share.find("People"), std::string::npos);
    EXPECT_NE(share.find("Organization"), std::string::npos);
    EXPECT_NE(share.find("Anyone holding the link"), std::string::npos);
    EXPECT_NE(share.find("Until when (UTC)"), std::string::npos);
    EXPECT_NE(share.find("Passcodes are not supported"), std::string::npos);
    EXPECT_NE(share.find("Revoke link"), std::string::npos);
    EXPECT_NE(share.find("person&lt;&amp;&gt;"), std::string::npos);
    EXPECT_EQ(share.find("person<&>"), std::string::npos);
    expect_theme_scripts(share);

    const std::string detail =
        lake::api::web_ui_drive_share_link(repository, "main", "docs", link, "");
    EXPECT_NE(detail.find("Revoking prevents this link"), std::string::npos);
    EXPECT_NE(detail.find("It does not recall files"), std::string::npos);
    EXPECT_NE(detail.find("action=\"/drive/share/revoke\""), std::string::npos);
    expect_theme_scripts(detail);

    const nlohmann::json folder{{"repository", {{"id", "r1"}, {"name", "lake<&>"}}},
                                {"ref", "main"},
                                {"path", "docs"},
                                {"shared_by", "taap:1:ed25519:owner"},
                                {"actions", {"read", "write"}},
                                {"available", true}};
    const std::string shared_with_me = lake::api::web_ui_shared_with_me({folder}, "");
    EXPECT_NE(shared_with_me.find("Shared with Me"), std::string::npos);
    EXPECT_NE(shared_with_me.find("lake&lt;&amp;&gt;/docs"), std::string::npos);
    EXPECT_NE(shared_with_me.find("taap:1:ed25519:owner"), std::string::npos);
    EXPECT_EQ(shared_with_me.find("lake<&>"), std::string::npos);
    expect_theme_scripts(shared_with_me);
}

TEST(WebUi, HomeSaysDesignTrainAndShip) {
    const auto html = lake::api::web_ui_home();
    EXPECT_TRUE(html.starts_with("<!doctype html>"));
    EXPECT_NE(html.find("<h1>Design, train, and ship AI on your own data.</h1>"),
              std::string::npos);
    EXPECT_NE(html.find("lake by Orama"), std::string::npos);
    EXPECT_NE(html.find("<h3>Design</h3>"), std::string::npos);
    EXPECT_NE(html.find("<h3>Train</h3>"), std::string::npos);
    EXPECT_NE(html.find("<h3>Ship</h3>"), std::string::npos);
    EXPECT_NE(html.find("Your data lives in your workspace"), std::string::npos)
        << "one story about where data lives (lake#256)";
    EXPECT_EQ(html.find("requires access to its source repository"), std::string::npos);
    EXPECT_NE(html.find("href=\"/docs\">Read the docs</a>"), std::string::npos);
    EXPECT_NE(html.find("href=\"/sign-up\""), std::string::npos);
    EXPECT_EQ(html.find("href=\"/deployments\""), std::string::npos);
    const auto account = lake::api::web_ui_home(true);
    EXPECT_NE(account.find("href=\"/docs\">Getting started</a>"), std::string::npos);
    EXPECT_NE(account.find("href=\"/deployments\""), std::string::npos);
    EXPECT_NE(html.find(lake::api::web_ui_stylesheet_url()), std::string::npos);
    EXPECT_EQ(html.find("href=\"/style.css\""), std::string::npos);
    EXPECT_NE(html.find("class=\"skip-link\""), std::string::npos);
    EXPECT_NE(html.find("<aside class=\"sidebar\""), std::string::npos);
    EXPECT_NE(html.find("<nav class=\"primary-nav\" aria-label=\"Primary\""), std::string::npos);
    EXPECT_NE(html.find("<header class=\"topbar\""), std::string::npos);
    EXPECT_NE(html.find("<main id=\"main-content\""), std::string::npos);
    expect_theme_scripts(html);
}

// docs/ui/front-door.md quotes the homepage in bold under "Landing copy"; every quoted phrase must
// appear on one of the rendered variants (visitor without and with Clerk, signed-in account), so
// the spec cannot describe a homepage that no longer exists (lake#448).
TEST(WebUi, FrontDoorDocQuotesTheRenderedHomePage) {
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "docs/ui/front-door.md");
    ASSERT_TRUE(file);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string doc = buffer.str();
    const auto start = doc.find("## Landing copy");
    ASSERT_NE(start, std::string::npos);
    const auto end = doc.find("\n## ", start + 1);
    const std::string section = doc.substr(start, end - start);
    const auto collapse = [](std::string text) {
        return std::regex_replace(text, std::regex(R"(\s+)"), " ");
    };
    lake::api::WebUiViewer clerk_visitor;
    clerk_visitor.clerk = true;
    const std::string rendered = collapse(lake::api::web_ui_home() +
                                          lake::api::web_ui_home(clerk_visitor) +
                                          lake::api::web_ui_home(true));
    std::size_t quoted = 0;
    const std::regex bold(R"(\*\*([^*]+)\*\*)");
    for (auto match = std::sregex_iterator(section.begin(), section.end(), bold);
         match != std::sregex_iterator();
         ++match) {
        const std::string phrase = collapse((*match)[1].str());
        ++quoted;
        EXPECT_NE(rendered.find(phrase), std::string::npos)
            << "front-door.md quotes \"" << phrase << "\", which the homepage does not render";
    }
    EXPECT_GE(quoted, 10U) << "the Landing copy section quotes the homepage";
}

TEST(WebUi, EveryPageHeadUsesTheSharedStylesheetLink) {
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
    const std::string shell = lake::api::web_ui_home();
    const std::string hosted = lake::api::web_ui_application_sign_in(application, "");
    const std::string link = "<link rel=\"stylesheet\" href=\"" +
                             std::string(lake::api::web_ui_stylesheet_url()) + "\">";
    EXPECT_EQ(occurrences(shell, link), 1u);
    EXPECT_EQ(occurrences(hosted, link), 1u);
}

TEST(WebUi, CustomerPagesDoNotAdvertiseImplementationOrOperatorCredentials) {
    for (const auto& html : {lake::api::web_ui_home(),
                             lake::api::web_ui_home(true),
                             lake::api::web_ui_sign_up(""),
                             lake::api::web_ui_sign_in(""),
                             lake::api::web_ui_login(""),
                             lake::api::web_ui_error("not_found", "No repository", true)}) {
        for (const std::string_view forbidden : {"Zetta",
                                                 "Beast2",
                                                 "Capy",
                                                 "Corosio",
                                                 "admin token",
                                                 "Orama data plane",
                                                 "hello world"}) {
            EXPECT_EQ(html.find(forbidden), std::string::npos) << forbidden;
        }
    }
}

// The breadcrumb renderer: escaped labels, links for every level but the current one, and the full
// id on hover for an abbreviated one (docs/ui/breadcrumb.md).
TEST(WebUi, BreadcrumbLinksEveryLevelButTheCurrentOne) {
    using lake::api::WebUiCrumb;
    const std::string html = lake::api::web_ui_breadcrumb(
        {{"Ship", "/ship/models", {}},
         {"Search Indexes", "/ship/indexes?a=1&b=2", {}},
         lake::api::web_ui_entity_crumb("5b42a297-cc49-429c-8f42-7acf0685bf96"),
         {"<b>", {}, {}}});
    EXPECT_NE(html.find("aria-label=\"Breadcrumb\""), std::string::npos);
    EXPECT_NE(html.find("<a href=\"/ship/models\">Ship</a>"), std::string::npos) << html;
    EXPECT_NE(html.find("<a href=\"/ship/indexes?a=1&amp;b=2\">Search Indexes</a>"),
              std::string::npos)
        << html;
    EXPECT_NE(html.find("title=\"5b42a297-cc49-429c-8f42-7acf0685bf96\">5b42a297…"),
              std::string::npos)
        << html;
    EXPECT_NE(html.find("<span aria-current=\"page\">&lt;b&gt;</span>"), std::string::npos) << html;
    EXPECT_EQ(lake::api::web_ui_breadcrumb({}), "");
    EXPECT_EQ(lake::api::web_ui_entity_crumb("retention-review").label, "retention-review");
    EXPECT_TRUE(lake::api::web_ui_entity_crumb("retention-review").full.empty());

    // Without its own trail a page sits under its sidebar item and group, then its own level.
    lake::api::WebUiViewer viewer(true);
    viewer.path = "/organizations";
    const std::string page =
        lake::api::web_ui_shell("lake — organization settings", "Acme", "<p>x</p>", viewer);
    EXPECT_NE(page.find("<a href=\"/organizations\">Organization</a>"), std::string::npos);
    EXPECT_NE(page.find("<a href=\"/organizations\">Organizations</a>"), std::string::npos);
    EXPECT_NE(page.find("<span aria-current=\"page\">Acme</span>"), std::string::npos);
}

// The MCP page: a key form until a key exists, then the key once and in every setup block; one
// Copy button per block; the tools by group (docs/api/mcp.md).
TEST(WebUi, McpPageShowsTheKeyOnceInEveryBlockAndTheToolsByGroup) {
    lake::api::McpPage page{.origin = "https://lake.example"};
    page.groups.push_back(
        {"Services", "Databases.", {{"create_service", "Create a service", "", true, true}}});
    std::string html = lake::api::web_ui_mcp(page, true);
    EXPECT_NE(html.find("action=\"/developer/mcp/key\""), std::string::npos);
    EXPECT_NE(html.find("Bearer $LAKE_KEY"), std::string::npos);
    EXPECT_NE(html.find("<code>create_service</code></td><td>Create a service</td><td>write"),
              std::string::npos);
    EXPECT_NE(html.find("href=\"/developer/mcp\" aria-current=\"page\""), std::string::npos)
        << "Developer → MCP is in the sidebar";
    EXPECT_NE(html.find("id=\"copy-buttons\""), std::string::npos);

    page.key = "lake_tok_abc";
    html = lake::api::web_ui_mcp(page, true);
    EXPECT_EQ(html.find("action=\"/developer/mcp/key\""), std::string::npos);
    EXPECT_NE(html.find("<code id=\"mcp-key\">lake_tok_abc</code>"), std::string::npos);
    EXPECT_NE(html.find("claude mcp add --transport http lake https://lake.example/mcp --header "
                        "&quot;Authorization: Bearer lake_tok_abc&quot;"),
              std::string::npos);
    EXPECT_EQ(html.find("$LAKE_KEY"), std::string::npos);
    std::size_t buttons = 0;
    for (auto at = html.find("data-copy=\""); at != std::string::npos;
         at = html.find("data-copy=\"", at + 1)) {
        ++buttons;
    }
    EXPECT_EQ(buttons, 5u) << "the key, Claude Code, Cursor, Claude Desktop and curl";
}

TEST(WebUi, NavigationMatchesVisitorAccountAndOperatorAccess) {
    const std::string visitor = lake::api::web_ui_home();
    EXPECT_NE(visitor.find("Public Repositories"), std::string::npos);
    EXPECT_NE(visitor.find("href=\"/sign-in\""), std::string::npos);
    EXPECT_NE(visitor.find("href=\"/sign-up\""), std::string::npos);
    for (const std::string_view path : {"/shared-with-me",
                                        "/search",
                                        "/customers",
                                        "/organizations",
                                        "/applications",
                                        "/metering",
                                        "/secret-mounts",
                                        "/audit",
                                        "/settings",
                                        "/sessions",
                                        "/passkeys",
                                        "/tokens",
                                        "/v1/openapi.json",
                                        "/login"}) {
        EXPECT_EQ(visitor.find("href=\"" + std::string(path) + "\""), std::string::npos) << path;
    }
    const std::string account = lake::api::web_ui_home(true);
    EXPECT_EQ(account.find("Administration"), std::string::npos);
    EXPECT_NE(account.find("href=\"/tokens\""), std::string::npos);
    EXPECT_NE(account.find("href=\"/shared-with-me\""), std::string::npos);
    EXPECT_NE(account.find("href=\"/ship/apps\""), std::string::npos);
    const auto streams = account.find("href=\"/train/streams\"");
    ASSERT_NE(streams, std::string::npos);
    const auto streams_end = account.find("</a>", streams);
    ASSERT_NE(streams_end, std::string::npos);
    const std::string streams_link = account.substr(streams, streams_end - streams);
    EXPECT_NE(streams_link.find("<svg class=\"icon\""), std::string::npos);
    EXPECT_NE(streams_link.find("<span>Streams</span>"), std::string::npos);
    EXPECT_NE(account.find("action=\"/logout\""), std::string::npos);
    EXPECT_NE(account.find("aria-current=\"page\""), std::string::npos);
    const std::string administrator = lake::api::web_ui_home({true, true});
    EXPECT_NE(administrator.find("Administration"), std::string::npos);
    EXPECT_NE(administrator.find("href=\"/customers\""), std::string::npos);
    for (const std::string_view path :
         {"/shared-with-me", "/settings", "/sessions", "/passkeys", "/tokens"}) {
        EXPECT_EQ(administrator.find("href=\"" + std::string(path) + "\""), std::string::npos)
            << path;
    }
    EXPECT_EQ(lake::api::web_ui_home({false, true}).find("Administration"), std::string::npos);
    expect_theme_scripts(account);
    const lake::control::Repository repository{
        .id = "r1", .name = "public-project", .visibility = "public"};
    const lake::api::RepositoryHomeData data;
    const std::string public_home = lake::api::web_ui_repository(repository, nullptr, data, false);
    for (const std::string_view path :
         {"/environments?", "/webhooks?", "/protection?", "/repository/access?"}) {
        EXPECT_EQ(public_home.find(path), std::string::npos) << path;
    }
    const std::string owner_home = lake::api::web_ui_repository(repository, nullptr, data, true);
    EXPECT_NE(owner_home.find("/webhooks?"), std::string::npos);
    EXPECT_EQ(owner_home.find("/protection?"), std::string::npos);
    const std::string operator_home =
        lake::api::web_ui_repository(repository, nullptr, data, {true, true});
    EXPECT_NE(operator_home.find("/protection?"), std::string::npos);
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
    EXPECT_NE(html.find("datetime=\"2026-09-04T00:00:00Z\">2026-09-04 00:00 UTC"),
              std::string::npos);
    EXPECT_NE(html.find("customer&lt;&amp;&gt;"), std::string::npos);
    EXPECT_EQ(html.find("customer<&>"), std::string::npos);
}

TEST(WebUi, UsageUsesPlainUnitsAndNamesWhatIsNotMeasured) {
    nlohmann::json usage{
        {"customer_id", "space-1"},
        {"currency", "USD"},
        {"quota_bytes", 0},
        {"over_quota", false},
        {"cost_micros", 12'345'678},
        {"summary",
         {{"storage",
           {{"repositories_bytes", 2048},
            {"images_bytes", 3 * 1024 * 1024},
            {"workspace_data_bytes", 5 * 1024},
            {"total_bytes", 2048 + 3 * 1024 * 1024 + 5 * 1024}}},
          {"compute",
           {{"repository_build_seconds", 45},
            {"image_build_seconds", 125},
            {"image_builds", 2},
            {"workspace_run_seconds", 3 * 3600 + 4 * 60},
            {"workspace_runs", 7},
            {"total_seconds", 45 + 125 + 3 * 3600 + 4 * 60}}},
          {"workspaces",
           nlohmann::json::array({{{"name", "main<1>"},
                                   {"measured", true},
                                   {"data_bytes", 5 * 1024},
                                   {"datasets", 2},
                                   {"run_seconds", 3 * 3600 + 4 * 60},
                                   {"runs", 7}},
                                  {{"name", "paused"},
                                   {"measured", false},
                                   {"message", "workspace_id is not running"}}})},
          {"not_measured",
           nlohmann::json::array({"Model files and indexes inside a workspace: not yet."})},
          {"priced", true}}},
        {"items",
         nlohmann::json::array({{{"hour", "2026-09-04T00:00:00Z"},
                                 {"stored_bytes", 2048},
                                 {"transfer_in_bytes", 0},
                                 {"transfer_out_bytes", 1536},
                                 {"compute_seconds", 45},
                                 {"cost_micros", 7}}})}};
    std::string html = lake::api::web_ui_metering(usage);
    EXPECT_NE(html.find("<h1>Usage</h1>"), std::string::npos);
    EXPECT_EQ(html.find("economics"), std::string::npos);
    EXPECT_EQ(html.find("micro-"), std::string::npos);
    EXPECT_NE(html.find("<dt>Container images</dt><dd>3.0 MiB</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Workspace data</dt><dd>5.0 KiB</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Repositories</dt><dd>2.0 KiB</dd>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Repository storage limit</dt><dd>No limit</dd>"), std::string::npos);
    EXPECT_NE(html.find("2 min 5 s across 2 builds, including time queued"), std::string::npos);
    EXPECT_NE(html.find("3 h 4 min across 7 runs"), std::string::npos);
    EXPECT_NE(html.find("main&lt;1&gt;"), std::string::npos);
    EXPECT_NE(html.find("Not measured: workspace_id is not running"), std::string::npos);
    EXPECT_NE(html.find("<h2>Not Measured Yet</h2>"), std::string::npos);
    EXPECT_NE(html.find("12.35 USD"), std::string::npos);
    EXPECT_NE(html.find("less than 0.01 USD"), std::string::npos);
    EXPECT_NE(html.find("<td>1.5 KiB</td>"), std::string::npos);

    usage["summary"]["priced"] = false;
    usage["summary"]["storage"]["images_bytes"] = nullptr;
    html = lake::api::web_ui_metering(usage);
    EXPECT_NE(html.find("This lake has no prices set"), std::string::npos);
    EXPECT_EQ(html.find("<th>Charge</th>"), std::string::npos);
    EXPECT_NE(html.find("<dt>Container images</dt><dd>Not measured</dd>"), std::string::npos);
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

TEST(WebUi, OrganizationsPageCreatesJoinsAndConvertsWithoutNamingEndpoints) {
    lake::api::OrganizationsPageData data;
    data.person = true;
    const std::string empty = lake::api::web_ui_organizations(data);
    EXPECT_NE(empty.find("action=\"/organizations/create\""), std::string::npos);
    EXPECT_NE(empty.find("You are not in an organization yet"), std::string::npos);
    EXPECT_EQ(empty.find("action=\"/organizations/convert\""), std::string::npos);
    EXPECT_EQ(empty.find("POST /v1/"), std::string::npos);
    expect_theme_scripts(empty);

    data.organizations.push_back(
        {"o1", "", "Acme", "2026-10-01T00:00:00Z", "2026-10-01T00:00:00Z"});
    data.memberships.push_back(
        {"m1", "o1", "", "user", "u1", "taap:1:user", "owner", "e", "g", "c", "u"});
    lake::control::Invitation invitation;
    invitation.id = "i<1>";
    invitation.organization_id = "o2";
    invitation.role = "admin";
    invitation.expires_at = "2026-10-09T00:00:00Z";
    data.invitations.push_back({invitation, "<Other Org>"});
    data.personal_space =
        lake::control::Customer{"c1", "individual", "u1", "", "brave-otter-7", "c", "u"};
    const std::string page = lake::api::web_ui_organizations(data);
    EXPECT_NE(page.find("Invitations for You"), std::string::npos);
    EXPECT_NE(page.find("&lt;Other Org&gt;"), std::string::npos);
    EXPECT_NE(page.find("value=\"i&lt;1&gt;\""), std::string::npos);
    EXPECT_NE(page.find("action=\"/invitations/accept\""), std::string::npos);
    EXPECT_NE(page.find("/organization?id=o1&amp;tab=invitations"), std::string::npos);
    EXPECT_NE(page.find("<span class=\"badge\">owner</span>"), std::string::npos);
    EXPECT_NE(page.find("action=\"/organizations/convert\""), std::string::npos);
    EXPECT_NE(page.find("Type <code>brave-otter-7</code> to confirm"), std::string::npos);

    lake::api::OrganizationsPageData operator_view;
    const std::string administration = lake::api::web_ui_organizations(operator_view);
    EXPECT_EQ(administration.find("action=\"/organizations/create\""), std::string::npos);
    EXPECT_NE(administration.find("No organizations yet"), std::string::npos);

    for (const std::string& empty_state :
         {lake::api::web_ui_teams("o1", {}),
          lake::api::web_ui_memberships("o1", "", {}),
          lake::api::web_ui_invitations("o1", "", {}),
          lake::api::web_ui_grants({"p1", "team", "t1", "", "c", "u", true}, {}),
          lake::api::web_ui_customers({})}) {
        EXPECT_EQ(empty_state.find("POST /v1/"), std::string::npos) << empty_state;
    }
}

TEST(WebUi, SettingsPagesExposeTabsMutationsAndConfirmation) {
    const lake::control::Organization organization{
        "o1", "taap:1:ed25519:org", "Orama", "created", "updated"};
    const lake::control::Membership membership{"m1",
                                               "o1",
                                               "",
                                               "user",
                                               "u1",
                                               "taap:1:ed25519:user",
                                               "owner",
                                               "digest<&>",
                                               "taap:1:ed25519:owner",
                                               "created",
                                               "updated"};
    lake::api::OrganizationSettingsData data;
    data.memberships.push_back(membership);
    const std::string organization_page =
        lake::api::web_ui_organization(organization, data, "invitations");
    EXPECT_NE(organization_page.find("Organization settings"), std::string::npos);
    EXPECT_NE(organization_page.find("action=\"/organization/invitations\""), std::string::npos);
    expect_theme_scripts(organization_page);

    const std::string teams_page = lake::api::web_ui_organization(organization, data, "teams");
    EXPECT_NE(teams_page.find("action=\"/organization/teams\""), std::string::npos);
    const std::string sso_page = lake::api::web_ui_organization(organization, data, "sso");
    EXPECT_NE(sso_page.find("action=\"/organization/sso\""), std::string::npos);
    expect_theme_scripts(sso_page);

    const std::string member_page = lake::api::web_ui_member(membership, {}, "", true);
    EXPECT_NE(member_page.find("action=\"/member/role\""), std::string::npos);
    EXPECT_NE(member_page.find("action=\"/member/remove\""), std::string::npos);
    EXPECT_NE(member_page.find("derived grants immediately"), std::string::npos);
    EXPECT_NE(member_page.find("digest&lt;&amp;&gt;"), std::string::npos);
    expect_theme_scripts(member_page);

    const std::string personal_page = lake::api::web_ui_settings("taap:1:user", {}, {}, "tokens");
    EXPECT_NE(personal_page.find("Personal settings"), std::string::npos);
    EXPECT_NE(personal_page.find("href=\"/tokens\""), std::string::npos);
    expect_theme_scripts(personal_page);
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
    EXPECT_NE(page.find("/organization?id=o1&amp;tab=sso"), std::string::npos);
    EXPECT_EQ(page.find("POST /v1/"), std::string::npos);
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

    repo.deleted_at.clear();
    repo.retention_until.clear();
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
    EXPECT_NE(home.find("Recent Changes"), std::string::npos);
    EXPECT_NE(home.find("id=\"lake\""), std::string::npos);
    EXPECT_NE(home.find("Safe &lt;text&gt;"), std::string::npos);
    EXPECT_NE(home.find("Clone and Push"), std::string::npos);
    EXPECT_NE(home.find("C++"), std::string::npos);
    EXPECT_NE(home.find("Markdown"), std::string::npos);
    EXPECT_NE(home.find("Effective Access"), std::string::npos);
    EXPECT_NE(home.find("action=\"/repository/build\""), std::string::npos);
    EXPECT_NE(home.find("Build image"), std::string::npos);
}

TEST(WebUi, BuildPagesRenderLiveHistoryWithoutExposingCredentials) {
    const nlohmann::json catalog{
        {"oci", {{{"name", "ada/service"}, {"versions", nlohmann::json::array({"latest"})}}}},
        {"npm", nlohmann::json::array()},
        {"python", nlohmann::json::array()}};
    const nlohmann::json builds = nlohmann::json::array({{{"id", "build-1"},
                                                          {"repository", "repo-1"},
                                                          {"target", "registry/ada/service:latest"},
                                                          {"phase", "Running"},
                                                          {"digest", ""}}});
    const std::string packages =
        lake::api::web_ui_packages(catalog, true, "https://lake.test", true, builds, "customer-1");
    EXPECT_NE(packages.find("ada/service"), std::string::npos);
    EXPECT_NE(packages.find("data-build-id=\"build-1\""), std::string::npos);
    EXPECT_NE(packages.find("receive_build_events"), std::string::npos);
    EXPECT_EQ(packages.find("token_hashes"), std::string::npos);
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
        .outputs = {{"build/app", 0755, "blob-3"}},
        .attestations = {{"attestation-1", "slsa_provenance/1", "subject", "issuer", true, ""}},
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
    EXPECT_NE(change_page.find("href=\"#readme-md-new-2\">line 2</a>"), std::string::npos);
    EXPECT_NE(change_page.find("1 attestations"), std::string::npos);
    EXPECT_NE(change_page.find("outputs: <code>build/app</code>"), std::string::npos);
    EXPECT_NE(change_page.find("attested: <code>slsa_provenance/1</code>"), std::string::npos);
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

    const lake::api::RepositoryConflict conflict{
        .id = "conflict<&>",
        .base = "base",
        .left = "left",
        .right = "right",
        .region = {.path = "<config>"},
        .kind = "crdt-suspicious",
        .explanation = "Two offline lock records disagree <now>.",
        .reasons = {"The holders differ."},
    };
    const std::string conflicts =
        lake::api::web_ui_repository_conflicts(repository, {conflict}, false);
    EXPECT_NE(conflicts.find("&lt;config&gt;"), std::string::npos);
    EXPECT_NE(conflicts.find("Suspicious concurrent update"), std::string::npos);
    EXPECT_NE(conflicts.find("Two offline lock records disagree &lt;now&gt;."), std::string::npos);
    EXPECT_NE(conflicts.find("The holders differ."), std::string::npos);
    EXPECT_EQ(conflicts.find("/conflicts/resolve"), std::string::npos);

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
    EXPECT_NE(artifacts.find("Build Results"), std::string::npos);
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
    EXPECT_NE(detail.find("Linked Changes"), std::string::npos);
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
    EXPECT_NE(protection.find("Refused Updates"), std::string::npos);
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

TEST(WebUi, ActivityPageNamesPrincipalsAndWorkspaceCapabilities) {
    lake::audit::Event event{
        .id = "event-1",
        .occurred_at = "2026-10-01T00:00:00.000000Z",
        .draft = {.kind = "workspace.act",
                  .principal = "taap:1:ed25519:person",
                  .object = "customer/c",
                  .action = "workspace_act",
                  .outcome = "success"},
        .digest = "abcdef",
    };
    const nlohmann::json items = nlohmann::json::array(
        {{{"id", "event-1"},
          {"principal_name", "Project Agent for <Ada>"},
          {"capability", "create_agent"}}});
    lake::api::WebUiViewer viewer{true, false};
    viewer.path = "/activity";
    const lake::audit::Filter filter{.newest_first = true};
    const std::string page = lake::api::web_ui_audit(
        {event}, filter, nlohmann::json::object(), viewer, lake::api::audit_page_view(items, false));
    EXPECT_NE(page.find("Project Agent for &lt;Ada&gt;"), std::string::npos);
    EXPECT_NE(page.find("title=\"taap:1:ed25519:person\""), std::string::npos);
    EXPECT_NE(page.find("workspace_act <code>create_agent</code>"), std::string::npos);
    EXPECT_NE(page.find("value=\"newest\" selected"), std::string::npos);
    EXPECT_NE(page.find("name=\"reads\" value=\"all\">"), std::string::npos)
        << "reads are offered, unchecked";
    EXPECT_NE(page.find("Successful reads"), std::string::npos);
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

TEST(WebUi, SharedLinkProvidesScopedNavigationPreviewAndDownload) {
    const std::vector<lake::api::RepositoryTreeEntry> entries{{"shared/child", "tree"},
                                                              {"shared/hello & bye.txt", "blob"},
                                                              {"shared/conflict", "conflict"}};
    const std::string page =
        lake::api::web_ui_drive_link("shared", entries, "", "token<&", "release one", "shared");
    EXPECT_NE(page.find("View – browse and download"), std::string::npos);
    EXPECT_NE(page.find("/share/token%3C%26?ref=release%20one&amp;path=shared%2Fchild"),
              std::string::npos);
    EXPECT_NE(page.find("/share/token%3C%26/preview?"), std::string::npos);
    EXPECT_NE(page.find("hello &amp; bye.txt"), std::string::npos);
    EXPECT_NE(page.find("download>Download"), std::string::npos);
    EXPECT_EQ(page.find("Up one folder"), std::string::npos);
    EXPECT_EQ(page.find("repository_id="), std::string::npos);
    const std::string child =
        lake::api::web_ui_drive_link("shared/child", {}, "", "token", "main", "shared");
    EXPECT_NE(child.find("Up one folder"), std::string::npos);
    EXPECT_NE(child.find("This folder is empty"), std::string::npos);
    const lake::api::RepositoryFile file{"shared/child/hello.txt", "blob", "hello", 5};
    const auto preview = lake::api::DriveFormatRegistry::defaults().preview(file);
    ASSERT_TRUE(preview);
    const std::string opened =
        lake::api::web_ui_drive_link_preview("token", "main", file, *preview);
    EXPECT_NE(opened.find("Back to folder"), std::string::npos);
    EXPECT_NE(opened.find("/share/token/download?ref=main&amp;path=shared%2Fchild%2Fhello.txt"),
              std::string::npos);
    EXPECT_NE(opened.find("hello</pre>"), std::string::npos);
    EXPECT_EQ(opened.find("repository_id="), std::string::npos);
    const std::string denied =
        lake::api::web_ui_drive_link("Shared folder", {}, "expired or revoked");
    EXPECT_NE(denied.find("expired or revoked"), std::string::npos);
    EXPECT_EQ(denied.find("download>Download"), std::string::npos);
}

TEST(WebUi, NavigationFollowsTheRequestPath) {
    lake::api::WebUiViewer viewer{true};
    viewer.path = "/drive/preview";
    const std::string drive = lake::api::web_ui_home(viewer);
    EXPECT_NE(drive.find("href=\"/repositories\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(drive.find("href=\"/\" aria-current=\"page\""), std::string::npos);
    viewer.path = "/deployments";
    const std::string deployments = lake::api::web_ui_home(viewer);
    EXPECT_NE(deployments.find("href=\"/deployments\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(deployments.find("href=\"/repositories\" aria-current=\"page\""), std::string::npos);
    viewer.path = "/ship/app";
    const std::string app = lake::api::web_ui_home(viewer);
    EXPECT_NE(app.find("href=\"/ship/apps\" aria-current=\"page\""), std::string::npos);
    viewer.path = "/drivers";
    // No sidebar item owns the path (the breadcrumb still marks its own current level).
    const std::string unowned = lake::api::web_ui_home(viewer);
    const std::string sidebar = unowned.substr(
        unowned.find("primary-nav"), unowned.find("</nav>") - unowned.find("primary-nav"));
    EXPECT_EQ(sidebar.find("aria-current=\"page\""), std::string::npos);
    viewer.path = "/";
    EXPECT_NE(lake::api::web_ui_home(viewer).find("href=\"/\" aria-current=\"page\""),
              std::string::npos);
}

TEST(WebUi, AccountPagesLiveInTheAccountMenu) {
    const std::string account = lake::api::web_ui_home(true);
    const auto menu = account.find("<details class=\"account-menu\">");
    ASSERT_NE(menu, std::string::npos);
    const auto sidebar_end = account.find("</aside>");
    ASSERT_NE(sidebar_end, std::string::npos);
    for (const std::string_view path : {"/settings", "/sessions", "/passkeys", "/tokens"}) {
        const auto link = account.find("href=\"" + std::string(path) + "\"");
        ASSERT_NE(link, std::string::npos) << path;
        EXPECT_GT(link, sidebar_end) << path;
    }
    EXPECT_NE(account.find("<p class=\"nav-label\">Ship</p>"), std::string::npos);
    EXPECT_EQ(lake::api::web_ui_home({true, true}).find("account-menu"), std::string::npos);
    EXPECT_EQ(lake::api::web_ui_home().find("account-menu"), std::string::npos);
}

TEST(WebUi, AgentTurnsLinkToFilteredAgentEvents) {
    lake::api::AskPage ask;
    ask.customer_id = "customer-1";
    ask.task_id = "task-ask";
    const std::string ask_html = lake::api::web_ui_ask(ask, true);
    EXPECT_NE(ask_html.find("topic=agent-events&amp;task=task-ask"), std::string::npos);

    lake::api::FactoryPage factory;
    factory.customer_id = "customer-1";
    factory.task_id = "task-factory";
    const std::string factory_html = lake::api::web_ui_factory(factory, true);
    EXPECT_NE(factory_html.find("topic=agent-events&amp;task=task-factory"), std::string::npos);
}

TEST(WebUi, ShipAppsShowAFailedAgentTaskInsteadOfDroppingThePage) {
    // A failed task carries "answer": null. Rendering it must not throw (that dropped the
    // connection and surfaced as a 502), and the person sees why it failed.
    lake::api::AppsPage list;
    list.customer_id = "customer-1";
    list.customers = {{"customer-1", "Personal", true}};
    list.application_agent = true;
    list.agent_task = {{"task_id", "task-1"},
                       {"agent", "agent-application"},
                       {"kind", "app"},
                       {"status", "failed"},
                       {"answer", nullptr},
                       {"message", "create_app: LAKE_APPS_WILDCARD is not configured"}};
    std::string apps;
    ASSERT_NO_THROW(apps = lake::api::web_ui_apps(list, true));
    EXPECT_NE(apps.find("LAKE_APPS_WILDCARD is not configured"), std::string::npos) << apps;
    EXPECT_NE(apps.find("topic=agent-events&amp;task=task-1"), std::string::npos) << apps;
}

TEST(WebUi, ShipAppsRenderADeployedAppWhoseUrlIsNotKnownYet) {
    // A deployed App carries "url": null until its rollout reports one. Reading it as a string
    // threw, which dropped the connection (a 502 at the edge).
    lake::api::AppsPage list;
    list.customer_id = "customer-1";
    list.customers = {{"customer-1", "Personal", true}};
    list.application_agent = true;
    list.agent_task = {
        {"task_id", "task-deployed"},
        {"status", "succeeded"},
        {"answer",
         {{"reply", "The App is deployed."},
          {"result",
           {{"confirm", false},
            {"app",
             {{"name", "e2e-webhook"},
              {"host", "e2e-webhook.apps.example.test"},
              {"url", nullptr},
              {"rollout", {{"type", "deploy"}, {"sequence", 2}}}}}}}}}};
    std::string apps;
    ASSERT_NO_THROW(apps = lake::api::web_ui_apps(list, true));
    EXPECT_NE(apps.find("The App is deployed."), std::string::npos) << apps;
    EXPECT_NE(apps.find("href=\"https://e2e-webhook.apps.example.test\""), std::string::npos)
        << apps;

    list.agent_task["answer"]["reply"] = nullptr;
    list.agent_task["message"] = "done";
    ASSERT_NO_THROW(apps = lake::api::web_ui_apps(list, true));
    EXPECT_NE(apps.find("done"), std::string::npos);
}

TEST(WebUi, ShipAppsShowAReplyBoxUnderAnAgentQuestion) {
    lake::api::AppsPage list;
    list.customer_id = "customer-1";
    list.customers = {{"customer-1", "Personal", true}};
    list.application_agent = true;
    list.agent_reply = true;
    list.agent_task = {{"task_id", "task-question"},
                       {"status", "succeeded"},
                       {"answer",
                        {{"reply", "Which dataset should the app use?"},
                         {"result", {{"kind", "question"}, {"confirm", false}}}}}};
    const std::string apps = lake::api::web_ui_apps(list, true);
    EXPECT_NE(apps.find("Which dataset should the app use?"), std::string::npos);
    EXPECT_NE(apps.find("<h3>Reply</h3>"), std::string::npos);
    EXPECT_NE(apps.find("name=\"task_id\" value=\"task-question\""), std::string::npos);
    EXPECT_NE(apps.find("name=\"reply\""), std::string::npos);
}

TEST(WebUi, ShipAppsRenderControlsMaskedSecretsAndLiveEvents) {
    lake::api::AppsPage list;
    list.customer_id = "customer-1";
    list.customers = {{"customer-1", "Personal", true}};
    list.application_agent = true;
    list.apps = {{{"id", "app-1"},
                  {"name", "customer-portal"},
                  {"mode", "runtime"},
                  {"source", {{"repository", "repo-1"}, {"revision", "main"}}},
                  {"phase", "Running"},
                  {"ready", true},
                  {"url", "https://portal.example.test"},
                  {"bindings", {{"zetta", {{"scopes", {"read"}}}}, {"services", {"db-1"}}}}}};
    const std::string apps = lake::api::web_ui_apps(list, true);
    EXPECT_NE(apps.find("<h1>Apps</h1>"), std::string::npos);
    EXPECT_NE(apps.find("customer-portal"), std::string::npos);
    EXPECT_NE(apps.find("Zetta, 1 service"), std::string::npos);
    EXPECT_EQ(apps.find("ApplicationAgent is not registered"), std::string::npos);
    EXPECT_NE(apps.find("Start ApplicationAgent"), std::string::npos);

    list.agent_task = {
        {"task_id", "task-1"},
        {"status", "succeeded"},
        {"answer",
         {{"reply", "Review it."},
          {"result",
           {{"kind", "proposal"},
            {"confirm", true},
            {"files", nlohmann::json::array({{{"path", "app.py"}, {"contents", "ok"}}})},
            {"app",
             {{"name", "health-report"},
              {"mode", "runtime"},
              {"framework", "fastapi"},
              {"auth", "lake"},
              {"repository", {{"name", "app-health-report"}}},
              {"path", "."}}}}}}}};
    list.proposal_sha256 = std::string(64, 'a');
    const std::string proposal = lake::api::web_ui_apps(list, true);
    EXPECT_NE(proposal.find("Proposed App: health-report"), std::string::npos);
    EXPECT_NE(proposal.find("app.py"), std::string::npos);
    EXPECT_NE(proposal.find("Confirm and deploy"), std::string::npos);
    EXPECT_NE(proposal.find(std::string(64, 'a')), std::string::npos);

    lake::api::AppPage detail;
    detail.customer_id = "customer-1";
    detail.app = {
        {"id", "app-1"},
        {"name", "customer-portal"},
        {"mode", "runtime"},
        {"source",
         {{"repository", "repo-1"}, {"revision", "main"}, {"path", "."}, {"entrypoint", ""}}},
        {"replicas", 2},
        {"desired", "running"},
        {"generation", 3},
        {"observed_generation", 3},
        {"phase", "Running"},
        {"ready", true},
        {"message", "Ready"},
        {"env", {{"REGION", "ca-west"}}},
        {"secret_env_refs", {{"API_TOKEN", "secret/app-token"}}},
        {"bindings", {{"zetta", {{"scopes", {"read"}}}}, {"services", {"db-1"}}}}};
    detail.events = {{{"sequence", 4},
                      {"type", "deploy"},
                      {"generation", 3},
                      {"principal", "user:1"},
                      {"message", "Ready"},
                      {"status",
                       {{"source",
                         {{"repository", "repo-1"},
                          {"revision", "main"},
                          {"path", "."},
                          {"entrypoint", ""}}}}}}};
    detail.builds_available = true;
    detail.builds = {{{"id", "build-1"},
                      {"phase", "Building"},
                      {"revision", "main"},
                      {"target", "lake.test/customer-1/customer-portal:build-1"},
                      {"logs", {{"lines", nlohmann::json::array({"step one"})}}}}};
    const std::string app = lake::api::web_ui_app(detail, true);
    EXPECT_NE(app.find("action=\"/ship/app/restart\""), std::string::npos);
    EXPECT_NE(app.find("action=\"/ship/app/rollback\""), std::string::npos);
    EXPECT_NE(app.find("build-1"), std::string::npos);
    EXPECT_NE(app.find("Building"), std::string::npos);
    EXPECT_NE(app.find("step one"), std::string::npos);
    EXPECT_NE(app.find("receive_build_events"), std::string::npos);
    EXPECT_NE(app.find("••••••"), std::string::npos);
    EXPECT_EQ(app.find("secret/app-token"), std::string::npos);
    EXPECT_NE(app.find("id=\"app-live-events\""), std::string::npos);
    EXPECT_NE(app.find("receive_app_events"), std::string::npos);
}

TEST(WebUi, RepositoryTabsOpenOnFilesAndNameDeployments) {
    const lake::control::Repository repository{.id = "r1", .name = "data", .visibility = "public"};
    const lake::api::RepositoryHomeData data;
    const std::string owner = lake::api::web_ui_repository(repository, nullptr, data, true);
    const auto files = owner.find(">Files</a>");
    const auto code = owner.find(">Code</a>");
    ASSERT_NE(files, std::string::npos);
    ASSERT_NE(code, std::string::npos);
    EXPECT_LT(files, code);
    EXPECT_NE(owner.find("href=\"/drive?repository_id=r1\""), std::string::npos);
    EXPECT_NE(owner.find(">Deployments</a>"), std::string::npos);
    EXPECT_EQ(owner.find(">Platform</a>"), std::string::npos);
    const std::string visitor = lake::api::web_ui_repository(repository, nullptr, data, false);
    EXPECT_EQ(visitor.find(">Files</a>"), std::string::npos);
}

TEST(WebUi, DeploymentsListEveryRepositoryEnvironment) {
    const lake::control::Repository repository{.id = "r 1", .name = "svc<&>"};
    const nlohmann::json environment{{"name", "production"},
                                     {"phase", "Running"},
                                     {"change", "7f3c2a1"},
                                     {"url", "https://svc.example.test"},
                                     {"updated_at", "2026-09-25T00:00:00Z"}};
    const std::string out = lake::api::web_ui_deployments({{repository, environment}}, "", true);
    EXPECT_NE(out.find("<h1>Deployments</h1>"), std::string::npos);
    EXPECT_NE(out.find("svc&lt;&amp;&gt;"), std::string::npos);
    EXPECT_EQ(out.find("svc<&>"), std::string::npos);
    EXPECT_NE(out.find("href=\"/environment?repository_id=r%201&amp;name=production\""),
              std::string::npos);
    EXPECT_NE(out.find("Running"), std::string::npos);
    EXPECT_NE(lake::api::web_ui_deployments({}, "", true).find("Nothing is deployed yet"),
              std::string::npos);
    expect_theme_scripts(out);
}
