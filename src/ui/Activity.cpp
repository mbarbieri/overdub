#include "ui/Activity.h"

#include <utility>

namespace overdub::ui
{
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
