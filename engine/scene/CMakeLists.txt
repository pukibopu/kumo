add_library(kumo_scene STATIC
    src/transform.cpp
    src/camera.cpp
    src/scene.cpp
)
target_include_directories(kumo_scene PUBLIC include)
target_link_libraries(kumo_scene PUBLIC kumo_math kumo_core)
kumo_target_warnings(kumo_scene)
