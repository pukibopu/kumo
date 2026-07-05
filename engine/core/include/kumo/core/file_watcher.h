#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <vector>

namespace kumo {

class FileWatcher {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;

    // interval: minimum time between filesystem checks across all watched files.
    explicit FileWatcher(std::chrono::milliseconds interval = std::chrono::milliseconds(500));

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void watch(std::filesystem::path path, Callback callback);

    // Call once per frame; cheap no-op until the interval elapses.
    // Invokes callbacks for files whose mtime changed since last check.
    void poll();

private:
    struct Entry {
        std::filesystem::path path;
        Callback callback;
        std::filesystem::file_time_type lastWrite;
        bool present;
    };

    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point lastCheck_;
    std::vector<Entry> entries_;
};

} // namespace kumo
