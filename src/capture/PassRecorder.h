#pragma once

#include "capture/Camera.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "song/SongPackage.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
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
// captured. Its Clip (song::clipAsRecorded, song/TakeEdits.h) starts at the Pass start (the record point) or at the origin, whichever is
// later: input captured before the record point is in the file but not heard.
//
// On stop, the Pass, its Takes and one new top-layer Clip per Take are added as one undo step
// "Record Pass <N>", and each Take's peaks are written. Take files are never deleted once
// renamed, even when the Song change fails.
//
// Video Take (issue #35, ADR 0003): with a camera, start() also begins a movie in
// video/<video take id>.mov.tmp. On stop the movie is ended and renamed into place, and the Video
// Take joins the Pass's undo step, placed by capture/VideoPlacement.h from the Pass's anchors, the
// device's output latency and the camera calibration. Recording continues whenever it can (ADR
// 0006): a movie that cannot begin or finish leaves the Pass audio-only, and that problem, like
// dropped frames, comes back as a Notice in RecordedPass::notices. The movie runs from start() to
// finish(), so during pre-roll it holds frames from before the record point; they are placed
// before the Pass start like any other frame.
//
// Finishing without waiting (issue #70): AVFoundation may take seconds to complete the movie. The
// message thread calls prepareFinish() once hasStopped(), which ends the movie (Camera::endMovie
// completes it off that thread), polls isReadyToFinish(), and then calls finish(), which no longer
// waits. The Pass and its Video Take are still one undo step, added once the movie is complete.
// finish() alone (stop(), detaching the audio, tests) waits for the movie.
//
// Threads: start(), prepareFinish(), isReadyToFinish(), finish(), stop() and the destructor run on
// one non-audio thread (the message thread in the app); the package is single-threaded and only
// touched there. The destructor waits for a movie being completed and deletes it.
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
    // Pre-roll (issue #32): the Pass, and so its Takes, start at this Song sample rather than at the
    // first playing block; nothing before it is captured (RecordSession::startAt).
    std::optional<SampleCount> recordPoint;
    // The camera to record a Video Take from (open, not owned; it must outlive the recorder), and
    // this machine's calibration for it and the output device, in samples (CameraCalibration.h).
    Camera* camera = nullptr;
    SampleCount cameraCalibrationSamples = 0;
    // The output device the Pass plays through, as JUCE lists it: the Video Take keeps it as the
    // other half of its camera calibration key (song::VideoTake::outputDevice, issue #68).
    std::string outputDevice;
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
    // The Video Take added with the Pass, when the camera recorded one.
    std::optional<song::Id> videoTakeId;
    // Notice-tier problems that did not stop the Pass (the movie failed, frames were dropped);
    // not logged here: whoever presents them logs them once.
    std::vector<core::Error> notices;
};

class PassRecorder
{
public:
    // Prepares one Take per Armed Track with 1 or 2 input channels (any other Armed Track is
    // logged as capture.armed_track_skipped, warn, with `reason`, and skipped), opens their temp files, starts the io
    // thread and hands the capture to the engine, which starts it on its next playing block.
    // Errors: capture.nothing_armed, song.write_failed (a Take file could not be opened).
    static core::Expected<std::unique_ptr<PassRecorder>> start (song::SongPackage& package,
                                                                engine::Engine& engine,
                                                                RecordLatency latency,
                                                                PassRecorderOptions options = {});

    // The engine must outlive the recorder.
    // Without finish(): asks the engine to stop, waits for it briefly and stops the io thread. The
    // Pass is not added; its temp files are left for the next open to delete.
    ~PassRecorder();

    PassRecorder (const PassRecorder&) = delete;
    PassRecorder& operator= (const PassRecorder&) = delete;

    // Asks the audio thread to end the Pass at its next block.
    void requestStop();
    // True once the audio thread has ended the Pass (stop requested, transport stopped or seeked),
    // or the device stopped (Engine::deviceStopped).
    bool hasStopped() const;
    // Why the Pass ended; valid once hasStopped().
    engine::RecordStopReason stopReason() const;
    // The record point once the audio thread has begun the Pass and until it ends it; none before
    // (waiting for the transport, or for the record point during pre-roll) and after.
    std::optional<SampleCount> runningPassStart() const;

    // Requires hasStopped() (does nothing before, and after the first call). Stops the io thread,
    // closes the Take files and ends the movie of a Pass that will be kept, without waiting for the
    // camera to complete it.
    void prepareFinish();
    // True once prepareFinish() was called and the movie, if any, is complete: finish() then does
    // not wait for the camera.
    bool isReadyToFinish() const;

    // Requires hasStopped(). prepareFinish() if not done yet, waits for the movie to complete, writes the rest of every Take, renames the files into audio/, writes
    // peaks, and adds the Pass as one undo step. Logs capture.pass_stopped (Pass id, Tracks,
    // start sample, length, overrun and dropped counts, stop reason) whatever the outcome.
    // A failed peaks write is only logged (capture.peaks_write_failed, warn): it is a cache.
    // Errors: capture.not_stopped, capture.already_finished, capture.pass_empty (nothing was captured; temp files deleted),
    // capture.take_write_failed (track_id, take_id, path), song.write_failed (rename),
    // the package edit's Errors (the Take files are kept).
    core::Expected<RecordedPass> finish (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

    // requestStop(), then waits up to `timeout` for the audio thread, then finish().
    // Errors: capture.stop_timeout (the device delivered no callback), or finish()'s.
    core::Expected<RecordedPass> stop (std::chrono::milliseconds timeout = std::chrono::milliseconds (2000));

    // requestStop(), then waits up to `timeout` for the audio thread, then prepareFinish(): the
    // movie completes off this thread (issue #90). The engine may go once this returns.
    // Errors: capture.stop_timeout.
    core::Expected<void> stopWithoutWaitingForMovie (std::chrono::milliseconds timeout = std::chrono::milliseconds (2000));

    const song::Id& passId() const { return passIdentifier; }

private:
    struct TakeWriter;
    struct MovieInProgress
    {
        song::Id videoTakeId;
        juce::File destination; // video/<id>.mov
        juce::File tempFile;    // video/<id>.mov.tmp
        std::string cameraId;
    };

    PassRecorder (song::SongPackage& package, engine::Engine& engine, RecordLatency latency, PassRecorderOptions options);

    void runIoThread();
    void drainOnce();
    void logPassStartedOnce();
    void stopIoThread();
    bool waitUntilStopped (std::chrono::milliseconds timeout) const;
    core::Error failedTakeWrite (const TakeWriter& writer) const;
    void logPassStopped (const RecordedPass& pass, const char* outcome) const;
    void beginMovie();
    // Ends (or waits for the ending movie) and deletes its file: the Pass will not keep it.
    void discardMovie();
    // Waits for the ended movie, renames it into place and places it on the Pass's anchors. None (with the
    // reason in pass.notices) when there is no movie to keep.
    std::optional<song::VideoTake> finishMovie (RecordedPass& pass);

    song::SongPackage& package;
    engine::Engine& engine;
    RecordLatency latency;
    PassRecorderOptions options;
    song::Id passIdentifier;
    std::string recordedAt;

    std::vector<std::unique_ptr<TakeWriter>> writers; // one per session input, same order
    std::unique_ptr<engine::RecordSession> session;
    std::optional<MovieInProgress> movie;      // while the camera writes this Pass's movie
    std::optional<core::Error> movieBeginError; // why the movie never began
    MovieCompletion movieEnding; // from prepareFinish() until finishMovie()
    bool movieBegun = false;                    // set before the io thread starts, then read-only

    // io thread state. anchors and the writers are written by the io thread until it is joined.
    std::vector<engine::Anchor> anchors;
    bool passStartedLogged = false;
    std::mutex ioMutex;
    std::condition_variable ioWake;
    bool ioShouldExit = false; // under ioMutex
    std::thread ioThread;
    bool finishPrepared = false;
    bool finished = false;
};
} // namespace overdub::capture
