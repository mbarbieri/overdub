#pragma once

#include "song/AudioFileDecode.h"
#include "song/SongPackage.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The shared fixture Song builder (ADR 0005 UI amendment): a real `.overdub` package in a temp
// folder, built in code with generated media. Backing Tracks are sines written as WAV files and
// imported through SongPackage::importBackingTrack (one undo step "Import <file>" each); plain
// Tracks are added together as one more undo step, "Add Tracks". Nothing is committed as a file.
namespace overdub::song::testing
{
// Owns the temp folder and the package in it; the package is closed before the folder goes.
struct FixtureSong
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("overdub-fixture-song-" + juce::Uuid().toString());
    std::unique_ptr<SongPackage> package;

    FixtureSong() { folder.createDirectory(); }
    ~FixtureSong()
    {
        package.reset();
        folder.deleteRecursively();
    }

    FixtureSong (const FixtureSong&) = delete;
    FixtureSong& operator= (const FixtureSong&) = delete;

    const Song& song() const { return package->song(); }

    const Track& trackNamed (const std::string& name) const
    {
        const auto& tracks = package->song().tracks;
        const auto found = std::ranges::find (tracks, name, &Track::name);
        REQUIRE (found != tracks.end());
        return *found;
    }

    // A generated mono sine WAV beside the package (not inside it), e.g. to import.
    juce::File writeSineFile (const std::string& fileName, double seconds, double frequency) const
    {
        const auto sampleRate = package != nullptr ? package->song().sampleRate : 48000;
        juce::AudioBuffer<float> audio (1, (int) (seconds * sampleRate));
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate));
        const auto file = folder.getChildFile ("sources").getChildFile (fileName);
        REQUIRE (file.getParentDirectory().createDirectory().wasOk());
        REQUIRE (writeFloatWavAtomically (file, audio, sampleRate, {}).has_value());
        return file;
    }
};

class FixtureSongBuilder
{
public:
    FixtureSongBuilder& sampleRate (int rate)
    {
        songSampleRate = rate;
        return *this;
    }

    // A Backing Track: a mono sine of `seconds`, imported from "<name>.wav" onto its own Track.
    FixtureSongBuilder& backingTrack (const std::string& name, double seconds = 2.0, double frequency = 220.0)
    {
        backingTracks.push_back ({ name, seconds, frequency });
        return *this;
    }

    // An empty Track, Armed or not, recording these device input channels.
    FixtureSongBuilder& track (const std::string& name, bool armed = false, std::vector<int> inputChannels = {})
    {
        Track added;
        added.id = makeId();
        added.name = name;
        trackInputs[added.id] = TrackInput { .armed = armed, .inputChannels = std::move (inputChannels) };
        tracks.push_back (std::move (added));
        return *this;
    }

    std::unique_ptr<FixtureSong> build() const
    {
        auto fixture = std::make_unique<FixtureSong>();
        auto created = SongPackage::create (fixture->folder.getChildFile ("Fixture.overdub"), makeEmptySong (songSampleRate));
        REQUIRE (created.has_value());
        fixture->package = std::move (*created);

        for (const auto& backing : backingTracks)
        {
            const auto file = fixture->writeSineFile (backing.name + ".wav", backing.seconds, backing.frequency);
            REQUIRE (fixture->package->importBackingTrack (file).has_value());
        }

        if (! tracks.empty())
        {
            auto song = fixture->package->song();
            song.tracks.insert (song.tracks.end(), tracks.begin(), tracks.end());
            song.settings.trackInputs.insert (trackInputs.begin(), trackInputs.end());
            REQUIRE (fixture->package->edit (song, "Add Tracks").has_value());
        }
        return fixture;
    }

private:
    struct BackingSpec
    {
        std::string name;
        double seconds = 2.0;
        double frequency = 220.0;
    };

    int songSampleRate = 48000;
    std::vector<BackingSpec> backingTracks;
    std::vector<Track> tracks;
    std::map<Id, TrackInput> trackInputs;
};

// The 3-Track fixture the UI tests and snapshots use: a Backing Track "Band Mix", and "Guitar"
// (Armed, input 1) and "Vocals" (Armed, input 2) to record on.
inline std::unique_ptr<FixtureSong> threeTrackFixture()
{
    return FixtureSongBuilder {}
        .backingTrack ("Band Mix")
        .track ("Guitar", true, { 0 })
        .track ("Vocals", true, { 1 })
        .build();
}
} // namespace overdub::song::testing
