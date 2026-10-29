#include "ui/UserMessages.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
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
    CodeEntry { "ui.record_mode", Tier::notice, "Make and edit Loops and Markers in Transcribe or Edit mode." },
    CodeEntry { "song.loop_not_found", Tier::notice, "That Loop is no longer in the Song." },
    CodeEntry { "song.loop_name_empty", Tier::notice, "A Loop needs a name." },
    CodeEntry { "song.loop_empty", Tier::notice, "A Loop needs a start and an end apart." },
    CodeEntry { "song.no_active_loop", Tier::notice, "Pick a Loop to loop first." },
    CodeEntry { "ui.no_loop_start", Tier::notice, "Press [ at the Loop's start first." },
    CodeEntry { "song.marker_not_found", Tier::notice, "That Marker is no longer in the Song." },
    CodeEntry { "song.marker_name_empty", Tier::notice, "A Marker needs a name." },
    CodeEntry { "ui.transposing", Tier::notice, "Transposing\xe2\x80\xa6 Wait for the Backing Tracks to be transposed." },
    CodeEntry { "song.transpose_out_of_range", Tier::notice, "Transpose is a whole number of semitones from \xe2\x88\x92"
                                                             "12 to +12." },
    CodeEntry { "song.transpose_render_failed", Tier::notice, "A Backing Track could not be transposed. Set Transpose again to try again." },
    CodeEntry { "song.practice_speed_out_of_range", Tier::notice, "Practice Speed is a whole percent from 25 to 125." },
    CodeEntry { "song.practice_render_failed", Tier::notice, "A Track could not be slowed down or sped up. Set Practice Speed again to try again." },
    CodeEntry { "ui.export_finished", Tier::notice, "The export is written." },
    CodeEntry { "ui.export_cancelled", Tier::notice, "The export was cancelled." },
    CodeEntry { "export.nothing_to_export", Tier::notice, "The Song has no Clips to export." },
    CodeEntry { "export.folder_missing", Tier::notice, "That folder does not exist. Choose another place to export to." },
    CodeEntry { "export.video_take_outside_song", Tier::notice, "The Video Take ends before the Song starts, so there is nothing to export." },
    CodeEntry { "ui.no_video_take", Tier::notice, "Pick a Video Take to export an MP4." },
    CodeEntry { "ui.latency_not_calibrated", Tier::notice, "This audio device is not calibrated, so Takes may land late. Calibrate it in Settings." },
    CodeEntry { "ui.calibration_running", Tier::notice, "Wait for the latency measurement to finish." },
    CodeEntry { "capture.calibration_timeout", Tier::notice, "The audio device stopped during the measurement. Check the device, then measure again." },
    CodeEntry { "ui.sample_rate_mismatch", Tier::notice, "The audio device runs at another sample rate than this Song, so it plays and records nothing." },
    CodeEntry { "ui.device_rate_substituted", Tier::notice, "The audio device opened at another sample rate than the one picked." },
    CodeEntry { "ui.audio_device_lost", Tier::notice, "The audio device was disconnected. Overdub switched to another device." },
    CodeEntry { "ui.camera_disconnected", Tier::notice, "The camera was disconnected and is no longer Armed." },
    CodeEntry { "capture.clap_no_video_take", Tier::notice, "No Pass has a Video Take yet. Arm the camera and record a Pass clapping in frame." },
    CodeEntry { "ui.separation_running", Tier::notice, "Wait for the Separation to finish, or cancel it." },
    CodeEntry { "ui.conversion_running", Tier::notice, "Wait for the sample rate conversion to finish, or cancel it." },
    CodeEntry { "song.sample_rate_unchanged", Tier::notice, "The Song is at that sample rate already." },
    CodeEntry { "song.track_not_separable", Tier::notice, "Only a Track whose Clips all play one Backing Track can be separated." },
    CodeEntry { "ui.last_song_missing", Tier::notice, "Couldn't reopen the Last Song: it isn't where it was." },
    CodeEntry { "ui.last_song_failed", Tier::notice, "Couldn't reopen the Last Song." },

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
    CodeEntry { "song.conversion_failed", Tier::alert, "A Take or Backing Track could not be converted, so the Song keeps its sample rate." },
    CodeEntry { "song.sample_rate_invalid", Tier::alert, "That is not a sample rate the Song can be converted to." },
};

const CodeEntry* entryFor (std::string_view code)
{
    const auto found = std::ranges::find (codeTable, code, &CodeEntry::code);
    return found == codeTable.end() ? nullptr : &*found;
}

// Without both rates (a Notice posted by code only) the table's sentence is shown.
std::optional<std::string> sampleRateMismatchSentence (const core::Fields& fields)
{
    if (! fields.is_object() || ! fields.contains ("song_rate") || ! fields.contains ("device_rate"))
        return std::nullopt;
    const auto songRate = std::to_string (fields["song_rate"].get<int>());
    const auto deviceRate = std::to_string (fields["device_rate"].get<int>());
    if (fields.value ("rate_offered", false))
        return "The audio device runs at " + deviceRate + " Hz and this Song is at " + songRate + " Hz, so it plays and records nothing. Pick " + songRate + " Hz in Settings.";
    return "This Song is at " + songRate + " Hz, which the audio device (at " + deviceRate + " Hz) doesn't offer, so it plays and records nothing. Use a device that offers " + songRate + " Hz, or record in a new Song.";
}
std::optional<std::string> exportFinishedSentence (const core::Fields& fields)
{
    if (! fields.is_object() || ! fields.contains ("file_name"))
        return std::nullopt;
    return "Exported to " + fields["file_name"].get<std::string>() + ".";
}
// "Couldn't reopen “Name”: ", the start of both Last Song Notices (issue #207).
std::string couldNotReopen (const core::Fields& fields)
{
    return "Couldn't reopen \u201c" + fields["name"].get<std::string>() + "\u201d: ";
}

std::optional<std::string> lastSongMissingSentence (const core::Fields& fields)
{
    if (! fields.is_object() || ! fields.contains ("name") || ! fields.contains ("path"))
        return std::nullopt;
    return couldNotReopen (fields) + "it isn't at " + fields["path"].get<std::string>() + ". It will reopen once it's back there.";
}

// The reason is the sentence File > Open... shows for the open's own Error (code, error_fields).
std::optional<std::string> lastSongFailedSentence (const core::Fields& fields)
{
    if (! fields.is_object() || ! fields.contains ("name") || ! fields.contains ("code"))
        return std::nullopt;
    return couldNotReopen (fields) + sentenceFor (fields["code"].get<std::string>(), fields.value ("error_fields", core::Fields::object()));
}

std::optional<std::string> rateSubstitutedSentence (const core::Fields& fields)
{
    if (! fields.is_object() || ! fields.contains ("requested_rate") || ! fields.contains ("opened_rate"))
        return std::nullopt;
    return "The audio device doesn't offer " + std::to_string (fields["requested_rate"].get<int>()) + " Hz, so it opened at " + std::to_string (fields["opened_rate"].get<int>()) + " Hz.";
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

std::string sentenceFor (std::string_view code, const core::Fields& fields)
{
    if (code == "ui.sample_rate_mismatch")
        if (auto sentence = sampleRateMismatchSentence (fields))
            return *sentence;
    if (code == "ui.device_rate_substituted")
        if (auto sentence = rateSubstitutedSentence (fields))
            return *sentence;
    if (code == exportFinishedCode)
        if (auto sentence = exportFinishedSentence (fields))
            return *sentence;
    if (code == "ui.last_song_missing")
        if (auto sentence = lastSongMissingSentence (fields))
            return *sentence;
    if (code == "ui.last_song_failed")
        if (auto sentence = lastSongFailedSentence (fields))
            return *sentence;
    if (const auto* entry = entryFor (code))
        return std::string (entry->sentence);
    return "Something went wrong (" + std::string (code) + ").";
}

std::optional<int> conversionOfferedBy (const UserMessage& message)
{
    const auto& fields = message.fields;
    if (message.code != "ui.sample_rate_mismatch" || ! fields.is_object() || fields.value ("rate_offered", true))
        return std::nullopt;
    const auto deviceRate = fields.value ("device_rate", 0);
    return deviceRate > 0 ? std::optional (deviceRate) : std::nullopt;
}

std::string conversionActionText (int toRate)
{
    return "Convert to " + std::to_string (toRate) + " Hz";
}

UserMessage userMessageFor (std::string_view code, core::Fields fields)
{
    return UserMessage { tierFor (code), std::string (code), std::move (fields) };
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

std::vector<UserMessage> UserMessageQueue::deferredAlerts() const
{
    std::vector<UserMessage> alerts;
    std::ranges::copy_if (queued, std::back_inserter (alerts), [] (const UserMessage& message) { return message.tier == Tier::alert; });
    return alerts;
}
} // namespace overdub::ui
