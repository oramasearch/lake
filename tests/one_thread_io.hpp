#pragma once

// Runs a liquid client task on an io_context that exactly one thread drives (lake#510).
//
// liquid::runtime::block_on drives its io_context from several runner threads. corosio's reactor
// (kqueue and epoll alike) hands each runner the descriptor states of the events it collected,
// as raw pointers. A socket closed on one runner frees its descriptor state at once unless that
// state is already queued; another runner returning from kevent()/epoll_wait() with an event for
// that socket then queues the freed state, and the runner that pops it locks a destroyed mutex:
// "libc++abi: ... mutex lock failed: Invalid argument" in the middle of a test. A test's HTTP
// client (liquid::http1::request) and its liquid sync pushes and pulls open and close a socket per
// call, so they run here instead, the way every socket in the lake binary does: on one thread per
// io_context (the API server's io thread, the outbound HTTPS client, Zetta's transport).

#include <chrono>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <liquid/runtime.hpp>

#include "http1/client.hpp"

namespace lake::test {

namespace detail {

template <class T>
boost::capy::task<> complete_on_one_thread(liquid::runtime::task<T> task,
                                           std::optional<liquid::result<T>>& output) {
    try {
        output.emplace(co_await std::move(task));
    } catch (...) {
        output.emplace(std::unexpected(liquid::error{
            liquid::errc::internal, "the task threw", "lake::test::block_on_one_thread"}));
    }
    co_return;
}

} // namespace detail

// block_on, with the calling thread as the only thread running the io_context.
template <class T> liquid::result<T> block_on_one_thread(liquid::runtime::task<T> task) {
    boost::corosio::io_context io;
    std::optional<liquid::result<T>> output;
    boost::capy::run_async(io.get_executor())(
        detail::complete_on_one_thread<T>(std::move(task), output));
    // run() returns when it sees no outstanding work. A completion still on its way from a thread
    // the context does not count (liquid's offload pool, the resolver) restarts it.
    for (;;) {
        io.run();
        if (output.has_value()) {
            break;
        }
        io.restart();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::move(*output);
}

inline liquid::result<liquid::http1::response> http_request(liquid::http1::request_options options) {
    return block_on_one_thread(liquid::http1::request(std::move(options)));
}

} // namespace lake::test
