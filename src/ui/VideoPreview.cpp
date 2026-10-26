#include "ui/VideoPreview.h"

#include "ui/Colours.h"

namespace overdub::ui
{
VideoPreview::VideoPreview()
    : camera (11.0f)
{
    addAndMakeVisible (camera);
    addChildComponent (playback);
}

void VideoPreview::show (void* previewLayer, const juce::String& message, bool showPlayback)
{
    camera.show (previewLayer, message);
    camera.setVisible (! showPlayback);
    playback.setVisible (showPlayback);
}

void VideoPreview::resized()
{
    camera.setBounds (getLocalBounds());
    playback.setBounds (getLocalBounds().reduced (1));
}

void VideoPreview::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
