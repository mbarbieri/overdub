#pragma once

#include "capture/PassRecorder.h"
#include "song/SampleRateConversion.h"
#include "song/SeparationRun.h"
#include "ui/ExportRun.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// What an OpenSong runs besides playback (issue #145): nothing, a Pass, a Separation, a sample rate
// conversion or an export, never two at once. Which Intents each one refuses is ui/Availability.h's.
namespace overdub::ui
{
struct Idle
{
};

struct PassActivity
{
    std::unique_ptr<capture::PassRecorder> recorder;
    int number = 0; // from 1
    // The record point the Pass waits for, when it started with pre-roll.
    std::optional<song::SampleCount> preRollPoint;
    // The Armed camera was unplugged during the Pass: it is disarmed once the Pass is finished.
    bool cameraLost = false;
};

struct SeparationActivity
{
    std::unique_ptr<song::SeparationRun> run;
};

struct ConversionActivity
{
    std::unique_ptr<song::SampleRateConversionRun> run;
};

struct ExportActivity
{
    std::unique_ptr<ExportRun> run;
};

using Activity = std::variant<Idle, PassActivity, SeparationActivity, ConversionActivity, ExportActivity>;

// What runs in the app (issue #167): an OpenSong's activity, or a Workspace's calibration run.
enum class ActivityKind
{
    idle,
    pass,
    separation,
    conversion,
    exportSong,
    calibrationLatency,
    calibrationClap,
};
inline constexpr std::pair<ActivityKind, std::string_view> activityKindNames[] {
    { ActivityKind::idle, "idle" },
    { ActivityKind::pass, "pass" },
    { ActivityKind::separation, "separation" },
    { ActivityKind::conversion, "conversion" },
    { ActivityKind::exportSong, "export" },
    { ActivityKind::calibrationLatency, "calibration_latency" },
    { ActivityKind::calibrationClap, "calibration_clap" },
};
std::string_view activityKindName (ActivityKind kind);
ActivityKind kindOf (const Activity& activity);

// How the last activity to finish ended: the code of the Error it ended with, none on success.
struct ActivityFinish
{
    ActivityKind kind = ActivityKind::idle;
    std::uint64_t counter = 0;
    std::optional<std::string> errorCode;
};

// The app's activities in order: start() numbers each one from 1 (its counter), finish() records
// how it ended. A Workspace owns one and shares it with its OpenSong, so the counter never goes
// back when a Song closes.
class ActivityLog
{
public:
    std::uint64_t start() { return ++started; }
    void finish (ActivityKind kind, std::uint64_t counter, std::optional<std::string> errorCode)
    {
        last = ActivityFinish { kind, counter, std::move (errorCode) };
    }

    std::uint64_t counter() const { return started; }
    const std::optional<ActivityFinish>& lastFinish() const { return last; }

private:
    std::uint64_t started = 0;
    std::optional<ActivityFinish> last;
};
} // namespace overdub::ui
