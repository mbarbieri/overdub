# overdub_export: what an Export writes (issue #37): the offline render of a Song's graph (the same
# RenderPlan the engine plays) over the Song span plus its Insert Chain tails, written as 24-bit WAV
# or AAC M4A through Core Audio's ExtAudioFile, on a worker thread. Events and Error codes use the
# `export.*` area (ADR 0006). MP4 (issue #38): the mix and one Video Take composed with
# AVMutableComposition and written by AVAssetExportSession.

add_library(overdub_export STATIC)

target_sources(overdub_export PRIVATE
    ExportFormat.cpp
    MixExport.cpp
    MixFileWriter.cpp
    MixRender.cpp
    MovieExport.mm
    MovieSpan.cpp)

# The AVFoundation source holds Objective-C objects: ARC manages them.
set_source_files_properties(MovieExport.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(overdub_export
    PUBLIC
        overdub_engine
        overdub_video
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreMedia"
    PRIVATE
        juce::juce_recommended_warning_flags)
