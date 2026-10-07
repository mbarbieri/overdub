# overdub_core: shared infrastructure every feature folder links (logging, errors, RT event ring,
# test guards). Feature code includes it as "core/<File>.h" (include root is src/).
# Not an event area: events and Error codes use the feature areas from ADR 0006.

add_library(overdub_core STATIC)

target_sources(overdub_core PRIVATE
    BuildInfo.cpp
    Error.cpp
    Log.cpp
    LogFiles.cpp
    RtEventRing.cpp)

# The git SHA is regenerated on every build (not just at configure) so it never goes stale.
set(overdub_git_sha_file "${CMAKE_CURRENT_BINARY_DIR}/generated/GitSha.cpp")
add_custom_target(overdub_git_sha
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT_FILE=${overdub_git_sha_file}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/WriteGitSha.cmake"
    BYPRODUCTS "${overdub_git_sha_file}"
    COMMENT "Recording the git SHA"
    VERBATIM)
target_sources(overdub_core PRIVATE "${overdub_git_sha_file}")
add_dependencies(overdub_core overdub_git_sha)

target_include_directories(overdub_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_compile_definitions(overdub_core PRIVATE
    OVERDUB_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(overdub_core
    PUBLIC
        overdub_juce
        nlohmann_json::nlohmann_json
    PRIVATE
        juce::juce_recommended_warning_flags)
