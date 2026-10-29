#include "song/TransposeRender.h"

#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace overdub::song
{
namespace
{
// A fixed seed: the same source and Transpose always render the same samples.
constexpr long stretchSeed = 209;
// How many samples are processed between two looks at the stop request.
constexpr int blockSamples = 1 << 16;
// The stretcher's spectral window and hop, in seconds. Twice presetDefault's window and half its
// hop: presetDefault puts a sine up to 17 cents off its shifted pitch, these within ADR 0005's 5
// (issue #211), at about 2.7 times the render time.
constexpr double stretchWindowSeconds = 0.24;
constexpr double stretchHopSeconds = 0.015;

core::Error renderFailed (const juce::File& file, int semitones, const char* step, const core::Error& cause)
{
    return core::Error { "song.transpose_render_failed",
                         "a Backing Track could not be transposed",
                         { { "path", pathOf (file) }, { "semitones", semitones }, { "step", step }, { "cause", cause.code } } };
}

core::Error renderCancelled (const juce::File& file, int semitones)
{
    return core::Error { "song.transpose_render_cancelled", "the Transpose render was cancelled", { { "path", pathOf (file) }, { "semitones", semitones } } };
}

core::Fields practiceFields (const juce::File& file, int semitones, int speedPercent)
{
    return { { "path", pathOf (file) }, { "semitones", semitones }, { "speed_percent", speedPercent } };
}

core::Error practiceRenderFailed (const juce::File& file, int semitones, int speedPercent, const char* step, const core::Error& cause)
{
    auto fields = practiceFields (file, semitones, speedPercent);
    fields["step"] = step;
    fields["cause"] = cause.code;
    return core::Error { "song.practice_render_failed", "a source could not be rendered at the Practice Speed", std::move (fields) };
}

// Channel pointers `offset` samples into a buffer, as Signalsmith Stretch indexes its IO:
// io[channel][sample].
std::vector<float*> pointersAt (juce::AudioBuffer<float>& audio, int offset)
{
    std::vector<float*> pointers;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        pointers.push_back (audio.getWritePointer (channel) + offset);
    return pointers;
}

// `input` shifted by `semitones` and played at `rate` (input samples per output sample) into
// `output` (as many channels; its length is the input's divided by the rate), as
// SignalsmithStretch::exact() does, but block by block so a stop request is heard. Returns false
// when stopped. An input shorter than the stretcher's seek length comes out silent.
bool stretchAudio (juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int sampleRate, int semitones, double rate, const std::stop_token& stop)
{
    output.clear();
    signalsmith::stretch::SignalsmithStretch<float> stretch (stretchSeed);
    stretch.configure (input.getNumChannels(), (int) (sampleRate * stretchWindowSeconds), (int) (sampleRate * stretchHopSeconds));
    stretch.setTransposeSemitones ((float) semitones);

    const auto inputLength = input.getNumSamples();
    const auto outputLength = output.getNumSamples();
    const auto seekLength = stretch.outputSeekLength ((float) rate);
    if (inputLength < seekLength)
        return ! stop.stop_requested();

    // After the seek, the next output sample is the sound's first. The rest of the input fills the
    // output but its last stretch, which the flush writes.
    stretch.outputSeek (pointersAt (input, 0), seekLength);
    const auto processedInput = inputLength - seekLength;
    const auto processedOutput = std::max (0, outputLength - (int) std::lround (seekLength / rate));
    // The input each output block consumes, proportionally, so the whole keeps the exact rate.
    const auto inputAt = [&] (int outputIndex) { return (int) ((std::int64_t) outputIndex * processedInput / std::max (processedOutput, 1)); };
    for (int done = 0; done < processedOutput; done += blockSamples)
    {
        if (stop.stop_requested())
            return false;
        const auto block = std::min (blockSamples, processedOutput - done);
        const auto inputStart = inputAt (done);
        stretch.process (pointersAt (input, seekLength + inputStart), inputAt (done + block) - inputStart, pointersAt (output, done), block);
    }
    if (stop.stop_requested())
        return false;
    stretch.flush (pointersAt (output, processedOutput), outputLength - processedOutput, (float) rate);
    return true;
}

// Only `range` of `input` stretched as stretchAudio does, into `output` (the whole input's length
// divided by the rate) where the whole render has it; the rest of `output` silent.
bool stretchRange (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, SampleRange range, int sampleRate, int semitones, double rate, const std::stop_token& stop)
{
    output.clear();
    const auto inputLength = (SampleCount) input.getNumSamples();
    const auto start = std::clamp<SampleCount> (range.start, 0, inputLength);
    const auto end = std::clamp<SampleCount> (range.end, start, inputLength);
    const auto outputStart = std::min ((int) std::llround ((double) start / rate), output.getNumSamples());
    const auto outputEnd = std::min ((int) std::llround ((double) end / rate), output.getNumSamples());

    juce::AudioBuffer<float> segment (input.getNumChannels(), (int) (end - start));
    for (int channel = 0; channel < input.getNumChannels(); ++channel)
        segment.copyFrom (channel, 0, input, channel, (int) start, segment.getNumSamples());
    juce::AudioBuffer<float> stretched (input.getNumChannels(), outputEnd - outputStart);
    if (! stretchAudio (segment, stretched, sampleRate, semitones, rate, stop))
        return false;
    for (int channel = 0; channel < output.getNumChannels(); ++channel)
        output.copyFrom (channel, outputStart, stretched, channel, 0, stretched.getNumSamples());
    return true;
}
} // namespace

core::Expected<void> renderTransposed (const juce::File& source, const juce::File& destination, int sampleRate, int semitones, std::stop_token stop)
{
    if (stop.stop_requested())
        return std::unexpected (renderCancelled (destination, semitones));
    auto input = readWavFile (source);
    if (! input.has_value())
        return std::unexpected (renderFailed (source, semitones, "read", input.error()));

    juce::AudioBuffer<float> output (input->getNumChannels(), input->getNumSamples());
    if (! stretchAudio (*input, output, sampleRate, semitones, 1.0, stop))
        return std::unexpected (renderCancelled (destination, semitones));

    if (auto written = writeFloatWavAtomically (destination, output, sampleRate, {}); ! written.has_value())
    {
        destination.deleteFile();
        tempFileFor (destination).deleteFile();
        return std::unexpected (renderFailed (destination, semitones, "write", written.error()));
    }
    return {};
}

core::Expected<void> renderAtPracticeSpeed (const juce::File& source,
                                            const juce::File& destination,
                                            int sampleRate,
                                            int semitones,
                                            int speedPercent,
                                            std::stop_token stop,
                                            std::optional<SampleRange> sourceRange)
{
    const auto cancelled = [&] { return core::Error { "song.practice_render_cancelled", "the Practice Speed render was cancelled", practiceFields (destination, semitones, speedPercent) }; };
    if (stop.stop_requested())
        return std::unexpected (cancelled());
    auto input = readWavFile (source);
    if (! input.has_value())
        return std::unexpected (practiceRenderFailed (source, semitones, speedPercent, "read", input.error()));

    const auto rate = speedPercent / 100.0;
    juce::AudioBuffer<float> output (input->getNumChannels(), (int) std::llround (input->getNumSamples() / rate));
    const auto stretched = sourceRange.has_value() ? stretchRange (*input, output, *sourceRange, sampleRate, semitones, rate, stop)
                                                   : stretchAudio (*input, output, sampleRate, semitones, rate, stop);
    if (! stretched)
        return std::unexpected (cancelled());

    if (auto written = writeFloatWavAtomically (destination, output, sampleRate, {}); ! written.has_value())
    {
        destination.deleteFile();
        tempFileFor (destination).deleteFile();
        return std::unexpected (practiceRenderFailed (destination, semitones, speedPercent, "write", written.error()));
    }
    return {};
}

//==============================================================================

std::unique_ptr<TransposeRun> TransposeRun::start (int sampleRate, int semitones, std::vector<TransposeRenderJob> jobs, Clock::time_point now)
{
    std::unique_ptr<TransposeRun> run (new TransposeRun (semitones, std::move (jobs), now));
    core::logEvent (core::Level::info, "song.transpose_started", run->fields());
    std::vector<RenderWorker::Render> renders;
    for (const auto& job : run->jobs)
        renders.push_back ([job, sampleRate, semitones] (std::stop_token stop) -> core::Expected<void> {
            auto rendered = renderTransposed (job.source, job.destination, sampleRate, semitones, stop);
            if (! rendered.has_value())
                rendered.error().fields["backing_track_id"] = job.backingTrackId;
            return rendered;
        });
    run->worker = std::make_unique<RenderWorker> (std::move (renders));
    return run;
}

TransposeRun::TransposeRun (int semitones, std::vector<TransposeRenderJob> renderJobs, Clock::time_point now)
    : target (semitones),
      jobs (std::move (renderJobs)),
      startedAt (now)
{
}

TransposeRun::~TransposeRun()
{
    cancel (Clock::now());
}

core::Fields TransposeRun::fields() const
{
    return { { "semitones", target }, { "renders", jobs.size() } };
}

std::optional<core::Expected<void>> TransposeRun::advance (Clock::time_point now)
{
    auto ended = worker->take();
    if (ended.has_value() && ended->has_value())
    {
        auto logged = fields();
        logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
        core::logEvent (core::Level::info, "song.transpose_rendered", logged);
    }
    return ended;
}

void TransposeRun::cancel (Clock::time_point now)
{
    if (! worker->stop())
        return;
    auto logged = fields();
    logged["renders_done"] = worker->rendersDone();
    logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
    core::logEvent (core::Level::info, "song.transpose_cancelled", logged);
}
} // namespace overdub::song
