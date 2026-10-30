#pragma once

#include "song/Song.h"

// A Song with every entity present at least twice and every field away from its default in at
// least one element, built in code (ADR 0005 amendment: no committed .overdub fixtures). Ids are
// fixed so failures read clearly. Clip ids are deliberately out of alphabetical order, so a
// reordering of layers shows up.
namespace overdub::song::testing
{
inline InsertSlot insertSlot (const char* id, const char* hash)
{
    return InsertSlot {
        .id = id,
        .component = { .type = "aufx", .subtype = "dely", .manufacturer = "appl" },
        .name = "AUDelay",
        .version = "1.7.0",
        .bypassed = true,
        .stateHash = hash,
    };
}

inline Song fullyPopulatedSong()
{
    Song song;
    song.sampleRate = 44100;

    song.passes = {
        Pass { .id = "pass-1", .startSample = 88200, .recordedAt = "2026-10-07T18:30:00Z" },
        Pass { .id = "pass-2", .startSample = 176400, .recordedAt = "2026-10-07T18:35:12Z" },
    };
    song.takes = {
        Take {
            .id = "take-1",
            .trackId = "track-guitar",
            .passId = "pass-1",
            .file = "audio/take-1.wav",
            .channels = 2,
            .lengthSamples = 441000,
            .origin = 88150,
            .compensation = { .reportedLatencySamples = 128, .calibrationOffsetSamples = -50 },
        },
        Take {
            .id = "take-2",
            .trackId = "track-guitar",
            .passId = "pass-2",
            .file = "audio/take-2.wav",
            .channels = 1,
            .lengthSamples = 220500,
            .origin = 176350,
            .compensation = { .reportedLatencySamples = 256, .calibrationOffsetSamples = 12 },
        },
    };
    song.backingTracks = {
        BackingTrack {
            .id = "backing-1",
            .originalFilename = "Band Mix.mp3",
            .originalExtension = "mp3",
            .originalSampleRate = 48000,
            .channels = 2,
            .formatName = "MPEG Layer 3",
            .lengthSamples = 7938000,
        },
        BackingTrack {
            .id = "backing-2",
            .originalFilename = "Click.wav",
            .originalExtension = "wav",
            .originalSampleRate = 44100,
            .channels = 1,
            .formatName = "WAV",
            .lengthSamples = 4410000,
            .separatedFrom = { .backingTrackId = "backing-1", .model = "bs_roformer_sw.fp32" },
        },
    };
    song.videoTakes = {
        VideoTake {
            .id = "video-1",
            .passId = "pass-1",
            .file = "video/video-1.mov",
            .startSample = 88200,
            .songSamplesPerMovieSecond = 44100.25,
            .cameraId = "camera-facetime",
            .calibrationOffsetSamples = 1764,
            .outputDevice = "MacBook Pro Speakers",
        },
        VideoTake {
            .id = "video-2",
            .passId = "pass-2",
            .file = "video/video-2.mov",
            .startSample = 176400,
            .songSamplesPerMovieSecond = 44099.875,
            .cameraId = "camera-usb",
            .calibrationOffsetSamples = -441,
            .outputDevice = "Scarlett 2i2 USB",
        },
    };

    song.tracks = {
        Track {
            .id = "track-guitar",
            .name = "Guitar",
            .colour = "#c0ffee", // not a palette colour: the Song keeps the value, not an index
            .volumeDb = -3.5,
            .pan = -0.25,
            .mute = true,
            .solo = true,
            .settings = {
                .armed = true,
                .transcribeMute = true,
                .octaveShift = 2,
                .bandFilter = { .on = true, .lowHz = 250, .highHz = 2500 },
                .inputChannels = { 2, 3 },
            },
            .insertChain = { insertSlot ("slot-1", "aaaa"), insertSlot ("slot-2", "abab") },
            .clips = {
                // Layer order: clip-b is below clip-a.
                Clip {
                    .id = "clip-b",
                    .source = { .kind = ClipSourceKind::take, .id = "take-1" },
                    .timelineStart = 88200,
                    .sourceOffset = 50,
                    .length = 440000,
                    .gainDb = 1.5,
                    .fadeInLength = 441,
                    .fadeOutLength = 882,
                },
                Clip {
                    .id = "clip-a",
                    .source = { .kind = ClipSourceKind::take, .id = "take-2" },
                    .timelineStart = 176400,
                    .sourceOffset = 50,
                    .length = 220000,
                    .gainDb = -2.0,
                    .fadeInLength = 100,
                    .fadeOutLength = 200,
                },
            },
        },
        Track {
            .id = "track-backing",
            .name = "Band Mix",
            .colour = "#f5e19c",
            .volumeDb = -6.0,
            .pan = 0.5,
            .mute = false,
            .solo = false,
            .settings = {
                .armed = false,
                .transcribeSolo = true,
                .inputChannels = { 0 },
            },
            .insertChain = { insertSlot ("slot-3", "acac"), insertSlot ("slot-4", "adad") },
            .clips = {
                Clip {
                    .id = "clip-d",
                    .source = { .kind = ClipSourceKind::backing, .id = "backing-1" },
                    .length = 7938000,
                },
                Clip {
                    .id = "clip-c",
                    .source = { .kind = ClipSourceKind::backing, .id = "backing-2" },
                    .timelineStart = 44100,
                    .length = 4410000,
                },
            },
        },
    };
    song.master = Master { .volumeDb = -1.0, .insertChain = { insertSlot ("slot-master", "bbbb"), insertSlot ("slot-master-2", "bcbc") } };

    song.settings.preRoll = PreRoll::twoSeconds;
    song.settings.cameraArmed = true;
    song.settings.cameraDeviceId = "camera-facetime";
    song.settings.playhead = 132300;
    song.settings.exportVideoTakeId = "video-1";
    song.settings.exportFormat = ExportFormat::mp4;
    song.settings.exportLength = VideoLength::fullSong;
    song.settings.exportDestination = "/Users/someone/Music/Mixes/Band Mix.mp4";
    song.transpose = { .semitones = -3, .cents = 25 };
    song.markers = { Marker { .id = "marker-1", .name = "Verse", .position = 48000 }, Marker { .id = "marker-2", .name = "Chorus", .position = 96000 } };
    song.loops = { Loop { .id = "loop-1", .name = "Intro riff", .start = 24000, .end = 72000, .settings = { .practiceSpeedPercent = 40 } }, Loop { .id = "loop-2", .name = "Solo", .start = 96000, .end = 144000, .settings = { .practiceSpeedPercent = 75 } } };
    song.settings.focusTrackId = "track-backing";
    song.settings.activeLoopId = "loop-2";
    song.settings.looping = true;
    song.settings.exportLoopId = "loop-1";
    return song;
}
} // namespace overdub::song::testing
