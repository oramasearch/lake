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
    EXPECT_NE(out.find("href=\"/organizations\""), std::string::npos);
    EXPECT_NE(out.find("href=\"/sign-in\""), std::string::npos);
    const std::string in = lake::api::web_ui_hello("n", "0.0.1", true);
    EXPECT_NE(in.find("action=\"/logout\""), std::string::npos);
    EXPECT_EQ(in.find("<script"), std::string::npos);
}

TEST(WebUi, OrganizationAndPolicyPagesEscapeTheirData) {
    const lake::control::Organization organization{
        "o1", "taap:1:ed25519:org", "<Orama>", "2026-09-04T00:00:00Z", "2026-09-04T00:00:00Z"};
    const std::string organizations = lake::api::web_ui_organizations({organization});
    EXPECT_NE(organizations.find("&lt;Orama&gt;"), std::string::npos);
    EXPECT_NE(organizations.find("/teams?organization_id=o1"), std::string::npos);
    EXPECT_EQ(organizations.find("<script"), std::string::npos);

    const lake::control::PolicyObject object{"p1", "team", "t1", "", "created", "updated", true};
    const lake::control::EffectiveAccessEntry entry{
        "taap:1:ed25519:user", "p0", "admin", {"read", "write"}, true};
    const std::string access = lake::api::web_ui_effective_access(object, {entry});
    EXPECT_NE(access.find("inherited from"), std::string::npos);
    EXPECT_NE(access.find("read, write"), std::string::npos);
    EXPECT_EQ(access.find("<script"), std::string::npos);
}

TEST(WebUi, IdentityFormsUseCapabilityBackedActions) {
    const std::string sign_up = lake::api::web_ui_sign_up("bad <identity>");
    EXPECT_NE(sign_up.find("action=\"/identity/sign-up\""), std::string::npos);
    EXPECT_NE(sign_up.find("bad &lt;identity&gt;"), std::string::npos);
    EXPECT_EQ(sign_up.find("<script"), std::string::npos);

    const std::string sign_in = lake::api::web_ui_sign_in("");
    EXPECT_NE(sign_in.find("action=\"/identity/sign-in\""), std::string::npos);
    EXPECT_NE(sign_in.find("action=\"/identity/oauth\""), std::string::npos);
    EXPECT_EQ(sign_in.find("<script"), std::string::npos);

    const std::string verify = lake::api::web_ui_mfa("", "", false);
    EXPECT_NE(verify.find("action=\"/identity/mfa/verify\""), std::string::npos);
    const std::string enroll = lake::api::web_ui_mfa("", "secret<&", true);
    EXPECT_NE(enroll.find("action=\"/identity/mfa/enroll\""), std::string::npos);
    EXPECT_NE(enroll.find("secret&lt;&amp;"), std::string::npos);
    EXPECT_EQ(enroll.find("<script"), std::string::npos);
}

TEST(WebUi, PasskeyPagesCarryOnlyTheirDocumentedScript) {
    const std::string registration = lake::api::web_ui_passkeys("");
    EXPECT_NE(registration.find("navigator.credentials.create"), std::string::npos);
    EXPECT_EQ(registration.find("<script"), registration.rfind("<script"));
    const std::string assertion = lake::api::web_ui_passkey_sign_in("");
    EXPECT_NE(assertion.find("navigator.credentials.get"), std::string::npos);
    EXPECT_EQ(assertion.find("<script"), assertion.rfind("<script"));
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
    EXPECT_EQ(tokens.find("<script"), std::string::npos);
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
