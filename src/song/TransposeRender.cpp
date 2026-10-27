#include "song/TransposeRender.h"

#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
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

// Channel pointers `offset` samples into a buffer, as Signalsmith Stretch indexes its IO:
// io[channel][sample].
std::vector<float*> pointersAt (juce::AudioBuffer<float>& audio, int offset)
{
    std::vector<float*> pointers;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        pointers.push_back (audio.getWritePointer (channel) + offset);
    return pointers;
}

// `input` shifted by `semitones` into `output` (same shape), as SignalsmithStretch::exact() does at
// a playback rate of 1, but block by block so a stop request is heard. Returns false when stopped.
// An input shorter than the stretcher's seek length comes out silent.
bool shiftPitch (juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int sampleRate, int semitones, const std::stop_token& stop)
{
    output.clear();
    signalsmith::stretch::SignalsmithStretch<float> stretch (stretchSeed);
    stretch.configure (input.getNumChannels(), (int) (sampleRate * stretchWindowSeconds), (int) (sampleRate * stretchHopSeconds));
    stretch.setTransposeSemitones ((float) semitones);

    const auto length = input.getNumSamples();
    const auto seekLength = stretch.outputSeekLength (1.0f);
    if (length < seekLength)
        return ! stop.stop_requested();

    // After the seek, the next output sample is the sound's first.
    stretch.outputSeek (pointersAt (input, 0), seekLength);
    const auto processedLength = length - seekLength;
    for (int done = 0; done < processedLength; done += blockSamples)
    {
        if (stop.stop_requested())
            return false;
        const auto block = std::min (blockSamples, processedLength - done);
        stretch.process (pointersAt (input, seekLength + done), block, pointersAt (output, done), block);
    }
    if (stop.stop_requested())
        return false;
    stretch.flush (pointersAt (output, processedLength), seekLength, 1.0f);
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
    if (! shiftPitch (*input, output, sampleRate, semitones, stop))
        return std::unexpected (renderCancelled (destination, semitones));

    if (auto written = writeFloatWavAtomically (destination, output, sampleRate, {}); ! written.has_value())
    {
        destination.deleteFile();
        tempFileFor (destination).deleteFile();
        return std::unexpected (renderFailed (destination, semitones, "write", written.error()));
    }
    return {};
}

//==============================================================================

std::unique_ptr<TransposeRun> TransposeRun::start (int sampleRate, int semitones, std::vector<TransposeRenderJob> jobs, Clock::time_point now)
{
    std::unique_ptr<TransposeRun> run (new TransposeRun (semitones, std::move (jobs), now));
    core::logEvent (core::Level::info, "song.transpose_started", run->fields());
    run->worker = std::jthread ([self = run.get(), sampleRate] (std::stop_token stop) {
        core::Expected<void> result;
        for (const auto& job : self->jobs)
        {
            result = renderTransposed (job.source, job.destination, sampleRate, self->target, stop);
            if (! result.has_value())
            {
                result.error().fields["backing_track_id"] = job.backingTrackId;
                break;
            }
            ++self->rendersDone;
        }
        self->outcome = std::move (result);
        self->done.store (true, std::memory_order_release);
    });
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
    if (over || ! done.load (std::memory_order_acquire))
        return std::nullopt;
    worker.join();
    over = true;
    if (outcome->has_value())
    {
        auto logged = fields();
        logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
        core::logEvent (core::Level::info, "song.transpose_rendered", logged);
    }
    return std::move (*outcome);
}

void TransposeRun::cancel (Clock::time_point now)
{
    if (over)
        return;
    worker.request_stop();
    worker.join();
    over = true;
    if (done.load (std::memory_order_acquire) && outcome.has_value() && outcome->has_value())
        return; // every render was written before the stop: nothing was cancelled
    auto logged = fields();
    logged["renders_done"] = rendersDone.load();
    logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
    core::logEvent (core::Level::info, "song.transpose_cancelled", logged);
}
} // namespace overdub::song
