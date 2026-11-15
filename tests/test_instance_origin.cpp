// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "lake/api/server.hpp"
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

// The clone and push origin (LAKE_EXTERNAL_ORIGIN, else https://<public host>); without either,
// lake names none, and the web app shows no terminal commands for a repository.
TEST(InstanceOrigin, MissingConfigurationNamesNoOrigin) {
    lake::api::ServerOptions options;
    EXPECT_TRUE(lake::api::repository_external_origin(options).empty());
}

TEST(InstanceOrigin, PublicHostSuppliesHttpsFallbackAndExplicitOriginWins) {
    lake::api::ServerOptions options;
    options.public_host = "lake.example.internal";
    EXPECT_EQ(lake::api::repository_external_origin(options), "https://lake.example.internal");

    options.external_origin = "http://127.0.0.1:49127";
    EXPECT_EQ(lake::api::repository_external_origin(options), "http://127.0.0.1:49127");

    options.external_origin.clear();
    options.port = 0;
    options.worker_coroutines = 1;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    server.stop();
}

} // namespace
