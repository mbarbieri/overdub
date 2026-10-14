#include "ui/UserMessages.h"

#include <algorithm>
#include <array>
#include <utility>

namespace overdub::ui
{
namespace
{
struct CodeEntry
{
    std::string_view code;
    Tier tier;
    std::string_view sentence;
};

// The one Error.code -> sentence table (ADR 0006). Keep sentences short: a Notice is one line.
constexpr std::array codeTable {
    // Notices: degraded, nothing for the user to decide.
    CodeEntry { "song.history_dropped", Tier::notice, "Undo history could not be read and was reset." },
    CodeEntry { "song.backing_track_missing", Tier::notice, "A Backing Track's audio file is missing; its Clips play silence." },
    CodeEntry { "capture.nothing_armed", Tier::notice, "Arm a Track and pick its input to record." },
    CodeEntry { "capture.pass_empty", Tier::notice, "The Pass stopped before any audio was recorded." },
    CodeEntry { "ui.no_audio_device", Tier::notice, "No audio device is open. Pick one in Settings." },
    CodeEntry { "ui.stop_recording_first", Tier::notice, "Stop recording first." },
    CodeEntry { "song.split_outside_clip", Tier::notice, "Move the playhead inside the selected Clip to split it." },
    CodeEntry { "song.clip_not_found", Tier::notice, "That Clip is no longer in the Song." },
    CodeEntry { "song.track_not_found", Tier::notice, "That Track is no longer in the Song." },
    CodeEntry { "ui.plugin_missing", Tier::notice, "That plugin is not installed or could not be loaded." },
    CodeEntry { "ui.plugin_crashed", Tier::notice, "A plugin crashed. It is bypassed until it reloads, after recording stops." },
    CodeEntry { "ui.plugin_crashed_repeatedly", Tier::notice, "A plugin keeps crashing and stays bypassed. Reopen the Song to try it again." },
    CodeEntry { "song.insert_not_found", Tier::notice, "That Insert is no longer in the Song." },
    CodeEntry { "capture.camera_access_denied", Tier::notice, "Allow Overdub to use the camera in System Settings, then arm the camera again." },
    CodeEntry { "capture.camera_unavailable", Tier::notice, "The camera could not be opened." },
    CodeEntry { "capture.video_frames_dropped", Tier::notice, "The camera dropped frames during this Pass." },
    CodeEntry { "ui.export_running", Tier::notice, "Wait for the export to finish." },
    CodeEntry { "export.nothing_to_export", Tier::notice, "The Song has no Clips to export." },
    CodeEntry { "export.video_take_outside_song", Tier::notice, "The Video Take ends before the Song starts, so there is nothing to export." },
    CodeEntry { "ui.no_video_take", Tier::notice, "Pick a Video Take to export an MP4." },
    CodeEntry { "ui.latency_not_calibrated", Tier::notice, "This audio device is not calibrated, so Takes may land late. Calibrate it in Settings." },
    CodeEntry { "ui.calibration_running", Tier::notice, "Wait for the latency measurement to finish." },
    CodeEntry { "capture.calibration_timeout", Tier::notice, "The audio device stopped during the measurement. Check the device, then measure again." },
    CodeEntry { "ui.sample_rate_mismatch", Tier::notice, "The audio device runs at another sample rate than this Song, so it plays and records nothing. Pick the Song's rate in Settings." },
    CodeEntry { "ui.audio_device_lost", Tier::notice, "The audio device was disconnected. Overdub switched to another device." },
    CodeEntry { "ui.camera_disconnected", Tier::notice, "The camera was disconnected and is no longer Armed." },
    CodeEntry { "capture.clap_no_video_take", Tier::notice, "No Pass has a Video Take yet. Arm the camera and record a Pass clapping in frame." },
    CodeEntry { "ui.separation_running", Tier::notice, "Wait for the Separation to finish, or cancel it." },
    CodeEntry { "song.track_not_separable", Tier::notice, "Only a Track whose Clips all play one Backing Track can be separated." },

    // Alerts: an action failed or data may be lost.
    CodeEntry { "capture.take_write_failed", Tier::alert, "A Take could not be written to disk; this Pass was not added." },
    CodeEntry { "capture.movie_no_frames", Tier::alert, "The camera recorded no picture; this Pass has no Video Take." },
    CodeEntry { "capture.movie_write_failed", Tier::alert, "The camera's video could not be written; this Pass has no Video Take." },
    CodeEntry { "capture.video_unplaced", Tier::alert, "The camera's video could not be placed; this Pass has no Video Take." },
    CodeEntry { "capture.camera_not_open", Tier::alert, "The camera closed; this Pass has no Video Take." },
    CodeEntry { "ui.audio_device_lost_during_pass", Tier::alert, "The audio device stopped during the Pass. What was recorded until then is kept." },
    CodeEntry { "capture.stop_timeout", Tier::alert, "The audio device stopped responding; this Pass was not added." },
    CodeEntry { "song.write_failed", Tier::alert, "The Song could not be saved to disk." },
    CodeEntry { "song.json_unserializable", Tier::alert, "The Song could not be saved to disk." },
    CodeEntry { "song.import_unreadable", Tier::alert, "That file could not be read as audio." },
    CodeEntry { "song.import_too_many_channels", Tier::alert, "Only mono and stereo files can be imported." },
    CodeEntry { "song.package_exists", Tier::alert, "Something already exists at that location." },
    CodeEntry { "song.create_failed", Tier::alert, "The Song could not be created there." },
    CodeEntry { "song.package_not_found", Tier::alert, "That is not an Overdub Song." },
    CodeEntry { "song.read_failed", Tier::alert, "The Song could not be read." },
    CodeEntry { "song.json_unparsable", Tier::alert, "The Song file is damaged and could not be opened." },
    CodeEntry { "song.json_invalid", Tier::alert, "The Song file is damaged and could not be opened." },
    CodeEntry { "song.format_too_new", Tier::alert, "This Song was made by a newer version of Overdub." },
    CodeEntry { "song.migration_failed", Tier::alert, "This Song could not be upgraded to the current format." },
    CodeEntry { "song.duplicate_failed", Tier::alert, "The Song could not be duplicated there." },
    CodeEntry { "export.write_failed", Tier::alert, "The export could not be written there." },
    CodeEntry { "export.video_take_unreadable", Tier::alert, "The Video Take's movie could not be read; nothing was exported." },
    CodeEntry { "export.movie_failed", Tier::alert, "The MP4 could not be made." },
    CodeEntry { "ui.export_plugin_crashed", Tier::alert, "A plugin crashed during the export, so nothing was exported. Export again." },
    CodeEntry { "ui.export_audio_unresponsive", Tier::alert, "The audio device stopped responding, so nothing was exported. Check the device, then export again." },
    CodeEntry { "engine.calibration_write_failed", Tier::alert, "The latency calibration could not be saved." },
    CodeEntry { "engine.calibration_unreadable", Tier::alert, "The latency calibration file is damaged and could not be updated." },
    CodeEntry { "capture.camera_calibration_write_failed", Tier::alert, "The camera calibration could not be saved." },
    CodeEntry { "capture.camera_calibration_unreadable", Tier::alert, "The camera calibration file is damaged and could not be updated." },
    CodeEntry { "engine.device_settings_write_failed", Tier::alert, "The audio device choice could not be saved for next time." },
    CodeEntry { "song.separation_model_missing", Tier::alert, "The Separation model is not installed. Run \"make separation-model\" in the Overdub source folder (it needs Docker), then separate again." },
    CodeEntry { "song.separation_model_invalid", Tier::alert, "The installed Separation model is not the expected one. Run \"make separation-model\" in the Overdub source folder (it needs Docker) to install it again, then separate again." },
    CodeEntry { "song.separation_unavailable", Tier::alert, "This build of Overdub cannot separate Backing Tracks." },
    CodeEntry { "song.separation_start_failed", Tier::alert, "The Separation could not be started; the Song is unchanged." },
    CodeEntry { "song.separation_failed", Tier::alert, "The Separation failed; the Song is unchanged." },
    CodeEntry { "song.separation_parts_invalid", Tier::alert, "The Separation failed; the Song is unchanged." },
};

const CodeEntry* entryFor (std::string_view code)
{
    const auto found = std::ranges::find (codeTable, code, &CodeEntry::code);
    return found == codeTable.end() ? nullptr : &*found;
}
} // namespace

const char* tierName (Tier tier)
{
    return tier == Tier::notice ? "notice" : "alert";
}

Tier tierFor (std::string_view code)
{
    const auto* entry = entryFor (code);
    return entry != nullptr ? entry->tier : Tier::alert;
}

std::string sentenceFor (std::string_view code)
{
    if (const auto* entry = entryFor (code))
        return std::string (entry->sentence);
    return "Something went wrong (" + std::string (code) + ").";
}

UserMessage userMessageFor (std::string_view code)
{
    return UserMessage { tierFor (code), std::string (code) };
}

void UserMessageQueue::post (UserMessage message)
{
    queued.push_back (std::move (message));
}

std::vector<UserMessage> UserMessageQueue::takeReady (bool passRunning)
{
    std::vector<UserMessage> ready;
    std::vector<UserMessage> kept;
    for (auto& message : queued)
    {
        if (message.tier == Tier::alert && passRunning)
            kept.push_back (std::move (message));
        else
            ready.push_back (std::move (message));
    }
    queued = std::move (kept);
    return ready;
}
} // namespace overdub::ui
