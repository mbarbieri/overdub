// PROTOTYPE — issue #7 spike.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

// A layer-backed NSView: the live camera preview while recording, or Video Take frames
// pulled by movie time during playback (option P3 in research/av-sync.md: the audio
// transport is the only clock; this view never runs its own).
class VideoView final : public juce::Component
{
public:
    VideoView();
    ~VideoView() override;

    void showLivePreview (void* previewLayer); // AVCaptureVideoPreviewLayer*

    bool openMovie (const juce::File& movie, juce::String& error);
    void closeMovie();

    // Shows the latest frame whose movie time is <= t; black outside the movie.
    // Forward-only. Returns the shown frame's movie time, or -1 when black.
    double showMovieTime (double t);

    void resized() override;

    struct Impl;

private:
    juce::NSViewComponent host;
    std::unique_ptr<Impl> impl;
};
