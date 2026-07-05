add_library(kumo_rhi_metal STATIC src/rhi_metal.cpp)
target_include_directories(kumo_rhi_metal PUBLIC include)
target_link_libraries(kumo_rhi_metal
    PUBLIC kumo_rhi
    PRIVATE kumo_core metal_cpp
)
kumo_target_warnings(kumo_rhi_metal)
