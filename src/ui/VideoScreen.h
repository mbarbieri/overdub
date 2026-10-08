#pragma once

#include "video/FrameSelection.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

// A screen that shows Video Take playback (issue #36, ADR 0001): decoded frames go to an
// AVSampleBufferDisplayLayer hosted in an NSView (VideoScreen.mm), displayed at once; with no
// Video Take at the playhead it is black with "No Video Take at the playhead". The frame shown is
// chosen elsewhere (video::VideoPlayback); the screen only displays it. Off-screen (tests,
// snapshots) the layer area renders blank.
namespace overdub::ui
{
inline constexpr const char* noVideoTakeMessage = "No Video Take at the playhead";

class VideoScreen final : public juce::Component
{
public:
    VideoScreen();
    ~VideoScreen() override;

    // Shows this choice. `sampleBuffer` is its decoded frame (a CMSampleBufferRef), enqueued when
    // its Video Take or frame differs from the one the layer holds; null keeps the layer as it is.
    void show (const video::FrameChoice& choice, void* sampleBuffer);

    // The choice shown last.
    const video::FrameChoice& shownChoice() const { return shown; }
    // True while a Video Take is at the playhead (its frames show, not the message).
    bool showsVideo() const { return shown.videoTakeId.has_value(); }
    juce::String message() const { return showsVideo() ? juce::String() : juce::String (noVideoTakeMessage); }
    // Frames enqueued on the display layer since construction.
    int framesDisplayed() const { return displayedCount; }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct DisplayView; // the NSView and its AVSampleBufferDisplayLayer, in VideoScreen.mm

    void ensureDisplayView();

    video::FrameChoice shown;
    std::optional<video::FrameChoice> displayed; // the choice whose frame the layer holds
    int displayedCount = 0;
    juce::NSViewComponent display;
    std::unique_ptr<DisplayView> displayView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoScreen)
};
} // namespace overdub::ui
