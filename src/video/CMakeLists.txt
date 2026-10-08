# overdub_video: Video Take playback on the audio clock (ADR 0003, issue #36): the pure frame
# selection (heard position -> Video Take -> movie time -> frame), the movie's frame times and the
# frame decoder (AVAssetReader), and VideoPlayback, which pulls the frame for each display refresh.
# Events and Error codes use the `video.*` area.

add_library(overdub_video STATIC)

target_sources(overdub_video PRIVATE
    FrameSelection.cpp)

target_link_libraries(overdub_video
    PUBLIC
        overdub_engine
    PRIVATE
        juce::juce_recommended_warning_flags)
