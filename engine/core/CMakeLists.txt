add_library(kumo_core STATIC
    src/assert.cpp
    src/log.cpp
)
target_include_directories(kumo_core PUBLIC include)
kumo_target_warnings(kumo_core)
