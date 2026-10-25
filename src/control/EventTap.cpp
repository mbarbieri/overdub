#include "control/EventTap.h"

#include <algorithm>
#include <utility>

namespace overdub::control
{
EventTap::EventTap (std::size_t queueCapacity)
    : capacity (std::max<std::size_t> (1, queueCapacity))
{
}

void EventTap::writeLine (std::string_view line)
{
    const std::scoped_lock lock (mutex);
    if (queued.size() >= capacity)
    {
        queued.pop_front();
        ++droppedSinceDrain;
    }
    queued.push_back (TappedLine { .seq = nextSeq++, .line = std::string (line) });
}

TappedBatch EventTap::drain()
{
    const std::scoped_lock lock (mutex);
    TappedBatch batch;
    batch.lines.assign (std::make_move_iterator (queued.begin()), std::make_move_iterator (queued.end()));
    queued.clear();
    batch.dropped = std::exchange (droppedSinceDrain, 0);
    return batch;
}
} // namespace overdub::control
