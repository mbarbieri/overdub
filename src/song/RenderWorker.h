#pragma once

#include "core/Error.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

// The worker threads of a render run (song::TransposeRun, song::PracticeSpeedRun, ADR 0009): they
// make its renders in order, up to a cap at once, and the first that fails stops the others. Message
// thread only, but for the renders themselves, which run on the workers and must hear the stop token.
namespace overdub::song
{
class RenderWorker
{
public:
    // A render; on failure its Error, which ends the run. Renders may run side by side, so they
    // share nothing they write.
    using Render = std::function<core::Expected<void> (std::stop_token)>;

    // Starts `rendersAtOnce` workers (at least 1, at most one per render) on `renders`.
    RenderWorker (std::vector<Render> renders, int rendersAtOnce);
    // Stops the workers and waits for them.
    ~RenderWorker() { stop(); }

    RenderWorker (const RenderWorker&) = delete;
    RenderWorker& operator= (const RenderWorker&) = delete;

    // Once every worker is done: how the run ended (none on success, or the first failure; a failure
    // beside it is dropped). Nothing while one runs, or once over: the run is over once it returns.
    std::optional<core::Expected<void>> take();

    // Stops every render after its current block and waits for them; the run is then over. Returns
    // whether this cancelled anything: false when every render had been written, or the run was
    // over already.
    bool stop();

    int rendersDone() const { return done.load(); }

private:
    void renderUntilDoneOrStopped();

    std::vector<Render> renders;
    bool over = false;
    std::stop_source stopSource; // every render's; requested by stop() or the first failure
    std::atomic<std::size_t> nextRender { 0 };
    std::atomic<int> done { 0 };
    std::optional<core::Error> failure; // written only by the worker whose request stopped the run, read after the workers end
    std::atomic<int> workersLeft { 0 };
    std::vector<std::jthread> workers; // last: joined first
};
} // namespace overdub::song
