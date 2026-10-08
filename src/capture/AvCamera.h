#pragma once

#include "capture/Camera.h"

#include <functional>
#include <memory>

// The real camera (ADR 0003): AVCaptureSession with the camera only (audio comes from the
// engine), AVCaptureVideoDataOutput on a serial `video` dispatch queue, and capture/MovieWriter.h
// for the movie. The first written frame's PTS is converted to host time with CMSyncConvertTime
// from the session's synchronization clock. Logs capture.camera_opened / capture.camera_closed
// (camera id and name, format, frame rate, sync clock). Never used by tests (ADR 0005): they use a
// fake Camera.
namespace overdub::capture
{
std::unique_ptr<Camera> makeAvCamera();

// Message thread. Calls `done` on the message thread with whether the app may use the camera: at
// once when the user already decided, or after the system prompt. Logs capture.camera_access
// (info when granted, warn otherwise) with `status`: authorized, granted, denied, restricted or
// refused.
void requestCameraAccess (std::function<void (bool granted)> done);
} // namespace overdub::capture
