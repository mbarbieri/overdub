# overdub_capture: recording Passes (issue #26): the io thread that writes dry Takes from the
# engine's capture FIFOs, Take placement and the Pass's undo step, and the loopback latency
# measurement the selftest and the calibration flow share. Video Takes (issue #35): the camera
# (AVCaptureSession behind the plain C++ Camera interface), the movie writer and reader, placement
# on the audio clock (ADR 0003), the camera calibration and the clap measurement. Events use the
# `capture.*` area.

add_library(overdub_capture STATIC)

target_sources(overdub_capture PRIVATE
    AvCamera.mm
    CameraCalibration.cpp
    ClapMeasurement.cpp
    ClapSelftest.cpp
    LoopbackMeasurement.cpp
    MovieFrames.mm
    MovieWriter.mm
    PassRecorder.cpp
    VideoPlacement.cpp)

# The AVFoundation sources hold Objective-C objects in C++ structs: ARC manages them.
set_source_files_properties(AvCamera.mm MovieFrames.mm MovieWriter.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(overdub_capture
    PUBLIC
        overdub_engine
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
    PRIVATE
        juce::juce_recommended_warning_flags)
