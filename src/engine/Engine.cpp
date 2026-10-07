#include "engine/Engine.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace overdub::engine
{
namespace
{
constexpr std::uint64_t loadSummaryIntervalNs = 1'000'000'000;
}

Engine::Engine (EngineConfig engineConfig, std::size_t rtEventCapacity)
    : config (engineConfig), rtEventRing (rtEventCapacity)
{
}

Engine::~Engine()
{
    delete currentPlan;
    delete pendingPlan.exchange (nullptr);
    for (auto& slot : retiredPlans)
        delete slot.exchange (nullptr);
}

//==============================================================================
// Non-audio threads

void Engine::loadSong (const song::Song& song, const juce::File& packageRoot, const TrackInsertFactory& makeInserts)
{
    const std::scoped_lock lock { nonRealtimeMutex };

    auto* plan = new RenderPlan (song, packageRoot, config, makeInserts);
    // Right before publishing, so the audio thread has a free slot for the plan it replaces
    // (see the progress invariant in Engine.h).
    freeRetiredPlans();
    // A plan the audio thread never took is replaced; it was never used, so it is freed here.
    delete pendingPlan.exchange (plan, std::memory_order_acq_rel);
}

void Engine::play()
{
    playRequested.store (true);
}

void Engine::stop()
{
    playRequested.store (false);
}

void Engine::seek (SampleCount songSample)
{
    seekRequest.store (std::max<SampleCount> (songSample, 0));
}

bool Engine::isPlaying() const
{
    return publishedPlaying.load();
}

SampleCount Engine::position() const
{
    const auto pendingSeek = seekRequest.load();
    return pendingSeek != noSeek ? pendingSeek : publishedPosition.load();
}

int Engine::latencySamples() const
{
    return currentLatency.load();
}

std::optional<Anchor> Engine::latestAnchor() const
{
    while (true)
    {
        const auto before = anchorSequence.load (std::memory_order_acquire);
        if (before == 0)
            return std::nullopt;
        if ((before & 1u) != 0)
            continue;

        const Anchor anchor { anchorSongSample.load (std::memory_order_relaxed), anchorHostTimeNs.load (std::memory_order_relaxed) };
        std::atomic_thread_fence (std::memory_order_acquire);
        if (anchorSequence.load (std::memory_order_relaxed) == before)
            return anchor;
    }
}

void Engine::housekeeping (std::uint64_t nowHostNs)
{
    const std::scoped_lock lock { nonRealtimeMutex };
    freeRetiredPlans();

    if (! lastLoadSummaryNs.has_value())
    {
        lastLoadSummaryNs = nowHostNs;
        return;
    }
    if (nowHostNs - *lastLoadSummaryNs < loadSummaryIntervalNs)
        return;
    lastLoadSummaryNs = nowHostNs;

    const auto busyNs = loadBusyNs.exchange (0);
    const auto samples = loadSamples.exchange (0);
    const auto callbacks = loadCallbacks.exchange (0);
    const auto peakNs = loadPeakCallbackNs.exchange (0);
    const auto silencedNodes = silencedNodeBlocks.exchange (0);
    const auto sampleRate = currentSampleRate.load();

    if (! isPlaying() || callbacks == 0 || samples == 0 || sampleRate <= 0)
        return;

    // Load = time spent in process() / audio time those callbacks produced.
    const auto audioNs = (double) samples * 1.0e9 / sampleRate;
    core::logEvent (core::Level::debug,
                    "engine.load",
                    { { "load", (double) busyNs / audioNs },
                      { "peak_callback_us", (double) peakNs / 1000.0 },
                      { "callbacks", callbacks },
                      { "silenced_node_blocks", silencedNodes } });
}

void Engine::freeRetiredPlans()
{
    for (auto& slot : retiredPlans)
        delete slot.exchange (nullptr, std::memory_order_acq_rel);
}

//==============================================================================
// Audio thread

void Engine::process (const float* const*,
                      int,
                      float* const* outputs,
                      int numOutputChannels,
                      int numSamples,
                      std::uint64_t hostTimeNs) noexcept
{
    const auto callbackStartNs = core::hostTimeNowNs();

    takePendingPlan (hostTimeNs);
    applyTransportCommands (hostTimeNs);
    publishAnchor (hostTimeNs);

    const auto fitsPlan = currentPlan != nullptr
                       && numSamples <= currentPlan->maxBlockSize()
                       && numOutputChannels == currentPlan->numOutputChannels();

    if (fitsPlan)
    {
        // Render-ahead (ADR 0003 amendment): the graph delays every path by its latency L, so the
        // Track nodes read L samples ahead and output sample n of the block is Song sample
        // playPosition + n. Song samples 0..L-1 after a start are never heard.
        const auto silencedNodes = currentPlan->render (outputs, numSamples, playPosition + currentPlan->latencySamples(), playing);
        reportSilencedNodes (silencedNodes, hostTimeNs);
        silenceStaleOutput (outputs, numOutputChannels, numSamples);
        rejectedShape = {};
    }
    else
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
            std::memset (outputs[channel], 0, sizeof (float) * (std::size_t) std::max (numSamples, 0));

        if (currentPlan != nullptr)
            reportRejectedBlock (numSamples, numOutputChannels, hostTimeNs);
    }

    if (playing)
        playPosition += numSamples;
    publishedPosition.store (playPosition);

    recordLoad (core::hostTimeNowNs() - callbackStartNs, numSamples);
}

void Engine::takePendingPlan (std::uint64_t hostTimeNs) noexcept
{
    // A slot seen empty here stays empty until this thread fills it. By the progress invariant
    // (Engine.h) one is always empty when a plan is pending; if not, the swap waits a callback.
    const auto emptySlot = std::ranges::find_if (retiredPlans, [] (const auto& slot) { return slot.load (std::memory_order_acquire) == nullptr; });
    if (emptySlot == retiredPlans.end())
        return;

    auto* next = pendingPlan.exchange (nullptr, std::memory_order_acq_rel);
    if (next == nullptr)
        return;

    emptySlot->store (currentPlan, std::memory_order_release);
    currentPlan = next;
    staleOutputSamples = next->latencySamples();
    currentLatency.store (next->latencySamples());
    currentSampleRate.store (next->sampleRate());
    pushEvent (planSwapped, hostTimeNs, next->latencySamples(), next->numTracks());
}

void Engine::applyTransportCommands (std::uint64_t hostTimeNs) noexcept
{
    const auto seekTarget = seekRequest.exchange (noSeek);
    if (seekTarget != noSeek)
    {
        const auto from = playPosition;
        playPosition = seekTarget;
        restartStaleOutput();
        pushEvent (seeked, hostTimeNs, (double) from);
    }

    const auto wantPlaying = playRequested.load();
    if (wantPlaying != playing)
    {
        playing = wantPlaying;
        restartStaleOutput();
        pushEvent (playing ? transportStarted : transportStopped, hostTimeNs);
        publishedPlaying.store (playing);
    }
}

void Engine::restartStaleOutput() noexcept
{
    staleOutputSamples = currentPlan != nullptr ? currentPlan->latencySamples() : 0;
}

void Engine::silenceStaleOutput (float* const* outputs, int numOutputChannels, int numSamples) noexcept
{
    const auto count = (int) std::min<SampleCount> (staleOutputSamples, numSamples);
    if (count <= 0)
        return;

    for (int channel = 0; channel < numOutputChannels; ++channel)
        std::memset (outputs[channel], 0, sizeof (float) * (std::size_t) count);
    staleOutputSamples -= count;
}

void Engine::reportSilencedNodes (std::uint64_t silencedNodes, std::uint64_t hostTimeNs) noexcept
{
    // ADR 0006: counted every block for engine.load; an RT event only when a busy streak starts.
    const auto busy = silencedNodes > 0;
    if (busy)
    {
        silencedNodeBlocks.fetch_add (silencedNodes, std::memory_order_relaxed);
        if (! nodesBusyLastBlock)
            pushEvent (nodesSilenced, hostTimeNs, (double) silencedNodes);
    }
    nodesBusyLastBlock = busy;
}

void Engine::reportRejectedBlock (int numSamples, int numOutputChannels, std::uint64_t hostTimeNs) noexcept
{
    // ADR 0006: a device stuck on a wrong shape would otherwise flood the ring every callback.
    const BlockShape shape { numSamples, numOutputChannels };
    if (shape == rejectedShape)
        return;

    rejectedShape = shape;
    pushEvent (blockRejected, hostTimeNs, numSamples, numOutputChannels, currentPlan->maxBlockSize(), currentPlan->numOutputChannels());
}

void Engine::publishAnchor (std::uint64_t hostTimeNs) noexcept
{
    const auto sequence = anchorSequence.load (std::memory_order_relaxed);
    anchorSequence.store (sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    anchorSongSample.store (playPosition, std::memory_order_relaxed);
    anchorHostTimeNs.store (hostTimeNs, std::memory_order_relaxed);
    anchorSequence.store (sequence + 2, std::memory_order_release);
}

void Engine::recordLoad (std::uint64_t callbackNs, int numSamples) noexcept
{
    loadBusyNs.fetch_add (callbackNs, std::memory_order_relaxed);
    loadSamples.fetch_add ((std::uint64_t) std::max (numSamples, 0), std::memory_order_relaxed);
    loadCallbacks.fetch_add (1, std::memory_order_relaxed);

    auto peak = loadPeakCallbackNs.load (std::memory_order_relaxed);
    while (callbackNs > peak && ! loadPeakCallbackNs.compare_exchange_weak (peak, callbackNs, std::memory_order_relaxed))
    {
    }
}

void Engine::pushEvent (EngineRtCode code, std::uint64_t hostTimeNs, double arg0, double arg1, double arg2, double arg3) noexcept
{
    rtEventRing.push ({ code, playPosition, hostTimeNs, { arg0, arg1, arg2, arg3 } });
}
} // namespace overdub::engine
