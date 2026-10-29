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

void Engine::loadSong (const song::Song& song, const juce::File& packageRoot, const InsertFactory& makeInserts, const MonitoredTracks& monitoredTracks, const PracticePlayback& practice)
{
    const std::scoped_lock lock { nonRealtimeMutex };

    auto* plan = new RenderPlan (song, packageRoot, config, makeInserts, monitoredTracks, practice);
    plan->applyBandFilter (heardBandFilter);
    // Right before publishing, so the audio thread has a free slot for the plan it replaces
    // (see the progress invariant in Engine.h).
    freeRetiredPlans();
    // A plan the audio thread never took is replaced; it was never used, so it is freed here.
    delete pendingPlan.exchange (plan, std::memory_order_acq_rel);
    latestPlan = plan;
}

bool Engine::setMix (const song::Song& song)
{
    const std::scoped_lock lock { nonRealtimeMutex };
    if (latestPlan == nullptr || ! latestPlan->hasTracksOf (song))
    {
        core::logEvent (core::Level::debug, "engine.mix_refused", { { "reason", latestPlan == nullptr ? "no_plan" : "tracks_differ" }, { "song_tracks", song.tracks.size() } });
        return false;
    }
    latestPlan->applyMix (song);
    core::logEvent (core::Level::debug, "engine.mix_applied", { { "tracks", song.tracks.size() }, { "master_volume_db", song.master.volumeDb } });
    return true;
}

void Engine::setBandFilter (const std::optional<HeardBandFilter>& filter)
{
    const std::scoped_lock lock { nonRealtimeMutex };
    heardBandFilter = filter;
    if (latestPlan != nullptr)
        latestPlan->applyBandFilter (filter);
    core::logEvent (core::Level::debug,
                    "engine.band_filter_set",
                    { { "on", filter.has_value() },
                      { "track_id", filter.has_value() ? filter->trackId : song::Id {} },
                      { "low_hz", filter.has_value() ? filter->lowHz : 0 },
                      { "high_hz", filter.has_value() ? filter->highHz : 0 } });
}

MeterPeaks Engine::takeMeterPeaks()
{
    const std::scoped_lock lock { nonRealtimeMutex };
    return latestPlan != nullptr ? latestPlan->takeMeterPeaks() : MeterPeaks {};
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

void Engine::startRecording (RecordSession& session)
{
    // A session nobody took is replaced; its owner sees it cancelled.
    if (auto* replaced = pendingRecord.exchange (&session, std::memory_order_acq_rel); replaced != nullptr && replaced != &session)
        replaced->cancel();
}

void Engine::stopRecording (RecordSession& session)
{
    // Not taken yet: cancel it here. Taken (or about to be): ask the audio thread to end it.
    auto* expected = &session;
    if (pendingRecord.compare_exchange_strong (expected, nullptr, std::memory_order_acq_rel))
    {
        session.cancel();
        return;
    }
    if (session.state() != RecordState::stopped)
        recordStopRequestedFor.store (&session, std::memory_order_release);
}

void Engine::deviceStopped()
{
    // No process() runs (see Engine.h), so the audio thread's own state is safe to change here.
    const auto wasPlaying = playing || playRequested.load();
    playRequested.store (false);
    playing = false;
    publishedPlaying.store (false);
    publishedPosition.store (playPosition);

    auto passEnded = false;
    SampleCount capturedFrames = 0;
    if (activeRecord != nullptr)
    {
        capturedFrames = activeRecord->capturedFrames();
        activeRecord->end (RecordStopReason::deviceStopped);
        activeRecord = nullptr;
        activeRecordBegun = false;
        publishedRecording.store (false);
        passEnded = true;
    }
    if (auto* waiting = pendingRecord.exchange (nullptr, std::memory_order_acq_rel); waiting != nullptr)
    {
        waiting->end (RecordStopReason::deviceStopped);
        passEnded = true;
    }
    recordStopRequestedFor.store (nullptr, std::memory_order_release);

    core::logEvent (core::Level::info,
                    "engine.device_stopped",
                    { { "was_playing", wasPlaying }, { "pass_ended", passEnded }, { "captured_frames", capturedFrames }, { "position", position() } });
}

void Engine::setLoop (std::optional<LoopRange> range)
{
    const std::scoped_lock lock { loopWriteMutex };
    const auto valid = range.has_value() && range->start >= 0 && range->start < range->end;
    const auto sequence = loopSequence.load (std::memory_order_relaxed);
    loopSequence.store (sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    loopStartRequest.store (valid ? range->start : 0, std::memory_order_relaxed);
    loopEndRequest.store (valid ? range->end : 0, std::memory_order_relaxed);
    loopSequence.store (sequence + 2, std::memory_order_release);
    core::logEvent (core::Level::debug, "engine.loop_set", { { "looping", valid }, { "start", valid ? range->start : 0 }, { "end", valid ? range->end : 0 } });
}

bool Engine::isRecording() const
{
    return publishedRecording.load();
}

void Engine::setMonitoring (bool on)
{
    monitoringOn.store (on);
}

bool Engine::isMonitoring() const
{
    return monitoringOn.load();
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

int Engine::tailSamples() const
{
    return currentTail.load();
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

void Engine::process (const float* const* inputs,
                      int numInputChannels,
                      float* const* outputs,
                      int numOutputChannels,
                      int numSamples,
                      std::uint64_t hostTimeNs) noexcept
{
    const auto callbackStartNs = core::hostTimeNowNs();

    takePendingPlan (hostTimeNs);
    takeLoop();
    const auto seekedThisBlock = applyTransportCommands (hostTimeNs);
    publishAnchor (hostTimeNs);
    const auto firstRecordedFrame = updateRecording (seekedThisBlock, numSamples, hostTimeNs);
    if (activeRecord != nullptr && activeRecordBegun)
        activeRecord->capture (inputs, numInputChannels, numSamples, Anchor { playPosition, hostTimeNs }, firstRecordedFrame);

    const auto fitsPlan = currentPlan != nullptr
                       && numSamples <= currentPlan->maxBlockSize()
                       && numOutputChannels == currentPlan->numOutputChannels();

    if (fitsPlan)
    {
        // Render-ahead (ADR 0003 amendment): the graph delays every path by its latency L, so the
        // Track nodes read L samples ahead and output sample n of the block is plan sample
        // planPosition + n (Song sample playPosition + n at 100%). Plan samples 0..L-1 after a
        // start are never heard.
        const auto silencedNodes = renderLooped (outputs, LiveInput { inputs, numInputChannels, monitoringOn.load (std::memory_order_relaxed) }, numSamples);
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
    {
        planPosition = loopedPlanSample (planPosition, numSamples);
        playPosition = planToSongSample (planPosition, currentSpeedPercent());
    }
    publishedPosition.store (playPosition);

    recordLoad (core::hostTimeNowNs() - callbackStartNs, numSamples);
}

void Engine::takeLoop() noexcept
{
    const auto before = loopSequence.load (std::memory_order_acquire);
    if ((before & 1u) != 0)
        return;
    const auto start = loopStartRequest.load (std::memory_order_relaxed);
    const auto end = loopEndRequest.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    if (loopSequence.load (std::memory_order_relaxed) != before)
        return;
    heardLoop = end > start ? std::optional (LoopRange { start, end }) : std::nullopt;
}

std::optional<Engine::PlanLoop> Engine::wrappingLoop (SampleCount planBase) const noexcept
{
    if (! heardLoop.has_value() || ! playing)
        return std::nullopt;
    const auto speed = currentSpeedPercent();
    const PlanLoop loop { songToPlanSample (heardLoop->start, speed), songToPlanSample (heardLoop->end, speed) };
    if (loop.end <= loop.start || planBase >= loop.end)
        return std::nullopt;
    return loop;
}

SampleCount Engine::loopedPlanSample (SampleCount planBase, SampleCount offset) const noexcept
{
    const auto sample = planBase + offset;
    const auto loop = wrappingLoop (planBase);
    if (! loop.has_value() || sample < loop->end)
        return sample;
    return loop->start + (sample - loop->end) % (loop->end - loop->start);
}

std::uint64_t Engine::renderLooped (float* const* outputs, const LiveInput& input, int numSamples) noexcept
{
    const auto latency = (SampleCount) currentPlan->latencySamples();
    std::uint64_t silencedNodes = 0;
    int frame = 0;
    while (frame < numSamples)
    {
        // Render-ahead (ADR 0003 amendment): the graph delays every path by its latency L, so the
        // Track nodes read L samples ahead: output sample n of the block is the plan sample
        // planPosition + n (through the Loop), and is rendered at planPosition + L + n.
        const auto renderSample = loopedPlanSample (planPosition, latency + frame);
        auto count = numSamples - frame;
        // The span ends where the render position wraps: at the Loop's end in plan samples.
        if (const auto loop = wrappingLoop (planPosition); loop.has_value())
            count = (int) std::min<SampleCount> (count, loop->end - renderSample);
        auto spanInput = input;
        spanInput.firstFrame = frame;
        silencedNodes += currentPlan->render (outputs, spanInput, frame, count, renderSample, playing);
        frame += count;
    }
    return silencedNodes;
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

    // A plan at another Practice Speed plays on from the same Song sample.
    if (next->speedPercent() != currentSpeedPercent())
        planPosition = songToPlanSample (playPosition, next->speedPercent());
    emptySlot->store (currentPlan, std::memory_order_release);
    currentPlan = next;
    staleOutputSamples = next->latencySamples();
    currentLatency.store (next->latencySamples());
    currentTail.store (next->tailSamples());
    currentSampleRate.store (next->sampleRate());
    pushEvent (planSwapped, hostTimeNs, next->latencySamples(), next->numTracks());
}

bool Engine::applyTransportCommands (std::uint64_t hostTimeNs) noexcept
{
    const auto seekTarget = seekRequest.exchange (noSeek);
    if (seekTarget != noSeek)
    {
        const auto from = playPosition;
        playPosition = seekTarget;
        planPosition = songToPlanSample (seekTarget, currentSpeedPercent());
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
    return seekTarget != noSeek;
}

int Engine::updateRecording (bool seekedThisBlock, int numSamples, std::uint64_t hostTimeNs) noexcept
{
    if (activeRecord != nullptr)
    {
        auto reason = RecordStopReason::none;
        auto* stopFor = activeRecord;
        if (recordStopRequestedFor.compare_exchange_strong (stopFor, nullptr, std::memory_order_acq_rel))
            reason = RecordStopReason::requested;
        else if (seekedThisBlock)
            reason = RecordStopReason::seeked;
        else if (! playing)
            reason = RecordStopReason::transportStopped;

        if (reason != RecordStopReason::none)
        {
            const auto frames = activeRecord->capturedFrames();
            const auto overrun = activeRecord->overrunFrames();
            // The last touch of the session: its owner may free it from here on.
            activeRecord->end (reason);
            activeRecord = nullptr;
            activeRecordBegun = false;
            publishedRecording.store (false);
            pushEvent (recordingStopped, hostTimeNs, (double) frames, (double) overrun, (double) reason);
        }
    }

    if (activeRecord == nullptr)
    {
        if (! playing || pendingRecord.load (std::memory_order_acquire) == nullptr)
            return 0;
        auto* session = pendingRecord.exchange (nullptr, std::memory_order_acq_rel);
        if (session == nullptr)
            return 0;
        activeRecord = session;
        activeRecordBegun = false;
    }

    if (activeRecordBegun)
        return 0;

    // Pre-roll: a session with a record point begins at the block that plays it.
    const auto recordPoint = activeRecord->recordPoint();
    if (recordPoint.has_value() && *recordPoint >= playPosition + numSamples)
        return 0;

    const auto firstFrame = recordPoint.has_value() ? (int) std::clamp<SampleCount> (*recordPoint - playPosition, 0, numSamples) : 0;
    const auto latency = currentPlan != nullptr ? currentPlan->latencySamples() : 0;
    activeRecord->begin (playPosition + firstFrame, latency);
    activeRecordBegun = true;
    publishedRecording.store (true);
    pushEvent (recordingStarted, hostTimeNs, (double) activeRecord->inputs().size(), latency);
    return firstFrame;
}

int Engine::currentSpeedPercent() const noexcept
{
    return currentPlan != nullptr ? currentPlan->speedPercent() : song::normalPracticeSpeed;
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
