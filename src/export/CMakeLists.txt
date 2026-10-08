# overdub_export: Export mode's mixdown (issue #37): the offline render of a Song's graph (the same
# RenderPlan the engine plays) over the Song span plus its Insert Chain tails, written as 24-bit WAV
# or AAC M4A through Core Audio's ExtAudioFile, on a worker thread. Events and Error codes use the
# `export.*` area (ADR 0006).

add_library(overdub_export STATIC)

target_sources(overdub_export PRIVATE
    ExportFormat.cpp
    MixExport.cpp
    MixFileWriter.cpp
    MixRender.cpp)

target_link_libraries(overdub_export
    PUBLIC
        overdub_engine
        "-framework AudioToolbox"
    PRIVATE
        juce::juce_recommended_warning_flags)
