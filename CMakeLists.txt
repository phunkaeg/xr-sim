cmake_minimum_required(VERSION 3.24)

project(xr-sim VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "xr-sim currently implements Win32 graphics bindings only")
endif()

option(XRSIM_BUILD_TESTS "Build direct runtime smoke tests" ON)

set(XRSIM_SOURCES
    src/xrsim_actions.cpp
    src/xrsim_common.h
    src/xrsim_compositor.cpp
    src/xrsim_control.cpp
    include/xrsim/xrsim_extensions.h
    src/xrsim_frame.cpp
    src/xrsim_graphics.cpp
    src/xrsim_headset.cpp
    src/xrsim_instance.cpp
    src/xrsim_internal.h
    src/xrsim_log.cpp
    src/xrsim_math.h
    src/xrsim_session.cpp
    src/xrsim.def
)

add_library(xrsim SHARED ${XRSIM_SOURCES})
target_compile_features(xrsim PRIVATE cxx_std_17)
target_include_directories(xrsim PRIVATE
    src
    include
    third_party/openxr/include
    third_party/vulkan/include
)
target_compile_definitions(xrsim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xrsim PRIVATE
    d3d9 d3d10 d3d11 d3d12 dxgi d3dcompiler
    opengl32 windowscodecs ole32 shell32
)
set_target_properties(xrsim PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xrsim"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

if(MSVC)
    target_compile_options(xrsim PRIVATE /W4 /permissive- /EHa /utf-8)
    target_compile_definitions(xrsim PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(XRSIM_ARCH x64)
else()
    set(XRSIM_ARCH x86)
endif()

configure_file(runtime/xrsim.json.in "${CMAKE_BINARY_DIR}/bin/xrsim-${XRSIM_ARCH}.json" @ONLY)

if(XRSIM_BUILD_TESTS)
    enable_testing()
    add_executable(xrsim_probe tests/xrsim_probe.cpp)
    target_compile_features(xrsim_probe PRIVATE cxx_std_17)
    target_include_directories(xrsim_probe PRIVATE
        src
        include
        third_party/openxr/include
        third_party/vulkan/include
    )
    target_compile_definitions(xrsim_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(xrsim_probe PRIVATE d3d9 d3d10 d3d10_1 d3d11 d3d12 dxgi opengl32)
    set_target_properties(xrsim_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(MSVC)
        target_compile_options(xrsim_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME xrsim_headless_smoke
             COMMAND xrsim_probe "$<TARGET_FILE:xrsim>" headless)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-state/ctest")
    set_tests_properties(xrsim_headless_smoke PROPERTIES
        ENVIRONMENT "XRSIM_DIR=${CMAKE_BINARY_DIR}/test-state/ctest")

    add_executable(xrsim_headset_probe tests/xrsim_headset_probe.cpp)
    target_compile_features(xrsim_headset_probe PRIVATE cxx_std_17)
    target_include_directories(xrsim_headset_probe PRIVATE third_party/openxr/include)
    target_compile_definitions(xrsim_headset_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(xrsim_headset_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(MSVC)
        target_compile_options(xrsim_headset_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
    foreach(profile_case default valid fallback invalid-json invalid-fov invalid-nonfinite invalid-size invalid-missing)
        add_test(NAME xrsim_headset_profile_${profile_case}
                 COMMAND xrsim_headset_probe "$<TARGET_FILE:xrsim>" ${profile_case})
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-state/profile-${profile_case}")
        set_tests_properties(xrsim_headset_profile_${profile_case} PROPERTIES
            ENVIRONMENT "XRSIM_DIR=${CMAKE_BINARY_DIR}/test-state/profile-${profile_case}")
    endforeach()

    add_executable(xrsim_control_probe tests/xrsim_control_probe.cpp)
    target_compile_features(xrsim_control_probe PRIVATE cxx_std_17)
    target_include_directories(xrsim_control_probe PRIVATE third_party/openxr/include)
    target_compile_definitions(xrsim_control_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(xrsim_control_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(MSVC)
        target_compile_options(xrsim_control_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
    foreach(control_case exclusive-hold truncate-write rename-hold)
        add_test(NAME xrsim_control_${control_case}
                 COMMAND xrsim_control_probe "$<TARGET_FILE:xrsim>" ${control_case})
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-state/control-${control_case}")
        set_tests_properties(xrsim_control_${control_case} PROPERTIES
            ENVIRONMENT "XRSIM_DIR=${CMAKE_BINARY_DIR}/test-state/control-${control_case}")
    endforeach()

    # Executes the compositor's own quad-pass shader bytes on WARP. Needs no runtime,
    # session or GPU; exit 77 is a missing-WARP skip.
    add_executable(xrsim_compositor_probe tests/xrsim_compositor_probe.cpp)
    target_compile_features(xrsim_compositor_probe PRIVATE cxx_std_17)
    target_include_directories(xrsim_compositor_probe PRIVATE src third_party/openxr/include)
    target_compile_definitions(xrsim_compositor_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(xrsim_compositor_probe PRIVATE d3d11 d3dcompiler)
    set_target_properties(xrsim_compositor_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(MSVC)
        target_compile_options(xrsim_compositor_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME xrsim_compositor_quad_mvp COMMAND xrsim_compositor_probe)
    set_tests_properties(xrsim_compositor_quad_mvp PROPERTIES SKIP_RETURN_CODE 77)
endif()

install(TARGETS xrsim RUNTIME DESTINATION bin/${XRSIM_ARCH})
install(FILES "${CMAKE_BINARY_DIR}/bin/xrsim-${XRSIM_ARCH}.json" DESTINATION bin/${XRSIM_ARCH})
install(FILES include/xrsim/xrsim_extensions.h DESTINATION include/xrsim)
