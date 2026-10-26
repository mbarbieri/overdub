#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

// Record mode's camera stage (issue #35): the camera's live feed while the camera is Armed, a
// line of text otherwise. The feed is the camera's AVCaptureVideoPreviewLayer hosted in an
// NSView (CameraStage.mm); the stage never touches frames itself.
namespace overdub::ui
{
class CameraStage final : public juce::Component
{
public:
    // messageFontSize: the line's font height (the Edit Video Preview's is smaller than the stage's).
    explicit CameraStage (float messageFontSize = 13.0f);
    ~CameraStage() override;

    // The feed to show (a CALayer*, from OpenSong::cameraPreviewLayer), or null for `message`.
    void show (void* previewLayer, const juce::String& message);

    bool showsFeed() const { return currentLayer != nullptr; }
    const juce::String& message() const { return currentMessage; }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct FeedView; // the NSView hosting the layer, in CameraStage.mm

    float messageFontSize;
    void* currentLayer = nullptr;
    juce::String currentMessage;
    juce::NSViewComponent feed;
    std::unique_ptr<FeedView> feedView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraStage)
};
} // namespace overdub::ui
