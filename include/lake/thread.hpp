// SPDX-License-Identifier: MIT

#pragma once

#include <pthread.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

// A joinable thread whose stack size lake chooses instead of the platform default (lake#478).
//
// std::thread takes the platform's default stack for a secondary thread: 8 MiB on glibc Linux,
// 512 KiB on macOS. Lake runs request handling on the threads it starts, and with it a nested
// liquid runtime through liquid::runtime::block_on, so their stack demand is set by lake and
// liquid, not by the platform. In a Debug build, liquid's chunk_index::compact reserves 516 KiB
// of stack in one frame, and a store flush that compacted overflowed the macOS default with a
// Bus error. Every thread lake starts therefore takes `thread_stack_bytes`. The measured peaks
// behind the number are in docs/build/local.md ("Thread stacks").
namespace lake {

inline constexpr std::size_t thread_stack_bytes = std::size_t{8} * 1024 * 1024;

class Thread {
public:
    Thread() = default;

    explicit Thread(std::function<void()> body, std::size_t stack_bytes = thread_stack_bytes) {
        auto owned = std::make_unique<std::function<void()>>(std::move(body));
        pthread_attr_t attributes;
        if (int error = pthread_attr_init(&attributes); error != 0)
            throw std::system_error(error, std::generic_category(), "pthread_attr_init");
        int error = pthread_attr_setstacksize(&attributes, stack_bytes);
        if (error == 0)
            error = pthread_create(&handle_, &attributes, &Thread::run, owned.get());
        pthread_attr_destroy(&attributes);
        if (error != 0)
            throw std::system_error(error, std::generic_category(), "pthread_create");
        owned.release(); // the new thread owns the body now
        joinable_ = true;
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&& other) noexcept
        : handle_(other.handle_), joinable_(std::exchange(other.joinable_, false)) {}

    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            join();
            handle_ = other.handle_;
            joinable_ = std::exchange(other.joinable_, false);
        }
        return *this;
    }

    // Joins, like std::jthread: a Thread never outlives its owner silently.
    ~Thread() { join(); }

    [[nodiscard]] bool joinable() const noexcept { return joinable_; }

    void join() noexcept {
        if (joinable_) {
            pthread_join(handle_, nullptr);
            joinable_ = false;
        }
    }

private:
    static void* run(void* argument) noexcept {
        std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(argument));
        (*body)();
        return nullptr;
    }

    pthread_t handle_{};
    bool joinable_ = false;
};

} // namespace lake
