#include <doctest/doctest.h>

#include <kumo/core/main_thread_queue.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace kumo;

namespace {

// Every wait in this file is bounded: a hung future would otherwise stall the
// whole test binary instead of failing.
constexpr std::chrono::seconds kWait{5};

std::string waitFor(std::future<std::string>& future) {
    REQUIRE(future.valid());
    REQUIRE(future.wait_for(kWait) == std::future_status::ready);
    return future.get();
}

} // namespace

TEST_CASE("MainThreadQueue runs work on the draining thread") {
    MainThreadQueue queue;
    std::thread::id workThread;

    std::future<std::string> future = queue.post([&] {
        workThread = std::this_thread::get_id();
        return std::string("done");
    });
    CHECK(queue.pending() == 1);

    // REQUIRE unwinds via an exception, so the worker thread only uses CHECK.
    std::string postedResult;
    std::thread poster([&] {
        std::future<std::string> other = queue.post([] { return std::string("other"); });
        if (other.wait_for(kWait) == std::future_status::ready) {
            postedResult = other.get();
        }
    });

    // Drain until both items have run; the poster thread may not have posted yet.
    std::size_t ran = 0;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (ran < 2 && std::chrono::steady_clock::now() < deadline) {
        ran += queue.drain();
    }
    poster.join();

    CHECK(postedResult == "other");
    CHECK(ran == 2);
    CHECK(waitFor(future) == "done");
    CHECK(workThread == std::this_thread::get_id());
    CHECK(queue.pending() == 0);
    CHECK(queue.drain() == 0);
}

TEST_CASE("MainThreadQueue::drain is a snapshot and runs in FIFO order") {
    MainThreadQueue queue;
    std::vector<std::string> order;

    std::future<std::string> nested;
    std::future<std::string> first = queue.post([&] {
        order.push_back("first");
        // Posted from inside a drain: must not run until the next drain.
        nested = queue.post([&] {
            order.push_back("nested");
            return std::string("nested");
        });
        CHECK(queue.pending() == 1);
        return std::string("first");
    });
    std::future<std::string> second = queue.post([&] {
        order.push_back("second");
        return std::string("second");
    });

    CHECK(queue.drain() == 2);
    CHECK(order == std::vector<std::string>{"first", "second"});
    CHECK(waitFor(first) == "first");
    CHECK(waitFor(second) == "second");
    CHECK(nested.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    CHECK(queue.drain() == 1);
    CHECK(order == std::vector<std::string>{"first", "second", "nested"});
    CHECK(waitFor(nested) == "nested");
}

TEST_CASE("MainThreadQueue::cancelAll fulfils pending and later posts") {
    MainThreadQueue queue;
    bool ran = false;

    std::future<std::string> pendingCall = queue.post([&] {
        ran = true;
        return std::string("ran");
    });
    queue.cancelAll("cancelled");

    CHECK(waitFor(pendingCall) == "cancelled");
    CHECK_FALSE(ran);
    CHECK(queue.pending() == 0);
    CHECK(queue.drain() == 0);

    std::future<std::string> late = queue.post([&] {
        ran = true;
        return std::string("ran");
    });
    CHECK(late.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK(waitFor(late) == "cancelled");
    CHECK_FALSE(ran);
}

TEST_CASE("MainThreadQueue destruction fulfils outstanding futures") {
    std::future<std::string> orphan;
    std::future<std::string> cancelled;
    {
        MainThreadQueue queue;
        orphan = queue.post([] { return std::string("never"); });
        CHECK(queue.pending() == 1);
    }
    {
        MainThreadQueue queue;
        queue.cancelAll("bye");
        cancelled = queue.post([] { return std::string("never"); });
    }

    // A broken promise would throw here, which self-authored code must never do.
    REQUIRE(orphan.valid());
    REQUIRE(orphan.wait_for(kWait) == std::future_status::ready);
    CHECK_FALSE(orphan.get().empty());
    CHECK(waitFor(cancelled) == "bye");
}

TEST_CASE("MainThreadQueue holds no lock while work runs") {
    MainThreadQueue queue;
    std::size_t seenPending = 99;

    std::future<std::string> future = queue.post([&] {
        // Both calls would deadlock if drain() kept the mutex locked.
        seenPending = queue.pending();
        return std::string("ok");
    });
    std::future<std::string> other = queue.post([] { return std::string("other"); });

    CHECK(queue.drain() == 2);
    CHECK(seenPending == 0);
    CHECK(waitFor(future) == "ok");
    CHECK(waitFor(other) == "other");
}
