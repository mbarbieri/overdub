#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <string>

// The Mixer's Song operations (issue #31) as pure functions: a Track's volume, pan, mute and solo,
// and the Master volume. Each returns the changed Song; the caller makes it one undo step with its
// label (SongPackage::edit). What they sound like is engine/MixLaw.h.
// Errors: song.track_not_found (track_id).
namespace overdub::song
{
// The faders' range, for Tracks and the Master. The bottom is a real gain (-60 dB), not silence:
// mute silences.
inline constexpr double minVolumeDb = -60.0;
inline constexpr double maxVolumeDb = 12.0;

inline constexpr const char* masterVolumeLabel = "Master Volume";

// Volume clamped to [minVolumeDb, maxVolumeDb].
core::Expected<Song> setTrackVolume (Song song, const Id& trackId, double volumeDb);
// Pan clamped to [-1, 1] (hard left .. hard right).
core::Expected<Song> setTrackPan (Song song, const Id& trackId, double pan);
core::Expected<Song> setTrackMute (Song song, const Id& trackId, bool mute);
core::Expected<Song> setTrackSolo (Song song, const Id& trackId, bool solo);
// Volume clamped to [minVolumeDb, maxVolumeDb].
Song setMasterVolume (Song song, double volumeDb);

// Undo labels: "Guitar Volume", "Guitar Pan", "Mute Guitar" / "Unmute Guitar",
// "Solo Guitar" / "Unsolo Guitar". An unknown Track is named "Track".
std::string volumeLabel (const Song& song, const Id& trackId);
std::string panLabel (const Song& song, const Id& trackId);
std::string muteLabel (const Song& song, const Id& trackId, bool mute);
std::string soloLabel (const Song& song, const Id& trackId, bool solo);

// True when the two Songs are equal except for Track volumes, pans, mutes and solos, the Master
// volume, and the Transcribe Mix and Focus Track (song/TranscribeMix.h): the engine can then apply the mix without rebuilding its plan (Engine::setMix).
bool differsOnlyInMix (const Song& a, const Song& b);
} // namespace overdub::song
