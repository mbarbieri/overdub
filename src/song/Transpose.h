#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

// Transpose (ADR 0009, issue #209): the Song's lasting pitch shift of every Backing Track in whole
// semitones, one undo step per change. Takes are never transposed.
//
// Each Backing Track at a Transpose other than 0 plays a render of its derived WAV, made in one
// pass from it (song/TransposeRender.h):
//   backing/<id>.transpose+3.wav   32-bit float at the Song's rate, as long as the derived WAV
// The package keeps only the renders of one Transpose; the others are deleted
// (deleteTransposeRendersExcept). The peaks cache stays the derived WAV's.
namespace overdub::song
{
inline constexpr int minTranspose = -12;
inline constexpr int maxTranspose = 12;

// The Song at this Transpose. Errors: song.transpose_out_of_range (semitones, min, max).
core::Expected<Song> setTranspose (Song song, int semitones);

// A Transpose as the user reads it: "+3", "0", "−1" (U+2212 minus).
std::string transposeText (int semitones);

// The undo step label: "Transpose +3", "Transpose 0", "Transpose −1".
std::string transposeLabel (int semitones);

// The file a Backing Track plays at `semitones`: its derived WAV at 0, its render otherwise.
juce::File transposedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack, int semitones);

// The media file a Clip source plays in `song`: a Take's file, or a Backing Track's
// transposedFileFor at the Song's Transpose. Empty when `song` has no such Take or Backing Track.
// The file itself may be missing.
std::optional<juce::File> playedFileFor (const juce::File& packageRoot, const Song& song, const ClipSource& source);

// One render to make: a Backing Track's derived WAV into its render at some Transpose.
struct TransposeRenderJob
{
    Id backingTrackId;
    juce::File source;      // backing/<id>.wav
    juce::File destination; // backing/<id>.transpose<±n>.wav
};

// The renders `song` lacks at its Transpose, in Backing Track order: one per Backing Track whose
// derived WAV is there and whose render is not. None at Transpose 0.
std::vector<TransposeRenderJob> missingTransposeRenders (const juce::File& packageRoot, const Song& song);

// Deletes every render in backing/ but those at `keptSemitones` (all of them at 0), whichever
// Backing Track it belongs to. Returns how many it deleted.
int deleteTransposeRendersExcept (const juce::File& packageRoot, int keptSemitones);
} // namespace overdub::song
