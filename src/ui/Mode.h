#pragma once

#include <string_view>
#include <utility>

// The window's three modes (issue #9 resolution, issue #225): Record, Edit and Transcribe.
namespace overdub::ui
{
enum class Mode
{
    record,
    edit,
    transcribe
};
inline constexpr std::pair<Mode, std::string_view> modeNames[] { { Mode::record, "record" }, { Mode::edit, "edit" }, { Mode::transcribe, "transcribe" } };

// "record", "edit" or "transcribe": the name in ui.* event fields and in the Intent's JSON.
inline const char* modeName (Mode mode)
{
    switch (mode)
    {
        case Mode::record: return "record";
        case Mode::edit: return "edit";
        case Mode::transcribe: return "transcribe";
    }
    return "unknown";
}
} // namespace overdub::ui
