#include "song/Transpose.h"

#include "song/BackingTrackImport.h"

#include <catch2/catch_test_macros.hpp>

using overdub::song::BackingTrack;
using overdub::song::ClipSource;
using overdub::song::ClipSourceKind;
using overdub::song::Song;

namespace
{
Song songWithBackingTrack (int transpose)
{
    Song song = overdub::song::makeEmptySong (48000);
    song.transpose = transpose;
    song.backingTracks.push_back ({ .id = "backing-1", .originalFilename = "Band.wav", .originalExtension = "wav", .originalSampleRate = 48000, .channels = 1, .formatName = "WAV", .lengthSamples = 4800 });
    return song;
}

const juce::File packageRoot ("/tmp/Song.overdub");
} // namespace

TEST_CASE ("setTranspose takes a whole number of semitones from -12 to +12", "[song][transpose]")
{
    const auto song = overdub::song::makeEmptySong (48000);

    for (const int semitones : { -12, -1, 0, 7, 12 })
    {
        const auto changed = overdub::song::setTranspose (song, semitones);
        REQUIRE (changed.has_value());
        CHECK (changed->transpose == semitones);
    }
    for (const int semitones : { -13, 13, 100 })
    {
        const auto refused = overdub::song::setTranspose (song, semitones);
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.transpose_out_of_range");
        CHECK (refused.error().fields.at ("semitones") == semitones);
        CHECK (refused.error().fields.at ("min") == -12);
        CHECK (refused.error().fields.at ("max") == 12);
    }
}

TEST_CASE ("a Transpose undo step is named with the new value and a true minus sign", "[song][transpose]")
{
    CHECK (overdub::song::transposeLabel (-1) == "Transpose \xe2\x88\x92"
                                                 "1");
    CHECK (overdub::song::transposeLabel (3) == "Transpose +3");
    CHECK (overdub::song::transposeLabel (0) == "Transpose 0");
}

TEST_CASE ("a Backing Track plays its derived file at Transpose 0 and its render otherwise", "[song][transpose]")
{
    const ClipSource backing { ClipSourceKind::backing, "backing-1" };

    CHECK (overdub::song::playedFileFor (packageRoot, songWithBackingTrack (0), backing) == packageRoot.getChildFile ("backing/backing-1.wav"));
    CHECK (overdub::song::playedFileFor (packageRoot, songWithBackingTrack (3), backing) == packageRoot.getChildFile ("backing/backing-1.transpose+3.wav"));
    CHECK (overdub::song::playedFileFor (packageRoot, songWithBackingTrack (-12), backing) == packageRoot.getChildFile ("backing/backing-1.transpose-12.wav"));
    // The peaks and every other reader of the derived audio are not transposed.
    CHECK (overdub::song::mediaFileFor (packageRoot, songWithBackingTrack (3), backing) == packageRoot.getChildFile ("backing/backing-1.wav"));
}

TEST_CASE ("Takes are never transposed", "[song][transpose]")
{
    auto song = songWithBackingTrack (5);
    song.takes.push_back ({ .id = "take-1", .trackId = "track-1", .passId = "pass-1", .file = "audio/take-1.wav", .channels = 1, .lengthSamples = 10 });

    CHECK (overdub::song::playedFileFor (packageRoot, song, { ClipSourceKind::take, "take-1" }) == packageRoot.getChildFile ("audio/take-1.wav"));
    CHECK_FALSE (overdub::song::playedFileFor (packageRoot, song, { ClipSourceKind::take, "unknown" }).has_value());
}
