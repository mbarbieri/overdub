#include "core/RtEventRing.h"

namespace overdub::core
{
namespace
{
void logOneRtEvent (const RtEvent& event, std::span<const RtEventSpec> catalog)
{
    if (event.code >= catalog.size() || catalog[event.code].event == nullptr)
    {
        logEventCapturedAt (Level::warn,
                            "engine.rt_unknown_code",
                            ThreadRole::audio,
                            event.hostTimeNs,
                            { { "code", event.code }, { "sample", event.sample } });
        return;
    }

    const auto& spec = catalog[event.code];
    Fields fields = { { "sample", event.sample } };
    for (std::size_t argIndex = 0; argIndex < spec.argNames.size(); ++argIndex)
        if (spec.argNames[argIndex] != nullptr)
            fields[spec.argNames[argIndex]] = event.args[argIndex];

    logEventCapturedAt (spec.level, spec.event, ThreadRole::audio, event.hostTimeNs, fields);
}
} // namespace

RtEventRing::RtEventRing (std::size_t capacity)
    : slots (capacity + 1)
{
}

bool RtEventRing::push (const RtEvent& event) noexcept
{
    const auto write = writeIndex.load (std::memory_order_relaxed);
    const auto next = (write + 1) % slots.size();

    if (next == readIndex.load (std::memory_order_acquire))
    {
        droppedCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    slots[write] = event;
    writeIndex.store (next, std::memory_order_release);
    return true;
}

bool RtEventRing::pop (RtEvent& event) noexcept
{
    const auto read = readIndex.load (std::memory_order_relaxed);
    if (read == writeIndex.load (std::memory_order_acquire))
        return false;

    event = slots[read];
    readIndex.store ((read + 1) % slots.size(), std::memory_order_release);
    return true;
}

std::uint64_t RtEventRing::dropped() const noexcept
{
    return droppedCount.load (std::memory_order_relaxed);
}

std::uint64_t RtEventRing::takeDropped() noexcept
{
    return droppedCount.exchange (0, std::memory_order_relaxed);
}

void drainRtEvents (RtEventRing& ring, std::span<const RtEventSpec> catalog)
{
    RtEvent event;
    while (ring.pop (event))
        logOneRtEvent (event, catalog);

    if (const auto dropped = ring.takeDropped(); dropped > 0)
        logEvent (Level::warn, "engine.rt_events_dropped", { { "dropped", dropped } });
}

RtEventDrainThread::RtEventDrainThread (RtEventRing& ringToDrain,
                                        std::span<const RtEventSpec> eventCatalog,
                                        std::chrono::milliseconds drainInterval)
    : ring (ringToDrain),
      catalog (eventCatalog),
      interval (drainInterval),
      thread ([this] (std::stop_token stopToken) { run (stopToken); })
{
}

RtEventDrainThread::~RtEventDrainThread()
{
    thread.request_stop();
    thread.join();
}

void RtEventDrainThread::run (std::stop_token stopToken)
{
    setCurrentThreadRole (ThreadRole::logger);

    while (! stopToken.stop_requested())
    {
        drainRtEvents (ring, catalog);

        std::unique_lock lock (wakeMutex);
        wakeCondition.wait_for (lock, stopToken, interval, [] { return false; });
    }

    drainRtEvents (ring, catalog);
}
} // namespace overdub::core
