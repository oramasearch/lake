#include <gtest/gtest.h>

#include "lake/api/web_ui.hpp"
#include "lake/version.hpp"

TEST(WebUi, EscapesHtml) {
    EXPECT_EQ(lake::api::web_ui_escape("<a href=\"x\">&'"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
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
