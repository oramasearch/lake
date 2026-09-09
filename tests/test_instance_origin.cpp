#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "lake/api/server.hpp"
#include "lake/api/web_ui.hpp"
#include "postgres_fixture.hpp"
#include "temporary_directory.hpp"

namespace {

std::string shell_argument(std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

TEST(InstanceOrigin, BrowserToTerminal) {
    const char* liquid = std::getenv("LAKE_TEST_LIQUID");
    if (!liquid || !*liquid) {
        GTEST_SKIP() << "Set LAKE_TEST_LIQUID to the pinned Liquid CLI";
    }
    const char* database = lake::test::postgres_uri();
    if (!database) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const auto root = std::filesystem::absolute(lake::test::fixture_path("instance-origin"));
    ASSERT_EQ(setenv("LAKE_ORIGIN_DB_URI", database, 1), 0);
    ASSERT_EQ(setenv("LAKE_ORIGIN_ROOT", root.c_str(), 1), 0);
    const auto script = std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "tests/instance_origin.mjs";
    const std::string command = shell_argument(LAKE_TEST_NODE) + " " +
                                shell_argument(script.string()) + " " +
                                shell_argument(LAKE_TEST_SERVER_PATH);
    const int result = std::system(command.c_str());
    unsetenv("LAKE_ORIGIN_DB_URI");
    unsetenv("LAKE_ORIGIN_ROOT");
    EXPECT_EQ(result, 0) << "The browser-to-terminal proof failed";
}

class InvalidInstanceOrigin : public testing::TestWithParam<const char*> {};

TEST_P(InvalidInstanceOrigin, RefusesStartup) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.external_origin = GetParam();
    lake::api::ApiServer server(options);
    EXPECT_FALSE(server.start());
    EXPECT_NE(server.reason().find("LAKE_EXTERNAL_ORIGIN"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(InstanceOrigin,
                         InvalidInstanceOrigin,
                         testing::Values("code.example.internal",
                                         "//code.example.internal",
                                         "ftp://code.example.internal",
                                         "https:///missing-host",
                                         "https://user:password@code.example.internal",
                                         "https://code.example.internal/path",
                                         "https://code.example.internal/?query=1",
                                         "https://code.example.internal/#fragment",
                                         "http://127.0.0.1:0",
                                         "http://127.0.0.1:65536",
                                         "http://127.0.0.1:invalid",
                                         "http://127.0.0.1:",
                                         "https://code.example.internal\n"));

TEST(InstanceOrigin, AcceptsConfiguredHttpAndHttpsOrigins) {
    for (const auto* origin : {"http://127.0.0.1:49127",
                               "https://code.example.internal:8443/",
                               "https://code.example.internal",
                               "HTTPS://code.example.internal:8443/",
                               "http://[::1]:49127"}) {
        SCOPED_TRACE(origin);
        lake::api::ServerOptions options;
        options.port = 0;
        options.worker_coroutines = 1;
        options.external_origin = origin;
        lake::api::ApiServer server(options);
        ASSERT_TRUE(server.start()) << server.reason();
        server.stop();
    }
}

TEST(InstanceOrigin, MissingConfigurationOmitsTerminalCommands) {
    lake::control::Repository repository;
    repository.id = "repository-id";
    repository.name = "example";
    lake::api::ServerOptions options;
    const auto page = lake::api::web_ui_repository(repository, nullptr);
    EXPECT_TRUE(lake::api::repository_external_origin(options).empty());
    EXPECT_NE(page.find("LAKE_EXTERNAL_ORIGIN"), std::string::npos);
    EXPECT_EQ(page.find("liquid clone "), std::string::npos);
    EXPECT_EQ(page.find("liquid push "), std::string::npos);
    EXPECT_EQ(page.find("lake.live"), std::string::npos);
}

TEST(InstanceOrigin, PublicHostSuppliesHttpsFallbackAndExplicitOriginWins) {
    lake::control::Repository repository;
    repository.id = "repository-id";
    repository.name = "example";

    lake::api::ServerOptions options;
    options.public_host = "lake.example.internal";
    lake::api::RepositoryHomeData data;
    data.external_origin = lake::api::repository_external_origin(options);
    EXPECT_EQ(data.external_origin, "https://lake.example.internal");
    const auto fallback_page = lake::api::web_ui_repository(repository, nullptr, data);
    EXPECT_NE(fallback_page.find("https://lake.example.internal/v1/repositories/repository-id"),
              std::string::npos);

    options.external_origin = "http://127.0.0.1:49127";
    data.external_origin = lake::api::repository_external_origin(options);
    EXPECT_EQ(data.external_origin, "http://127.0.0.1:49127");
    const auto explicit_page = lake::api::web_ui_repository(repository, nullptr, data);
    EXPECT_NE(explicit_page.find("http://127.0.0.1:49127/v1/repositories/repository-id"),
              std::string::npos);
    EXPECT_EQ(explicit_page.find("lake.example.internal"), std::string::npos);

    options.external_origin.clear();
    options.port = 0;
    options.worker_coroutines = 1;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    server.stop();
}

TEST(InstanceOrigin, CommandsPreserveOriginAndQuoteShellArguments) {
    lake::control::Repository repository;
    repository.id = "repository-id";
    repository.name = "owner's <repo>";
    for (const auto* origin : {"http://127.0.0.1:49127", "https://code.example.internal:8443"}) {
        SCOPED_TRACE(origin);
        lake::api::RepositoryHomeData data;
        data.external_origin = origin;
        const auto page = lake::api::web_ui_repository(repository, nullptr, data);
        const auto url = std::string(origin) + "/v1/repositories/repository-id";
        EXPECT_NE(page.find("liquid clone " + lake::api::web_ui_escape("'" + url + "'")),
                  std::string::npos);
        EXPECT_NE(page.find("liquid push " + lake::api::web_ui_escape("'" + url + "'")),
                  std::string::npos);
        EXPECT_NE(page.find(lake::api::web_ui_escape("cd './owner'\\''s <repo>'")),
                  std::string::npos);
        EXPECT_NE(page.find("LIQUID_TOKEN"), std::string::npos);
        EXPECT_EQ(page.find("liquid pull "), std::string::npos);
        EXPECT_EQ(page.find("lake.live"), std::string::npos);
    }
}

} // namespace
