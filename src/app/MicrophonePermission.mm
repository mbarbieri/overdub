#include "app/MicrophonePermission.h"

#include "core/Log.h"

#include <juce_events/juce_events.h>

#import <AVFoundation/AVFoundation.h>

#include <memory>

namespace overdub::app
{
namespace
{
void report (bool granted, const char* status)
{
    core::logEvent (granted ? core::Level::info : core::Level::warn, "app.microphone_access", { { "granted", granted }, { "status", status } });
}
} // namespace

void requestMicrophoneAccess (std::function<void (bool granted)> done)
{
    const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (status)
    {
        case AVAuthorizationStatusAuthorized:
            report (true, "authorized");
            done (true);
            return;
        case AVAuthorizationStatusDenied:
            report (false, "denied");
            done (false);
            return;
        case AVAuthorizationStatusRestricted:
            report (false, "restricted");
            done (false);
            return;
        case AVAuthorizationStatusNotDetermined:
            break;
    }

    // The completion handler runs on an arbitrary queue; hop back to the message thread.
    auto shared = std::make_shared<std::function<void (bool)>> (std::move (done));
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                               const bool allowed = granted == YES;
                               juce::MessageManager::callAsync ([shared, allowed] {
                                   report (allowed, allowed ? "granted" : "refused");
                                   (*shared) (allowed);
                               });
                             }];
}
} // namespace overdub::app
