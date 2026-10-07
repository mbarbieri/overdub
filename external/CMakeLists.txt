# Third-party code. Every dependency is a git submodule; see docs/building.md.

set(OVERDUB_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
# Every patch applied to JUCE, each with the ADR that requires it:
#   juce-force-auv2-oop.patch       ADR 0002: AUv2 loads out-of-process.
#   juce-graph-realtime-safe.patch  ADR 0001/0005: AudioProcessorGraph neither locks nor allocates
#                                   on the audio thread (try-lock per node, no output-buffer realloc).
set(OVERDUB_JUCE_PATCHES
    "${PROJECT_SOURCE_DIR}/patches/juce-force-auv2-oop.patch"
    "${PROJECT_SOURCE_DIR}/patches/juce-graph-realtime-safe.patch")

foreach(submodule JUCE Catch2 json)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}/CMakeLists.txt")
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
        JUCE_USE_CURL=0)

# Consumers see the same headers and module definitions the modules were compiled with.
target_include_directories(overdub_juce
    INTERFACE "$<TARGET_PROPERTY:overdub_juce,INCLUDE_DIRECTORIES>")
target_compile_definitions(overdub_juce
    INTERFACE "$<TARGET_PROPERTY:overdub_juce,COMPILE_DEFINITIONS>")
