# PROTOTYPE — throwaway spike for issue #6. Not production code; never merged to main.
cmake_minimum_required(VERSION 3.22)
project(OverdubAuSpike VERSION 0.0.1 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OVERDUB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${OVERDUB_ROOT}/external/JUCE ${CMAKE_BINARY_DIR}/JUCE)

juce_add_gui_app(OverdubAuSpike
    PRODUCT_NAME "Overdub AU Spike"
    BUNDLE_ID com.mbarbieri.overdub.auspike
    COMPANY_NAME "Overdub"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The spike records dry input from your audio interface.")

target_sources(OverdubAuSpike PRIVATE
    src/Main.cpp
    src/SpikeLog.cpp
    src/SpikeProcessors.cpp
    src/SpikeEngine.cpp
    src/LoopbackTest.cpp
    src/AutoRun.cpp
    src/HalDump.cpp
    src/SpikeComponent.cpp)

target_compile_definitions(OverdubAuSpike PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OverdubAuSpike,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OverdubAuSpike,JUCE_VERSION>")

# Real-time-safety check from ADR 0001, kept as warnings so the spike can count them.
set_source_files_properties(src/SpikeEngine.cpp src/SpikeProcessors.cpp
    PROPERTIES COMPILE_OPTIONS "-Wfunction-effects")

target_link_libraries(OverdubAuSpike
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
