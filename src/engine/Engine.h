#pragma once

#include "core/RtEventRing.h"
#include "engine/Anchor.h"
#include "engine/EngineEvents.h"
#include "engine/RecordSession.h"
#include "engine/RenderPlan.h"
#include "song/Song.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

// The headless engine (ADR 0005): one plain duplex callback, process(), that the live device and
// tests call the same way. It owns the transport (play / stop / seek, position in Song samples),
// the current render plan (one AudioProcessorGraph per Song snapshot, ADR 0001), the per-callback
// anchor (ADR 0003) and the RT event ring (ADR 0006).
//
// Threads: process() runs on the audio thread only and never allocates, frees or blocks.
// Everything else is called from non-audio threads. A new plan is built on the caller's thread
// and handed over through an atomic pointer; the replaced plan is freed off the audio thread by
// the next loadSong() or housekeeping().
namespace overdub::engine
{
using song::SampleCount;

class Engine
{
public:
    explicit Engine (EngineConfig config, std::size_t rtEventCapacity = 1024);
    // Frees every plan. The device must have stopped calling process().
    ~Engine();

    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // Non-audio thread. Builds a plan from this Song snapshot (media read from packageRoot) and
    // hands it to the audio thread, which plays it from the next callback at the current position.
    void loadSong (const song::Song& song, const juce::File& packageRoot, const TrackInsertFactory& makeInserts = {});

    // Transport commands, any non-audio thread. Each takes effect at the start of the next block;
    // within one block a seek applies before play/stop. After each of them, and after a plan
    // swap, the first latencySamples() output samples are silence: the graph's delay lines still
    // hold audio rendered before the change.
    void play();
    void stop();
    void seek (SampleCount songSample);

    // The device shape every plan is prepared for, fixed at construction. A device that starts
    // with a different shape needs a new Engine.
    const EngineConfig& configuration() const { return config; }

    bool isPlaying() const;
    // True while the audio thread captures a Pass.
    bool isRecording() const;
    // The Song position of the next block's first sample (a pending seek counts as done).
    SampleCount position() const;
    // The latency of the plan the audio thread plays now (0 before the first plan arrives).
    int latencySamples() const;
    // The latest callback's anchor; empty before the first callback.
    std::optional<Anchor> latestAnchor() const;

    // Recording (issue #26), any non-audio thread. The session must stay alive until its state()
    // is `stopped`. It starts at the first block that plays after this call: that block's first
    // input sample is attributed to the block's Song position (RecordSession::begin). It ends at
    // the first block after stopRecording(), or when the transport stops or seeks. A session the
    // audio thread has not taken yet is cancelled by stopRecording() (state `stopped` at once).
    void startRecording (RecordSession& session);
    void stopRecording();

    // Plain Monitoring (issue #26): while on, every Armed Track with input channels hears its live
    // input through its Insert Chain to Master, whether or not the transport plays. Takes effect at
    // the next block. Which Tracks are Armed comes from the loaded Song.
    void setMonitoring (bool on);
    bool isMonitoring() const;

    // RT events: drain with core::drainRtEvents (rtEvents(), engineRtCatalog) or an RtEventDrainThread.
    core::RtEventRing& rtEvents() { return rtEventRing; }

    // Non-audio thread, call regularly (e.g. every 50-100 ms): frees replaced plans, and once per
    // second since the previous summary logs `engine.load` (debug) while the transport plays.
    void housekeeping (std::uint64_t nowHostNs);

    // The duplex device callback. Writes numOutputChannels x numSamples into outputs. Reads
    // numInputChannels x numSamples from inputs (null when there are none) for Monitoring and
    // recording; a missing input channel reads as silence.
    void process (const float* const* inputs,
                  int numInputChannels,
                  float* const* outputs,
                  int numOutputChannels,
                  int numSamples,
                  std::uint64_t hostTimeNs) noexcept;

private:
    static constexpr SampleCount noSeek = std::numeric_limits<SampleCount>::min();

    // Audio thread.
    void takePendingPlan (std::uint64_t hostTimeNs) noexcept;
    // Returns true when a seek moved the position this block.
    bool applyTransportCommands (std::uint64_t hostTimeNs) noexcept;
    void updateRecording (bool seekedThisBlock, std::uint64_t hostTimeNs) noexcept;
    void restartStaleOutput() noexcept;
    void silenceStaleOutput (float* const* outputs, int numOutputChannels, int numSamples) noexcept;
    void reportSilencedNodes (std::uint64_t silencedNodes, std::uint64_t hostTimeNs) noexcept;
    void reportRejectedBlock (int numSamples, int numOutputChannels, std::uint64_t hostTimeNs) noexcept;
    void publishAnchor (std::uint64_t hostTimeNs) noexcept;
    void recordLoad (std::uint64_t callbackNs, int numSamples) noexcept;
    void pushEvent (EngineRtCode code, std::uint64_t hostTimeNs, double arg0 = 0.0, double arg1 = 0.0, double arg2 = 0.0, double arg3 = 0.0) noexcept;

    // Non-audio thread, under nonRealtimeMutex.
    void freeRetiredPlans();

    EngineConfig config;
    core::RtEventRing rtEventRing;

    // Plan handover: non-audio threads publish into pendingPlan; the audio thread moves it to
    // currentPlan and parks the old one in an empty retiredPlans slot for a non-audio thread to
    // free. Only the audio thread fills a slot; only non-audio threads empty one.
    // Invariant (progress): loadSong empties both slots right before it publishes. Until then the
    // audio thread can make at most one swap (taking the one plan already pending), so at most
    // one slot is full when it reaches the new plan, and every published plan is taken by the
    // next callback, with or without housekeeping().
    std::atomic<RenderPlan*> pendingPlan { nullptr };
    std::array<std::atomic<RenderPlan*>, 2> retiredPlans {};
    RenderPlan* currentPlan = nullptr; // audio thread only
    std::atomic<int> currentLatency { 0 };
    std::atomic<int> currentSampleRate { 0 };

    // The shape of the block being rejected (audio thread only); {0, 0} while blocks fit.
    // engine.block_rejected is pushed only when this changes to a new rejected shape.
    struct BlockShape
    {
        int numSamples = 0;
        int numOutputChannels = 0;

        bool operator== (const BlockShape&) const = default;
    };
    BlockShape rejectedShape;

    // Whether the previous rendered block had a node silenced by a busy callback lock (audio
    // thread only); engine.nodes_silenced is pushed when a block has one and the previous did not.
    bool nodesBusyLastBlock = false;

    // Transport: requests in, audio-thread state, published state out.
    std::atomic<bool> playRequested { false };
    std::atomic<SampleCount> seekRequest { noSeek };
    bool playing = false;        // audio thread only
    SampleCount playPosition = 0; // audio thread only: Song sample of the next block start
    // Audio thread only. Output samples still to silence because the graph's delay lines hold
    // audio rendered before the last seek, play, stop or plan swap: set to the plan's latency
    // at each of those, counted down as blocks render.
    SampleCount staleOutputSamples = 0;
    std::atomic<bool> publishedPlaying { false };
    std::atomic<SampleCount> publishedPosition { 0 };

    // Latest anchor, as a seqlock: odd sequence = write in progress, 0 = none yet.
    std::atomic<std::uint32_t> anchorSequence { 0 };
    std::atomic<SampleCount> anchorSongSample { 0 };
    std::atomic<std::uint64_t> anchorHostTimeNs { 0 };

    // engine.load inputs, summed by the audio thread and taken by housekeeping().
    std::atomic<std::uint64_t> loadBusyNs { 0 };
    std::atomic<std::uint64_t> loadSamples { 0 };
    std::atomic<std::uint64_t> loadCallbacks { 0 };
    std::atomic<std::uint64_t> loadPeakCallbackNs { 0 };
    std::atomic<std::uint64_t> silencedNodeBlocks { 0 };

    // Recording: the non-audio thread publishes a session; the audio thread takes it into
    // activeRecord on a playing block and drops it after RecordSession::end().
    std::atomic<RecordSession*> pendingRecord { nullptr };
    std::atomic<bool> recordStopRequested { false };
    RecordSession* activeRecord = nullptr; // audio thread only
    std::atomic<bool> publishedRecording { false };

    std::atomic<bool> monitoringOn { false };

    std::mutex nonRealtimeMutex; // serialises loadSong and housekeeping; never taken by process()
    std::optional<std::uint64_t> lastLoadSummaryNs;
};
} // namespace overdub::engine
