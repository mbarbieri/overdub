#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The camera a Pass records its Video Take from (ADR 0003): a plain C++ interface over
// AVCaptureSession (capture/AvCamera.h in the app) so tests use a fake and never open a camera.
//
// Threads: every call comes from one non-audio thread (the message thread in the app). The real
// camera delivers frames on its own `video` dispatch queue.
namespace overdub::capture
{
struct CameraDevice
{
    std::string id;   // AVCaptureDevice uniqueID: stable on this machine
    std::string name; // localized name, for the picker

    bool operator== (const CameraDevice&) const = default;
};

// What a finished movie holds, measured from the frames' own timestamps (never from frame counts:
// cameras stall for over a second without reporting a drop).
struct CameraMovie
{
    std::string cameraId;
    std::uint64_t firstFrameHostNs = 0; // the first written frame's PTS converted to host time
    double captureClockRate = 1.0;      // the capture clock's rate against host time
    std::int64_t frames = 0;            // frames written
    std::int64_t droppedFrames = 0;     // frames the capture output dropped or the writer refused
    double durationSeconds = 0.0;       // movie time from the first frame to the end of the last
    double longestGapSeconds = 0.0;     // the longest interval between two written frames
};

// A movie being completed off the camera's calling thread (issue #70); ready once the file is.
using MovieCompletion = std::future<core::Expected<CameraMovie>>;

// A completion that is already ready: for a movie that could not be ended, or completed at once.
inline MovieCompletion completedMovie (core::Expected<CameraMovie> result)
{
    std::promise<core::Expected<CameraMovie>> ready;
    ready.set_value (std::move (result));
    return ready.get_future();
}

class Camera
{
public:
    virtual ~Camera() = default;

    // The cameras this machine has now.
    virtual std::vector<CameraDevice> devices() = 0;
    // True once after a camera was connected or disconnected since the previous call (issue #41):
    // the caller lists devices() again. The open camera stays "open" after it is unplugged (its
    // feed and a movie being written simply stop getting frames) until close().
    virtual bool takeDevicesChanged() = 0;

    // Opens this camera (the system default for an empty or unknown id) and starts its live feed;
    // a camera already open is closed first. Returns the camera opened.
    // Errors: capture.camera_access_denied, capture.camera_unavailable (camera_id, reason).
    virtual core::Expected<CameraDevice> open (const std::string& cameraId) = 0;
    // Stops the feed (and a movie being written, which is discarded).
    virtual void close() = 0;
    // The open camera; none when closed.
    virtual std::optional<CameraDevice> openCamera() const = 0;

    // Starts writing every frame from the next one on into `movieFile` (H.264 QuickTime movie).
    // Errors: capture.camera_not_open, capture.movie_write_failed (path, reason).
    virtual core::Expected<void> beginMovie (const juce::File& movieFile) = 0;
    // Stops writing and returns at once; the file is completed off the calling thread (AVFoundation
    // may take seconds, issue #70) and the future becomes ready once it is. The movie's facts (camera,
    // clock rate, drops) are those of the moment it ended: the camera may close or begin the next
    // movie meanwhile. Implementations complete it with std::async, whose future waits for the
    // completion when destroyed.
    // Errors: capture.camera_not_open (no movie is being written, e.g. the camera closed),
    // capture.movie_no_frames, capture.movie_write_failed (path, reason).
    virtual MovieCompletion endMovie() = 0;
    // Stops writing without finishing the file, and deletes it: a movie no Pass will keep.
    virtual void cancelMovie() = 0;

    // The live feed as an AVCaptureVideoPreviewLayer (a CALayer*); null when closed or fake.
    virtual void* previewLayer() const = 0;
};
} // namespace overdub::capture
