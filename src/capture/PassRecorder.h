#pragma once

#include "core/Error.h"
#include "engine/Engine.h"
#include "song/SongPackage.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Recording one Pass (issue #26, ADR 0002, ADR 0004): one dry Take per Armed Track, written by an
// `io` thread from the engine's capture FIFOs (engine/RecordSession.h) into the package as
// audio/<take id>.wav.tmp (32-bit float WAV at the Song's rate), renamed into place on stop.
//
// Placement (ADR 0002 with the ADR 0003 render-ahead amendment): the first captured input sample
// was captured while the Track nodes rendered Song sample start + L (L = plan PDC latency). A Take
// therefore belongs at
//     origin = (start + L) - (input latency + output latency + L + calibration offset)
// so a sound lands on the Song sample at which it was heard. The Take keeps every dry sample it
// captured. Its Clip starts at the Pass start (the record point) or at the origin, whichever is
// later: input captured before the record point is in the file but not heard.
//
// On stop, the Pass, its Takes and one new top-layer Clip per Take are added as one undo step
// "Record Pass <N>", and each Take's peaks are written. Take files are never deleted once
// renamed, even when the Song change fails.
//
// Threads: start(), finish(), stop() and the destructor run on one non-audio thread (the message
// thread in the app); the package is single-threaded and only touched there.
namespace overdub::capture
{
using song::SampleCount;

// The latency compensation for this device (ADR 0002). calibrationOffsetSamples is the
// per-machine loopback calibration for this device and sample rate (engine/LatencyCalibration.h).
struct RecordLatency
{
    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    SampleCount calibrationOffsetSamples = 0;
};

struct PassRecorderOptions
{
    // Seconds of input each Armed Track can queue before the io thread drains it.
    double fifoSeconds = 4.0;
    // How often the io thread drains the FIFOs. Tests set it long to provoke an overrun.
    std::chrono::milliseconds ioInterval { 10 };
};

// What a finished Pass produced, beyond what the Song now holds.
struct RecordedPass
{
    song::Id passId;
    std::vector<song::Id> takeIds; // in Armed Track order
    SampleCount startSample = 0;   // the record point
    SampleCount lengthSamples = 0; // captured frames, gaps included
    // One anchor per callback of the Pass (ADR 0003), for the Video Take's clock fit (slice 14).
    std::vector<engine::Anchor> anchors;
    std::uint64_t overrunFrames = 0;
};

class PassRecorder
{
public:
    // Prepares one Take per Armed Track with input channels (an Armed Track without any is logged
    // as capture.track_without_input, warn, and skipped), opens their temp files, starts the io
    // thread and hands the capture to the engine, which starts it on its next playing block.
    // Errors: capture.nothing_armed, song.write_failed (a Take file could not be opened).
    static core::Expected<std::unique_ptr<PassRecorder>> start (song::SongPackage& package,
                                                                engine::Engine& engine,
                                                                RecordLatency latency,
                                                                PassRecorderOptions options = {});

    // Without finish(): asks the engine to stop, waits for it briefly and stops the io thread. The
    // Pass is not added; its temp files are left for the next open to delete.
    ~PassRecorder();

    PassRecorder (const PassRecorder&) = delete;
    PassRecorder& operator= (const PassRecorder&) = delete;

    // Asks the audio thread to end the Pass at its next block.
    void requestStop();
    // True once the audio thread has ended the Pass (stop requested, transport stopped or seeked).
    bool hasStopped() const;

    // Requires hasStopped(). Writes the rest of every Take, renames the files into audio/, writes
    // peaks, and adds the Pass as one undo step. Logs capture.pass_stopped (Pass id, Tracks,
    // start sample, length, overrun and dropped counts, stop reason) whatever the outcome.
    // Errors: capture.not_stopped, capture.pass_empty (nothing was captured; temp files deleted),
    // capture.take_write_failed (track_id, take_id, path), song.write_failed (rename),
    // the package edit's Errors (the Take files are kept).
    core::Expected<RecordedPass> finish (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

    // requestStop(), then waits up to `timeout` for the audio thread, then finish().
    // Errors: capture.stop_timeout (the device delivered no callback), or finish()'s.
    core::Expected<RecordedPass> stop (std::chrono::milliseconds timeout = std::chrono::milliseconds (2000));

    const song::Id& passId() const { return passIdentifier; }

private:
    struct TakeWriter;

    PassRecorder (song::SongPackage& package, engine::Engine& engine, RecordLatency latency, PassRecorderOptions options);

    void runIoThread();
    void drainOnce();
    void logPassStartedOnce();
    void stopIoThread();
    bool waitUntilStopped (std::chrono::milliseconds timeout) const;
    core::Error failedTakeWrite (const TakeWriter& writer) const;
    void logPassStopped (const RecordedPass& pass, const char* outcome) const;

    song::SongPackage& package;
    engine::Engine& engine;
    RecordLatency latency;
    PassRecorderOptions options;
    song::Id passIdentifier;
    std::string recordedAt;

    std::vector<std::unique_ptr<TakeWriter>> writers; // one per session input, same order
    std::unique_ptr<engine::RecordSession> session;

    // io thread state. anchors and the writers are written by the io thread until it is joined.
    std::vector<engine::Anchor> anchors;
    bool passStartedLogged = false;
    std::mutex ioMutex;
    std::condition_variable ioWake;
    bool ioShouldExit = false; // under ioMutex
    std::thread ioThread;
    bool finished = false;
};
} // namespace overdub::capture
