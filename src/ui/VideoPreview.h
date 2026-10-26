#pragma once

#include "ui/CameraStage.h"
#include "ui/VideoScreen.h"

#include <juce_gui_basics/juce_gui_basics.h>

// Edit mode's Video Preview (issue #198, GLOSSARY.md): the small picture docked at the Mixer
// region's right end while the camera is Armed. Like Record mode's stage it shows the camera
// (CameraStage: its live feed, or its line in small type while it has no picture) or Video Take playback
// (VideoScreen); which one is the caller's choice (stageShowsLiveFeed).
//
// Message thread only.
namespace overdub::ui
{
class VideoPreview final : public juce::Component
{
public:
    VideoPreview();

    // previewLayer: the camera's feed (a CALayer*, OpenSong::cameraPreviewLayer), or null for
    // `message`. playback: show `playback` instead of `camera`.
    void show (void* previewLayer, const juce::String& message, bool playback);
    bool showsPlayback() const { return playback.isVisible(); }

    void resized() override;
    void paint (juce::Graphics& g) override;

    CameraStage camera;
    VideoScreen playback;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoPreview)
};
} // namespace overdub::ui
