# overdub_engine: the headless engine (ADR 0005): the process() callback, transport, one
# AudioProcessorGraph per Song snapshot (ADR 0001), layered Clip playback and the per-callback
# anchor (ADR 0003). Events use the `engine.*` area (ADR 0006).

add_library(overdub_engine STATIC)

target_sources(overdub_engine PRIVATE
    ClipSchedule.cpp
    DeviceBinding.cpp
    DeviceSettings.cpp
    Engine.cpp
    LatencyCalibration.cpp
    MixLaw.cpp
    MixStrip.cpp
    RecordSession.cpp
    RenderPlan.cpp
    SharedInsertNode.cpp
    TrackPlayer.cpp)

target_link_libraries(overdub_engine
    PUBLIC
        overdub_song
    PRIVATE
        juce::juce_recommended_warning_flags)
