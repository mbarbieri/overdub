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

    engineRtCodeCount // not a code: the number of codes, which the catalog must match
};

// Row N describes code N.
inline constexpr auto engineRtCatalog = std::to_array<core::RtEventSpec> ({
    { "engine.transport_started", core::Level::info, {} },
    { "engine.transport_stopped", core::Level::info, {} },
    { "engine.seeked", core::Level::info, { "from_sample" } },
    { "engine.plan_swapped", core::Level::info, { "latency_samples", "tracks" } },
    { "engine.block_rejected", core::Level::error, { "num_samples", "output_channels", "max_block_size", "plan_output_channels" } },
});

static_assert (engineRtCatalog.size() == engineRtCodeCount, "every EngineRtCode needs exactly one catalog row, in code order");
} // namespace overdub::engine
