add_library(kumo_core STATIC
    src/asset_name.cpp
    src/assert.cpp
    src/file.cpp
    src/file_watcher.cpp
    src/log.cpp
    src/main_thread_queue.cpp
)
target_include_directories(kumo_core PUBLIC include)
kumo_target_warnings(kumo_core)
