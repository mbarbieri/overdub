#pragma once

#include <functional>

// Microphone access through AVCaptureDevice (ADR 0002): JUCE's RuntimePermissions is a no-op on
// macOS, and if the TCC prompt first appears inside CoreAudio's device open it blocks the message
// thread. So the app asks here, before it opens the audio device.
namespace overdub::app
{
// Message thread. Calls `done` on the message thread with whether the app may record: at once
// when the user already decided, or after the system prompt. Logs app.microphone_access (info
// when granted, warn otherwise) with `status`: authorized, granted, denied, restricted or refused.
void requestMicrophoneAccess (std::function<void (bool granted)> done);
} // namespace overdub::app
