#include "capture/AvCamera.h"

#include "capture/MovieWriter.h"
#include "core/Log.h"

#include <juce_events/juce_events.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <atomic>
#include <memory>
#include <optional>

// Compiled with ARC (src/capture/CMakeLists.txt).
namespace overdub::capture
{
class AvCamera;
}

@interface OverdubCaptureVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) overdub::capture::AvCamera* owner;
@end

namespace overdub::capture
{
namespace
{
std::string toStd (NSString* text)
{
    const char* utf8 = text != nil ? text.UTF8String : nullptr;
    return utf8 != nullptr ? std::string (utf8) : std::string {};
}

std::uint64_t toHostNs (CMTime time)
{
    return (std::uint64_t) CMTimeConvertScale (time, 1'000'000'000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}

NSArray<AVCaptureDeviceType>* cameraTypes()
{
    if (@available (macOS 14.0, *))
        return @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeContinuityCamera ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown ];
#pragma clang diagnostic pop
}

core::Error unavailable (const std::string& cameraId, const std::string& reason)
{
    return core::Error { "capture.camera_unavailable", "the camera could not be opened", { { "camera_id", cameraId }, { "reason", reason } } };
}
} // namespace

class AvCamera final : public Camera
{
public:
    AvCamera()
    {
        videoQueue = dispatch_queue_create ("com.mbarbieri.overdub.video", DISPATCH_QUEUE_SERIAL);
        sessionQueue = dispatch_queue_create ("com.mbarbieri.overdub.camera-session", DISPATCH_QUEUE_SERIAL);
        delegate = [[OverdubCaptureVideoDelegate alloc] init];
        delegate.owner = this;

        // A camera plugged in or out (issue #41): the owner lists devices() again at its next poll.
        auto changed = devicesChanged;
        auto note = ^(NSNotification* notification) {
          auto* camera = (AVCaptureDevice*) notification.object;
          if (camera != nil && ! [camera hasMediaType:AVMediaTypeVideo])
              return;
          changed->store (true);
          core::logEvent (core::Level::info,
                          "capture.camera_device_changed",
                          { { "camera_id", camera != nil ? toStd (camera.uniqueID) : std::string {} },
                            { "connected", [notification.name isEqualToString:AVCaptureDeviceWasConnectedNotification] ? true : false } });
        };
        auto* center = [NSNotificationCenter defaultCenter];
        connectedObserver = [center addObserverForName:AVCaptureDeviceWasConnectedNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:note];
        disconnectedObserver = [center addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:note];
    }

    ~AvCamera() override
    {
        [[NSNotificationCenter defaultCenter] removeObserver:connectedObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:disconnectedObserver];
        close();
        // No frame callback may still be running into this object.
        dispatch_sync (sessionQueue, ^ {
                       });
        dispatch_sync (videoQueue, ^ {
                       });
        delegate.owner = nullptr;
    }

    std::vector<CameraDevice> devices() override
    {
        auto* discovery = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:cameraTypes()
                                                                                 mediaType:AVMediaTypeVideo
                                                                                  position:AVCaptureDevicePositionUnspecified];
        std::vector<CameraDevice> found;
        for (AVCaptureDevice* camera in discovery.devices)
            found.push_back (CameraDevice { toStd (camera.uniqueID), toStd (camera.localizedName) });
        return found;
    }

    bool takeDevicesChanged() override { return devicesChanged->exchange (false); }

    core::Expected<CameraDevice> open (const std::string& cameraId) override
    {
        close();
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            return std::unexpected (core::Error { "capture.camera_access_denied", "the app may not use the camera", { { "camera_id", cameraId } } });

        NSString* uniqueId = [NSString stringWithUTF8String:cameraId.c_str()];
        AVCaptureDevice* chosen = cameraId.empty() || uniqueId == nil ? nil : [AVCaptureDevice deviceWithUniqueID:(NSString* _Nonnull) uniqueId];
        if (chosen == nil)
            chosen = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (chosen == nil)
            return std::unexpected (unavailable (cameraId, "no camera found"));

        NSError* error = nil;
        auto* input = [AVCaptureDeviceInput deviceInputWithDevice:chosen error:&error];
        if (input == nil)
            return std::unexpected (unavailable (cameraId, error != nil ? toStd (error.localizedDescription) : "no input"));

        auto* newSession = [[AVCaptureSession alloc] init];
        [newSession beginConfiguration];
        if (! [newSession canAddInput:input])
            return std::unexpected (unavailable (cameraId, "the session refused the camera input"));
        [newSession addInput:input];
        auto* newOutput = [[AVCaptureVideoDataOutput alloc] init];
        newOutput.alwaysDiscardsLateVideoFrames = YES;
        [newOutput setSampleBufferDelegate:delegate queue:videoQueue];
        if (! [newSession canAddOutput:newOutput])
            return std::unexpected (unavailable (cameraId, "the session refused the video output"));
        [newSession addOutput:newOutput];
        [newSession commitConfiguration];

        session = newSession;
        output = newOutput;
        device = chosen;
        preview = [AVCaptureVideoPreviewLayer layerWithSession:newSession];
        preview.videoGravity = AVLayerVideoGravityResizeAspect;
        syncClock = CMClockGetHostTimeClock();
        if (@available (macOS 12.3, *))
            if (newSession.synchronizationClock != nullptr)
                syncClock = newSession.synchronizationClock;
        current = CameraDevice { toStd (chosen.uniqueID), toStd (chosen.localizedName) };

        // startRunning blocks for a while: keep it off the message thread.
        dispatch_async (sessionQueue, ^{
          [newSession startRunning];
        });

        const auto dimensions = CMVideoFormatDescriptionGetDimensions (chosen.activeFormat.formatDescription);
        const auto minFrame = CMTimeGetSeconds (chosen.activeVideoMinFrameDuration);
        core::logEvent (core::Level::info,
                        "capture.camera_opened",
                        { { "camera_id", current->id },
                          { "camera_name", current->name },
                          { "width", dimensions.width },
                          { "height", dimensions.height },
                          { "max_fps", minFrame > 0.0 ? 1.0 / minFrame : 0.0 },
                          { "sync_clock_is_host", CFEqual (syncClock, CMClockGetHostTimeClock()) ? true : false } });
        return *current;
    }

    void close() override
    {
        if (session == nil)
            return;
        auto* closing = session;
        cancelMovie();
        dispatch_async (sessionQueue, ^{
          [closing stopRunning];
        });
        core::logEvent (core::Level::info, "capture.camera_closed", { { "camera_id", current.has_value() ? current->id : std::string {} } });
        session = nil;
        output = nil;
        device = nil;
        preview = nil;
        current.reset();
        syncClock = nullptr; // owned by the closed session
    }

    std::optional<CameraDevice> openCamera() const override { return current; }

    core::Expected<void> beginMovie (const juce::File& movieFile) override
    {
        if (session == nil || device == nil)
            return std::unexpected (core::Error { "capture.camera_not_open", "no camera is open", {} });
        const auto dimensions = CMVideoFormatDescriptionGetDimensions (device.activeFormat.formatDescription);
        auto created = MovieWriter::create (movieFile, dimensions.width, dimensions.height);
        if (! created.has_value())
            return std::unexpected (created.error());

        __block auto writer = std::move (*created);
        dispatch_sync (videoQueue, ^{
          movie = std::move (writer);
          firstFrameHostNs.reset();
          droppedFrames = 0;
        });
        return {};
    }

    core::Expected<CameraMovie> endMovie() override
    {
        __block std::unique_ptr<MovieWriter> finishing;
        __block std::optional<std::uint64_t> firstHostNs;
        __block std::int64_t dropped = 0;
        dispatch_sync (videoQueue, ^{
          finishing = std::move (movie);
          firstHostNs = firstFrameHostNs;
          dropped = droppedFrames;
        });
        if (finishing == nullptr)
            return std::unexpected (core::Error { "capture.camera_not_open", "no movie was being written: the camera closed", {} });

        const auto stats = finishing->finish();
        if (! stats.has_value())
            return std::unexpected (stats.error());

        if (! firstHostNs.has_value())
            return std::unexpected (core::Error { "capture.movie_write_failed", "the movie's start time is unknown", { { "path", finishing->file().getFullPathName().toStdString() }, { "reason", "no first frame host time" } } });

        CameraMovie finished;
        finished.cameraId = current.has_value() ? current->id : std::string {};
        finished.firstFrameHostNs = *firstHostNs;
        finished.captureClockRate = syncClock != nullptr ? CMSyncGetRelativeRate (syncClock, CMClockGetHostTimeClock()) : 1.0;
        finished.frames = stats->frames;
        finished.droppedFrames = dropped + stats->refusedFrames;
        finished.durationSeconds = stats->durationSeconds();
        finished.longestGapSeconds = (double) stats->longestGapNs / 1.0e9;
        return finished;
    }

    void cancelMovie() override
    {
        __block std::unique_ptr<MovieWriter> abandoned;
        dispatch_sync (videoQueue, ^{
          abandoned = std::move (movie);
        });
        if (abandoned == nullptr)
            return;
        const auto file = abandoned->file();
        abandoned.reset(); // cancels the writer
        file.deleteFile();
    }

    void* previewLayer() const override { return (__bridge void*) preview; }

    //==========================================================================
    // `video` queue

    void handleFrame (CMSampleBufferRef sampleBuffer)
    {
        if (movie == nullptr)
            return;
        core::setCurrentThreadRole (core::ThreadRole::video);
        const auto sessionStarted = movie->stats().sessionStartPtsNs.has_value();
        movie->appendSampleBuffer ((void*) sampleBuffer);
        // Movie time 0 is the frame that started the writer's session, written or not.
        if (! sessionStarted && movie->stats().sessionStartPtsNs.has_value())
        {
            const auto pts = CMSampleBufferGetPresentationTimeStamp (sampleBuffer);
            firstFrameHostNs = toHostNs (CMSyncConvertTime (pts, syncClock, CMClockGetHostTimeClock()));
        }
    }

    void handleDrop()
    {
        if (movie != nullptr)
            ++droppedFrames;
    }

private:
    dispatch_queue_t videoQueue = nil;
    dispatch_queue_t sessionQueue = nil;
    OverdubCaptureVideoDelegate* delegate = nil;

    // Set by the connect / disconnect observers (main queue), taken by takeDevicesChanged.
    // Shared, so an observer block never reaches into a destroyed camera.
    std::shared_ptr<std::atomic<bool>> devicesChanged = std::make_shared<std::atomic<bool>> (false);
    id connectedObserver = nil;
    id disconnectedObserver = nil;

    // Message thread.
    AVCaptureSession* session = nil;
    AVCaptureVideoDataOutput* output = nil;
    AVCaptureDevice* device = nil;
    AVCaptureVideoPreviewLayer* preview = nil;
    std::optional<CameraDevice> current;
    CMClockRef syncClock = nullptr; // owned by the session (or the host clock); read on both

    // `video` queue only (handed over with dispatch_sync).
    std::unique_ptr<MovieWriter> movie;
    std::optional<std::uint64_t> firstFrameHostNs;
    std::int64_t droppedFrames = 0;
};

std::unique_ptr<Camera> makeAvCamera()
{
    return std::make_unique<AvCamera>();
}

namespace
{
void reportAccess (bool granted, const char* status)
{
    core::logEvent (granted ? core::Level::info : core::Level::warn, "capture.camera_access", { { "granted", granted }, { "status", status } });
}
} // namespace

void requestCameraAccess (std::function<void (bool granted)> done)
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo])
    {
        case AVAuthorizationStatusAuthorized:
            reportAccess (true, "authorized");
            done (true);
            return;
        case AVAuthorizationStatusDenied:
            reportAccess (false, "denied");
            done (false);
            return;
        case AVAuthorizationStatusRestricted:
            reportAccess (false, "restricted");
            done (false);
            return;
        case AVAuthorizationStatusNotDetermined:
            break;
    }

    // The completion handler runs on an arbitrary queue; hop back to the message thread.
    auto shared = std::make_shared<std::function<void (bool)>> (std::move (done));
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) {
                               const bool allowed = granted == YES;
                               juce::MessageManager::callAsync ([shared, allowed] {
                                   reportAccess (allowed, allowed ? "granted" : "refused");
                                   (*shared) (allowed);
                               });
                             }];
}
} // namespace overdub::capture

@implementation OverdubCaptureVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    if (self.owner != nullptr)
        self.owner->handleFrame (sampleBuffer);
}

- (void)captureOutput:(AVCaptureOutput*)output didDropSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    if (self.owner != nullptr)
        self.owner->handleDrop();
}
@end
