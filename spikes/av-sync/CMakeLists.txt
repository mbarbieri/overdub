# PROTOTYPE — throwaway spike for issue #7. Not production code; never merged to main.
cmake_minimum_required(VERSION 3.22)
project(OverdubAvSpike VERSION 0.0.1 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

set(OVERDUB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${OVERDUB_ROOT}/external/JUCE ${CMAKE_BINARY_DIR}/JUCE)

juce_add_gui_app(OverdubAvSpike
    PRODUCT_NAME "Overdub AV Spike"
    BUNDLE_ID com.mbarbieri.overdub.avspike
    COMPANY_NAME "Overdub"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The spike records audio from your input to measure A/V sync."
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "The spike records a Video Take to measure A/V sync.")

target_sources(OverdubAvSpike PRIVATE
    src/Main.cpp
    src/SpikeLog.cpp
    src/ClockMap.cpp
    src/Session.cpp
    src/AudioEngine.cpp
    src/CameraRecorder.mm
    src/VideoView.mm
    src/Exporter.mm
    src/ClapAnalysis.mm
    src/SpikeComponent.cpp)

# Objective-C++ files use ARC.
set_source_files_properties(src/CameraRecorder.mm src/VideoView.mm src/Exporter.mm src/ClapAnalysis.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-deprecated-declarations")

target_compile_definitions(OverdubAvSpike PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OverdubAvSpike,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OverdubAvSpike,JUCE_VERSION>")

target_link_libraries(OverdubAvSpike
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
