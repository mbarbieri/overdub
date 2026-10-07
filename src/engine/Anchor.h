#pragma once

#include "song/Song.h"

#include <cstdint>

namespace overdub::engine
{
using song::SampleCount;

// One per callback: the Song position of the block's first output sample, and the output host
// time of that sample as the device reported it (ADR 0003).
// Invariant (render-ahead, ADR 0003 amendment): the Track nodes render L = plan latency samples
// ahead, so after PDC output sample n is Song sample songSample + n. hostTimeNs excludes the
// device's output latency; consumers add it to get heard time.
struct Anchor
{
    SampleCount songSample = 0;
    std::uint64_t hostTimeNs = 0;

    bool operator== (const Anchor&) const = default;
};
} // namespace overdub::engine
