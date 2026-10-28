#pragma once

#include "core/Error.h"

#include <atomic>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

// The worker thread of a render run (song::TransposeRun, song::PracticeSpeedRun, ADR 0009): it
// makes its renders one after another, ending at the first that fails. Message thread only, but for
// the renders themselves, which run on the worker and must hear its stop token.
namespace overdub::song
{
class RenderWorker
{
public:
    // A render; on failure its Error, which ends the run.
    using Render = std::function<core::Expected<void> (std::stop_token)>;

    // Starts the worker on `renders`.
    explicit RenderWorker (std::vector<Render> renders);
    // Stops the worker and waits for it.
    ~RenderWorker() { stop(); }

    RenderWorker (const RenderWorker&) = delete;
    RenderWorker& operator= (const RenderWorker&) = delete;

    // Once the worker is done: how the run ended (none on success). Nothing while it runs, or once
    // over: the run is over once it returns.
    std::optional<core::Expected<void>> take();

    // Stops the worker after its current block and waits for it; the run is then over. Returns
    // whether this cancelled anything: false when every render had been written, or the run was
    // over already.
    bool stop();

    bool isOver() const { return over; }
    int rendersDone() const { return done.load(); }

private:
    std::vector<Render> renders;
    bool over = false;
    std::atomic<int> done { 0 };
    std::optional<core::Expected<void>> outcome; // written by the worker before `finished`
    std::atomic<bool> finished { false };
    std::jthread worker; // last: joined first
};
} // namespace overdub::song
