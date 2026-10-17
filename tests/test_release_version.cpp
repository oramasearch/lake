// The version lives in three places (root CLAUDE.md, *Cutting a release*): `project(lake VERSION)`
// in CMakeLists.txt, `version_string()` in src/version.cpp and `current_version()` in
// include/lake/version.hpp. These tests read each from the code, never repeating the number, and
// require that the version has its release page in docs/releases/, indexed in its README.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "lake/version.hpp"

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string releases_dir() {
    return std::string(LAKE_TEST_SOURCE_DIR) + "/docs/releases/";
}

} // namespace

TEST(ReleaseVersion, VersionStringIsTheCMakeProjectVersion) {
    EXPECT_EQ(lake::version_string(), std::string_view(LAKE_TEST_PROJECT_VERSION));
}

TEST(ReleaseVersion, CurrentVersionMatchesVersionString) {
    const auto version = lake::current_version();
    const std::string formatted = std::to_string(version.major) + "." +
                                  std::to_string(version.minor) + "." +
                                  std::to_string(version.patch);
    EXPECT_EQ(formatted, lake::version_string());
}

TEST(ReleaseVersion, VersionHasAReleasePageIndexedInTheReadme) {
    const std::string page = "v" + std::string(lake::version_string()) + ".md";
    ASSERT_TRUE(fs::exists(releases_dir() + page)) << "missing docs/releases/" << page;
    const std::string heading = "# v" + std::string(lake::version_string()) + "\n";
    EXPECT_TRUE(read_file(releases_dir() + page).starts_with(heading))
        << "docs/releases/" << page << " must start with " << heading;
    EXPECT_NE(read_file(releases_dir() + "README.md").find("](" + page + ")"), std::string::npos)
        << "docs/releases/README.md does not link " << page;
}
