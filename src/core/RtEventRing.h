#pragma once

#include "core/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

// The RT event path (ADR 0006): the audio thread pushes fixed-size POD events into a lock-free
// single-producer single-consumer ring owned by the engine and passed explicitly; a logger thread
// drains it into the process logger. push() never allocates, locks or logs.
namespace overdub::core
{
struct RtEvent
{
    std::uint16_t code = 0;          // index into the owner's RtEventSpec catalog
    std::int64_t sample = 0;         // audio-clock sample position at capture
    std::uint64_t hostTimeNs = 0;    // host time at capture (mach_absolute_time in ns)
    std::array<double, 4> args {};   // named by the catalog; unnamed args are not logged
};

static_assert (std::is_trivially_copyable_v<RtEvent>);

// One row of a feature's RT name table: RT code N is described by catalog[N].
struct RtEventSpec
{
    const char* event = nullptr;            // "area.snake_case"
    Level level = Level::info;
    std::array<const char*, 4> argNames {}; // nullptr = arg unused
};

class RtEventRing
{
public:
    // Allocates all storage up front; capacity is the number of events held before dropping.
    explicit RtEventRing (std::size_t capacity);

    RtEventRing (const RtEventRing&) = delete;
    RtEventRing& operator= (const RtEventRing&) = delete;

    // Producer (audio thread) only. Returns false and counts a drop when the ring is full.
    bool push (const RtEvent& event) noexcept;

    // Consumer (drain thread) only. Returns false when the ring is empty.
    bool pop (RtEvent& event) noexcept;

    // Drops since the last takeDropped().
    std::uint64_t dropped() const noexcept;
    std::uint64_t takeDropped() noexcept;

private:
    std::vector<RtEvent> slots; // capacity + 1: one slot stays empty to tell full from empty
    std::atomic<std::size_t> readIndex { 0 };
    std::atomic<std::size_t> writeIndex { 0 };
    std::atomic<std::uint64_t> droppedCount { 0 };
};

// Logs every pending event in push order, with its capture-time sample and host_ns on thread
// `audio`, then logs `engine.rt_events_dropped` if events were dropped since the last drain.
void drainRtEvents (RtEventRing& ring, std::span<const RtEventSpec> catalog);

// Drains a ring every `interval` on its own thread (role `logger`), and once more when destroyed.
// The ring and catalog must outlive it.
class RtEventDrainThread
{
public:
    RtEventDrainThread (RtEventRing& ring,
                        std::span<const RtEventSpec> catalog,
                        std::chrono::milliseconds interval = std::chrono::milliseconds (50));
    ~RtEventDrainThread();

    RtEventDrainThread (const RtEventDrainThread&) = delete;
    RtEventDrainThread& operator= (const RtEventDrainThread&) = delete;

private:
    void run (std::stop_token stopToken);

    RtEventRing& ring;
    std::span<const RtEventSpec> catalog;
    std::chrono::milliseconds interval;
    std::mutex wakeMutex;
    std::condition_variable_any wakeCondition;
    std::jthread thread; // last member: starts after everything it uses is constructed
};
} // namespace overdub::core
