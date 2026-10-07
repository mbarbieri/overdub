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

    // Alerts: an action failed or data may be lost.
    CodeEntry { "capture.take_write_failed", Tier::alert, "A Take could not be written to disk; this Pass was not added." },
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
    CodeEntry { "engine.device_settings_write_failed", Tier::alert, "The audio device choice could not be saved for next time." },
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
