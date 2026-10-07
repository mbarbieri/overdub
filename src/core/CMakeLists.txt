# overdub_core: shared infrastructure every feature folder links (logging, errors, RT event ring,
# test guards). Feature code includes it as "core/<File>.h" (include root is src/).
# Not an event area: events and Error codes use the feature areas from ADR 0006.

add_library(overdub_core STATIC)

target_sources(overdub_core PRIVATE
    BuildInfo.cpp)

target_include_directories(overdub_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_compile_definitions(overdub_core PRIVATE
    OVERDUB_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(overdub_core
    PUBLIC
        overdub_juce
        nlohmann_json::nlohmann_json
    PRIVATE
        juce::juce_recommended_warning_flags)
