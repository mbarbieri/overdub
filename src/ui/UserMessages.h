#pragma once

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

    bool operator== (const UserMessage&) const = default;
};

// The tier for a code: the Notice codes are listed; every other code is an Alert.
Tier tierFor (std::string_view code);

// The sentence the user reads for a code; unknown codes get a generic sentence that names it.
std::string sentenceFor (std::string_view code);

// {tierFor (code), code}.
UserMessage userMessageFor (std::string_view code);

class UserMessageQueue
{
public:
    void post (UserMessage message);

    // Takes every queued Notice, and every queued Alert unless a Pass is running, in posting order.
    std::vector<UserMessage> takeReady (bool passRunning);

private:
    std::vector<UserMessage> queued;
};
} // namespace overdub::ui
