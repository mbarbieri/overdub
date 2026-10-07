// PROTOTYPE — issue #6 spike.
#include "SpikeProcessors.h"
#include "SpikeLog.h"

using namespace juce;

//==============================================================================
BackingPlayer::BackingPlayer (Transport& t)
    : SpikeProcessorBase (BusesProperties().withOutput ("Out", AudioChannelSet::stereo())),
      transport (t)
{
}

void BackingPlayer::processBlock (AudioBuffer<float>& buffer, MidiBuffer&) [[clang::nonblocking]]
{
    buffer.clear();
    auto* source = current.load();

    if (source == nullptr || ! transport.playing.load())
        return;

    const auto start = transport.blockStart.load();
    const auto length = (int64) source->getNumSamples();

    if (start >= length)
        return;

    const auto count = (int) jmin ((int64) buffer.getNumSamples(), length - start);

    for (int ch = 0; ch < jmin (2, buffer.getNumChannels()); ++ch)
        buffer.copyFrom (ch, 0, *source, jmin (ch, source->getNumChannels() - 1), (int) start, count);
}

//==============================================================================
MonitorGain::MonitorGain()
    : SpikeProcessorBase (BusesProperties().withInput ("In", AudioChannelSet::stereo())
                                           .withOutput ("Out", AudioChannelSet::stereo()))
{
}

void MonitorGain::processBlock (AudioBuffer<float>& buffer, MidiBuffer&) [[clang::nonblocking]]
{
    buffer.applyGain (gain.load());
}

//==============================================================================
DryRecorder::DryRecorder (int numInputs, TimeSliceThread& thread)
    : SpikeProcessorBase (BusesProperties().withInput ("In", AudioChannelSet::discreteChannels (jmax (1, numInputs)))),
      writerThread (thread)
{
}

void DryRecorder::processBlock (AudioBuffer<float>& buffer, MidiBuffer&) [[clang::nonblocking]]
{
    const bool playing = transport != nullptr && transport->playing.load();
    const auto blockStart = transport != nullptr ? transport->blockStart.load() : 0;

    for (auto& slot : slots)
    {
        auto* writer = slot.writer.load();

        if (writer == nullptr || ! playing)
            continue;

        const int channel = jlimit (0, buffer.getNumChannels() - 1, slot.inputChannel.load());
        const float* data[] = { buffer.getReadPointer (channel) };

        if (slot.firstBlockStart.load() < 0)
            slot.firstBlockStart.store (blockStart);

        if (writer->write (data, buffer.getNumSamples()))
            slot.samplesWritten.fetch_add (buffer.getNumSamples());
        else
            slot.samplesDropped.fetch_add (buffer.getNumSamples());
    }

    processedBlocks.fetch_add (1);
}

bool DryRecorder::startTake (int track, int inputChannel, const File& file, double sampleRate)
{
    auto& slot = slots[track];
    jassert (slot.writer.load() == nullptr);

    file.deleteFile();
    std::unique_ptr<OutputStream> stream = std::make_unique<FileOutputStream> (file);
    auto options = AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                             .withNumChannels (1)
                                             .withBitsPerSample (32)
                                             .withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = WavAudioFormat().createWriterFor (stream, options);

    if (writer == nullptr)
    {
        spikelog::line ("take_start_failed", { { "track", String (track) }, { "file", file.getFullPathName() } });
        return false;
    }

    // 2^19 frames ≈ 11 s at 48 kHz of slack before the writer thread must catch up.
    slot.owned = std::make_unique<AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 19);
    slot.file = file;
    slot.inputChannel.store (inputChannel);
    slot.firstBlockStart.store (-1);
    slot.samplesWritten.store (0);
    slot.samplesDropped.store (0);
    slot.writer.store (slot.owned.get());
    return true;
}

void DryRecorder::stopAllTakes()
{
    for (auto& slot : slots)
        slot.writer.store (nullptr);

    // Wait for two blocks so the audio thread can't still be inside write().
    const auto target = processedBlocks.load() + 2;
    const auto deadline = Time::getMillisecondCounter() + 1000;

    while (processedBlocks.load() < target && Time::getMillisecondCounter() < deadline)
        Thread::sleep (2);

    for (auto& slot : slots)
        slot.owned.reset();   // flushes to disk
}

DryRecorder::TakeStats DryRecorder::getStats (int track) const
{
    auto& slot = slots[track];
    return { slot.file, slot.firstBlockStart.load(), slot.samplesWritten.load(), slot.samplesDropped.load() };
}
