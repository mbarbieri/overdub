// PROTOTYPE — issue #6 spike.
#include "LoopbackTest.h"
#include "SpikeLog.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

using namespace juce;

namespace
{
constexpr double burstSeconds = 0.010;
constexpr double signalSeconds = 6.0;
const double burstTimes[] = { 0.5, 1.3, 2.1, 2.9, 3.6, 4.4, 5.2 };
constexpr int minLag = -2048;
constexpr int maxLag = 24000;

AudioBuffer<float> makeSignal (double sampleRate, Array<int64>& burstStarts, int& burstLength)
{
    AudioBuffer<float> signal (2, (int) (signalSeconds * sampleRate));
    signal.clear();
    burstLength = (int) (burstSeconds * sampleRate);
    Random random (42);

    for (auto t : burstTimes)
    {
        const auto start = (int64) (t * sampleRate);
        burstStarts.add (start);

        for (int i = 0; i < burstLength; ++i)
        {
            const float sample = 0.5f * (random.nextFloat() * 2.0f - 1.0f);
            signal.setSample (0, (int) start + i, sample);
            signal.setSample (1, (int) start + i, sample);
        }
    }
    return signal;
}

AudioBuffer<float> readTake (const File& file)
{
    WavAudioFormat wav;
    std::unique_ptr<AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));

    if (reader == nullptr)
        return {};

    AudioBuffer<float> take (1, (int) reader->lengthInSamples);
    reader->read (&take, 0, take.getNumSamples(), 0, true, false);
    return take;
}

LoopbackResult analyse (const AudioBuffer<float>& source, const AudioBuffer<float>& take, int64 takeStart,
                        const Array<int64>& burstStarts, int burstLength, int expected)
{
    LoopbackResult result;
    result.expected = expected;
    const float* src = source.getReadPointer (0);
    const float* rec = take.getNumSamples() > 0 ? take.getReadPointer (0) : nullptr;
    double bestNormalised = 0;

    for (auto burst : burstStarts)
    {
        double bestValue = 0;
        int bestLag = 0;
        double bestEnergy = 0;
        double sourceEnergy = 0;

        for (int i = 0; i < burstLength; ++i)
            sourceEnergy += (double) src[burst + i] * src[burst + i];

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const auto offset = burst + lag - takeStart;   // index into take for i = 0
            if (rec == nullptr || offset < 0 || offset + burstLength > take.getNumSamples())
                continue;

            double sum = 0;
            double energy = 0;
            for (int i = 0; i < burstLength; ++i)
            {
                const double r = rec[offset + i];
                sum += src[burst + i] * r;
                energy += r * r;
            }

            if (std::abs (sum) > bestValue)
            {
                bestValue = std::abs (sum);
                bestLag = lag;
                bestEnergy = energy;
            }
        }

        result.lags.add (bestLag);
        if (bestEnergy > 0 && sourceEnergy > 0)
            bestNormalised = jmax (bestNormalised, bestValue / std::sqrt (bestEnergy * sourceEnergy));
    }

    auto sorted = result.lags;
    sorted.sort();
    result.peakCorrelation = bestNormalised;
    result.signalFound = bestNormalised > 0.2 && ! sorted.isEmpty();

    if (! sorted.isEmpty())
    {
        result.measuredMedian = sorted[sorted.size() / 2];
        result.spreadSamples = sorted.getLast() - sorted.getFirst();
        result.errorSamples = result.measuredMedian - expected;
    }
    return result;
}
}

StringPairArray LoopbackResult::toFields (double sampleRate) const
{
    StringArray lagText;
    for (auto lag : lags)
        lagText.add (String (lag));

    const auto ms = [sampleRate] (int samples) { return String (1000.0 * samples / sampleRate, 3); };
    StringPairArray fields;
    fields.set ("signal_found", signalFound ? "yes" : "no");
    fields.set ("peak_corr", String (peakCorrelation, 3));
    fields.set ("expected", String (expected));
    fields.set ("measured", String (measuredMedian));
    fields.set ("measured_ms", ms (measuredMedian));
    fields.set ("error", String (errorSamples));
    fields.set ("error_ms", ms (errorSamples));
    fields.set ("spread", String (spreadSamples));
    fields.set ("lags", lagText.joinIntoString (","));
    fields.set ("dropped", String (dropped));
    return fields;
}

void runLoopbackTest (SpikeEngine& engine, std::function<void (LoopbackResult)> done)
{
    // Monitoring off: with speakers and mic, monitored input would feed back into the take.
    const bool savedMonitoring[] = { engine.getMonitoring (0), engine.getMonitoring (1) };
    engine.setMonitoring (0, false);
    engine.setMonitoring (1, false);

    const auto sampleRate = engine.getLatency().sampleRate;
    auto burstStarts = std::make_shared<Array<int64>>();
    int burstLength = 0;
    auto signal = std::make_shared<AudioBuffer<float>> (makeSignal (sampleRate, *burstStarts, burstLength));
    engine.stop();
    engine.setBackingBuffer (*signal);

    const bool armed[] = { true, true };
    engine.start (armed);
    spikelog::line ("loopback_begin", { { "sr", String (sampleRate) }, { "bursts", String (burstStarts->size()) } });

    Timer::callAfterDelay ((int) (signalSeconds * 1000) + 600,
        [&engine, signal, burstStarts, burstLength, savedMonitoring0 = savedMonitoring[0],
         savedMonitoring1 = savedMonitoring[1], done]
        {
            engine.stop();
            engine.setMonitoring (0, savedMonitoring0);
            engine.setMonitoring (1, savedMonitoring1);

            const auto stats = engine.getTakeStats (0);
            const auto latency = engine.getLatencyAtLastStart();
            auto result = analyse (*signal, readTake (stats.file), stats.firstBlockStart, *burstStarts, burstLength,
                                   latency.total());
            result.dropped = stats.samplesDropped + engine.getTakeStats (1).samplesDropped;

            auto fields = result.toFields (latency.sampleRate);
            fields.set ("buffer", String (latency.bufferSize));
            fields.set ("in_lat", String (latency.input));
            fields.set ("out_lat", String (latency.output));
            fields.set ("graph_lat", String (latency.graph));
            spikelog::line ("loopback_result", fields);
            done (result);
        });
}
