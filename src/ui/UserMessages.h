#pragma once

#include "core/Log.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the user sees of a failure or a degraded situation (ADR 0006 "What the user sees"):
// - Notice: a non-modal one-liner that goes away on its own (degraded, nothing to decide).
// - Alert: a modal dialog, only when the user must decide or data was lost, and never during a
//   Pass: Alerts wait in the queue until the Pass stops.
// The one Error.code -> sentence table lives here, with a generic fallback naming the code.
// Pure: the Components present what takeReady() hands them and log ui.notice_shown,
// ui.alert_shown and ui.alert_deferred.
namespace overdub::ui
{
enum class Tier
{
    notice,
    alert
};

// "notice" or "alert": the `tier` field of ui.* events.
const char* tierName (Tier tier);

struct UserMessage
{
    Tier tier = Tier::alert;
    std::string code; // an Error.code or Notice code, e.g. "capture.pass_empty"
    core::Fields fields = {}; // the Error's fields, which a few sentences name

    bool operator== (const UserMessage&) const = default;
};

// The tier for a code: the Notice codes are listed; every other code is an Alert.
Tier tierFor (std::string_view code);

// The sentence the user reads for a code; unknown codes get a generic sentence that names it.
// ui.sample_rate_mismatch names song_rate and device_rate, and points to Settings only when
// rate_offered (the device offers the Song's rate); ui.device_rate_substituted names
// requested_rate and opened_rate.
std::string sentenceFor (std::string_view code, const core::Fields& fields = {});

// The rate a Notice offers to convert the Song to: the device's, for a ui.sample_rate_mismatch
// whose device does not offer the Song's rate (issue #160). None for any other message.
std::optional<int> conversionOfferedBy (const UserMessage& message);
// The Notice action's label: "Convert to 48000 Hz".
std::string conversionActionText (int toRate);

// {tierFor (code), code, fields}.
UserMessage userMessageFor (std::string_view code, core::Fields fields = {});

class UserMessageQueue
{
public:
    void post (UserMessage message);

    // Takes every queued Notice, and every queued Alert unless a Pass is running, in posting order.
    std::vector<UserMessage> takeReady (bool passRunning);
    // The Alerts a running Pass holds back, in posting order.
    std::vector<UserMessage> deferredAlerts() const;

private:
    std::vector<UserMessage> queued;
};
} // namespace overdub::ui
