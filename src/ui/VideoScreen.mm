#include "ui/VideoScreen.h"

#include "ui/Colours.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <QuartzCore/QuartzCore.h>

// Compiled with ARC (src/ui/CMakeLists.txt).
namespace overdub::ui
{
struct VideoScreen::DisplayView
{
    NSView* view = nil;
    AVSampleBufferDisplayLayer* layer = nil;
};

VideoScreen::VideoScreen()
{
    setInterceptsMouseClicks (false, false);
    setOpaque (true);
    addChildComponent (display);
}

VideoScreen::~VideoScreen()
{
    display.setView (nullptr);
}

void VideoScreen::ensureDisplayView()
{
    if (displayView != nullptr)
        return;
    displayView = std::make_unique<DisplayView>();
    displayView->layer = [AVSampleBufferDisplayLayer layer];
    displayView->layer.videoGravity = AVLayerVideoGravityResizeAspect;
    displayView->layer.backgroundColor = NSColor.blackColor.CGColor;
    // A layer-hosting view: the view shows exactly this layer, resized with it.
    displayView->view = [[NSView alloc] initWithFrame:NSMakeRect (0, 0, getWidth(), getHeight())];
    displayView->view.layer = displayView->layer;
    displayView->view.wantsLayer = YES;
    display.setView ((__bridge void*) displayView->view);
    resized();
}

void VideoScreen::show (const video::FrameChoice& choice, void* sampleBuffer)
{
    const auto wasShowingVideo = showsVideo();
    shown = choice;
    const auto sameFrame = displayed.has_value() && displayed->videoTakeId == choice.videoTakeId && displayed->frameIndex == choice.frameIndex;
    if (choice.videoTakeId.has_value() && sampleBuffer != nullptr && ! sameFrame)
    {
        ensureDisplayView();
        auto sample = (CMSampleBufferRef) sampleBuffer;
        // Shown at once, whatever its timestamp: the frame was chosen for this refresh.
        if (auto attachments = CMSampleBufferGetSampleAttachmentsArray (sample, true); attachments != nullptr && CFArrayGetCount (attachments) > 0)
            CFDictionarySetValue ((CFMutableDictionaryRef) CFArrayGetValueAtIndex (attachments, 0), kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        [displayView->layer.sampleBufferRenderer flush];
        [displayView->layer.sampleBufferRenderer enqueueSampleBuffer:sample];
        displayed = choice;
        ++displayedCount;
    }
    // A frame of another Video Take (this one's could not be decoded) is never shown: black.
    const auto holdsThisVideoTake = displayed.has_value() && displayed->videoTakeId == shown.videoTakeId;
    display.setVisible (showsVideo() && displayView != nullptr && holdsThisVideoTake);
    if (wasShowingVideo != showsVideo())
        repaint();
}

void VideoScreen::resized()
{
    display.setBounds (getLocalBounds());
}

void VideoScreen::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    if (showsVideo())
        return;
    g.setColour (colours::dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (noVideoTakeMessage, getLocalBounds(), juce::Justification::centred);
}
} // namespace overdub::ui
