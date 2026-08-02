add_library(kumo_asset STATIC
    src/loader.cpp
    src/model_resolver.cpp
    src/primitives.cpp
    src/procedural_sky.cpp
    src/texture_set.cpp
    src/cgltf_impl.cpp
    src/stb_impl.cpp
)
target_include_directories(kumo_asset PUBLIC include)
target_link_libraries(kumo_asset
    PUBLIC kumo_math kumo_core
    PRIVATE cgltf stb
)
kumo_target_warnings(kumo_asset)

# Third-party single-header implementations: silence their warnings so our
# -Werror mirror stays clean without policing upstream code.
if(MSVC)
    set(_kumo_asset_no_warn /w)
else()
    set(_kumo_asset_no_warn -w)
endif()
set_source_files_properties(src/cgltf_impl.cpp src/stb_impl.cpp
    PROPERTIES COMPILE_OPTIONS "${_kumo_asset_no_warn}")
