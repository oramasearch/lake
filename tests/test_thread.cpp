#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "lake/thread.hpp"

namespace {

// Touches `bytes` of its own frame, as liquid's chunk_index::compact does in a Debug build.
[[gnu::noinline]] std::uint64_t use_stack(std::size_t index) {
    std::array<volatile std::uint8_t, std::size_t{2} * 1024 * 1024> frame{};
    frame[index] = 1;
    frame[frame.size() - 1 - index] = 2;
    return frame[index] + frame[frame.size() - 1 - index];
}

} // namespace

// lake#478: a 2 MiB frame overflows the 512 KiB macOS default for a std::thread; a lake::Thread
// takes thread_stack_bytes on every platform, so the same frame runs.
TEST(Thread, RunsFramesLargerThanThePlatformDefaultStack) {
    static_assert(lake::thread_stack_bytes >= std::size_t{4} * 1024 * 1024);
    std::uint64_t result = 0;
    {
        lake::Thread thread([&result] { result = use_stack(7); });
    } // the destructor joins
    EXPECT_EQ(result, 3U);
}

TEST(Thread, MovesAndJoinsOnce) {
    int runs = 0;
    lake::Thread first([&runs] { ++runs; });
    lake::Thread second(std::move(first));
    EXPECT_FALSE(first.joinable());
    EXPECT_TRUE(second.joinable());
    second.join();
    EXPECT_FALSE(second.joinable());
    EXPECT_EQ(runs, 1);
}
