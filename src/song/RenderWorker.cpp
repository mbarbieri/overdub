#include "song/RenderWorker.h"

namespace overdub::song
{
RenderWorker::RenderWorker (std::vector<Render> toRender)
    : renders (std::move (toRender))
{
    worker = std::jthread ([this] (std::stop_token stop) {
        core::Expected<void> result;
        for (const auto& render : renders)
        {
            result = render (stop);
            if (! result.has_value())
                break;
            ++done;
        }
        outcome = std::move (result);
        finished.store (true, std::memory_order_release);
    });
}

std::optional<core::Expected<void>> RenderWorker::take()
{
    if (over || ! finished.load (std::memory_order_acquire))
        return std::nullopt;
    worker.join();
    over = true;
    return std::move (*outcome);
}

bool RenderWorker::stop()
{
    if (over)
        return false;
    worker.request_stop();
    worker.join();
    over = true;
    const auto everyRenderWritten = finished.load (std::memory_order_acquire) && outcome.has_value() && outcome->has_value();
    return ! everyRenderWritten;
}
} // namespace overdub::song
