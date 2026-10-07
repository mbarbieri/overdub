#pragma once

#include "song/Song.h"

// A Song with every entity present and every field away from its default, built in code
// (ADR 0005 amendment: no committed .overdub fixtures). Ids are fixed so failures read clearly.
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
        },
    };

    song.tracks = {
        Track {
            .id = "track-guitar",
            .name = "Guitar",
            .volumeDb = -3.5,
            .pan = -0.25,
            .mute = true,
            .solo = true,
            .armed = true,
            .inputChannels = { 2, 3 },
            .insertChain = { insertSlot ("slot-1", "aaaa") },
            .clips = {
                Clip {
                    .id = "clip-1",
                    .source = { .kind = ClipSourceKind::take, .id = "take-1" },
                    .timelineStart = 88200,
                    .sourceOffset = 50,
                    .length = 440000,
                    .gainDb = 1.5,
                    .fadeInLength = 441,
                    .fadeOutLength = 882,
                },
            },
        },
        Track {
            .id = "track-backing",
            .name = "Band Mix",
            .clips = {
                Clip {
                    .id = "clip-2",
                    .source = { .kind = ClipSourceKind::backing, .id = "backing-1" },
                    .length = 7938000,
                },
            },
        },
    };
    song.master = Master { .volumeDb = -1.0, .insertChain = { insertSlot ("slot-master", "bbbb") } };

    song.preRoll = PreRoll::twoSeconds;
    song.cameraArmed = true;
    song.cameraDeviceId = "camera-facetime";
    song.playhead = 132300;
    song.exportVideoTakeId = "video-1";
    return song;
}
} // namespace overdub::song::testing
