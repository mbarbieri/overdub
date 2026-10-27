#pragma once

#include "ui/Intent.h"

#include <array>
#include <string_view>

// The File menu items that run one Song Intent as they are (issues #200, #202): the app's
// MainWindow lists them, each enabled while its op may run (ui/Availability.h) and a Song is
// open, and runs its Intent when picked; the gesture census fires each one the same way.
namespace overdub::ui
{
struct SongMenuItem
{
    std::string_view name;
    std::string_view shortcut; // as the menu shows it; empty for none
    SongIntent intent;
};

inline const std::array songMenuItems {
    SongMenuItem { "Export...", "\xe2\x8c\x98"
                                "E",
                   SongIntent { intent::ShowExport { true } } },
    SongMenuItem { "Cancel Export", "", SongIntent { intent::CancelExport {} } },
    SongMenuItem { "Record Again", "\xe2\x87\xa7"
                                   "R",
                   SongIntent { intent::RecordAgain {} } },
};
} // namespace overdub::ui
