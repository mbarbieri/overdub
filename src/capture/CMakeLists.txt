# overdub_capture: recording Passes (issue #26): the io thread that writes dry Takes from the
# engine's capture FIFOs, Take placement and the Pass's undo step, and the loopback latency
# measurement the selftest and the calibration flow share. Events use the `capture.*` area.

add_library(overdub_capture STATIC)

target_sources(overdub_capture PRIVATE
    CameraCalibration.cpp
    LoopbackMeasurement.cpp
    PassRecorder.cpp
    VideoPlacement.cpp)

target_link_libraries(overdub_capture
    PUBLIC
        overdub_engine
    PRIVATE
        juce::juce_recommended_warning_flags)
