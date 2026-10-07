#include "engine/RecordSession.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace overdub::engine
{
namespace
{
constexpr int zeroChunkFrames = 4096;

// Two channel pointers into `storage` at `start`, for an input of `channels` channels.
std::array<const float*, 2> channelPointers (const juce::AudioBuffer<float>& storage, int channels, int start)
{
    std::array<const float*, 2> pointers {};
    for (int c = 0; c < channels; ++c)
        pointers[(std::size_t) c] = storage.getReadPointer (c, start);
    return pointers;
}
} // namespace

RecordSession::RecordSession (std::vector<RecordInput> inputs, int fifoFrames, int anchorCapacity, int gapCapacity)
    : recordInputs (std::move (inputs)),
      anchorFifo (anchorCapacity),
      anchorStorage ((std::size_t) anchorCapacity),
      gapFifo (gapCapacity),
      gapStorage ((std::size_t) gapCapacity),
      zeros (2, zeroChunkFrames)
{
    zeros.clear();
    // AbstractFifo holds one item less than its size.
    for (const auto& input : recordInputs)
        fifos.push_back (std::make_unique<InputFifo> ((int) input.inputChannels.size(), fifoFrames + 1));
}

//==============================================================================
// Audio thread

void RecordSession::begin (SampleCount songSample, int planLatency) noexcept
{
    startSample.store (songSample, std::memory_order_relaxed);
    planLatencyAtStart.store (planLatency, std::memory_order_relaxed);
    currentState.store (RecordState::recording, std::memory_order_release);
}

void RecordSession::capture (const float* const* deviceInputs, int numDeviceInputs, int numSamples, Anchor anchor) noexcept
{
    if (numSamples <= 0)
        return;

    if (anchorFifo.getFreeSpace() >= 1)
    {
        const auto scope = anchorFifo.write (1);
        anchorStorage[(std::size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = anchor;
    }
    else
    {
        droppedAnchorCount.fetch_add (1, std::memory_order_relaxed);
    }

    const auto fitsEveryFifo = std::ranges::all_of (fifos, [numSamples] (const auto& input) { return input->fifo.getFreeSpace() >= numSamples; });
    if (fitsEveryFifo)
    {
        for (std::size_t i = 0; i < fifos.size(); ++i)
            writeInput (*fifos[i], recordInputs[i], deviceInputs, numDeviceInputs, numSamples);
    }
    else
    {
        overrunCount.fetch_add ((std::uint64_t) numSamples, std::memory_order_relaxed);
        pushGap (numSamples);
    }

    streamFrames.fetch_add (numSamples, std::memory_order_release);
}

void RecordSession::end (RecordStopReason reason) noexcept
{
    reasonForStop.store (reason, std::memory_order_relaxed);
    currentState.store (RecordState::stopped, std::memory_order_release);
}

void RecordSession::cancel() noexcept
{
    end (RecordStopReason::cancelled);
}

void RecordSession::writeInput (InputFifo& input, const RecordInput& spec, const float* const* deviceInputs, int numDeviceInputs, int numSamples) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    input.fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    for (std::size_t c = 0; c < spec.inputChannels.size(); ++c)
    {
        const auto deviceChannel = spec.inputChannels[c];
        const float* source = deviceInputs != nullptr && deviceChannel >= 0 && deviceChannel < numDeviceInputs ? deviceInputs[deviceChannel] : nullptr;
        auto* destination = input.storage.getWritePointer ((int) c);

        if (source == nullptr)
        {
            std::fill_n (destination + start1, size1, 0.0f);
            std::fill_n (destination + start2, size2, 0.0f);
            continue;
        }
        std::memcpy (destination + start1, source, sizeof (float) * (std::size_t) size1);
        std::memcpy (destination + start2, source + size1, sizeof (float) * (std::size_t) size2);
    }

    input.fifo.finishedWrite (size1 + size2);
}

void RecordSession::pushGap (SampleCount frames) noexcept
{
    if (gapFifo.getFreeSpace() < 1)
    {
        // The frames are lost without a gap: every later frame of the Take lands early.
        lostGapCount.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    const auto scope = gapFifo.write (1);
    gapStorage[(std::size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = Gap { streamFrames.load (std::memory_order_relaxed), frames };
}

//==============================================================================
// Reader thread

SampleCount RecordSession::drain (const ChunkWriter& write)
{
    // Ready frames first, then gaps: a gap is pushed before any frame after it, so every gap
    // inside the ready frames is already visible below.
    int ready = std::numeric_limits<int>::max();
    for (const auto& input : fifos)
        ready = std::min (ready, input->fifo.getNumReady());
    if (fifos.empty())
        ready = 0;

    SampleCount handedOut = 0;
    while (true)
    {
        SampleCount limit = std::numeric_limits<SampleCount>::max();
        if (gapFifo.getNumReady() > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            gapFifo.prepareToRead (1, start1, size1, start2, size2);
            const auto gap = gapStorage[(std::size_t) (size1 > 0 ? start1 : start2)];
            if (gap.streamFrame == framesHandedOut)
            {
                handOutZeros (gap.frames, write);
                gapFifo.finishedRead (1);
                framesHandedOut += gap.frames;
                handedOut += gap.frames;
                continue;
            }
            limit = gap.streamFrame - framesHandedOut;
        }

        const auto frames = (int) std::min<SampleCount> (ready, limit);
        if (frames <= 0)
            break;

        for (std::size_t i = 0; i < fifos.size(); ++i)
            handOut (i, frames, write);
        ready -= frames;
        framesHandedOut += frames;
        handedOut += frames;
    }
    return handedOut;
}

void RecordSession::drainAnchors (std::vector<Anchor>& into)
{
    const auto ready = anchorFifo.getNumReady();
    const auto scope = anchorFifo.read (ready);
    for (int i = 0; i < scope.blockSize1; ++i)
        into.push_back (anchorStorage[(std::size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        into.push_back (anchorStorage[(std::size_t) (scope.startIndex2 + i)]);
}

void RecordSession::handOut (std::size_t inputIndex, int frames, const ChunkWriter& write)
{
    auto& input = *fifos[inputIndex];
    const auto channels = (int) recordInputs[inputIndex].inputChannels.size();
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    input.fifo.prepareToRead (frames, start1, size1, start2, size2);

    if (size1 > 0)
        write (inputIndex, channelPointers (input.storage, channels, start1).data(), size1);
    if (size2 > 0)
        write (inputIndex, channelPointers (input.storage, channels, start2).data(), size2);
    input.fifo.finishedRead (size1 + size2);
}

void RecordSession::handOutZeros (SampleCount frames, const ChunkWriter& write)
{
    const std::array<const float*, 2> silence { zeros.getReadPointer (0), zeros.getReadPointer (1) };
    for (auto remaining = frames; remaining > 0;)
    {
        const auto chunk = (int) std::min<SampleCount> (remaining, zeroChunkFrames);
        for (std::size_t i = 0; i < fifos.size(); ++i)
            write (i, silence.data(), chunk);
        remaining -= chunk;
    }
}
} // namespace overdub::engine
