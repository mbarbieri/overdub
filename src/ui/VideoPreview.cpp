#include "ui/VideoPreview.h"

#include "ui/Colours.h"

namespace overdub::ui
{
namespace
{
constexpr int inset = 6; // as the Mixer's strips sit inside its region (ui/Layout.cpp gap)
} // namespace

VideoPreview::VideoPreview()
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
    const auto area = getLocalBounds().reduced (inset);
    camera.setBounds (area);
    playback.setBounds (area.reduced (1));
}

void VideoPreview::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
