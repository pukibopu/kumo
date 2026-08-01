add_library(kumo_gpu STATIC src/gpu_metal.cpp)
target_include_directories(kumo_gpu PUBLIC include)
target_link_libraries(kumo_gpu
    PRIVATE kumo_core metal_cpp kumo_shaderabi
)
kumo_target_warnings(kumo_gpu)
