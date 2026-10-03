# Prefer exports from a configured OBS worktree when building this plugin on its own.
option(OBS2LED_USE_OBS_WORKTREE "Look for OBS libraries in the surrounding worktree" ON)
set(OBS2LED_OBS_BUILD_DIR "" CACHE PATH "Configured OBS build directory (auto-detected when empty)")

if(NOT OBS2LED_USE_OBS_WORKTREE OR TARGET OBS::libobs)
    return()
endif()

if(NOT OBS2LED_OBS_BUILD_DIR)
    get_filename_component(_obs2led_obs_source "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    if(EXISTS "${_obs2led_obs_source}/libobs/CMakeLists.txt")
        set(_obs2led_build_names build)
        if(CMAKE_VS_PLATFORM_NAME)
            string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" _obs2led_arch)
            list(PREPEND _obs2led_build_names "build_${_obs2led_arch}")
        elseif(APPLE)
            list(PREPEND _obs2led_build_names build_macos)
        elseif(UNIX)
            list(PREPEND _obs2led_build_names build_ubuntu)
        endif()
        foreach(_obs2led_name IN LISTS _obs2led_build_names)
            set(_obs2led_candidate "${_obs2led_obs_source}/${_obs2led_name}")
            if(EXISTS "${_obs2led_candidate}/libobs/libobsConfig.cmake"
               AND EXISTS "${_obs2led_candidate}/frontend/api/obs-frontend-apiConfig.cmake")
                set(OBS2LED_OBS_BUILD_DIR "${_obs2led_candidate}" CACHE PATH
                    "Configured OBS build directory (auto-detected when empty)" FORCE)
                break()
            endif()
        endforeach()
        if(NOT OBS2LED_OBS_BUILD_DIR)
            message(STATUS "OBS worktree found, but no configured build was detected. "
                "Set OBS2LED_OBS_BUILD_DIR to its build directory; trying an installed OBS SDK.")
        endif()
    endif()
endif()

if(NOT OBS2LED_OBS_BUILD_DIR)
    return()
endif()

if(NOT EXISTS "${OBS2LED_OBS_BUILD_DIR}/CMakeCache.txt"
   OR NOT EXISTS "${OBS2LED_OBS_BUILD_DIR}/libobs/libobsConfig.cmake"
   OR NOT EXISTS "${OBS2LED_OBS_BUILD_DIR}/frontend/api/obs-frontend-apiConfig.cmake")
    message(FATAL_ERROR "OBS2LED_OBS_BUILD_DIR must point to a configured OBS build containing "
        "libobs and frontend/api package exports. Configure and build OBS first.")
endif()

# Reuse the dependency prefixes and find modules from the same OBS build.
load_cache("${OBS2LED_OBS_BUILD_DIR}" READ_WITH_PREFIX _obs2led_cached_
    CMAKE_HOME_DIRECTORY CMAKE_PREFIX_PATH CMAKE_GENERATOR_PLATFORM)
if(CMAKE_VS_PLATFORM_NAME AND _obs2led_cached_CMAKE_GENERATOR_PLATFORM)
    string(REGEX REPLACE ",.*" "" _obs2led_obs_arch "${_obs2led_cached_CMAKE_GENERATOR_PLATFORM}")
    if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL _obs2led_obs_arch)
        message(FATAL_ERROR "OBS was configured for ${_obs2led_obs_arch}, but OBS2LED uses "
            "${CMAKE_VS_PLATFORM_NAME}. Select a matching toolchain or OBS2LED_OBS_BUILD_DIR.")
    endif()
endif()
list(APPEND CMAKE_MODULE_PATH "${_obs2led_cached_CMAKE_HOME_DIRECTORY}/cmake/finders")
list(PREPEND CMAKE_PREFIX_PATH
    "${OBS2LED_OBS_BUILD_DIR}/deps/w32-pthreads"
    ${_obs2led_cached_CMAKE_PREFIX_PATH})
set(libobs_DIR "${OBS2LED_OBS_BUILD_DIR}/libobs")
set(obs-frontend-api_DIR "${OBS2LED_OBS_BUILD_DIR}/frontend/api")
message(STATUS "OBS2LED: using OBS worktree build at ${OBS2LED_OBS_BUILD_DIR}")
