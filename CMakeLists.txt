cmake_minimum_required(VERSION 3.28)

# Allow build subdirectories, but keep generated files out of the source root.
file(REAL_PATH "${CMAKE_SOURCE_DIR}" source_dir)
file(REAL_PATH "${CMAKE_BINARY_DIR}" binary_dir)
if(source_dir STREQUAL binary_dir)
    message(FATAL_ERROR "Use a separate build directory, e.g. cmake -S . -B build or cmake -S . -B ../OBS2LED-build")
endif()

project(obs2led VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build configuration" FORCE)
endif()

set(OBS2LED_IN_OBS_BUILD OFF)
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio" AND NOT PROJECT_IS_TOP_LEVEL)
    set(OBS2LED_IN_OBS_BUILD ON)
endif()

option(OBS2LED_BUILD_PLUGIN "Build the OBS plugin" ON)
if(OBS2LED_BUILD_PLUGIN)
    if(NOT OBS2LED_IN_OBS_BUILD)
        include(cmake/FindObsWorktree.cmake)
        if(NOT TARGET OBS::libobs)
            find_package(libobs CONFIG REQUIRED)
        endif()
        if(NOT TARGET OBS::frontend-api AND NOT TARGET OBS::obs-frontend-api)
            find_package(obs-frontend-api CONFIG REQUIRED)
        endif()
    endif()

    # OBS defines its frontend target after plugins; its alias differs from the SDK export.
    if(OBS2LED_IN_OBS_BUILD OR TARGET OBS::frontend-api)
        set(obs2led_frontend_target OBS::frontend-api)
    else()
        set(obs2led_frontend_target OBS::obs-frontend-api)
    endif()
endif()
find_package(Threads REQUIRED)

add_library(obs2led-output STATIC src/output.cpp src/udp.cpp)
target_include_directories(obs2led-output PUBLIC src)
target_compile_features(obs2led-output PUBLIC cxx_std_20)
target_link_libraries(obs2led-output PUBLIC Threads::Threads)
set_target_properties(obs2led-output PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(WIN32)
    target_sources(obs2led-output PRIVATE src/serial-windows.cpp)
    target_link_libraries(obs2led-output PRIVATE ws2_32 setupapi cfgmgr32 advapi32 uuid)
elseif(APPLE)
    target_sources(obs2led-output PRIVATE src/serial-posix.cpp src/devices-macos.cpp)
    target_link_libraries(obs2led-output PRIVATE "-framework IOKit" "-framework CoreFoundation")
else()
    target_sources(obs2led-output PRIVATE src/serial-posix.cpp src/devices-linux.cpp)
endif()

if(MSVC)
    target_compile_options(obs2led-output PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(obs2led-output PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(OBS2LED_BUILD_PLUGIN)
add_library(obs2led MODULE
    src/plugin.cpp
    src/video-filter.cpp
    src/video-filter.hpp
    src/global-output.cpp
    src/global-output.hpp
)
target_link_libraries(obs2led PRIVATE OBS::libobs ${obs2led_frontend_target} obs2led-output)
target_compile_features(obs2led PRIVATE cxx_std_20)
target_compile_definitions(obs2led PRIVATE OBS2LED_VERSION="${PROJECT_VERSION}")
set_target_properties(obs2led PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
)

if(MSVC)
    target_compile_options(obs2led PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(obs2led PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(BUILD_TESTING)
    add_executable(obs2led-output-tests tests/output-tests.cpp)
    target_link_libraries(obs2led-output-tests PRIVATE obs2led-output)
    add_test(NAME output COMMAND obs2led-output-tests)
    set_tests_properties(output PROPERTIES TIMEOUT 20)
    option(OBS2LED_BUILD_GPU_TEST "Build the Windows libobs GPU integration test" OFF)
    if(OBS2LED_BUILD_GPU_TEST AND WIN32 AND OBS2LED_BUILD_PLUGIN)
        add_executable(obs2led-filter-smoke tests/filter-smoke.cpp tests/frontend-stub.cpp src/global-output.cpp)
        target_include_directories(obs2led-filter-smoke PRIVATE src
            "$<TARGET_PROPERTY:${obs2led_frontend_target},INTERFACE_INCLUDE_DIRECTORIES>")
        target_compile_features(obs2led-filter-smoke PRIVATE cxx_std_20)
        target_link_libraries(obs2led-filter-smoke PRIVATE OBS::libobs ws2_32)
    endif()
endif()

if(OBS2LED_BUILD_PLUGIN)
if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Build for 64-bit OBS; use an x64 or ARM64 toolchain.")
    endif()
endif()
if(OBS2LED_IN_OBS_BUILD)
    set_target_properties_obs(obs2led PROPERTIES FOLDER "plugins/obs2led" PREFIX "")
elseif(WIN32)
    install(TARGETS obs2led
        RUNTIME DESTINATION obs2led/bin/64bit
        LIBRARY DESTINATION obs2led/bin/64bit
    )
    install(DIRECTORY data/ DESTINATION obs2led/data)
elseif(APPLE)
    set_target_properties(obs2led PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION plugin
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    )
    install(TARGETS obs2led BUNDLE DESTINATION .)
    install(DIRECTORY data/ DESTINATION obs2led.plugin/Contents/Resources)
else()
    include(GNUInstallDirs)
    install(TARGETS obs2led LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/obs2led")
endif()
endif()
