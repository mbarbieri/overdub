#include "capture/PassRecorder.h"

#include "capture/VideoPlacement.h"
#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/PackageFolders.h"
#include "song/Peaks.h"
#include "song/TakeEdits.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <format>

namespace overdub::capture
{
struct PassRecorder::TakeWriter
{
    song::Id takeId;
    song::Id trackId;
    std::vector<int> inputChannels;
    juce::File destination; // audio/<take id>.wav
    juce::File tempFile;    // audio/<take id>.wav.tmp
    std::unique_ptr<juce::AudioFormatWriter> writer;
    const juce::FileOutputStream* stream = nullptr; // owned by writer
    song::PeaksBuilder peaks;
    SampleCount frames = 0;
    bool failed = false;     // a write failed; later chunks are discarded
    std::string failureText; // the stream's error, when it reports one

    TakeWriter (song::Id take, song::Id track, std::vector<int> channels, juce::File file)
        : takeId (std::move (take)),
          trackId (std::move (track)),
          inputChannels (std::move (channels)),
          destination (std::move (file)),
          tempFile (song::tempFileFor (destination)),
          peaks ((int) inputChannels.size())
    {
    }

    int numChannels() const { return (int) inputChannels.size(); }

    // Opens a 32-bit float WAV writer on the temp file. Errors: song.write_failed (step open).
    core::Expected<void> open (int sampleRate)
    {
        tempFile.deleteFile(); // FileOutputStream appends to an existing file
        auto fileStream = std::make_unique<juce::FileOutputStream> (tempFile);
        if (! fileStream->openedOk())
            return std::unexpected (song::writeFailed (tempFile, "open", 0, fileStream->getStatus().getErrorMessage().toStdString()));
        stream = fileStream.get();
        std::unique_ptr<juce::OutputStream> outputStream = std::move (fileStream);

        juce::WavAudioFormat wav;
        const auto writerOptions = juce::AudioFormatWriterOptions {}
                                       .withSampleRate (sampleRate)
                                       .withNumChannels (numChannels())
                                       .withBitsPerSample (32)
                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        writer = wav.createWriterFor (outputStream, writerOptions);
        if (writer == nullptr)
        {
            stream = nullptr;
            return std::unexpected (song::writeFailed (tempFile, "open", 0, "the WAV writer refused the format"));
        }
        return {};
    }

    // io thread (and finish() once that thread is joined).
    void write (const float* const* channels, int numFrames)
    {
        if (failed || writer == nullptr)
            return;
        if (! writer->writeFromFloatArrays (channels, numChannels(), numFrames))
        {
            failed = true;
            failureText = stream->getStatus().getErrorMessage().toStdString();
            return;
        }
        peaks.append (channels, numFrames);
        frames += numFrames;
    }

    // Writes the WAV header and closes the file. Returns false when the final flush failed.
    bool close()
    {
        if (writer == nullptr)
            return ! failed;
        const auto flushed = writer->flush();
        if (! flushed && ! failed)
        {
            failed = true;
            failureText = stream->getStatus().getErrorMessage().toStdString();
        }
        writer.reset();
        stream = nullptr;
        return ! failed;
    }
};

namespace
{
// Wall-clock now as ISO-8601 UTC to the second: "2026-10-07T18:30:00Z".
std::string isoUtcNow()
{
    const auto now = std::chrono::floor<std::chrono::seconds> (std::chrono::system_clock::now());
    return std::format ("{:%FT%TZ}", now);
}

const char* stopReasonName (engine::RecordStopReason reason)
{
    switch (reason)
    {
        case engine::RecordStopReason::none:
            return "none";
        case engine::RecordStopReason::requested:
            return "requested";
        case engine::RecordStopReason::transportStopped:
            return "transport_stopped";
        case engine::RecordStopReason::seeked:
            return "seeked";
        case engine::RecordStopReason::cancelled:
            return "cancelled";
        case engine::RecordStopReason::deviceStopped:
            return "device_stopped";
    }
    return "unknown";
}
} // namespace

//==============================================================================

PassRecorder::PassRecorder (song::SongPackage& songPackage, engine::Engine& audioEngine, RecordLatency recordLatency, PassRecorderOptions recorderOptions)
    : package (songPackage),
      engine (audioEngine),
      latency (recordLatency),
      options (recorderOptions),
      passIdentifier (song::makeId()),
      recordedAt (isoUtcNow())
{
}

core::Expected<std::unique_ptr<PassRecorder>> PassRecorder::start (song::SongPackage& package,
                                                                   engine::Engine& engine,
                                                                   RecordLatency latency,
                                                                   PassRecorderOptions options)
{
    std::unique_ptr<PassRecorder> recorder { new PassRecorder (package, engine, latency, options) };
    const auto& song = package.song();
    const auto audioFolder = package.location().getChildFile (song::audioFolderName);
    if (const auto created = audioFolder.createDirectory(); created.failed())
        return std::unexpected (song::writeFailed (audioFolder, "create_folder", 0, created.getErrorMessage().toStdString()));

    std::vector<engine::RecordInput> inputs;
    for (const auto& track : song.tracks)
    {
        if (! track.armed)
            continue;
        if (track.inputChannels.empty() || track.inputChannels.size() > 2)
        {
            core::logEvent (core::Level::warn, "capture.armed_track_skipped", { { "track_id", track.id }, { "input_channels", track.inputChannels }, { "reason", track.inputChannels.empty() ? "no_input_channels" : "more_than_two_input_channels" } });
            continue;
        }

        const auto takeId = song::makeId();
        auto writer = std::make_unique<TakeWriter> (takeId, track.id, track.inputChannels, audioFolder.getChildFile (juce::String (takeId) + ".wav"));
        if (auto opened = writer->open (song.sampleRate); ! opened.has_value())
        {
            for (auto& earlier : recorder->writers)
            {
                earlier->close();
                earlier->tempFile.deleteFile();
            }
            writer->tempFile.deleteFile();
            return std::unexpected (opened.error());
        }
        inputs.push_back (engine::RecordInput { track.id, track.inputChannels });
        recorder->writers.push_back (std::move (writer));
    }

    if (inputs.empty())
        return std::unexpected (core::Error { "capture.nothing_armed", "no Armed Track has input channels to record", {} });

    const auto fifoFrames = std::max (1, (int) (options.fifoSeconds * song.sampleRate));
    recorder->session = std::make_unique<engine::RecordSession> (std::move (inputs), fifoFrames);
    if (options.recordPoint.has_value())
        recorder->session->startAt (*options.recordPoint);
    if (options.camera != nullptr)
        recorder->beginMovie();
    recorder->ioThread = std::thread ([raw = recorder.get()] { raw->runIoThread(); });
    engine.startRecording (*recorder->session);
    return recorder;
}

PassRecorder::~PassRecorder()
{
    if (finished || session == nullptr)
        return;

    // A Pass the audio thread already ended (finishing off the message thread, issue #90) leaves
    // the engine untouched: it may be gone.
    if (! hasStopped())
        requestStop();
    if (! waitUntilStopped (std::chrono::milliseconds (2000)))
    {
        // The audio thread may still write into the session: leaking it is the only safe choice.
        core::logEvent (core::Level::error, "capture.session_abandoned", { { "pass_id", passIdentifier } });
        stopIoThread();
        (void) session.release();
        return;
    }
    stopIoThread();
    for (auto& writer : writers)
        writer->close();
    discardMovie();
}

void PassRecorder::requestStop()
{
    engine.stopRecording (*session);
}

bool PassRecorder::hasStopped() const
{
    return session->state() == engine::RecordState::stopped;
}

PassPhase PassRecorder::phase() const
{
    if (finished)
        return PassPhase::finished;
    switch (session->state())
    {
        case engine::RecordState::waiting:
            return PassPhase::waiting;
        case engine::RecordState::recording:
            return PassPhase::capturing;
        case engine::RecordState::stopped:
            return PassPhase::saving;
    }
    return PassPhase::saving;
}

std::optional<SampleCount> PassRecorder::runningPassStart() const
{
    if (session->state() != engine::RecordState::recording)
        return std::nullopt;
    return session->startSongSample();
}

bool PassRecorder::waitUntilStopped (std::chrono::milliseconds timeout) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (! hasStopped())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return true;
}

std::optional<FinishedPass> PassRecorder::poll (song::SongPackage::Clock::time_point now)
{
    if (finished || ! hasStopped())
        return std::nullopt;
    // The movie completes off this thread (issue #70): the Pass is finished once it has.
    prepareFinish();
    if (! isReadyToFinish())
        return std::nullopt;
    return finish (now);
}

std::optional<FinishedPass> PassRecorder::release (std::chrono::milliseconds timeout)
{
    requestStop();
    if (! waitUntilStopped (timeout))
        return FinishedPass { engine::RecordStopReason::none,
                              std::unexpected (core::Error { "capture.stop_timeout", "the audio device did not end the Pass", { { "pass_id", passIdentifier }, { "timeout_ms", timeout.count() } } }) };
    // The movie may complete after the engine is gone: poll() finishes the Pass then (issue #90).
    return poll();
}

//==============================================================================
// io thread

void PassRecorder::runIoThread()
{
    const core::ScopedThreadRole role { core::ThreadRole::io };
    while (true)
    {
        drainOnce();

        std::unique_lock lock { ioMutex };
        if (ioWake.wait_for (lock, options.ioInterval, [this] { return ioShouldExit; }))
            break;
    }
    // Everything the audio thread queued before it ended the Pass.
    drainOnce();
}

void PassRecorder::drainOnce()
{
    logPassStartedOnce();
    session->drain ([this] (std::size_t input, const float* const* channels, int frames) { writers[input]->write (channels, frames); });
    session->drainAnchors (anchors);
}

void PassRecorder::logPassStartedOnce()
{
    const auto state = session->state();
    // A Pass that ended before it began (cancelled, or stopped during pre-roll) captured nothing.
    const auto neverBegan = state == engine::RecordState::stopped && session->capturedFrames() == 0;
    if (passStartedLogged || state == engine::RecordState::waiting || neverBegan)
        return;

    passStartedLogged = true;
    auto trackIds = core::Fields::array();
    auto inputChannels = core::Fields::array();
    for (const auto& input : session->inputs())
    {
        trackIds.push_back (input.trackId);
        inputChannels.push_back (input.inputChannels);
    }
    core::logEvent (core::Level::info,
                    "capture.pass_started",
                    {
                        { "pass_id", passIdentifier },
                        { "track_ids", trackIds },
                        { "input_channels", inputChannels },
                        { "camera", movieBegun },
                        { "start_sample", session->startSongSample() },
                        { "plan_latency_samples", session->startPlanLatency() },
                        { "input_latency_samples", latency.inputLatencySamples },
                        { "output_latency_samples", latency.outputLatencySamples },
                        { "calibration_offset_samples", latency.calibrationOffsetSamples },
                    });
}

void PassRecorder::stopIoThread()
{
    {
        const std::scoped_lock lock { ioMutex };
        ioShouldExit = true;
    }
    ioWake.notify_all();
    if (ioThread.joinable())
        ioThread.join();
}

//==============================================================================
// finish

core::Error PassRecorder::failedTakeWrite (const TakeWriter& writer) const
{
    return core::Error { "capture.take_write_failed",
                         "a Take file could not be written",
                         { { "pass_id", passIdentifier },
                           { "track_id", writer.trackId },
                           { "take_id", writer.takeId },
                           { "path", song::pathOf (writer.tempFile) },
                           { "detail", writer.failureText } } };
}

void PassRecorder::logPassStopped (const RecordedPass& pass, const char* outcome) const
{
    auto trackIds = core::Fields::array();
    for (const auto& writer : writers)
        trackIds.push_back (writer->trackId);
    core::logEvent (std::string_view (outcome) == "recorded" ? core::Level::info : core::Level::warn,
                    "capture.pass_stopped",
                    {
                        { "pass_id", passIdentifier },
                        { "outcome", outcome },
                        { "track_ids", trackIds },
                        { "take_ids", pass.takeIds },
                        { "camera", movieBegun },
                        { "video_take_id", pass.videoTakeId.has_value() ? nlohmann::json (*pass.videoTakeId) : nlohmann::json (nullptr) },
                        { "start_sample", pass.startSample },
                        { "length_samples", pass.lengthSamples },
                        { "stop_reason", stopReasonName (session->stopReason()) },
                        { "overrun_frames", session->overrunFrames() },
                        { "dropped_anchors", session->droppedAnchors() },
                        { "lost_gaps", session->lostGaps() },
                        { "anchors", pass.anchors.size() },
                    });
}

void PassRecorder::prepareFinish()
{
    if (finishPrepared || ! hasStopped())
        return;
    finishPrepared = true;
    stopIoThread();
    for (auto& writer : writers)
        writer->close();

    // Only a Pass that will be kept ends its movie; finish() discards the others' movies.
    const auto captured = session->capturedFrames() > 0;
    const auto writeFailed = std::ranges::any_of (writers, [] (const auto& writer) { return writer->failed; });
    if (movie.has_value() && captured && ! writeFailed)
    {
        movieEnding = options.camera->endMovie();
        core::logEvent (core::Level::info, "capture.video_ending", { { "pass_id", passIdentifier }, { "video_take_id", movie->videoTakeId } });
    }
}

bool PassRecorder::isReadyToFinish() const
{
    if (! finishPrepared)
        return false;
    return ! movieEnding.valid() || movieEnding.wait_for (std::chrono::seconds (0)) == std::future_status::ready;
}

FinishedPass PassRecorder::finish (song::SongPackage::Clock::time_point now)
{
    // The reason is read once addPass has seen the audio thread end the Pass (it is valid only then).
    auto pass = addPass (now);
    return FinishedPass { session->stopReason(), std::move (pass) };
}

core::Expected<RecordedPass> PassRecorder::addPass (song::SongPackage::Clock::time_point now)
{
    if (finished)
        return std::unexpected (core::Error { "capture.already_finished", "the Pass was already finished", { { "pass_id", passIdentifier } } });
    if (! hasStopped())
        return std::unexpected (core::Error { "capture.not_stopped", "the audio thread has not ended the Pass yet", { { "pass_id", passIdentifier } } });

    prepareFinish();
    finished = true;

    RecordedPass pass;
    pass.passId = passIdentifier;
    pass.startSample = session->startSongSample();
    pass.lengthSamples = session->capturedFrames();
    pass.anchors = std::move (anchors);
    pass.overrunFrames = session->overrunFrames();

    if (pass.lengthSamples == 0)
    {
        for (auto& writer : writers)
            writer->tempFile.deleteFile();
        discardMovie();
        logPassStopped (pass, "empty");
        return std::unexpected (core::Error { "capture.pass_empty", "the Pass captured no audio", { { "pass_id", passIdentifier } } });
    }

    // Every Take or none: a failed write leaves all of them as temp files, which the next open deletes.
    if (const auto failedWriter = std::ranges::find_if (writers, [] (const auto& writer) { return writer->failed; }); failedWriter != writers.end())
    {
        discardMovie();
        logPassStopped (pass, "failed");
        return std::unexpected (failedTakeWrite (**failedWriter));
    }

    for (auto& writer : writers)
    {
        if (auto renamed = song::renameIntoPlace (writer->tempFile, writer->destination, {}); ! renamed.has_value())
        {
            discardMovie();
            logPassStopped (pass, "failed");
            return std::unexpected (renamed.error());
        }
        if (auto peaks = song::writePeaksFile (song::peaksFileFor (package.location(), writer->takeId), writer->peaks.finish(), {}); ! peaks.has_value())
            core::logError (core::Error { "capture.peaks_write_failed", "could not write the peaks cache", peaks.error().fields }, core::Level::warn);
    }

    // ADR 0002 with the render-ahead amendment: see the header.
    const auto planLatency = session->startPlanLatency();
    const auto reportedLatency = (SampleCount) latency.inputLatencySamples + latency.outputLatencySamples + planLatency;
    const auto firstSampleRenderPosition = pass.startSample + planLatency;
    const auto origin = firstSampleRenderPosition - (reportedLatency + latency.calibrationOffsetSamples);

    auto changed = package.song();
    if (auto videoTake = finishMovie (pass))
        changed.videoTakes.push_back (std::move (*videoTake));
    changed.passes.push_back (song::Pass { passIdentifier, pass.startSample, recordedAt });
    for (const auto& writer : writers)
    {
        song::Take take;
        take.id = writer->takeId;
        take.trackId = writer->trackId;
        take.passId = passIdentifier;
        take.file = std::string (song::audioFolderName) + "/" + writer->takeId + ".wav";
        take.channels = writer->numChannels();
        take.lengthSamples = writer->frames;
        take.origin = origin;
        take.compensation = song::TakeCompensation { reportedLatency, latency.calibrationOffsetSamples };
        changed.takes.push_back (take);
        pass.takeIds.push_back (take.id);

        auto* track = song::findTrack (changed, writer->trackId);
        if (track == nullptr)
            continue; // deleted during the Pass: the Take stays, unheard
        if (auto clip = song::clipAsRecorded (take, pass.startSample, song::makeId()))
            track->clips.push_back (std::move (*clip));
    }

    const auto label = "Record Pass " + std::to_string (changed.passes.size());
    if (auto edited = package.edit (std::move (changed), label, now); ! edited.has_value())
    {
        // A failed song.json write comes after the step is committed: the Pass then stands.
        const auto& passes = package.song().passes;
        const auto committed = std::ranges::find (passes, passIdentifier, &song::Pass::id) != passes.end();
        logPassStopped (pass, committed ? "recorded" : "failed");
        return std::unexpected (edited.error());
    }

    logPassStopped (pass, "recorded");
    return pass;
}
//==============================================================================
// Video Take

void PassRecorder::beginMovie()
{
    const auto openCamera = options.camera->openCamera();
    MovieInProgress next;
    next.videoTakeId = song::makeId();
    next.cameraId = openCamera.has_value() ? openCamera->id : std::string {};
    const auto videoFolder = package.location().getChildFile (song::videoFolderName);
    next.destination = videoFolder.getChildFile (juce::String (next.videoTakeId) + ".mov");
    next.tempFile = song::tempFileFor (next.destination);

    if (const auto created = videoFolder.createDirectory(); created.failed())
    {
        movieBeginError = song::writeFailed (videoFolder, "create_folder", 0, created.getErrorMessage().toStdString());
        core::logEvent (core::Level::info, "capture.video_stopped", { { "pass_id", passIdentifier }, { "outcome", "not_started" } });
        return;
    }
    if (auto begun = options.camera->beginMovie (next.tempFile); ! begun.has_value())
    {
        movieBeginError = begun.error();
        // The Error itself is logged where it is presented (RecordedPass::notices).
        core::logEvent (core::Level::info, "capture.video_stopped", { { "pass_id", passIdentifier }, { "outcome", "not_started" } });
        return;
    }

    core::logEvent (core::Level::info,
                    "capture.video_started",
                    { { "pass_id", passIdentifier }, { "video_take_id", next.videoTakeId }, { "camera_id", next.cameraId }, { "path", song::pathOf (next.tempFile) } });
    movie = std::move (next);
    movieBegun = true;
}

void PassRecorder::discardMovie()
{
    if (! movie.has_value())
        return;
    const auto inProgress = std::move (*movie);
    movie.reset();
    if (movieEnding.valid())
        (void) movieEnding.get(); // the camera no longer writes it, but completes it: wait, then delete
    else
        options.camera->cancelMovie();
    inProgress.tempFile.deleteFile();
    core::logEvent (core::Level::info, "capture.video_stopped", { { "pass_id", passIdentifier }, { "video_take_id", inProgress.videoTakeId }, { "outcome", "discarded" } });
}

std::optional<song::VideoTake> PassRecorder::finishMovie (RecordedPass& pass)
{
    if (movieBeginError.has_value())
    {
        pass.notices.push_back (*movieBeginError);
        return std::nullopt;
    }
    if (! movie.has_value())
        return std::nullopt;

    const auto inProgress = std::move (*movie);
    movie.reset();
    const auto failed = [&] (core::Error error) -> std::optional<song::VideoTake> {
        inProgress.tempFile.deleteFile();
        core::logEvent (core::Level::info, "capture.video_stopped", { { "pass_id", passIdentifier }, { "video_take_id", inProgress.videoTakeId }, { "outcome", "failed" } });
        pass.notices.push_back (std::move (error));
        return std::nullopt;
    };

    // Invariant: prepareFinish() ended the movie of every Pass that reaches here.
    if (! OVERDUB_CHECK (movieEnding.valid(), "capture.movie_not_ended", { "pass_id", passIdentifier }))
        return failed (core::Error { "capture.movie_write_failed", "the movie was not ended", { { "pass_id", passIdentifier }, { "reason", "not ended" } } });
    const auto ended = movieEnding.get();
    if (! ended.has_value())
        return failed (ended.error());

    const auto sampleRate = package.song().sampleRate;
    const auto fit = fitAnchors (pass.anchors, sampleRate);
    if (! fit.has_value())
        return failed (core::Error { "capture.video_unplaced", "the Pass has no anchors to place its Video Take on", { { "pass_id", passIdentifier } } });

    if (auto renamed = song::renameIntoPlace (inProgress.tempFile, inProgress.destination, {}); ! renamed.has_value())
        return failed (renamed.error());

    const auto placement = placeVideoTake (*fit, ended->firstFrameHostNs, latency.outputLatencySamples, options.cameraCalibrationSamples, ended->captureClockRate, sampleRate);
    song::VideoTake videoTake;
    videoTake.id = inProgress.videoTakeId;
    videoTake.passId = passIdentifier;
    videoTake.file = std::string (song::videoFolderName) + "/" + inProgress.videoTakeId + ".mov";
    videoTake.startSample = placement.startSample;
    videoTake.songSamplesPerMovieSecond = placement.songSamplesPerMovieSecond;
    videoTake.cameraId = ended->cameraId.empty() ? inProgress.cameraId : ended->cameraId;
    videoTake.calibrationOffsetSamples = options.cameraCalibrationSamples;
    videoTake.outputDevice = options.outputDevice;
    pass.videoTakeId = videoTake.id;

    if (ended->droppedFrames > 0)
        pass.notices.push_back (core::Error { "capture.video_frames_dropped",
                                              "the camera dropped frames during the Pass",
                                              { { "pass_id", passIdentifier }, { "video_take_id", videoTake.id }, { "dropped_frames", ended->droppedFrames } } });

    core::logEvent (core::Level::info,
                    "capture.video_stopped",
                    {
                        { "pass_id", passIdentifier },
                        { "video_take_id", videoTake.id },
                        { "outcome", "recorded" },
                        { "camera_id", videoTake.cameraId },
                        { "output_device", videoTake.outputDevice },
                        { "frames", ended->frames },
                        { "dropped_frames", ended->droppedFrames },
                        { "duration_seconds", ended->durationSeconds },
                        { "longest_gap_seconds", ended->longestGapSeconds },
                        { "first_frame_host_ns", ended->firstFrameHostNs },
                        { "capture_clock_rate", ended->captureClockRate },
                        { "start_sample", placement.startSample },
                        { "song_samples_per_movie_second", placement.songSamplesPerMovieSecond },
                        { "drift_ppm", placement.driftPpm },
                        { "output_latency_samples", latency.outputLatencySamples },
                        { "calibration_offset_samples", options.cameraCalibrationSamples },
                        { "anchors", fit->anchorCount },
                        { "max_anchor_residual_us", fit->maxResidualNs / 1000.0 },
                    });
    return videoTake;
}
} // namespace overdub::capture
