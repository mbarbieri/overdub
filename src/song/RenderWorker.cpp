#include "song/RenderWorker.h"

#include <algorithm>

namespace overdub::song
{
RenderWorker::RenderWorker (std::vector<Render> toRender, int rendersAtOnce)
    : renders (std::move (toRender))
{
    OVERDUB_CHECK (rendersAtOnce >= 1, "song.render_worker_without_workers", { "renders_at_once", rendersAtOnce });
    const auto workerCount = std::min (std::max (rendersAtOnce, 1), (int) renders.size());
    workersLeft.store (workerCount);
    for (int i = 0; i < workerCount; ++i)
        workers.emplace_back ([this] { renderUntilDoneOrStopped(); });
}

void RenderWorker::renderUntilDoneOrStopped()
{
    const auto stop = stopSource.get_token();
    while (! stop.stop_requested())
    {
        const auto index = nextRender.fetch_add (1);
        if (index >= renders.size())
            break;
        auto result = renders[index](stop);
        if (result.has_value())
            ++done;
        else if (stopSource.request_stop()) // true only for the first request: anything returned after it is dropped
            failure = std::move (result.error());
    }
    workersLeft.fetch_sub (1, std::memory_order_release);
}

std::optional<core::Expected<void>> RenderWorker::take()
{
    if (over || workersLeft.load (std::memory_order_acquire) != 0)
        return std::nullopt;
    workers.clear();
    over = true;
    if (failure.has_value())
        return core::Expected<void> (std::unexpected (std::move (*failure)));
    return core::Expected<void> {};
}

bool RenderWorker::stop()
{
    if (over)
        return false;
    stopSource.request_stop();
    workers.clear();
    over = true;
    return done.load() != (int) renders.size();
}
} // namespace overdub::song
