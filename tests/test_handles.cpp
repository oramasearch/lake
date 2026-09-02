#include <gtest/gtest.h>

#include <set>

#include "lake/control/handles.hpp"

TEST(Handles, FormatIsLowercaseHyphenated) {
    using lake::control::valid_handle;
    EXPECT_TRUE(valid_handle("swift-falcon-42"));
    EXPECT_TRUE(valid_handle("a"));
    EXPECT_FALSE(valid_handle(""));
    EXPECT_FALSE(valid_handle("Swift-Falcon"));
    EXPECT_FALSE(valid_handle("has space"));
    EXPECT_FALSE(valid_handle("-leading"));
    EXPECT_FALSE(valid_handle("trailing-"));
    EXPECT_FALSE(valid_handle(std::string(257, 'a')));
}

TEST(Handles, GeneratedHandlesAreValidAndVaried) {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string h = lake::control::generate_handle();
        EXPECT_TRUE(lake::control::valid_handle(h)) << h;
        seen.insert(h);
    }
    EXPECT_GT(seen.size(), 150u);
}
