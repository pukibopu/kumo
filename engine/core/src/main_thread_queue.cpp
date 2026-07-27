#include <kumo/core/main_thread_queue.h>

#include <kumo/core/assert.h>

#include <string_view>
#include <utility>

namespace kumo {
namespace {

constexpr std::string_view kShutdownValue =
    R"({"status":"cancelled","message":"main thread queue shut down"})";
constexpr std::string_view kInvalidWorkValue =
    R"({"status":"error","message":"no work was supplied"})";
constexpr std::string_view kThrewValue = R"({"status":"error","message":"tool call failed"})";

} // namespace

MainThreadQueue::~MainThreadQueue() {
    fulfilAll({});
}

std::future<std::string> MainThreadQueue::post(std::function<std::string()> work) {
    std::promise<std::string> promise;
    std::future<std::string> future = promise.get_future();

    KUMO_ASSERT(static_cast<bool>(work));
    if (!work) {
        promise.set_value(std::string(kInvalidWorkValue));
        return future;
    }

    std::lock_guard lock(mutex_);
    if (cancelled_) {
        promise.set_value(cancelledValue_);
        return future;
    }
    items_.push_back(Item{std::move(work), std::move(promise)});
    return future;
}

std::size_t MainThreadQueue::drain() {
    std::deque<Item> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(items_);
    }
    for (Item& item : batch) {
        // `work` is caller-supplied: a throw escaping here would break the promise
        // and rethrow inside the waiting worker thread.
        try {
            item.result.set_value(item.work());
        } catch (...) {
            item.result.set_value(std::string(kThrewValue));
        }
    }
    return batch.size();
}

void MainThreadQueue::cancelAll(std::string cancelledValue) {
    fulfilAll(std::move(cancelledValue));
}

std::size_t MainThreadQueue::pending() const {
    std::lock_guard lock(mutex_);
    return items_.size();
}

void MainThreadQueue::fulfilAll(std::string value) {
    std::deque<Item> batch;
    std::string resolved;
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        if (!value.empty()) {
            cancelledValue_ = std::move(value);
        } else if (cancelledValue_.empty()) {
            cancelledValue_ = std::string(kShutdownValue);
        }
        resolved = cancelledValue_;
        batch.swap(items_);
    }
    for (Item& item : batch) {
        item.result.set_value(resolved);
    }
}

} // namespace kumo
