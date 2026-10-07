// PROTOTYPE — issue #7 spike.
#include "AudioEngine.h"
#include "SpikeLog.h"

using namespace juce;

AudioEngine::AudioEngine()
{
    writerThread.startThread();
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    activeWriter.store (nullptr);
    writer.reset();
    writerThread.stopThread (2000);
}

String AudioEngine::openDefaultDevice()
{
    auto error = deviceManager.initialise (1, 2, nullptr, true);
    if (error.isNotEmpty())
        return error;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return "no audio device opened";

    auto setup = deviceManager.getAudioDeviceSetup();
    spikelog::line ("audio_device_open", { { "type", device->getTypeName() },
                                           { "input", setup.inputDeviceName },
                                           { "output", setup.outputDeviceName },
                                           { "rate", String (device->getCurrentSampleRate()) },
                                           { "buffer", String (device->getCurrentBufferSizeSamples()) },
                                           { "in_latency", String (device->getInputLatencyInSamples()) },
                                           { "out_latency", String (device->getOutputLatencyInSamples()) },
                                           { "active_inputs", String (device->getActiveInputChannels().countNumberOfSetBits()) },
                                           { "active_outputs", String (device->getActiveOutputChannels().countNumberOfSetBits()) } });

    deviceManager.addAudioCallback (this);
    startTimer (100);
    return {};
}

double AudioEngine::getSampleRate() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getCurrentSampleRate() : 0.0;
}

int AudioEngine::getBufferSize() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
}

int AudioEngine::getInputLatencySamples() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getInputLatencyInSamples() : 0;
}

int AudioEngine::getOutputLatencySamples() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getOutputLatencyInSamples() : 0;
}

String AudioEngine::startRecording (const File& wavFile, int64_t fromSample)
{
    if (mode.load() != Mode::idle)
        return "engine busy";

    wavFile.deleteFile();
    std::unique_ptr<OutputStream> stream = std::make_unique<FileOutputStream> (wavFile);
    WavAudioFormat wav;
    auto fileWriter = wav.createWriterFor (stream, AudioFormatWriterOptions{}.withSampleRate (getSampleRate())
                                                                             .withNumChannels (1)
                                                                             .withBitsPerSample (24));
    if (fileWriter == nullptr)
        return "cannot create " + wavFile.getFullPathName();

    writer = std::make_unique<AudioFormatWriter::ThreadedWriter> (fileWriter.release(), writerThread, 1 << 17);
    collectedAnchors.clear();
    droppedAnchors.store (0);
    recordFromSample.store (fromSample);
    firstRecordedSample.store (-1);
    samplesWritten.store (0);
    writeFailures.store (0);
    transportSample.store (0);
    activeWriter.store (writer.get());
    mode.store (Mode::recording);

    spikelog::line ("audio_record_start", { { "file", wavFile.getFullPathName() }, { "record_from_sample", String ((int64) fromSample) } });
    return {};
}

AudioEngine::RecordingResult AudioEngine::stopRecording()
{
    mode.store (Mode::idle);
    activeWriter.store (nullptr);
    waitForCallbacksToPass();
    writer.reset(); // flushes the remaining FIFO to disk
    drainAnchors();

    RecordingResult result;
    result.firstRecordedSample = firstRecordedSample.load();
    result.samplesWritten = samplesWritten.load();
    result.anchors = std::move (collectedAnchors);
    collectedAnchors.clear();

    spikelog::line ("audio_record_stop", { { "first_recorded_sample", String ((int64) result.firstRecordedSample) },
                                           { "samples_written", String ((int64) result.samplesWritten) },
                                           { "write_failures", String (writeFailures.load()) },
                                           { "anchors", String ((int) result.anchors.size()) },
                                           { "dropped_anchors", String (droppedAnchors.load()) } });
    return result;
}

void AudioEngine::startPlayback (AudioBuffer<float> songMix)
{
    if (mode.load() != Mode::idle)
        return;

    playbackMix = std::move (songMix);
    collectedAnchors.clear();
    droppedAnchors.store (0);
    transportSample.store (0);
    latestSong.store (-1);
    mode.store (Mode::playing);
    spikelog::line ("audio_play_start", { { "samples", String (playbackMix.getNumSamples()) } });
}

std::vector<ClockAnchor> AudioEngine::stopPlayback()
{
    mode.store (Mode::idle);
    waitForCallbacksToPass();
    drainAnchors();
    auto anchors = std::move (collectedAnchors);
    collectedAnchors.clear();
    spikelog::line ("audio_play_stop", { { "transport", String ((int64) transportSample.load()) },
                                         { "anchors", String ((int) anchors.size()) },
                                         { "dropped_anchors", String (droppedAnchors.load()) } });
    return anchors;
}

bool AudioEngine::playbackReachedEnd() const
{
    return mode.load() == Mode::playing && transportSample.load() >= playbackMix.getNumSamples();
}

bool AudioEngine::songSampleHeardAt (uint64_t hostNs, double& songSample) const
{
    ClockAnchor anchor;
    if (! readLatestAnchor (anchor))
        return false;

    const double rate = getSampleRate();
    const double outputLatencyNs = getOutputLatencySamples() * 1.0e9 / rate;
    const double elapsedNs = (double) (int64_t) (hostNs - anchor.hostNs) - outputLatencyNs;
    songSample = (double) anchor.songSample + elapsedNs * rate / 1.0e9;
    return true;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                                    float* const* output, int numOutputs,
                                                    int numSamples, const AudioIODeviceCallbackContext& context)
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (output[ch] != nullptr)
            FloatVectorOperations::clear (output[ch], numSamples);

    const auto currentMode = mode.load();
    if (currentMode == Mode::idle)
        return;

    const auto position = transportSample.load();

    if (context.hostTimeNs != nullptr)
    {
        const ClockAnchor anchor { position, *context.hostTimeNs };
        publishLatestAnchor (anchor);

        const auto scope = anchorFifo.write (1);
        if (scope.blockSize1 + scope.blockSize2 == 1)
            anchorRing[(size_t) (scope.blockSize1 == 1 ? scope.startIndex1 : scope.startIndex2)] = anchor;
        else
            droppedAnchors.fetch_add (1);
    }

    if (currentMode == Mode::recording && numInputs > 0 && input[0] != nullptr && position >= recordFromSample.load())
    {
        if (auto* w = activeWriter.load())
        {
            if (firstRecordedSample.load() < 0)
                firstRecordedSample.store (position);

            const float* channels[] = { input[0] };
            if (w->write (channels, numSamples))
                samplesWritten.fetch_add (numSamples);
            else
                writeFailures.fetch_add (1);
        }
    }

    if (currentMode == Mode::playing && position < playbackMix.getNumSamples())
    {
        const auto count = (int) std::min<int64_t> (numSamples, playbackMix.getNumSamples() - position);
        for (int ch = 0; ch < numOutputs; ++ch)
            if (output[ch] != nullptr)
                FloatVectorOperations::copy (output[ch], playbackMix.getReadPointer (0, (int) position), count);
    }

    transportSample.store (position + numSamples);
}

void AudioEngine::audioDeviceAboutToStart (AudioIODevice* device)
{
    spikelog::line ("audio_device_start", { { "rate", String (device->getCurrentSampleRate()) },
                                            { "buffer", String (device->getCurrentBufferSizeSamples()) } });
}

void AudioEngine::audioDeviceStopped()
{
    spikelog::line ("audio_device_stopped");
}

void AudioEngine::timerCallback()
{
    drainAnchors();
}

void AudioEngine::publishLatestAnchor (ClockAnchor anchor)
{
    latestSequence.fetch_add (1, std::memory_order_acq_rel); // odd: write in progress
    latestSong.store (anchor.songSample, std::memory_order_relaxed);
    latestHostNs.store (anchor.hostNs, std::memory_order_relaxed);
    latestSequence.fetch_add (1, std::memory_order_release); // even: stable
}

bool AudioEngine::readLatestAnchor (ClockAnchor& anchor) const
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto before = latestSequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;

        anchor.songSample = latestSong.load (std::memory_order_relaxed);
        anchor.hostNs = latestHostNs.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);

        if (latestSequence.load (std::memory_order_relaxed) == before)
            return anchor.songSample >= 0;
    }
    return false;
}

void AudioEngine::drainAnchors()
{
    const auto scope = anchorFifo.read (anchorFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i)
        collectedAnchors.push_back (anchorRing[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        collectedAnchors.push_back (anchorRing[(size_t) (scope.startIndex2 + i)]);
}

void AudioEngine::waitForCallbacksToPass()
{
    // Spike shortcut: a callback that loaded the old mode or writer finishes within two buffers.
    const auto rate = getSampleRate() > 0 ? getSampleRate() : 44100.0;
    Thread::sleep (20 + (int) (2000.0 * getBufferSize() / rate));
}
