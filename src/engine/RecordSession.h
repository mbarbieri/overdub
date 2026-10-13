#pragma once

#include "engine/Anchor.h"
#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The audio side of one Pass (issue #26): what the engine's process() captures while recording,
// handed to a reader thread through lock-free single-producer single-consumer FIFOs.
//
// Threads: the owner builds the session off the audio thread and gives it to
// Engine::startRecording. The audio thread then calls begin(), capture() for every block of the
// Pass, and end(), and never touches the session after end(). One reader thread (the `io` thread
// of capture/PassRecorder) calls drain() and drainAnchors() at any time. The owner may free the
// session once state() is `stopped`.
//
// Invariant (alignment): every input is written in lockstep, so all inputs share one stream of
// frames numbered from 0 at the Pass start. A block that does not fit in every FIFO is not
// queued; it becomes a gap of that many frames at its stream position, and drain() hands out
// zeros for it, so every later frame keeps its position on the timeline.
namespace overdub::engine
{
// One Armed Track's capture: the device input channels it records, 1 (mono) or 2 (stereo).
struct RecordInput
{
    song::Id trackId;
    std::vector<int> inputChannels;
};

enum class RecordState
{
    waiting,   // published, not yet taken by the audio thread (or never published)
    recording, // begin() ran: start position known, blocks are being captured
    stopped    // end() ran (or the session was cancelled): the audio thread is done with it
};

// Why the audio thread ended the Pass.
enum class RecordStopReason
{
    none,
    requested,        // Engine::stopRecording
    transportStopped, // the transport stopped: a Pass covers one continuous run of the transport
    seeked,           // a seek: the captured stream would no longer be continuous Song time
    cancelled,        // stopped before the audio thread ever took it
    deviceStopped     // the device stopped calling back (unplugged, closed): Engine::deviceStopped
};

class RecordSession
{
public:
    // Allocates every FIFO up front. fifoFrames: frames each input can queue before the reader
    // drains them; anchorCapacity: callbacks' anchors queued; gapCapacity: gaps queued.
    RecordSession (std::vector<RecordInput> inputs, int fifoFrames, int anchorCapacity = 8192, int gapCapacity = 1024);

    RecordSession (const RecordSession&) = delete;
    RecordSession& operator= (const RecordSession&) = delete;

    const std::vector<RecordInput>& inputs() const { return recordInputs; }

    // Pre-roll (issue #32): the Pass starts at this Song sample instead of at the first playing
    // block. Set it before Engine::startRecording (publication makes it visible to the audio
    // thread; setting it later is a data race), off the audio thread. The Pass then begins at the
    // block that plays this sample and captures from it (from the block start if the transport is
    // already past it).
    void startAt (SampleCount songSample) { requestedRecordPoint = songSample; }
    std::optional<SampleCount> recordPoint() const { return requestedRecordPoint; }

    //==========================================================================
    // Audio thread. Never allocate, lock or log.

    // The first captured block's first input sample is attributed to Song sample `songSample`
    // (the block's first output sample, ADR 0003 amendment); `planLatency` is the plan's PDC then.
    void begin (SampleCount songSample, int planLatency) noexcept;
    // Queues one block of every input from frame `firstFrame` on (a missing device channel records
    // silence) and the block's anchor. firstFrame > 0 only on the block the Pass begins in, when
    // its record point falls inside the block; that block's anchor still names the block start
    // (the callback's Song position and host time), not the Pass start.
    void capture (const float* const* deviceInputs, int numDeviceInputs, int numSamples, Anchor anchor, int firstFrame = 0) noexcept;
    void end (RecordStopReason reason) noexcept;

    //==========================================================================
    // Any thread.

    RecordState state() const { return currentState.load (std::memory_order_acquire); }
    // Valid once state() is not `waiting`.
    SampleCount startSongSample() const { return startSample.load (std::memory_order_relaxed); }
    int startPlanLatency() const { return planLatencyAtStart.load (std::memory_order_relaxed); }
    // Valid once state() is `stopped`.
    RecordStopReason stopReason() const { return reasonForStop.load (std::memory_order_relaxed); }
    // Frames of the captured stream so far, gaps included.
    SampleCount capturedFrames() const { return streamFrames.load (std::memory_order_acquire); }
    // Frames that did not fit in the FIFOs (now gaps of zeros), anchors and gaps the full rings lost.
    std::uint64_t overrunFrames() const { return overrunCount.load (std::memory_order_relaxed); }
    std::uint64_t droppedAnchors() const { return droppedAnchorCount.load (std::memory_order_relaxed); }
    std::uint64_t lostGaps() const { return lostGapCount.load (std::memory_order_relaxed); }

    // Non-audio thread, only when the session was never taken by the audio thread.
    void cancel() noexcept;

    //==========================================================================
    // Reader thread only.

    // Called once per input per chunk, in stream order: `channels` holds that input's channel count
    // of `numFrames` samples each (zeros for a gap).
    using ChunkWriter = std::function<void (std::size_t inputIndex, const float* const* channels, int numFrames)>;

    // Hands every frame queued on every input, gaps included, to `write`. Returns frames handed out.
    SampleCount drain (const ChunkWriter& write);
    // Appends every queued anchor to `into`.
    void drainAnchors (std::vector<Anchor>& into);

private:
    struct Gap
    {
        SampleCount streamFrame = 0;
        SampleCount frames = 0;
    };

    struct InputFifo
    {
        juce::AbstractFifo fifo;
        juce::AudioBuffer<float> storage;

        InputFifo (int channels, int frames) : fifo (frames), storage (channels, frames) {}
    };

    void writeInput (InputFifo& input, const RecordInput& spec, const float* const* deviceInputs, int numDeviceInputs, int firstFrame, int numFrames) noexcept;
    void pushGap (SampleCount frames) noexcept;
    void handOut (std::size_t inputIndex, int frames, const ChunkWriter& write);
    void handOutZeros (SampleCount frames, const ChunkWriter& write);

    std::vector<RecordInput> recordInputs;
    std::optional<SampleCount> requestedRecordPoint; // written before publication, then read-only
    std::vector<std::unique_ptr<InputFifo>> fifos;

    juce::AbstractFifo anchorFifo;
    std::vector<Anchor> anchorStorage;
    juce::AbstractFifo gapFifo;
    std::vector<Gap> gapStorage;

    std::atomic<RecordState> currentState { RecordState::waiting };
    std::atomic<RecordStopReason> reasonForStop { RecordStopReason::none };
    std::atomic<SampleCount> startSample { 0 };
    std::atomic<int> planLatencyAtStart { 0 };
    std::atomic<SampleCount> streamFrames { 0 };
    std::atomic<std::uint64_t> overrunCount { 0 };
    std::atomic<std::uint64_t> droppedAnchorCount { 0 };
    std::atomic<std::uint64_t> lostGapCount { 0 };

    // Reader thread only.
    SampleCount framesHandedOut = 0;
    juce::AudioBuffer<float> zeros;
};
} // namespace overdub::engine
