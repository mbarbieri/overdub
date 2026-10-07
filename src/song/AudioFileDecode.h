#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <string>

// Decoding an imported audio file to float at the Song's rate, and the derived 32-bit float WAV
// that playback reads (ADR 0004, issue #14 amendment). Decoding uses Core Audio's ExtAudioFile with
// the sample rate converter at maximum quality; ExtAudioFile trims AAC/MP3 encoder priming. Any file
// it opens is accepted. Mono and stereo are kept as they are; more channels are refused.
// Whole files are decoded into memory, like the engine's playback sources.
namespace overdub::song
{
struct DecodedAudio
{
    juce::AudioBuffer<float> audio; // at the requested rate, 1 or 2 channels
    int originalSampleRate = 0;
    int channels = 0;
    std::string formatName; // Core Audio's name for the file's data format ("MPEG-4 AAC", "Linear PCM", ...)
};

inline constexpr int maxImportChannels = 2;

// Errors: song.import_unreadable (path, step, os_status: what ExtAudioFile could not do),
// song.import_too_many_channels (path, channels).
core::Expected<DecodedAudio> decodeAudioFile (const juce::File& file, int sampleRate);

// Writes `audio` as a 32-bit float WAV at `sampleRate` to `<destination>.tmp`, then renames it.
// Errors: song.write_failed (path, step open|write|fsync|rename, errno), or the hook's Error.
core::Expected<void> writeFloatWavAtomically (const juce::File& destination,
                                              const juce::AudioBuffer<float>& audio,
                                              int sampleRate,
                                              const BeforeRenameHook& beforeRename);

// Reads a WAV (any sample format) into float. Errors: song.read_failed (path).
core::Expected<juce::AudioBuffer<float>> readWavFile (const juce::File& file);
} // namespace overdub::song
