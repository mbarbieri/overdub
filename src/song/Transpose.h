#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

// Transpose (ADR 0009, issues #209, #232): the Song's lasting pitch shift of every Backing Track in
// whole semitones plus a fine tune in whole cents, one undo step per change. Takes are never
// transposed.
//
// Each Backing Track at a Transpose other than 0 plays a render of its derived WAV, made in one
// pass from it with semitones and cents together (song/TransposeRender.h):
//   backing/<id>.transpose+3.wav       32-bit float at the Song's rate, as long as the derived WAV
//   backing/<id>.transpose-2-30c.wav   at −2 semitones −30 cents (transposeFileTag)
// The package keeps only the renders of one Transpose; the others are deleted
// (deleteTransposeRendersExcept). The peaks cache stays the derived WAV's.
namespace overdub::song
{
inline constexpr int minTranspose = -12;
inline constexpr int maxTranspose = 12;
inline constexpr int minTransposeCents = -50;
inline constexpr int maxTransposeCents = 50;

// The Song at this Transpose. Errors: song.transpose_out_of_range (semitones, min, max),
// song.transpose_cents_out_of_range (cents, min, max).
core::Expected<Song> setTranspose (Song song, Transpose transpose);

// The pitch shift in semitones as the stretcher takes it: semitones + cents / 100.
double transposeSemitones (Transpose transpose);

// A whole number of semitones or cents as the user reads it: "+3", "0", "−1" (U+2212 minus).
std::string signedText (int value);

// A Transpose as the user reads it: "+3", "0", "−1", and with cents "+3 +20 ct", "0 −50 ct".
std::string transposeText (Transpose transpose);

// The undo step label: "Transpose +3", "Transpose 0", "Transpose −1 +20 ct".
std::string transposeLabel (Transpose transpose);

// A Transpose in a render's file name: "+3", "-2", "+0", and with cents "+3+20c", "-2-30c".
std::string transposeFileTag (Transpose transpose);

// The file a Backing Track plays at `transpose`: its derived WAV at 0, its render otherwise.
juce::File transposedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack, Transpose transpose);

// The media file a Clip source plays in `song`: a Take's file, or a Backing Track's
// transposedFileFor at the Song's Transpose. Empty when `song` has no such Take or Backing Track.
// The file itself may be missing.
std::optional<juce::File> playedFileFor (const juce::File& packageRoot, const Song& song, const ClipSource& source);

// One render to make: a Backing Track's derived WAV into its render at some Transpose.
struct TransposeRenderJob
{
    Id backingTrackId;
    juce::File source;      // backing/<id>.wav
    juce::File destination; // backing/<id>.transpose<tag>.wav
};

// The renders `song` lacks at its Transpose, in Backing Track order: one per Backing Track whose
// derived WAV is there and whose render is not. None at Transpose 0 (and 0 cents).
std::vector<TransposeRenderJob> missingTransposeRenders (const juce::File& packageRoot, const Song& song);

// Deletes every render in backing/ but those at `kept` (all of them at 0), whichever Backing Track
// it belongs to. Returns how many it deleted.
int deleteTransposeRendersExcept (const juce::File& packageRoot, Transpose kept);

// Deletes every render in backing/ of a Backing Track `song` does not have (one an undo removed),
// at any Transpose. Returns how many it deleted.
int deleteTransposeRendersOfRemovedBackingTracks (const juce::File& packageRoot, const Song& song);
} // namespace overdub::song
