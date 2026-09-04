#include <gtest/gtest.h>

#include "lake/api/web_ui.hpp"
#include "lake/version.hpp"

TEST(WebUi, EscapesHtml) {
  EXPECT_EQ(lake::api::web_ui_escape("<a href=\"x\">&'"),
            "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
}

TEST(WebUi, HelloIsASelfContainedDocument) {
  const auto html = lake::api::web_ui_hello("node-1", lake::version_string());
  EXPECT_TRUE(html.starts_with("<!doctype html>"));
  EXPECT_NE(html.find("hello from lake"), std::string::npos);
  EXPECT_NE(html.find("node-1"), std::string::npos);
  EXPECT_NE(html.find("/style.css"), std::string::npos);
  EXPECT_EQ(html.find("<script"), std::string::npos);
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
  EXPECT_NE(out.find("href=\"/search\""), std::string::npos);
  EXPECT_NE(out.find("href=\"/organizations\""), std::string::npos);
  EXPECT_NE(out.find("href=\"/login\""), std::string::npos);
  const std::string in = lake::api::web_ui_hello("n", "0.0.1", true);
  EXPECT_NE(in.find("action=\"/logout\""), std::string::npos);
  EXPECT_EQ(in.find("<script"), std::string::npos);
}

TEST(WebUi, OrganizationAndPolicyPagesEscapeTheirData) {
  const lake::control::Organization organization{
      "o1", "taap:1:ed25519:org", "<Orama>", "2026-09-04T00:00:00Z",
      "2026-09-04T00:00:00Z"};
  const std::string organizations =
      lake::api::web_ui_organizations({organization});
  EXPECT_NE(organizations.find("&lt;Orama&gt;"), std::string::npos);
  EXPECT_NE(organizations.find("/teams?organization_id=o1"), std::string::npos);
  EXPECT_EQ(organizations.find("<script"), std::string::npos);

  const lake::control::PolicyObject object{"p1",      "team",    "t1", "",
                                           "created", "updated", true};
  const lake::control::EffectiveAccessEntry entry{
      "taap:1:ed25519:user", "p0", "admin", {"read", "write"}, true};
  const std::string access =
      lake::api::web_ui_effective_access(object, {entry});
  EXPECT_NE(access.find("inherited from"), std::string::npos);
  EXPECT_NE(access.find("read, write"), std::string::npos);
  EXPECT_EQ(access.find("<script"), std::string::npos);
}

TEST(WebUi, SearchEscapesHitsAndOffersTheQueryForm) {
  const lake::search::Hit hit{"customer", "repository", "<path>",
                              "change",   "<snippet>",  1.0};
  const std::string page = lake::api::web_ui_search("needle", {hit});
  EXPECT_NE(page.find("action=\"/search\""), std::string::npos);
  EXPECT_NE(page.find("&lt;path&gt;"), std::string::npos);
  EXPECT_NE(page.find("&lt;snippet&gt;"), std::string::npos);
  EXPECT_EQ(page.find("<path>"), std::string::npos);
}

TEST(WebUi, RepositoriesListEscapesAndLinks) {
  lake::control::Customer ada{"c1",
                              "individual",
                              "u1",
                              "",
                              "ada",
                              "2026-09-02T00:00:00Z",
                              "2026-09-02T00:00:00Z"};
  lake::control::Repository repo{"r1",
                                 "c1",
                                 "liquid",
                                 "<b>the tool</b>",
                                 "public",
                                 "2026-09-02T00:00:00Z",
                                 "2026-09-02T00:00:00Z"};
  const std::string out = lake::api::web_ui_repositories({repo}, {{"c1", ada}});
  EXPECT_NE(out.find("ada/liquid"), std::string::npos);
  EXPECT_NE(out.find("href=\"/repository?id=r1\""), std::string::npos);
  EXPECT_NE(out.find("&lt;b&gt;the tool&lt;/b&gt;"), std::string::npos);
  EXPECT_EQ(out.find("<b>the tool</b>"), std::string::npos);
  EXPECT_NE(lake::api::web_ui_repositories({}, {}).find("No repositories yet"),
            std::string::npos);
  const std::string page = lake::api::web_ui_repository(repo, &ada);
  EXPECT_NE(page.find("<h1>ada/liquid</h1>"), std::string::npos);
  EXPECT_NE(lake::api::web_ui_login("bad token").find("bad token"),
            std::string::npos);
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
    const std::string page = lake::api::web_ui_audit({event}, filter);
    EXPECT_NE(page.find("action=\"/audit\""), std::string::npos);
    EXPECT_NE(page.find("name=\"principal\""), std::string::npos);
    EXPECT_NE(page.find("&lt;admin&gt;"), std::string::npos);
    EXPECT_EQ(page.find("<admin>"), std::string::npos);
    EXPECT_NE(page.find("capability/list_users"), std::string::npos);
    EXPECT_NE(page.find("GET /v1/verify_audit"), std::string::npos);
}
