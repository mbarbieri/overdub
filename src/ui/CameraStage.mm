#include "ui/CameraStage.h"

#include "ui/Colours.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

// Compiled with ARC (src/ui/CMakeLists.txt).
namespace overdub::ui
{
struct CameraStage::FeedView
{
    NSView* view = nil;
};

CameraStage::CameraStage (float fontSize)
    : messageFontSize (fontSize)
{
    setInterceptsMouseClicks (false, false);
    addChildComponent (feed);
}

CameraStage::~CameraStage()
{
    feed.setView (nullptr);
}

void CameraStage::show (void* previewLayer, const juce::String& message)
{
    currentMessage = message;
    if (previewLayer != currentLayer)
    {
        currentLayer = previewLayer;
        feed.setView (nullptr);
        feedView.reset();
        if (previewLayer != nullptr)
        {
            feedView = std::make_unique<FeedView>();
            auto* layer = (__bridge CALayer*) previewLayer;
            // A layer-hosting view: the view shows exactly this layer, resized with it.
            feedView->view = [[NSView alloc] initWithFrame:NSMakeRect (0, 0, getWidth(), getHeight())];
            feedView->view.layer = layer;
            feedView->view.wantsLayer = YES;
            layer.backgroundColor = NSColor.blackColor.CGColor;
            feed.setView ((__bridge void*) feedView->view);
        }
        feed.setVisible (previewLayer != nullptr);
        resized();
    }
    repaint();
}

void CameraStage::resized()
{
    feed.setBounds (getLocalBounds().reduced (1));
}

void CameraStage::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (currentLayer != nullptr ? juce::Colours::black : colours::panel.withAlpha (0.45f));
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);
    if (currentLayer != nullptr)
        return;
    g.setColour (colours::dimText);
    g.setFont (juce::FontOptions (messageFontSize));
    g.drawFittedText (currentMessage, getLocalBounds().reduced (8, 0), juce::Justification::centred, 3);
}
} // namespace overdub::ui
