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
};

constexpr GestureRule ruleFor (Gesture gesture)
{
    // The Separation and conversion dialogs are modal, and the Song stays as the run started (ADR
    // 0007, ADR 0004 amendment). A Pass keeps the Song and the engine's plan as it started. An
    // export renders its own plan, so the Song may change, but no Track is imported or deleted under
    // it.
    switch (gesture)
    {
        // { name, refused by a Pass, by a Separation, by a conversion, by an export }
        case Gesture::record: return { "record", false, true, true, true };
        case Gesture::playOrStop: return { "play_or_stop", false, true, true, true };
        case Gesture::seek: return { "seek", true, false, false, false };
        case Gesture::edit: return { "edit", true, true, true, false };
        case Gesture::undo: return { "undo", true, true, true, false };
        case Gesture::redo: return { "redo", true, true, true, false };
        case Gesture::addInsert: return { "add_insert", true, true, true, false };
        case Gesture::previewMix: return { "preview_mix", true, true, true, false };
        case Gesture::setPreRoll: return { "set_pre_roll", true, true, true, false };
        case Gesture::setArmed: return { "set_armed", true, true, true, false };
        case Gesture::setInputChannels: return { "set_input_channels", true, true, true, false };
        case Gesture::setMonitoring: return { "set_monitoring", true, true, true, false };
        case Gesture::setCameraArmed: return { "set_camera_armed", true, true, true, false };
        case Gesture::setCameraDevice: return { "set_camera_device", true, true, true, false };
        case Gesture::changeTracks: return { "change_tracks", true, true, true, true };
        case Gesture::startExport: return { "start_export", true, true, true, false };
        case Gesture::startSeparation: return { "start_separation", true, false, true, true };
        case Gesture::startSampleRateConversion: return { "start_sample_rate_conversion", true, true, false, true };
    }
    std::unreachable();
}

constexpr const char* passRefusal = "ui.stop_recording_first";
constexpr const char* separationRefusal = "ui.separation_running";
constexpr const char* conversionRefusal = "ui.conversion_running";
constexpr const char* exportRefusal = "ui.export_running";
} // namespace

std::optional<core::Error> refusal (const Activity& activity, Gesture gesture)
{
    const auto rule = ruleFor (gesture);
    const core::Fields fields { { "gesture", rule.name } };
    if (std::holds_alternative<PassActivity> (activity) && rule.refusedByPass)
        return core::Error { passRefusal, "a Pass is running", fields };
    if (std::holds_alternative<SeparationActivity> (activity) && rule.refusedBySeparation)
        return core::Error { separationRefusal, "a Separation is running", fields };
    if (std::holds_alternative<ConversionActivity> (activity) && rule.refusedByConversion)
        return core::Error { conversionRefusal, "a sample rate conversion is running", fields };
    if (std::holds_alternative<ExportActivity> (activity) && rule.refusedByExport)
        return core::Error { exportRefusal, "an export is running", fields };
    return std::nullopt;
}

bool isRefusal (const core::Error& error)
{
    const auto refusalCode = error.code == passRefusal || error.code == separationRefusal || error.code == conversionRefusal || error.code == exportRefusal;
    return refusalCode && error.fields.is_object() && error.fields.contains ("gesture");
}
} // namespace overdub::ui
