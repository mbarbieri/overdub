#include "ui/Activity.h"

#include <utility>

namespace overdub::ui
{
namespace
{
struct GestureRule
{
    const char* name;
    bool refusedByPass;
    bool refusedBySeparation;
    bool refusedByConversion;
    bool refusedByExport;
    bool refusedByCalibration;
};

constexpr GestureRule ruleFor (Gesture gesture)
{
    // The Separation and conversion dialogs are modal, and the Song stays as the run started (ADR
    // 0007, ADR 0004 amendment). A Pass keeps the Song and the engine's plan as it started. An
    // export renders its own plan, so the Song may change, but no Track is imported or deleted under
    // it. A loopback latency calibration holds the audio device, so nothing may play or record.
    switch (gesture)
    {
        // { name, refused by a Pass, by a Separation, by a conversion, by an export, by a calibration }
        case Gesture::record: return { "record", false, true, true, true, true };
        case Gesture::playOrStop: return { "play_or_stop", false, true, true, true, true };
        case Gesture::seek: return { "seek", true, false, false, false, false };
        case Gesture::edit: return { "edit", true, true, true, false, false };
        case Gesture::undo: return { "undo", true, true, true, false, false };
        case Gesture::redo: return { "redo", true, true, true, false, false };
        case Gesture::addInsert: return { "add_insert", true, true, true, false, false };
        case Gesture::previewMix: return { "preview_mix", true, true, true, false, false };
        case Gesture::setPreRoll: return { "set_pre_roll", true, true, true, false, false };
        case Gesture::setArmed: return { "set_armed", true, true, true, false, false };
        case Gesture::setInputChannels: return { "set_input_channels", true, true, true, false, false };
        case Gesture::setMonitoring: return { "set_monitoring", true, true, true, false, true };
        case Gesture::setCameraArmed: return { "set_camera_armed", true, true, true, false, false };
        case Gesture::setCameraDevice: return { "set_camera_device", true, true, true, false, false };
        case Gesture::changeTracks: return { "change_tracks", true, true, true, true, false };
        case Gesture::showExport: return { "show_export", true, true, true, true, true };
        case Gesture::startExport: return { "start_export", true, true, true, false, true };
        case Gesture::startSeparation: return { "start_separation", true, false, true, true, false };
        case Gesture::startSampleRateConversion: return { "start_sample_rate_conversion", true, true, false, true, false };
        case Gesture::setPracticeSpeed: return { "set_practice_speed", true, true, true, false, false };
        case Gesture::setTranscribeMix: return { "set_transcribe_mix", true, true, true, false, false };
    }
    std::unreachable();
}

constexpr const char* passRefusal = "ui.stop_recording_first";
constexpr const char* separationRefusal = "ui.separation_running";
constexpr const char* conversionRefusal = "ui.conversion_running";
constexpr const char* exportRefusal = "ui.export_running";
constexpr const char* calibrationRefusal = "ui.calibration_running";
} // namespace

std::optional<core::Error> refusal (ActivityKind kind, Gesture gesture)
{
    const auto rule = ruleFor (gesture);
    const core::Fields fields { { "gesture", rule.name } };
    if (kind == ActivityKind::pass && rule.refusedByPass)
        return core::Error { passRefusal, "a Pass is running", fields };
    if (kind == ActivityKind::separation && rule.refusedBySeparation)
        return core::Error { separationRefusal, "a Separation is running", fields };
    if (kind == ActivityKind::conversion && rule.refusedByConversion)
        return core::Error { conversionRefusal, "a sample rate conversion is running", fields };
    if (kind == ActivityKind::exportSong && rule.refusedByExport)
        return core::Error { exportRefusal, "an export is running", fields };
    if (kind == ActivityKind::calibrationLatency && rule.refusedByCalibration)
        return core::Error { calibrationRefusal, "a latency measurement is running", fields };
    return std::nullopt;
}

std::optional<core::Error> refusal (const Activity& activity, Gesture gesture)
{
    return refusal (kindOf (activity), gesture);
}

bool isRefusal (const core::Error& error)
{
    const auto refusalCode = error.code == passRefusal || error.code == separationRefusal || error.code == conversionRefusal || error.code == exportRefusal || error.code == calibrationRefusal;
    return refusalCode && error.fields.is_object() && error.fields.contains ("gesture");
}

std::string_view activityKindName (ActivityKind kind)
{
    for (const auto& [named, name] : activityKindNames)
        if (named == kind)
            return name;
    std::unreachable();
}

ActivityKind kindOf (const Activity& activity)
{
    if (std::holds_alternative<PassActivity> (activity))
        return ActivityKind::pass;
    if (std::holds_alternative<SeparationActivity> (activity))
        return ActivityKind::separation;
    if (std::holds_alternative<ConversionActivity> (activity))
        return ActivityKind::conversion;
    if (std::holds_alternative<ExportActivity> (activity))
        return ActivityKind::exportSong;
    return ActivityKind::idle;
}
} // namespace overdub::ui
