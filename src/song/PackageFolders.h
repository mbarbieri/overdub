#pragma once

// The media folder names of the `.overdub` package (ADR 0004), each defined once. The layout
// itself is described in song/SongPackage.h.
namespace overdub::song
{
inline constexpr const char* audioFolderName = "audio";     // Takes
inline constexpr const char* backingFolderName = "backing"; // Backing Track originals and derived WAVs
inline constexpr const char* videoFolderName = "video";     // Video Takes
inline constexpr const char* pluginsFolderName = "plugins"; // plugin state blobs
inline constexpr const char* peaksFolderName = "peaks";     // the peaks cache, created on first write
} // namespace overdub::song
