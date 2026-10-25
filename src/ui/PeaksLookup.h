#pragma once

#include "song/Peaks.h"
#include "song/Song.h"

#include <functional>

namespace overdub::ui
{
// The peaks of a Clip's source (no channels: draw no waveform). The reference must stay valid
// while the caller paints.
using PeaksLookup = std::function<const song::Peaks&(const song::ClipSource&)>;
} // namespace overdub::ui
