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
// Finishing without waiting (issues #70, #90, #104): AVFoundation may take seconds to complete the
// movie. The owner calls poll() regularly: once the audio thread has ended the Pass (phase() is
// saving), poll() closes the Take files and ends the movie (Camera::endMovie completes it off the
// calling thread), and once the movie is complete it adds the Pass and returns it, once, as a
// FinishedPass with its stop reason. The Pass and its Video Take are one undo step. release() asks
// the audio thread to end the Pass and waits for it only, so the engine may go; poll() still
// finishes the Pass afterwards. finish() waits for the movie (closing the Song, tests).
//
// Threads: start(), poll(), release(), finish() and the destructor run on one non-audio thread (the
// message thread in the app); the package is single-threaded and only touched there. The
// destructor waits for a movie being completed and deletes it.
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

// Where a Pass is, as the owner sees it.
enum class PassPhase
{
    waiting,   // for the transport, or for the record point during pre-roll: nothing captured yet
    capturing, // the audio thread captures input
    saving,    // the audio thread ended the Pass; it is being saved (its movie completing)
    finished   // poll(), release() or finish() returned it
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

// A Pass that is over: why the audio thread ended it, and the Pass or why it was not added.
struct FinishedPass
{
    engine::RecordStopReason stopReason = engine::RecordStopReason::none;
    core::Expected<RecordedPass> pass;
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

    // Asks the audio thread to end the Pass at its next block (it may also end it itself: transport
    // stopped or seeked, the device stopped, Engine::deviceStopped).
    void requestStop();
    PassPhase phase() const;
    // The record point once the audio thread has begun the Pass and until it ends it; none before
    // (waiting for the transport, or for the record point during pre-roll) and after.
    std::optional<SampleCount> runningPassStart() const;

    // Call regularly. Once the audio thread has ended the Pass, prepares it without waiting (see the
    // header comment); returns the FinishedPass once its movie, if any, is complete. Returns it once;
    // none before and after.
    std::optional<FinishedPass> poll (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

    // requestStop(), then waits up to `timeout` for the audio thread and prepares the Pass without
    // waiting for the movie (issue #90): the engine may go once this returns. Returns the
    // FinishedPass when it is already complete (poll() returns it otherwise), or capture.stop_timeout
    // with no stop reason (the device delivered no callback: the Pass is not finished, and the owner
    // destroys the recorder).
    std::optional<FinishedPass> release (std::chrono::milliseconds timeout = std::chrono::milliseconds (2000));

    // Requires the audio thread to have ended the Pass. Prepares it if not done yet, waits for the
    // movie to complete, writes the rest of every Take, renames the files into audio/, writes peaks,
    // and adds the Pass as one undo step. Logs capture.pass_stopped (Pass id, Tracks, start sample,
    // length, overrun and dropped counts, stop reason) whatever the outcome. A failed peaks write
    // is only logged (capture.peaks_write_failed, warn): it is a cache.
    // Errors: capture.not_stopped, capture.already_finished, capture.pass_empty (nothing was
    // captured; temp files deleted), capture.take_write_failed (track_id, take_id, path),
    // song.write_failed (rename), the package edit's Errors (the Take files are kept).
    FinishedPass finish (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

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

    bool hasStopped() const;
    // Requires hasStopped() (does nothing before, and after the first call). Stops the io thread,
    // closes the Take files and ends the movie of a Pass that will be kept, without waiting for the
    // camera to complete it.
    void prepareFinish();
    // True once prepareFinish() was called and the movie, if any, is complete.
    bool isReadyToFinish() const;
    core::Expected<RecordedPass> addPass (song::SongPackage::Clock::time_point now);
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
