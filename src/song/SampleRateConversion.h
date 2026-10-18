#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

// Converting a Song to another sample rate (issue #160, ADR 0004 amendment): offline, as one undo
// step "Convert to <rate> Hz". The Song's rate is fixed during playback and recording; this is the
// only way it changes.
//
// Every Take and Backing Track gets new media under a new id, so undo and redo only swap the Song:
// the old files stay in the package (media is never deleted by undo) and every cache keyed by media
// id (peaks, the UI's) stays true. A Take's WAV is resampled; a Backing Track is derived again from
// its kept original (its derived WAV when the original is gone), so it is never resampled twice. A
// Take or Backing Track whose media is missing keeps its id and stays missing. Conversion uses
// decodeAudioFile (ExtAudioFile, maximum quality), like import.
namespace overdub::song
{
class SongPackage;

// `samples` at `fromRate`, at `toRate`: llround (samples * toRate / fromRate).
SampleCount convertedSamples (SampleCount samples, int fromRate, int toRate);

// The new media of a conversion, by the id of the Take or Backing Track it replaces.
struct ConvertedMedia
{
    std::map<Id, Take> takes;
    std::map<Id, BackingTrack> backingTracks;

    int files() const { return (int) (takes.size() + backingTracks.size()); }
};

// The undo step label: "Convert to 48000 Hz".
std::string conversionStepLabel (int toRate);

// Pure: `song` at `toRate`, its Takes and Backing Tracks replaced by `media` (missing ones keep
// their id). Every sample position and length is converted; a span's start and end are converted
// separately, so Clips that touch still touch. A Clip stays inside its media and its fades inside
// its length. A Video Take's drift ratio is scaled with its start.
Song convertedSong (const Song& song, int toRate, const ConvertedMedia& media);

// How far convertMediaFiles has got, read from another thread.
struct ConversionProgress
{
    std::atomic<int> filesDone { 0 };
    std::atomic<int> files { 0 };
};

// Writes the new media of every Take and Backing Track of `song` at `toRate` under `packageRoot`:
// the WAV, the peaks and, for a Backing Track, the copy of its original. Safe on a worker thread:
// it reads `song` and the package's media and writes only new files. On an Error or a stop request
// every file it wrote is deleted. Errors: song.conversion_failed (path, media_id, step, cause: the
// decode or write Error's code), song.conversion_cancelled.
core::Expected<ConvertedMedia> convertMediaFiles (const juce::File& packageRoot, const Song& song, int toRate, ConversionProgress& progress, std::stop_token stop);

// Deletes the files convertMediaFiles wrote.
void deleteConvertedMedia (const juce::File& packageRoot, const ConvertedMedia& media);

// One conversion, driven from a timer like a Separation: start() checks the rate and starts
// convertMediaFiles on a worker thread with a copy of the Song; advance() (message thread), once it
// is done, applies it as one undo step (SongPackage::applySampleRateConversion). Logs
// song.sample_rate_conversion_started (info: from_rate, to_rate, files) and
// song.sample_rate_converted (info: from_rate, to_rate, files, duration_ms). cancel() stops the
// worker after the file it is on, deletes what it wrote and logs song.sample_rate_conversion_cancelled
// (info: from_rate, to_rate, files_done, files, duration_ms). Destroying a running run cancels it.
// The Song must not change while it runs (the caller locks it). The package must outlive the run.
class SampleRateConversionRun
{
public:
    using Clock = std::chrono::steady_clock;

    // Errors: song.sample_rate_unchanged (sample_rate: the Song is at it already),
    // song.sample_rate_invalid (sample_rate: not positive).
    static core::Expected<std::unique_ptr<SampleRateConversionRun>> start (SongPackage& package, int toRate, Clock::time_point now);

    ~SampleRateConversionRun();

    SampleRateConversionRun (const SampleRateConversionRun&) = delete;
    SampleRateConversionRun& operator= (const SampleRateConversionRun&) = delete;

    int fromRate() const { return from; }
    int toRate() const { return to; }
    // 0..1: the files done out of all (1 for a Song without media).
    double fraction() const;

    // Once the worker is done, applies the conversion and returns its result (convertMediaFiles'
    // and applySampleRateConversion's Errors); none while it runs. The run is over once it returns.
    std::optional<core::Expected<void>> advance (Clock::time_point now);

    // Stops the worker, waits for it and deletes the files it wrote; the Song is unchanged.
    // Nothing once the run is over.
    void cancel (Clock::time_point now);

private:
    SampleRateConversionRun (SongPackage& package, int toRate, Clock::time_point startedAt);

    core::Fields fields() const;

    SongPackage& package;
    int from;
    int to;
    Clock::time_point startedAt;
    bool over = false;
    ConversionProgress progress;
    std::optional<core::Expected<ConvertedMedia>> outcome; // written by the worker before `done`
    std::atomic<bool> done { false };
    std::jthread worker; // last: joined first
};
} // namespace overdub::song
