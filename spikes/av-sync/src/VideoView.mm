// PROTOTYPE — issue #7 spike.
#include "VideoView.h"
#include "SpikeLog.h"
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

using namespace juce;

struct VideoView::Impl
{
    NSView* view = nil;
    CALayer* frameLayer = nil;
    CALayer* previewLayer = nil;

    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    CMSampleBufferRef current = nullptr;
    CMSampleBufferRef next = nullptr;
    double movieDuration = 0;
    bool showingBlack = true;

    void releaseFrames()
    {
        if (current != nullptr) CFRelease (current);
        if (next != nullptr) CFRelease (next);
        current = next = nullptr;
    }

    CMSampleBufferRef readNext()
    {
        return output != nil ? [output copyNextSampleBuffer] : nullptr;
    }

    static double ptsSeconds (CMSampleBufferRef buffer)
    {
        return CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (buffer));
    }
};

VideoView::VideoView() : impl (std::make_unique<Impl>())
{
    impl->view = [[NSView alloc] initWithFrame:NSMakeRect (0, 0, 640, 360)];
    impl->view.wantsLayer = YES;
    impl->view.layer.backgroundColor = CGColorGetConstantColor (kCGColorBlack);

    impl->frameLayer = [CALayer layer];
    impl->frameLayer.contentsGravity = kCAGravityResizeAspect;
    impl->frameLayer.backgroundColor = CGColorGetConstantColor (kCGColorBlack);
    impl->frameLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [impl->view.layer addSublayer:impl->frameLayer];

    host.setView ((__bridge void*) impl->view);
    addAndMakeVisible (host);
}

VideoView::~VideoView()
{
    closeMovie();
    host.setView (nullptr);
}

void VideoView::showLivePreview (void* layer)
{
    closeMovie();
    impl->previewLayer = (__bridge CALayer*) layer;
    if (impl->previewLayer != nil)
    {
        impl->previewLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        [impl->view.layer addSublayer:impl->previewLayer];
        impl->previewLayer.hidden = NO;
    }
    impl->frameLayer.hidden = YES;
    resized();
}

bool VideoView::openMovie (const File& movie, String& error)
{
    closeMovie();

    auto* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:movie.getFullPathName().toRawUTF8()]];
    auto* asset = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    NSError* nsError = nil;
    impl->reader = [[AVAssetReader alloc] initWithAsset:asset error:&nsError];
    auto* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (impl->reader == nil || track == nil)
    {
        error = "cannot read video track of " + movie.getFullPathName();
        impl->reader = nil;
        return false;
    }

    NSDictionary* settings = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id) kCVPixelBufferIOSurfacePropertiesKey: @{} };
    impl->output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
    impl->output.alwaysCopiesSampleData = NO;
    [impl->reader addOutput:impl->output];
    if (! [impl->reader startReading])
    {
        error = "startReading failed";
        closeMovie();
        return false;
    }

    impl->movieDuration = CMTimeGetSeconds (asset.duration);
    impl->next = impl->readNext();
    if (impl->previewLayer != nil)
        impl->previewLayer.hidden = YES;
    impl->frameLayer.hidden = NO;
    impl->frameLayer.contents = nil;
    impl->showingBlack = true;

    spikelog::line ("video_open", { { "file", movie.getFullPathName() }, { "duration_s", String (impl->movieDuration, 3) } });
    return true;
}

void VideoView::closeMovie()
{
    impl->releaseFrames();
    if (impl->reader != nil)
        [impl->reader cancelReading];
    impl->reader = nil;
    impl->output = nil;
    impl->frameLayer.contents = nil;
}

double VideoView::showMovieTime (double t)
{
    if (impl->reader == nil)
        return -1;

    bool changed = false;
    while (impl->next != nullptr && Impl::ptsSeconds (impl->next) <= t)
    {
        if (impl->current != nullptr)
            CFRelease (impl->current);
        impl->current = impl->next;
        impl->next = impl->readNext();
        changed = true;
    }

    const bool outside = impl->current == nullptr || t < 0 || t >= impl->movieDuration;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (outside && ! impl->showingBlack)
    {
        impl->frameLayer.contents = nil;
        impl->showingBlack = true;
    }
    else if (! outside && (changed || impl->showingBlack))
    {
        auto pixelBuffer = CMSampleBufferGetImageBuffer (impl->current);
        impl->frameLayer.contents = (__bridge id) CVPixelBufferGetIOSurface (pixelBuffer);
        impl->showingBlack = false;
    }
    [CATransaction commit];

    return outside ? -1.0 : Impl::ptsSeconds (impl->current);
}

void VideoView::resized()
{
    host.setBounds (getLocalBounds());
    const auto bounds = impl->view.bounds;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    impl->frameLayer.frame = bounds;
    if (impl->previewLayer != nil)
        impl->previewLayer.frame = bounds;
    [CATransaction commit];
}
