# Third-party code. Every source dependency is a git submodule; ONNX Runtime is a pinned prebuilt
# download (below). See docs/building.md.

set(OVERDUB_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
# Every patch applied to JUCE, each with the ADR that requires it:
#   juce-force-auv2-oop.patch       ADR 0002: AUv2 loads out-of-process.
#   juce-graph-realtime-safe.patch  ADR 0001/0005: AudioProcessorGraph neither locks nor allocates
#                                   on the audio thread (try-lock per node, busy nodes silenced and
#                                   counted, no output-buffer realloc).
set(OVERDUB_JUCE_PATCHES
    "${PROJECT_SOURCE_DIR}/patches/juce-force-auv2-oop.patch"
    "${PROJECT_SOURCE_DIR}/patches/juce-graph-realtime-safe.patch")

# One file each submodule must have once checked out.
foreach(submodule_file JUCE/CMakeLists.txt Catch2/CMakeLists.txt json/CMakeLists.txt pocketfft/pocketfft_hdronly.h)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${submodule_file}")
        cmake_path(GET submodule_file PARENT_PATH submodule)
        message(FATAL_ERROR "overdub: external/${submodule} is empty. Run 'git submodule update --init --recursive'.")
    endif()
endforeach()

# Apply each patch idempotently at every configure:
# already applied -> nothing; applies cleanly -> apply; neither -> stop (JUCE was bumped or edited).
function(overdub_apply_juce_patch OVERDUB_JUCE_PATCH)
    execute_process(
        COMMAND git apply --reverse --check "${OVERDUB_JUCE_PATCH}"
        WORKING_DIRECTORY "${OVERDUB_JUCE_DIR}"
        RESULT_VARIABLE already_applied_result
        OUTPUT_QUIET ERROR_QUIET)
    if(already_applied_result EQUAL 0)
        message(STATUS "overdub: JUCE patch already applied: ${OVERDUB_JUCE_PATCH}")
        return()
    endif()

    execute_process(
        COMMAND git apply "${OVERDUB_JUCE_PATCH}"
        WORKING_DIRECTORY "${OVERDUB_JUCE_DIR}"
        RESULT_VARIABLE apply_result
        ERROR_VARIABLE apply_error)
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "overdub: could not apply ${OVERDUB_JUCE_PATCH} to ${OVERDUB_JUCE_DIR}:\n${apply_error}\nSee docs/building.md, 'Re-applying the JUCE patch'.")
    endif()
    message(STATUS "overdub: applied JUCE patch: ${OVERDUB_JUCE_PATCH}")
endfunction()

foreach(patch IN LISTS OVERDUB_JUCE_PATCHES)
    overdub_apply_juce_patch("${patch}")
endforeach()

add_subdirectory(JUCE EXCLUDE_FROM_ALL)
add_subdirectory(Catch2 EXCLUDE_FROM_ALL)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(json EXCLUDE_FROM_ALL)

# overdub_juce: every JUCE module Overdub uses, compiled exactly once.
# JUCE modules are INTERFACE libraries that compile their sources into each consumer, so linking
# them into several of our targets would duplicate symbols. They are linked PRIVATE here, and their
# include directories and compile definitions are re-exported to consumers below.
add_library(overdub_juce STATIC)

target_link_libraries(overdub_juce
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)

target_compile_definitions(overdub_juce
    PUBLIC
        JUCE_STANDALONE_APPLICATION=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_PLUGINHOST_AU=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # Debug builds would print "JUCE v9.0.3" to stderr at startup, breaking overdub-separate's
        # one-line stderr protocol (ADR 0007).
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1)

# Consumers see the same headers and module definitions the modules were compiled with.
target_include_directories(overdub_juce
    INTERFACE "$<TARGET_PROPERTY:overdub_juce,INCLUDE_DIRECTORIES>")
target_compile_definitions(overdub_juce
    INTERFACE "$<TARGET_PROPERTY:overdub_juce,COMPILE_DEFINITIONS>")

# pocketfft (BSD-3-Clause, header-only, the `cpp` branch): the STFT of the Separation helper
# (ADR 0007). Only overdub_separation and the helper use it, never overdub_core or Overdub.app.
add_library(overdub_pocketfft INTERFACE)
target_include_directories(overdub_pocketfft SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/pocketfft")
# The helper's parallelism is ONNX Runtime's; pocketfft's own thread pool is not needed.
target_compile_definitions(overdub_pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING)

# ONNX Runtime 1.23.2, prebuilt for osx-x86_64 (MIT; ADR 0007): the last release with Intel-macOS
# binaries. Fetched at configure time from a pinned URL and checked against its SHA-256 (the same
# digest GitHub lists for the release asset). Only the overdub-separate helper links it.
set(OVERDUB_ORT_VERSION "1.23.2")
set(OVERDUB_ORT_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v${OVERDUB_ORT_VERSION}/onnxruntime-osx-x86_64-${OVERDUB_ORT_VERSION}.tgz")
set(OVERDUB_ORT_SHA256 "d10359e16347b57d9959f7e80a225a5b4a66ed7d7e007274a15cae86836485a6")
set(OVERDUB_ORT_ARCHIVE "${CMAKE_BINARY_DIR}/downloads/onnxruntime-osx-x86_64-${OVERDUB_ORT_VERSION}.tgz")
# The unpacked package, its dylib's file name and path: src/separation and src/app read these.
set(OVERDUB_ORT_ROOT "${CMAKE_BINARY_DIR}/downloads/onnxruntime-osx-x86_64-${OVERDUB_ORT_VERSION}" CACHE INTERNAL "")
set(OVERDUB_ORT_DYLIB_NAME "libonnxruntime.${OVERDUB_ORT_VERSION}.dylib" CACHE INTERNAL "")
set(OVERDUB_ORT_DYLIB "${OVERDUB_ORT_ROOT}/lib/${OVERDUB_ORT_DYLIB_NAME}" CACHE INTERNAL "")

if(NOT EXISTS "${OVERDUB_ORT_DYLIB}")
    message(STATUS "overdub: downloading ${OVERDUB_ORT_URL}")
    file(DOWNLOAD "${OVERDUB_ORT_URL}" "${OVERDUB_ORT_ARCHIVE}"
        EXPECTED_HASH SHA256=${OVERDUB_ORT_SHA256}
        TLS_VERIFY ON
        STATUS ort_download_status)
    list(GET ort_download_status 0 ort_download_code)
    if(NOT ort_download_code EQUAL 0)
        file(REMOVE "${OVERDUB_ORT_ARCHIVE}")
        message(FATAL_ERROR "overdub: could not download ONNX Runtime from ${OVERDUB_ORT_URL}: ${ort_download_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${OVERDUB_ORT_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}/downloads")
    if(NOT EXISTS "${OVERDUB_ORT_DYLIB}")
        message(FATAL_ERROR "overdub: ${OVERDUB_ORT_ARCHIVE} has no lib/${OVERDUB_ORT_DYLIB_NAME}")
    endif()
endif()

add_library(overdub_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(overdub_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${OVERDUB_ORT_DYLIB}"
    IMPORTED_SONAME "@rpath/${OVERDUB_ORT_DYLIB_NAME}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${OVERDUB_ORT_ROOT}/include"
    INTERFACE_INCLUDE_DIRECTORIES "${OVERDUB_ORT_ROOT}/include")
