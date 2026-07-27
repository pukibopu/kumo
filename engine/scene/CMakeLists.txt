add_library(kumo_scene STATIC src/persistence.cpp)
target_include_directories(kumo_scene PUBLIC include)
target_link_libraries(kumo_scene PUBLIC kumo_math kumo_core PRIVATE nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_scene)
