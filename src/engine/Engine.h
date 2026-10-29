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

// A Loop's range in Song samples, [start, end) with start < end (issue #228, ADR 0010).
struct LoopRange
{
    SampleCount start = 0;
    SampleCount end = 0;

    bool operator== (const LoopRange&) const = default;
};

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
    // monitoredTracks: the Armed Tracks that hear their input while Monitoring is on.
    // practice: the Practice Speed and its renders (issue #212). The position stays in Song samples
    // at any speed: while playing it advances speedPercent / 100 Song samples per output sample, and
    // a plan at another speed takes over at the same Song sample.
    void loadSong (const song::Song& song, const juce::File& packageRoot, const InsertFactory& makeInserts = {}, const MonitoredTracks& monitoredTracks = std::nullopt, const PracticePlayback& practice = {});

    // The Mixer (issue #31), any non-audio thread. Applies the Song's Track volumes, pans, mutes and
    // solos and its Master volume (engine/MixLaw.h) to the most recently loaded plan without
    // rebuilding it: heard from the next block, ramped over that block. Returns false, changing
    // nothing, when no Song is loaded or the Song's Tracks (ids, in order) are not the loaded
    // Song's: then call loadSong. Logs engine.mix_applied or engine.mix_refused (debug, `reason`
    // no_plan or tracks_differ).
    bool setMix (const song::Song& song);
    // The band filter heard (issue #234, ADR 0010), any non-audio thread: applied to the most
    // recently loaded plan at once (heard from its next block, no rebuild) and to every plan
    // loadSong builds after. None: no strip filters. The UI sets it only in Transcribe mode, so
    // Record, Edit and Export (which builds its own plan) never hear it. Logs engine.band_filter_set
    // (debug: on, track_id, low_hz, high_hz).
    void setBandFilter (const std::optional<HeardBandFilter>& filter);
    // Any non-audio thread. Each strip's peak (post-fader, linear) and each Armed Track's input peak
    // (MeterPeaks) since the previous call, from the most recently loaded plan; empty before the
    // first loadSong.
    MeterPeaks takeMeterPeaks();

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

    // Looping (issue #228), any non-audio thread; heard from the next block. While a range is set
    // and the transport plays, playback that reaches its end goes on from its start in the same
    // block: the graph renders the block in spans, so the wrap is sample-continuous, with no gap and
    // no stale-output silence. The render-ahead wraps first (it reads latencySamples() ahead), and
    // the position follows. Playback from before the start plays into the range; from at or past its
    // end it plays straight on. At a Practice Speed the range is wrapped in plan samples
    // (songToPlanSample). None: no looping. The UI sets it only in Transcribe mode.
    void setLoop (std::optional<LoopRange> range);

    bool isPlaying() const;
    // True while the audio thread captures a Pass.
    bool isRecording() const;
    // The Song position of the next block's first sample (a pending seek counts as done).
    SampleCount position() const;
    // The latency of the plan the audio thread plays now (0 before the first plan arrives).
    int latencySamples() const;
    // The tail of the plan the audio thread plays now (RenderPlan::tailSamples; 0 before the first).
    int tailSamples() const;
    // True from loadSong until the audio thread has taken that plan (at the start of a callback).
    // Once false, the plan the audio thread renders is the latest one loaded, and every older plan
    // has finished its last block: export waits for this before rendering the shared plugins
    // itself (issue #37).
    bool hasPendingPlan() const { return pendingPlan.load (std::memory_order_acquire) != nullptr; }
    // The latest callback's anchor; empty before the first callback.
    std::optional<Anchor> latestAnchor() const;

    // Recording (issue #26), any non-audio thread. The session must stay alive until its state()
    // is `stopped`. It starts at the first block that plays after this call: that block's first
    // input sample is attributed to the block's Song position (RecordSession::begin). A Pass with
    // a record point (RecordSession::startAt, pre-roll, issue #32) is taken at the first playing
    // block too but begins at the block that plays that Song sample, capturing from that sample
    // on; isRecording() turns true then. Until it begins it ends like a running Pass. It ends at
    // the first block after stopRecording (session), or when the transport stops or seeks. A
    // session the audio thread has not taken yet is cancelled by stopRecording (state `stopped`
    // at once), and so is one still pending when another session is started.
    // stopRecording only acts on the session it names; any other is left alone.
    void startRecording (RecordSession& session);
    void stopRecording (RecordSession& session);

    // Plain Monitoring (issue #26): while on, every monitored Armed Track with input channels hears
    // its live input through its Insert Chain to Master, whether or not the transport plays. Takes
    // effect at the next block. Which Tracks are Armed and monitored comes from loadSong.
    void setMonitoring (bool on);
    bool isMonitoring() const;

    // Device changes (issue #41). Non-audio thread, and only while process() is not running and
    // cannot start concurrently: the device has stopped calling back. DeviceBinding calls it from
    // audioDeviceStopped, which JUCE calls after the device's IO proc has stopped (and with the
    // device manager's callback lock, or after the callback was removed under that lock). Stops the transport where it
    // is and ends the Pass (running, or published and not yet taken) with
    // RecordStopReason::deviceStopped, keeping what it captured. Logs engine.device_stopped (info:
    // was_playing, pass_ended, captured_frames, position). The next device to call process() finds
    // the transport stopped.
    void deviceStopped();

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
    // Reads the latest setLoop into heardLoop; keeps the previous one while a write is in progress.
    void takeLoop() noexcept;
    // The Loop in plan samples, while it wraps playback that is at plan sample planBase: a Loop is
    // set, the transport plays and planBase is before the Loop's end.
    struct PlanLoop
    {
        SampleCount start = 0;
        SampleCount end = 0;
    };
    std::optional<PlanLoop> wrappingLoop (SampleCount planBase) const noexcept;
    // Where plan sample `planBase + offset` plays while heardLoop wraps (see setLoop).
    SampleCount loopedPlanSample (SampleCount planBase, SampleCount offset) const noexcept;
    // Renders the block in spans, split where the render position wraps at the Loop's end.
    std::uint64_t renderLooped (float* const* outputs, const LiveInput& input, int numSamples) noexcept;
    void takePendingPlan (std::uint64_t hostTimeNs) noexcept;
    // Returns true when a seek moved the position this block.
    bool applyTransportCommands (std::uint64_t hostTimeNs) noexcept;
    // Ends and starts Passes; returns the block frame the active Pass captures from (0 unless a
    // Pass begins at a record point inside this block).
    int updateRecording (bool seekedThisBlock, int numSamples, std::uint64_t hostTimeNs) noexcept;
    void restartStaleOutput() noexcept;
    int currentSpeedPercent() const noexcept;
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
    // The plan loadSong published last (under nonRealtimeMutex). It is pending or current: a plan
    // is retired, and so freed, only after a newer one is published.
    RenderPlan* latestPlan = nullptr;
    std::atomic<int> currentLatency { 0 };
    std::atomic<int> currentTail { 0 };
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
    // Audio thread only: the current plan's sample of the next block start (RenderPlan::speedPercent);
    // playPosition is planToSongSample of it, and equals it at 100%.
    SampleCount planPosition = 0;
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
    // activeRecord on a playing block, begins it then (or, with a record point, at the block that
    // plays it) and drops it after RecordSession::end(). A taken Pass that has not begun ends
    // like a running one (stop, transport stop, seek), having captured nothing.
    std::atomic<RecordSession*> pendingRecord { nullptr };
    std::atomic<RecordSession*> recordStopRequestedFor { nullptr };
    RecordSession* activeRecord = nullptr; // audio thread only
    bool activeRecordBegun = false;        // audio thread only
    std::atomic<bool> publishedRecording { false };

    std::atomic<bool> monitoringOn { false };

    // The Loop, as a seqlock written by setLoop (odd sequence = write in progress); end 0 = none.
    std::atomic<std::uint32_t> loopSequence { 0 };
    std::atomic<SampleCount> loopStartRequest { 0 };
    std::atomic<SampleCount> loopEndRequest { 0 };
    std::optional<LoopRange> heardLoop; // audio thread only
    std::mutex loopWriteMutex; // serialises setLoop writers; never taken by process()

    std::optional<HeardBandFilter> heardBandFilter; // under nonRealtimeMutex
    std::mutex nonRealtimeMutex; // serialises loadSong and housekeeping; never taken by process()
    std::optional<std::uint64_t> lastLoadSummaryNs;
};
} // namespace overdub::engine
