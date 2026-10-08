# overdub_video: Video Take playback on the audio clock (ADR 0003, issue #36): the pure frame
# selection (heard position -> Video Take -> movie time -> frame), the movie's frame times and the
# frame decoder (AVAssetReader), and VideoPlayback, which pulls the frame for each display refresh.
# Events and Error codes use the `video.*` area.

add_library(overdub_video STATIC)

target_sources(overdub_video PRIVATE
    FrameSelection.cpp
    MovieFile.mm
    VideoPlayback.cpp)

# The AVFoundation sources hold Objective-C objects in C++ structs: ARC manages them.
set_source_files_properties(MovieFile.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(overdub_video
    PUBLIC
        overdub_engine
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
    PRIVATE
        juce::juce_recommended_warning_flags)
