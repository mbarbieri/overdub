#pragma once

#include "core/Error.h"
#include "song/TrackColours.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The v1 Song model: plain value structs (ADR 0004, issues #8, #14, #15, #23).
// Every position and length is an integer sample count at the Song's sample rate.
// Every entity has a permanent random id (lowercase dashed UUID); media files are named by it.
// Serialization lives in song/SongJson.h; the package on disk in song/SongPackage.h.
// Equality is exact, floating-point fields included: it compares stored values (round trips,
// snapshots), never computed ones.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"

namespace overdub::song
{
using Id = std::string;
using SampleCount = std::int64_t;

// The only format version this build reads and writes. Older documents are migrated
// (song/SongMigration.h); newer ones are refused.
inline constexpr int songFormatVersion = 5;

// Pre-roll is a Song setting, persisted but not undoable (ADR 0004, issue #15 amendment).
// Stored in song.json as whole seconds: 0, 1, 2 or 4.
enum class PreRoll
{
    off,
    oneSecond,
    twoSeconds,
    fourSeconds
};

// The export's format and an MP4's length: Song settings outside history (issue #201), what the
// Export sheet opens with. Exporting is export/ExportFormat.h, export/MovieSpan.h.
enum class ExportFormat
{
    wav, // 24-bit integer PCM at the Song's rate
    m4a, // AAC at 256 kbit/s
    mp4  // the mix and one Video Take (H.264 + AAC), export/MovieExport.h
};

// The MP4's length: trimmed to the chosen Video Take, or the full song with black where there is
// no video.
enum class VideoLength
{
    trimToVideoTake,
    fullSong
};

// Their names in song.json and on the control socket.
inline constexpr std::pair<ExportFormat, std::string_view> exportFormatNames[] {
    { ExportFormat::wav, "wav" },
    { ExportFormat::m4a, "m4a" },
    { ExportFormat::mp4, "mp4" },
};
inline constexpr std::pair<VideoLength, std::string_view> videoLengthNames[] {
    { VideoLength::trimToVideoTake, "trim_to_video_take" },
    { VideoLength::fullSong, "full_song" },
};

// The AudioComponentDescription that identifies an AU, each part a four-character code ("aufx").
struct AudioUnitComponent
{
    std::string type;
    std::string subtype;
    std::string manufacturer;

    bool operator== (const AudioUnitComponent&) const = default;
};

// One plugin in an Insert Chain. A missing plugin stays an inert placeholder; its state is kept.
struct InsertSlot
{
    Id id;
    AudioUnitComponent component;
    std::string name;
    std::string version; // as the plugin reports it (JUCE PluginDescription::version)
    bool bypassed = false;
    std::string stateHash; // lowercase hex SHA-256 of plugins/<stateHash>.state; empty = no state

    bool operator== (const InsertSlot&) const = default;
};

enum class ClipSourceKind
{
    take,
    backing
};

// "take" or "backing": the name in song.json and in event and Error fields (source_kind).
std::string_view toString (ClipSourceKind kind);

struct ClipSource
{
    ClipSourceKind kind = ClipSourceKind::take;
    Id id; // a Take id or a Backing Track id

    bool operator== (const ClipSource&) const = default;
};

// A window onto a Take or Backing Track. The one fade curve in v1 is linear in amplitude (issue #29,
// engine/ClipSchedule.h) and is not stored. Fade-in + fade-out never exceed the length.
struct Clip
{
    Id id;
    ClipSource source;
    SampleCount timelineStart = 0;
    SampleCount sourceOffset = 0;
    SampleCount length = 0;
    double gainDb = 0.0;
    SampleCount fadeInLength = 0;
    SampleCount fadeOutLength = 0;

    bool operator== (const Clip&) const = default;
};

struct Track
{
    Id id;
    std::string name;
    std::string colour { trackColourPalette[0] }; // "#rrggbb" (song/TrackColours.h); format 4
    double volumeDb = 0.0;
    double pan = 0.0; // -1 (left) .. +1 (right), 0 = centre
    bool mute = false;
    bool solo = false;
    bool armed = false;              // persisted, not undoable
    std::vector<int> inputChannels;  // 0-based device input channel indices, 1 (mono) or 2 (stereo); empty = none. Not undoable.
    std::vector<InsertSlot> insertChain;
    std::vector<Clip> clips; // layer order: the last Clip is on top

    bool operator== (const Track&) const = default;
};

struct Master
{
    double volumeDb = 0.0;
    std::vector<InsertSlot> insertChain;

    bool operator== (const Master&) const = default;
};

// One press of record through to stop.
struct Pass
{
    Id id;
    SampleCount startSample = 0;
    std::string recordedAt; // wall-clock start, ISO-8601 UTC ("2026-10-07T18:30:00Z")

    bool operator== (const Pass&) const = default;
};

// The latency compensation applied when a Take was placed. Kept for diagnosis only.
struct TakeCompensation
{
    SampleCount reportedLatencySamples = 0;   // what the device reported
    SampleCount calibrationOffsetSamples = 0; // the per-machine loopback calibration (ADR 0002)

    bool operator== (const TakeCompensation&) const = default;
};

struct Take
{
    Id id;
    Id trackId; // the Track it was recorded on; may dangle after that Track is deleted
    Id passId;
    std::string file; // package-relative: audio/<id>.wav
    int channels = 1; // 1 or 2
    SampleCount lengthSamples = 0;
    SampleCount origin = 0; // the compensated Song sample its first sample belongs at
    TakeCompensation compensation;

    bool operator== (const Take&) const = default;
};

// Where a Backing Track made by a Separation came from (issue #114, ADR 0007): the Backing Track
// it was separated from and the model's name. Empty for an imported Backing Track. Only a record:
// nothing behaves differently because of it.
struct SeparationOrigin
{
    Id backingTrackId; // may dangle once that Backing Track is gone
    std::string model; // the model file's name without ".onnx", e.g. "bs_roformer_sw.fp32"

    bool isEmpty() const { return backingTrackId.empty(); }
    bool operator== (const SeparationOrigin&) const = default;
};

// Fields per the issue #14 amendment of ADR 0004. Files: backing/<id>.orig.<originalExtension>
// (untouched original) and backing/<id>.wav (derived, Song rate). No content hash.
struct BackingTrack
{
    Id id;
    std::string originalFilename;
    std::string originalExtension; // without the dot
    int originalSampleRate = 0;
    int channels = 1; // 1 or 2
    std::string formatName;
    SampleCount lengthSamples = 0; // at the Song's rate
    SeparationOrigin separatedFrom; // empty unless a Separation made it (format 3)

    bool operator== (const BackingTrack&) const = default;
};

// Camera footage of a Pass, placed on the audio clock (ADR 0003). Not movable or trimmable in v1.
struct VideoTake
{
    Id id;
    Id passId;
    std::string file; // package-relative: video/<id>.mov
    SampleCount startSample = 0;
    double songSamplesPerMovieSecond = 0.0; // drift ratio: Song samples per second of movie time
    std::string cameraId;
    SampleCount calibrationOffsetSamples = 0; // the per-machine camera offset applied at placement
    // The audio output device the Pass played through (its name as JUCE lists it): with cameraId,
    // the key of the camera calibration a clap Pass measures (issue #68). "" = unknown (format 1).
    std::string outputDevice;

    bool operator== (const VideoTake&) const = default;
};

struct Song
{
    int sampleRate = 48000; // fixed from the device at creation
    std::vector<Track> tracks;
    Master master;
    std::vector<Pass> passes;
    std::vector<Take> takes;
    std::vector<BackingTrack> backingTracks;
    std::vector<VideoTake> videoTakes;

    // Persisted settings outside undo history.
    PreRoll preRoll = PreRoll::off;
    bool cameraArmed = false;
    std::string cameraDeviceId; // empty = no camera chosen
    SampleCount playhead = 0;
    // The Export sheet's choices (issue #201).
    std::optional<Id> exportVideoTakeId;
    ExportFormat exportFormat = ExportFormat::wav;
    VideoLength exportLength = VideoLength::trimToVideoTake;
    std::string exportDestination; // an absolute path; empty = <export folder>/<Song name>.<ext>

    bool operator== (const Song&) const = default;
};

// The pre-roll's length in whole seconds: 0, 1, 2 or 4.
int preRollSeconds (PreRoll preRoll);

// A new random id: a lowercase dashed UUID.
Id makeId();

// An empty Song at the given rate.
Song makeEmptySong (int sampleRate);

// The Track with this id, or null: for callers where a missing Track is not an error.
const Track* findTrack (const Song& song, const Id& trackId);
Track* findTrack (Song& song, const Id& trackId);

// The Track with this id, or the song.track_not_found Error (track_id): for edits where a missing
// Track is an error. The only place that Error is built.
core::Expected<const Track*> lookupTrack (const Song& song, const Id& trackId);
core::Expected<Track*> lookupTrack (Song& song, const Id& trackId);

// Undo and redo restore a snapshot's undoable content but keep the current state that lives
// outside history: pre-roll, camera arm and device, playhead (converted to the snapshot's rate), the
// export format, length and destination, the export's Video Take (unless the snapshot does not
// have it), per Track id Armed and input channels, and per
// Insert Chain slot id (Master's included) the plugin state hash: plugin parameters are outside
// history. A slot only the snapshot has keeps the snapshot's hash.
Song keepStateOutsideHistory (Song snapshot, const Song& current);

// The colour of every Track, in Song order: what a new Track's colour must not repeat.
std::vector<std::string> trackColours (const Song& song);

// Every non-empty plugin state hash used by the Song's Insert Chains.
std::set<std::string> pluginStateHashes (const Song& song);
} // namespace overdub::song

#pragma clang diagnostic pop
