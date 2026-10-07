// PROTOTYPE — issue #7 spike.
#include "SpikeComponent.h"
#include "ClapAnalysis.h"
#include "Exporter.h"
#include "SpikeLog.h"

using namespace juce;

namespace
{
// A frame handed to Core Animation now is on screen at the next refresh.
constexpr double displayLeadMs = 1000.0 / 60.0;

struct ErrorStats
{
    int count = 0;
    double sum = 0, min = 1e9, max = -1e9;
    void add (double x) { ++count; sum += x; min = std::min (min, x); max = std::max (max, x); }
    String describe() const
    {
        return count == 0 ? String ("n=0") : "n=" + String (count) + " mean=" + String (sum / count, 2)
                                           + " min=" + String (min, 2) + " max=" + String (max, 2);
    }
};
}

SpikeComponent::SpikeComponent (AudioEngine* engineIn, CameraRecorder* cameraIn, SpikeOptions optionsIn)
    : engine (engineIn), camera (cameraIn), options (optionsIn)
{
    addAndMakeVisible (videoView);
    for (auto* button : { &recordButton, &stopButton, &playButton, &exportButton, &analyseButton })
        addAndMakeVisible (button);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (logView);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
    statusLabel.setFont (FontOptions (15.0f));

    recordButton.onClick = [this] { startRecording(); };
    stopButton.onClick = [this] { state == State::playing ? stopPlayback() : stopRecording(); };
    playButton.onClick = [this] { startPlayback(); };
    exportButton.onClick = [this] { exportThenAnalyse ({}); };
    analyseButton.onClick = [this] { analyse ({}); };

    if (camera != nullptr)
        videoView.showLivePreview (camera->previewLayer());

    setStatus ("Ready. Record: clap 5 times, keep recording at least 10 minutes, clap 5 times, Stop.");
    setSize (1100, 820);
}

SpikeComponent::~SpikeComponent() = default;

void SpikeComponent::appendLog (const String& line)
{
    logView.moveCaretToEnd();
    logView.insertTextAtCaret (line + "\n");
}

void SpikeComponent::setStatus (const String& text)
{
    statusLabel.setText (text, dontSendNotification);
    updateButtons();
}

void SpikeComponent::updateButtons()
{
    const bool live = engine != nullptr && camera != nullptr;
    recordButton.setEnabled (live && state == State::idle);
    stopButton.setEnabled (state == State::recording || state == State::playing);
    playButton.setEnabled (live && state == State::idle && hasSession);
    exportButton.setEnabled (state == State::idle && hasSession);
    analyseButton.setEnabled (state == State::idle && hasSession);
}

bool SpikeComponent::loadExistingSession (const File& dir)
{
    String error;
    if (! loadSession (dir, session, error))
    {
        spikelog::line ("session_load_failed", { { "error", error } });
        return false;
    }
    hasSession = true;
    spikelog::line ("session_loaded", { { "dir", dir.getFullPathName() },
                                        { "video_offset_s", String (session.videoOffsetSamples / session.sampleRate, 6) } });
    setStatus ("Session " + dir.getFileName());
    return true;
}

//==============================================================================
void SpikeComponent::startRecording()
{
    session = {};
    session.dir = createSessionDirectory();
    session.sampleRate = engine->getSampleRate();
    session.bufferSize = engine->getBufferSize();
    session.inputLatencySamples = engine->getInputLatencySamples();
    session.outputLatencySamples = engine->getOutputLatencySamples();
    session.rtCalibrationSamples = options.rtCalibrationSamples;
    session.cameraCalibrationMs = options.cameraCalibrationMs;
    session.cameraName = camera->cameraName();
    hasSession = false;

    const auto preroll = (int64_t) (options.prerollMs * session.sampleRate / 1000.0);
    if (auto error = engine->startRecording (session.file ("take.wav"), preroll); error.isNotEmpty())
    {
        setStatus ("Record failed: " + error);
        return;
    }

    state = State::recording;
    cameraWriting = false;
    const int id = ++recordingId;
    videoView.showLivePreview (camera->previewLayer());
    setStatus ("Recording audio. Camera starts in " + String ((options.prerollMs + options.videoLeadInMs) / 1000.0, 1) + " s.");

    Timer::callAfterDelay (options.prerollMs + options.videoLeadInMs, [this, id]
    {
        if (state != State::recording || id != recordingId)
            return;
        if (auto error = camera->beginWriting (session.file ("video.mov"), session.file ("frames.csv")); error.isNotEmpty())
        {
            setStatus ("Camera failed: " + error + ". Stop to end the audio take.");
            return;
        }
        cameraWriting = true;
        setStatus ("RECORDING audio + camera. Clap 5 times now, wait at least 10 minutes, clap 5 times, then Stop.");
    });
}

void SpikeComponent::stopRecording()
{
    if (state != State::recording)
        return;
    state = State::stopping;

    if (! cameraWriting)
    {
        CameraTakeResult none;
        none.error = "camera never started";
        finishRecording (none);
        return;
    }

    setStatus ("Stopping camera; audio continues " + String (options.videoLeadOutMs / 1000.0, 1) + " s more.");
    camera->endWriting ([this] (CameraTakeResult result)
    {
        Timer::callAfterDelay (options.videoLeadOutMs, [this, result] { finishRecording (result); });
    });
}

void SpikeComponent::finishRecording (CameraTakeResult cam)
{
    auto audio = engine->stopRecording();
    cameraWriting = false;
    state = State::idle;

    const double rate = session.sampleRate;
    const int reportedRoundTrip = session.inputLatencySamples + session.outputLatencySamples;
    session.recordStartSample = audio.firstRecordedSample;
    session.takeStartSample = audio.firstRecordedSample - (reportedRoundTrip - session.rtCalibrationSamples);
    session.takeLengthSamples = audio.samplesWritten;
    session.clock = fitClock (audio.anchors, rate);
    writeAnchorsCsv (session.file ("anchors-record.csv"), audio.anchors);

    session.captureClock = cam.clockDescription;
    session.captureClockRelativeRate = cam.relativeRateEnd;
    session.videoFirstPtsNs = cam.firstPtsNs;
    session.videoFirstHostNs = cam.firstHostNs;
    session.videoFrameCount = cam.frames;
    session.videoDroppedFrames = cam.writerDrops + cam.captureDrops;
    session.videoDurationSeconds = cam.durationSeconds;

    const double outputLatencyNs = session.outputLatencySamples * 1.0e9 / rate;
    const double cameraCalibrationSamples = session.cameraCalibrationMs * rate / 1000.0;
    session.videoOffsetSamples = songSampleHeardAtHostNs (session.clock, (double) cam.firstHostNs, outputLatencyNs) - cameraCalibrationSamples;

    if (! audio.anchors.empty())
    {
        const double lastFrameHostNs = (double) cam.firstHostNs + cam.durationSeconds * 1.0e9;
        const double fitted = songSampleHeardAtHostNs (session.clock, lastFrameHostNs, outputLatencyNs);
        const double naive = songSampleHeardAtHostNsNaive (audio.anchors.front(), rate, lastFrameHostNs, outputLatencyNs);
        session.driftAtEndMs = (naive - fitted) * 1000.0 / rate;
    }

    saveSession (session);
    hasSession = cam.ok && audio.samplesWritten > 0;

    spikelog::line ("recording_summary", {
        { "dir", session.dir.getFullPathName() },
        { "camera_ok", cam.ok ? "yes" : "no" }, { "camera_error", cam.error },
        { "rate", String (rate) }, { "buffer", String (session.bufferSize) },
        { "in_latency", String (session.inputLatencySamples) }, { "out_latency", String (session.outputLatencySamples) },
        { "rt_calibration", String (session.rtCalibrationSamples) },
        { "take_start_s", String (session.takeStartSample / rate, 6) },
        { "take_length_s", String (session.takeLengthSamples / rate, 3) },
        { "clock_ppm", String (session.clock.ppm(), 3) },
        { "clock_rms_residual_us", String (session.clock.rmsResidualNs / 1000.0, 1) },
        { "clock_max_residual_us", String (session.clock.maxResidualNs / 1000.0, 1) },
        { "anchors", String (session.clock.anchorCount) },
        { "drift_at_end_ms", String (session.driftAtEndMs, 3) },
        { "capture_clock", cam.clockDescription },
        { "capture_rate_start", String (cam.relativeRateStart, 9) }, { "capture_rate_end", String (cam.relativeRateEnd, 9) },
        { "capture_vs_host_max_divergence_us", String (cam.maxHostVsPtsDivergenceUs, 1) },
        { "frames", String (cam.frames) }, { "writer_drops", String (cam.writerDrops) }, { "capture_drops", String (cam.captureDrops) },
        { "frame_interval_mean_ms", String (cam.meanFrameIntervalMs, 3) }, { "frame_interval_max_ms", String (cam.maxFrameIntervalMs, 3) },
        { "delivery_delay_mean_ms", String (cam.meanDeliveryDelayMs, 2) },
        { "video_duration_s", String (cam.durationSeconds, 3) },
        { "video_offset_s", String (session.videoOffsetSamples / rate, 6) } });

    setStatus (hasSession ? "Recorded " + session.dir.getFileName() + ". Play, then Export." : "Recording failed: " + cam.error);
}

//==============================================================================
void SpikeComponent::startPlayback (std::function<void()> onStopped)
{
    AudioBuffer<float> mix;
    String error;
    if (engine == nullptr || ! buildSongMix (session, mix, error) || ! videoView.openMovie (session.file ("video.mov"), error))
    {
        spikelog::line ("play_failed", { { "error", engine == nullptr ? String ("no audio engine") : error } });
        setStatus ("Play failed: " + error);
        if (onStopped)
            onStopped();
        return;
    }
    onPlaybackStopped = std::move (onStopped);
    vblankRows.clear();
    vblankRows.push_back ("host_ns,heard_song_sample,target_movie_s,shown_movie_s,error_ms");
    state = State::playing;
    engine->startPlayback (std::move (mix));
    setStatus ("Playing. Watch the claps.");
}

void SpikeComponent::onVBlank()
{
    if (state != State::playing)
        return;

    if (engine->playbackReachedEnd())
    {
        stopPlayback();
        return;
    }

    const auto now = hostNowNs();
    double heardSong = 0;
    if (! engine->songSampleHeardAt (now + (uint64_t) (displayLeadMs * 1.0e6), heardSong))
        return;

    const double target = (heardSong - session.videoOffsetSamples) * session.movieSecondsPerSongSample();
    const double shown = videoView.showMovieTime (target);
    const double errorMs = shown >= 0 ? (shown - target) * 1000.0 : 0.0;
    vblankRows.push_back (std::to_string ((int64) now) + "," + std::to_string (heardSong) + "," + std::to_string (target) + ","
                          + std::to_string (shown) + "," + (shown >= 0 ? std::to_string (errorMs) : std::string()));
}

void SpikeComponent::stopPlayback()
{
    if (state != State::playing)
        return;
    state = State::idle;
    auto anchors = engine->stopPlayback();
    videoView.closeMovie();
    if (camera != nullptr)
        videoView.showLivePreview (camera->previewLayer());

    writeAnchorsCsv (session.file ("playback-anchors.csv"), anchors);
    String csv;
    ErrorStats stats;
    for (auto& row : vblankRows)
    {
        csv << row << "\n";
        const auto errorField = String (row).fromLastOccurrenceOf (",", false, false);
        if (errorField.isNotEmpty() && ! row.starts_with ("host_ns"))
            stats.add (errorField.getDoubleValue());
    }
    session.file ("playback-vblank.csv").replaceWithText (csv);

    const auto fit = fitClock (anchors, session.sampleRate);
    spikelog::line ("playback_summary", { { "vblanks", String ((int) vblankRows.size() - 1) },
                                          { "shown_minus_target_ms", stats.describe() },
                                          { "display_lead_ms", String (displayLeadMs, 2) },
                                          { "playback_clock_ppm", String (fit.ppm(), 3) },
                                          { "record_clock_ppm", String (session.clock.ppm(), 3) } });
    setStatus ("Playback stopped. Error stats: " + stats.describe() + " ms");
    if (auto callback = std::exchange (onPlaybackStopped, nullptr))
        callback();
}

//==============================================================================
void SpikeComponent::exportThenAnalyse (std::function<void()> onDone)
{
    state = State::busy;
    setStatus ("Exporting trimmed MP4…");
    exportSession (session, false, [this, onDone] (ExportResult trimmed)
    {
        setStatus ("Exporting full-song MP4…");
        exportSession (session, true, [this, onDone, trimmed] (ExportResult full)
        {
            state = State::idle;
            setStatus ("Exports: trimmed " + String (trimmed.ok ? "ok" : trimmed.error) + ", full " + String (full.ok ? "ok" : full.error));
            analyse (onDone);
        });
    });
}

void SpikeComponent::analyse (std::function<void()> onDone)
{
    state = State::busy;
    setStatus ("Analysing claps…");
    const auto s = session;

    Thread::launch ([this, s, onDone]
    {
        String report;
        AudioBuffer<float> mix;
        String error;
        if (buildSongMix (s, mix, error))
        {
            ClapAnalysisInput in;
            in.label = "session (recorded offset)";
            in.audio.assign (mix.getReadPointer (0), mix.getReadPointer (0) + mix.getNumSamples());
            in.sampleRate = s.sampleRate;
            in.videoFile = s.file ("video.mov");
            in.videoStartSample = s.videoOffsetSamples;
            in.movieSecondsPerSample = s.movieSecondsPerSongSample();
            in.outputDir = s.file ("analysis/session");
            report << analyseClaps (in) << "\n";
        }
        else
        {
            report << "session analysis failed: " << error << "\n";
        }

        for (auto name : { "export-trimmed", "export-full" })
        {
            auto movie = s.file (String (name) + ".mp4");
            if (! movie.existsAsFile())
                continue;
            ClapAnalysisInput in;
            double firstPts = 0;
            if (! readMovieAudio (movie, s.sampleRate, in.audio, firstPts, error))
            {
                report << name << " analysis failed: " << error << "\n";
                continue;
            }
            in.label = String (name) + " (audio first pts " + String (firstPts * 1000.0, 3) + " ms)";
            in.sampleRate = s.sampleRate;
            in.videoFile = movie;
            in.videoStartSample = -firstPts * s.sampleRate;
            in.movieSecondsPerSample = 1.0 / s.sampleRate;
            in.outputDir = s.file ("analysis/" + String (name));
            report << analyseClaps (in) << "\n";
        }

        s.file ("analysis/report.txt").replaceWithText (report);
        MessageManager::callAsync ([this, report, onDone]
        {
            state = State::idle;
            for (auto& line : StringArray::fromLines (report))
                appendLog (line);
            setStatus ("Analysis written to " + session.file ("analysis").getFullPathName());
            if (onDone)
                onDone();
        });
    });
}

//==============================================================================
void SpikeComponent::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1e1e1e));
}

void SpikeComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    videoView.setBounds (area.removeFromTop (area.getHeight() / 2));
    area.removeFromTop (8);
    auto buttons = area.removeFromTop (32);
    for (auto* button : { &recordButton, &stopButton, &playButton, &exportButton, &analyseButton })
    {
        button->setBounds (buttons.removeFromLeft (button == &exportButton ? 190 : 100));
        buttons.removeFromLeft (8);
    }
    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (26));
    logView.setBounds (area);
}
