#pragma once

#include "core/RtEventRing.h"

#include <array>
#include <cstdint>

// The engine's RT event codes and their name table (ADR 0006). The audio thread pushes
// {code, sample, hostTimeNs, args}; a drain thread logs them with these names. Every event's
// `sample` is the song position of the block's first sample when it was captured.
namespace overdub::engine
{
enum EngineRtCode : std::uint16_t
{
    transportStarted = 0, // play took effect at the start of this block
    transportStopped = 1, // stop took effect at the start of this block
    seeked = 2,           // the block starts at the seek target; args: from_sample
    planSwapped = 3,      // a new render plan plays from this block; args: latency_samples, tracks
    blockRejected = 4,    // blocks of this shape do not fit the plan and are silenced, from this block on (pushed on change only); args: num_samples, output_channels, max_block_size, plan_output_channels
    nodesSilenced = 5,    // a node rendered silence because another thread held its callback lock; pushed when a busy streak starts (every block is counted in engine.load's silenced_node_blocks); args: node_blocks
    recordingStarted = 6, // a Pass's capture starts with this block (its first input sample is this block's Song sample); args: inputs, plan_latency_samples
    recordingStopped = 7, // a Pass's capture ended before this block; args: frames, overrun_frames, reason (RecordStopReason: 1 requested, 2 transport stopped, 3 seeked; a stopped device ends a Pass off the audio thread, logged as engine.device_stopped)

    engineRtCodeCount // not a code: the number of codes, which the catalog must match
};

// Row N describes code N.
inline constexpr auto engineRtCatalog = std::to_array<core::RtEventSpec> ({
    { "engine.transport_started", core::Level::info, {} },
    { "engine.transport_stopped", core::Level::info, {} },
    { "engine.seeked", core::Level::info, { "from_sample" } },
    { "engine.plan_swapped", core::Level::info, { "latency_samples", "tracks" } },
    { "engine.block_rejected", core::Level::error, { "num_samples", "output_channels", "max_block_size", "plan_output_channels" } },
    { "engine.nodes_silenced", core::Level::warn, { "node_blocks" } },
    { "engine.recording_started", core::Level::info, { "inputs", "plan_latency_samples" } },
    { "engine.recording_stopped", core::Level::info, { "frames", "overrun_frames", "reason" } },
});

static_assert (engineRtCatalog.size() == engineRtCodeCount, "every EngineRtCode needs exactly one catalog row, in code order");
} // namespace overdub::engine
