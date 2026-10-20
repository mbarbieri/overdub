#pragma once

#include "core/Log.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The log as the control socket streams it: installed with core::Logger::addTap, it numbers every
// line and queues it until the Controller drains it on the message thread.
namespace overdub::control
{
// One log line and its place in this launch's log. Sequence numbers start at 1 and never repeat.
struct TappedLine
{
    std::uint64_t seq = 0;
    std::string line; // the JSON object exactly as the log file has it
};

struct TappedBatch
{
    std::vector<TappedLine> lines;
    std::uint64_t dropped = 0; // lines lost to a full queue since the previous drain
};

class EventTap final : public core::LogSink
{
public:
    static constexpr std::size_t defaultCapacity = 8192;

    explicit EventTap (std::size_t capacity = defaultCapacity);

    // Any logging thread, under the Logger's mutex: copies the line into the queue and returns.
    // A full queue drops its oldest line and counts it. Never logs: a log line about a dropped
    // line would itself be tapped.
    void writeLine (std::string_view line) override;

    // Message thread. Takes everything queued.
    TappedBatch drain();

private:
    const std::size_t capacity;
    std::mutex mutex;
    std::deque<TappedLine> queued;
    std::uint64_t nextSeq = 1;
    std::uint64_t droppedSinceDrain = 0;
};
} // namespace overdub::control
