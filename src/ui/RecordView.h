#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/PeaksLookup.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Record mode's look-ahead strip and whole-Song overview (issue #32, ADR 0004 amendments for
// issues #14 and #15), pure and without JUCE: the strip's 30 s window with its fixed playhead at
// 20%, the combined envelope of the audible Backing Track Clips from the peaks cache, the
// "Silence in 4.0 s" / "Music back in 3.1 s" countdown, and the overview's sample <-> x. The LookAheadStrip and SongOverview Components only call these.
namespace overdub::ui
{
using song::SampleCount;

inline constexpr int lookAheadWindowSeconds = 30;
// The playhead sits this far into the window: 6 s of the past, 24 s of what is coming.
inline constexpr double lookAheadPlayheadFraction = 0.2;
// Below this linear level (-40 dBFS) the Backing Tracks count as silent.
inline constexpr float silenceLevel = 0.01f;

// The Song samples the strip shows: [start, start + length). start is negative near the Song start.
struct LookAheadWindow
{
    SampleCount start = 0;
    SampleCount length = 0;

    bool operator== (const LookAheadWindow&) const = default;
};

// 30 s with `playhead` at 20%: start = playhead - 6 s.
LookAheadWindow lookAheadWindow (SampleCount playhead, int sampleRate);

// The x of a Song sample in a strip `width` wide (linear; outside [0, width] when off the window).
double lookAheadX (const LookAheadWindow& window, SampleCount sample, int width);

// The combined envelope: levels[i] is the loudest linear level the audible Backing Track Clips
// reach over Song samples [256 i, 256 (i + 1)) (song::samplesPerPeak), read from their peaks.
// Audible: on a Track that sounds (not muted, and soloed when any Track is), and the top Clip of
// that Track at sample 256 i. A Clip's level is its source's peak there (the largest |min| or |max|
// over its channels), times its gain and fade envelope, times its Track's volume. Tracks combine by
// their maximum. levels ends at the last Backing Track Clip's end.
struct BackingEnvelope
{
    std::vector<float> levels;

    bool operator== (const BackingEnvelope&) const = default;
};

BackingEnvelope backingEnvelope (const song::Song& song, const PeaksLookup& peaksFor);

// The envelope's level at a Song sample (0 before 0 and past the end).
float envelopeLevelAt (const BackingEnvelope& envelope, SampleCount sample);

// The strip's columns: column c of `columns` holds the loudest level over its share of the window.
std::vector<float> lookAheadColumns (const BackingEnvelope& envelope, const LookAheadWindow& window, int columns);

// What the Backing Tracks do next, from the playhead to the window's end (24 s ahead).
struct MusicCountdown
{
    bool soundingNow = false;
    std::optional<SampleCount> changeIn; // samples until it turns silent (or sounding); none in reach

    bool operator== (const MusicCountdown&) const = default;
};

MusicCountdown musicCountdown (const BackingEnvelope& envelope, SampleCount playhead, int sampleRate);

// "Silence in 4.0 s" / "Music back in 3.1 s" (seconds rounded down to a tenth), or without a change
// in reach "Music continues" / "Silence".
std::string musicCountdownText (const MusicCountdown& countdown, int sampleRate);

// The strip's whole picture at one moment.
struct LookAheadView
{
    LookAheadWindow window;
    std::vector<float> columns;
    std::optional<SampleCount> passStart;   // the running Pass's record point: tinted red up to the playhead
    std::optional<SampleCount> recordPoint; // during pre-roll: where the Pass will start
    std::string countdown;                  // musicCountdownText
    std::string preRollText;                // "Pass starts in 1.9 s" (ui/TransportText.h) during pre-roll, else empty

    bool operator== (const LookAheadView&) const = default;
};

struct RecordTransport
{
    SampleCount playhead = 0;
    std::optional<SampleCount> passStart;   // the running Pass's record point, once it has begun
    std::optional<SampleCount> recordPoint; // the record point while pre-roll counts down to it
};

LookAheadView lookAheadView (const BackingEnvelope& envelope, const RecordTransport& transport, int sampleRate, int columns);

// The whole-Song overview: the Song's length is its last Clip's end, the playhead's position, or
// 30 s, whichever is greatest; it spans the overview's full width.
SampleCount overviewLength (const song::Song& song, SampleCount playhead);

// The Song sample under x in an overview `width` wide (clamped to [0, length]), and back.
SampleCount overviewSampleAt (int x, int width, SampleCount length);
double overviewX (SampleCount sample, int width, SampleCount length);
} // namespace overdub::ui
