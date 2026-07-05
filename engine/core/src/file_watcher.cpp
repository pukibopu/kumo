#include "kumo/core/file_watcher.h"

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

    for (Entry& entry : entries_) {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(entry.path, ec);
        if (ec) {
            entry.present = false;
            continue;
        }
        bool changed = !entry.present || mtime != entry.lastWrite;
        entry.present = true;
        entry.lastWrite = mtime;
        if (changed) {
            entry.callback(entry.path);
        }
    }
}

} // namespace kumo
