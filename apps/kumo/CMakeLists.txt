# CMake's Ninja generator cannot mix Swift sources with other languages in one
# target, so the ObjC++ facade is a separate static lib (kumo_bridge) that the
# Swift-only app target links; KumoEngine.h is handed to swiftc directly as a
# bridging header (SHELL:-import-objc-header), independent of target include
# dirs.
add_library(kumo_bridge STATIC KumoEngine.mm)
target_include_directories(kumo_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(KumoEngine.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(kumo_bridge
    PUBLIC kumo_facade kumo_gpu "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
           "-framework Security")
target_compile_definitions(kumo_bridge PRIVATE
    KUMO_VERSION_STRING="${PROJECT_VERSION}"
    KUMO_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    KUMO_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    KUMO_REPO_DIR="${CMAKE_SOURCE_DIR}")
kumo_target_warnings(kumo_bridge)

add_executable(kumo_app MACOSX_BUNDLE
    KumoApp.swift Viewport.swift Inspector.swift Chat.swift DirectorView.swift
    SettingsView.swift)
target_compile_options(kumo_app PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-import-objc-header \"${CMAKE_CURRENT_SOURCE_DIR}/KumoEngine.h\">")
if(CMAKE_OSX_ARCHITECTURES)
    # CMake's Swift support ignores CMAKE_OSX_ARCHITECTURES; mirror it so the
    # Swift objects match the C++ libraries' architecture.
    list(GET CMAKE_OSX_ARCHITECTURES 0 KUMO_SWIFT_ARCH)
    target_compile_options(kumo_app PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-target ${KUMO_SWIFT_ARCH}-apple-macosx14.0>")
endif()
target_link_libraries(kumo_app PRIVATE
    kumo_bridge "-framework Cocoa" "-framework QuartzCore" "-framework CoreVideo" "-lc++")
set_target_properties(kumo_app PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "kumo"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.kumo.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "kumo")
