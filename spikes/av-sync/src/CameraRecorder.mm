// PROTOTYPE — issue #7 spike.
#include "CameraRecorder.h"
#include "ClockMap.h"
#include "SpikeLog.h"
#include <juce_events/juce_events.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <string>

using namespace juce;

namespace
{
int64_t toNs (CMTime t)
{
    return CMTimeConvertScale (t, 1000000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}

String describeClock (CMClockRef clock)
{
    if (clock == nullptr)
        return "null";
    if (CFEqual (clock, CMClockGetHostTimeClock()))
        return "CMClockGetHostTimeClock";
    CFStringRef description = CFCopyDescription (clock);
    String text = String::fromCFString (description);
    CFRelease (description);
    return text.replaceCharacters ("\n\r", "  ");
}
}

struct CameraRecorder::Impl
{
    AVCaptureSession* session = nil;
    AVCaptureDevice* device = nil;
    AVCaptureVideoDataOutput* output = nil;
    AVCaptureVideoPreviewLayer* preview = nil;
    dispatch_queue_t queue = nil;
    id delegate = nil;

    // Everything below is touched only on `queue`.
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* writerInput = nil;
    bool writing = false;
    bool started = false;
    std::string csv;
    CameraTakeResult result;
    int64_t lastPtsNs = 0;
    double sumIntervalMs = 0;
    double sumDeliveryMs = 0;

    void handleFrame (CMSampleBufferRef sampleBuffer);
    void handleDrop() { if (writing) ++result.captureDrops; }
};

@interface OverdubVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic) CameraRecorder::Impl* owner;
@end

@implementation OverdubVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    self.owner->handleFrame (sampleBuffer);
}

- (void)captureOutput:(AVCaptureOutput*)output didDropSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    self.owner->handleDrop();
}
@end

void CameraRecorder::Impl::handleFrame (CMSampleBufferRef sampleBuffer)
{
    if (! writing)
        return;

    const auto deliveryNs = (int64_t) hostNowNs();
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp (sampleBuffer);
    const CMTime host = CMSyncConvertTime (pts, session.synchronizationClock, CMClockGetHostTimeClock());
    const auto ptsNs = toNs (pts);
    const auto hostNs = toNs (host);

    if (! started)
    {
        if (! [writer startWriting])
        {
            result.error = "startWriting failed: " + String (writer.error.localizedDescription.UTF8String);
            writing = false;
            return;
        }
        [writer startSessionAtSourceTime:pts];
        started = true;
        result.firstPtsNs = ptsNs;
        result.firstHostNs = hostNs;
        result.relativeRateStart = CMSyncGetRelativeRate (session.synchronizationClock, CMClockGetHostTimeClock());
        lastPtsNs = ptsNs;
    }

    bool appended = false;
    if (writerInput.readyForMoreMediaData)
        appended = [writerInput appendSampleBuffer:sampleBuffer];
    if (! appended)
        ++result.writerDrops;

    const double intervalMs = (double) (ptsNs - lastPtsNs) / 1.0e6;
    if (result.frames > 0)
    {
        sumIntervalMs += intervalMs;
        result.maxFrameIntervalMs = std::max (result.maxFrameIntervalMs, intervalMs);
    }
    sumDeliveryMs += (double) (deliveryNs - hostNs) / 1.0e6;
    const double divergenceUs = (double) ((hostNs - result.firstHostNs) - (ptsNs - result.firstPtsNs)) / 1.0e3;
    result.maxHostVsPtsDivergenceUs = std::max (result.maxHostVsPtsDivergenceUs, std::abs (divergenceUs));
    lastPtsNs = ptsNs;

    csv += std::to_string (result.frames) + "," + std::to_string (ptsNs) + "," + std::to_string (hostNs) + ","
         + std::to_string (deliveryNs) + "," + (appended ? "1" : "0") + "\n";
    ++result.frames;
}

CameraRecorder::CameraRecorder() : impl (std::make_unique<Impl>()) {}

CameraRecorder::~CameraRecorder()
{
    if (impl->session != nil)
        [impl->session stopRunning];
}

String CameraRecorder::start()
{
    impl->device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (impl->device == nil)
        return "no camera found";

    NSError* error = nil;
    auto* input = [AVCaptureDeviceInput deviceInputWithDevice:impl->device error:&error];
    if (input == nil)
        return "camera input failed: " + String (error.localizedDescription.UTF8String);

    impl->session = [[AVCaptureSession alloc] init];
    [impl->session beginConfiguration];
    if (! [impl->session canAddInput:input])
        return "cannot add camera input";
    [impl->session addInput:input];

    impl->queue = dispatch_queue_create ("overdub.camera", DISPATCH_QUEUE_SERIAL);
    auto* delegate = [[OverdubVideoDelegate alloc] init];
    delegate.owner = impl.get();
    impl->delegate = delegate;

    impl->output = [[AVCaptureVideoDataOutput alloc] init];
    impl->output.alwaysDiscardsLateVideoFrames = YES;
    [impl->output setSampleBufferDelegate:delegate queue:impl->queue];
    if (! [impl->session canAddOutput:impl->output])
        return "cannot add video data output";
    [impl->session addOutput:impl->output];
    [impl->session commitConfiguration];

    impl->preview = [AVCaptureVideoPreviewLayer layerWithSession:impl->session];
    impl->preview.videoGravity = AVLayerVideoGravityResizeAspect;
    [impl->session startRunning];

    const auto dims = CMVideoFormatDescriptionGetDimensions (impl->device.activeFormat.formatDescription);
    const auto frameDuration = impl->device.activeVideoMinFrameDuration;
    spikelog::line ("camera_start", { { "camera", cameraName() },
                                      { "unique_id", String (impl->device.uniqueID.UTF8String) },
                                      { "width", String (dims.width) },
                                      { "height", String (dims.height) },
                                      { "fps", String (CMTimeGetSeconds (frameDuration) > 0 ? 1.0 / CMTimeGetSeconds (frameDuration) : 0.0, 2) },
                                      { "running", impl->session.running ? "yes" : "no" },
                                      { "sync_clock", clockDescription() },
                                      { "relative_rate", String (CMSyncGetRelativeRate (impl->session.synchronizationClock, CMClockGetHostTimeClock()), 9) } });
    return {};
}

void* CameraRecorder::previewLayer() const
{
    return (__bridge void*) impl->preview;
}

String CameraRecorder::cameraName() const
{
    return impl->device != nil ? String (impl->device.localizedName.UTF8String) : String();
}

String CameraRecorder::clockDescription() const
{
    return impl->session != nil ? describeClock (impl->session.synchronizationClock) : String ("no session");
}

String CameraRecorder::beginWriting (const File& movieFile, const File& framesCsv)
{
    movieFile.deleteFile();
    NSError* error = nil;
    auto* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:movieFile.getFullPathName().toRawUTF8()]];
    auto* writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    if (writer == nil)
        return "AVAssetWriter failed: " + String (error.localizedDescription.UTF8String);

    const auto dims = CMVideoFormatDescriptionGetDimensions (impl->device.activeFormat.formatDescription);
    NSDictionary* settings = @{ AVVideoCodecKey: AVVideoCodecTypeH264,
                                AVVideoWidthKey: @(dims.width),
                                AVVideoHeightKey: @(dims.height) };
    auto* writerInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
    writerInput.expectsMediaDataInRealTime = YES;
    if (! [writer canAddInput:writerInput])
        return "cannot add writer input";
    [writer addInput:writerInput];

    dispatch_sync (impl->queue, ^{
        impl->writer = writer;
        impl->writerInput = writerInput;
        impl->started = false;
        impl->csv = "frame,pts_ns,host_ns,delivery_ns,appended\n";
        impl->result = {};
        impl->result.clockDescription = describeClock (impl->session.synchronizationClock);
        impl->sumIntervalMs = 0;
        impl->sumDeliveryMs = 0;
        impl->writing = true;
    });

    spikelog::line ("camera_write_begin", { { "file", movieFile.getFullPathName() }, { "frames_csv", framesCsv.getFullPathName() } });
    framesCsvPath = framesCsv;
    return {};
}

void CameraRecorder::endWriting (std::function<void (CameraTakeResult)> onDone)
{
    __block CameraTakeResult result;
    __block std::string csv;
    __block AVAssetWriter* writer = nil;
    __block AVAssetWriterInput* writerInput = nil;
    __block bool started = false;

    dispatch_sync (impl->queue, ^{
        impl->writing = false;
        result = impl->result;
        result.relativeRateEnd = CMSyncGetRelativeRate (impl->session.synchronizationClock, CMClockGetHostTimeClock());
        if (result.frames > 1)
        {
            result.meanFrameIntervalMs = impl->sumIntervalMs / (result.frames - 1);
            result.durationSeconds = (double) (impl->lastPtsNs - result.firstPtsNs) / 1.0e9 + result.meanFrameIntervalMs / 1000.0;
        }
        if (result.frames > 0)
            result.meanDeliveryDelayMs = impl->sumDeliveryMs / result.frames;
        csv = std::move (impl->csv);
        writer = impl->writer;
        writerInput = impl->writerInput;
        started = impl->started;
        impl->writer = nil;
        impl->writerInput = nil;
    });

    framesCsvPath.replaceWithText (String (csv));

    if (! started)
    {
        CameraTakeResult failed = result;
        if (failed.error.isEmpty())
            failed.error = "no frames arrived while writing";
        MessageManager::callAsync ([onDone, failed] { onDone (failed); });
        return;
    }

    [writerInput markAsFinished];
    [writer finishWritingWithCompletionHandler:^{
        CameraTakeResult finished = result;
        finished.ok = writer.status == AVAssetWriterStatusCompleted;
        if (! finished.ok)
            finished.error = "finishWriting status " + String ((int) writer.status) + ": "
                           + String (writer.error != nil ? writer.error.localizedDescription.UTF8String : "");
        MessageManager::callAsync ([onDone, finished] { onDone (finished); });
    }];
}

void requestCaptureAccess (std::function<void (bool, bool)> onDone)
{
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL camera) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL microphone) {
            MessageManager::callAsync ([onDone, camera, microphone] { onDone (camera, microphone); });
        }];
    }];
}
