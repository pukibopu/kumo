#include "kumo/core/file_watcher.h"

#include <cstddef>
#include <system_error>
#include <utility>

namespace kumo {

FileWatcher::FileWatcher(std::chrono::milliseconds interval)
    : interval_(interval), lastCheck_(std::chrono::steady_clock::now()) {}

void FileWatcher::watch(std::filesystem::path path, Callback callback) {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path, ec);
    entries_.push_back(Entry{
        .path = std::move(path),
        .callback = std::move(callback),
        .lastWrite = mtime,
        .present = !ec,
    });
}

void FileWatcher::poll() {
    auto now = std::chrono::steady_clock::now();
    if (now - lastCheck_ < interval_) {
        return;
    }
    lastCheck_ = now;

    // Index-based over a captured count: a callback may call watch() and reallocate
    // entries_, so hold no Entry reference across it; new entries wait for next poll.
    const std::size_t count = entries_.size();
    for (std::size_t i = 0; i < count; ++i) {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(entries_[i].path, ec);
        if (ec) {
            entries_[i].present = false;
            continue;
        }
        bool changed = !entries_[i].present || mtime != entries_[i].lastWrite;
        entries_[i].present = true;
        entries_[i].lastWrite = mtime;
        if (changed) {
            Callback callback = entries_[i].callback;
            std::filesystem::path path = entries_[i].path;
            callback(path);
        }
    }
}

} // namespace kumo
