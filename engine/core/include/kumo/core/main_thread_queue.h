#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>

namespace kumo {

// Hands JSON-returning work from worker threads to the thread that owns the engine
// state, which drains the queue once per frame (ADR 0005). Guarantees callers rely
// on:
//   - drain() takes a snapshot on entry, so work posted from inside a callback runs
//     on the next drain, never in the current one;
//   - no lock is held while work runs, so work may post again or call pending();
//   - cancelAll() fulfils every queued promise with the cancel value, and posts
//     after it are fulfilled with that value immediately without running;
//   - the destructor fulfils whatever is left. No path leaves a broken promise,
//     because future::get() on one throws (ADR 0035).
class MainThreadQueue {
public:
    MainThreadQueue() = default;
    ~MainThreadQueue();

    MainThreadQueue(const MainThreadQueue&) = delete;
    MainThreadQueue& operator=(const MainThreadQueue&) = delete;

    // Worker threads: enqueue `work` and block on the returned future.
    std::future<std::string> post(std::function<std::string()> work);

    // Main thread, once per frame: runs the queued work, returns how many items ran.
    std::size_t drain();

    void cancelAll(std::string cancelledValue);

    std::size_t pending() const;

private:
    struct Item {
        std::function<std::string()> work;
        std::promise<std::string> result;
    };

    void fulfilAll(std::string value);

    mutable std::mutex mutex_;
    std::deque<Item> items_;
    std::string cancelledValue_;
    bool cancelled_ = false;
};

} // namespace kumo
