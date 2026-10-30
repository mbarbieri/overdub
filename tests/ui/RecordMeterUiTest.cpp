#include "ui/RecordPage.h"

#include "song/MixEdits.h"
#include "ui/MainWindowFixture.h"
#include "ui/MixerGeometry.h"
#include "ui/TrackRowEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Each Record row's level meter and clip indicator (issue #131, ADR 0005 UI amendment) through the
// real window: input fed to the loopback fake device, polled by the window's tick, then what the
// row's meter, the Mixer's meter and the clip indicator show.
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;
using overdub::engine::testing::impulse;
using overdub::engine::testing::sine;

namespace
{
struct RecordMeterFixture : MainWindowFixture
{
    RecordMeterFixture()
    {
        // A visit to Edit mode builds the Mixer's strips; their meters move in every mode.
        window->run (WindowIntent { intent::ShowMode { Mode::edit } });
        window->run (WindowIntent { intent::ShowMode { Mode::record } });
        REQUIRE (isShown (window->songView()->recordPage));
        REQUIRE (window->songView()->editPage.mixer.strips.size() == song().tracks.size());
    }

    // Puts `samples` on a device input channel from the next block on.
    void feed (int channel, const std::vector<float>& samples)
    {
        auto& input = device.extraInput[(std::size_t) channel];
        const auto start = (std::size_t) device.streamPosition();
        input.resize (start + samples.size(), 0.0f);
        std::ranges::copy (samples, input.begin() + (std::ptrdiff_t) start);
    }

    MixerStrip& stripNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& strip : window->songView()->editPage.mixer.strips)
            if (strip->trackId() == trackId)
                return *strip;
        FAIL ("no strip for Track " << name);
        return *window->songView()->editPage.mixer.strips.front();
    }
};

float peakOf (const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const auto sample : samples)
        peak = std::max (peak, std::abs (sample));
    return peak;
}

// The fixture's Guitar is Armed on input 0.
constexpr int guitarInput = 0;
} // namespace

TEST_CASE ("an Armed Track's Record meter follows its input whatever its fader; disarmed, it follows the post-fader output", "[ui][record_meter]")
{
    RecordMeterFixture f;
    const auto guitar = f.trackNamed ("Guitar").id;
    auto& row = f.rowNamed ("Guitar");
    auto& strip = f.stripNamed ("Guitar");
    const auto onInput = sine (2 * deviceBlockSize, 1000.0, 0.5f);
    const auto inputPeak = peakOf (onInput);

    // Disarmed, stopped and not monitored: the Track outputs nothing, whatever is on its input.
    f.openSong->changeSettings (withArmed (f.openSong->song(), guitar, false));
    f.feed (guitarInput, onInput);
    f.runBlocks (2);
    CHECK_THAT (row.meter.level(), WithinULP (0.0f, 0));

    // Armed and turned down: the meter reads the raw input; the Mixer meter still reads the strip.
    f.openSong->changeSettings (withArmed (f.openSong->song(), guitar, true));
    auto quieter = overdub::song::setTrackVolume (f.song(), guitar, -20.0);
    REQUIRE (quieter.has_value());
    REQUIRE (f.openSong->edit (std::move (*quieter), "Guitar Volume").has_value());
    f.feed (guitarInput, onInput);
    f.runBlocks (2);
    CHECK_THAT (row.meter.level(), WithinULP (inputPeak, 0));
    CHECK_THAT (strip.meter.level(), WithinULP (0.0f, 0));

    // Disarmed again: the input no longer moves it, so it falls back from the input peak.
    f.openSong->changeSettings (withArmed (f.openSong->song(), guitar, false));
    f.feed (guitarInput, onInput);
    f.runBlocks (2);
    CHECK_THAT (row.meter.level(), WithinULP (inputPeak * meterFallPerPoll, 0));
}

TEST_CASE ("a Track that is not Armed meters on its Record row what its Mixer meter shows", "[ui][record_meter]")
{
    RecordMeterFixture f;
    auto& row = f.rowNamed ("Band Mix");
    auto& strip = f.stripNamed ("Band Mix");
    REQUIRE (! f.trackNamed ("Band Mix").settings.armed);

    f.openSong->playOrStop();
    f.runBlocks (4);
    REQUIRE (strip.meter.level() > 0.0f);
    CHECK_THAT (row.meter.level(), WithinULP (strip.meter.level(), 0));
}

TEST_CASE ("a Record meter lights up to the fader's thumb at the same dB: 0 dBFS under 0 dB", "[ui][record_meter]")
{
    RecordMeterFixture f;
    auto& row = f.rowNamed ("Guitar");
    REQUIRE (row.meter.getWidth() > 100);

    // Rising, so each poll's peak is the level shown.
    for (const auto db : { -40.0, -12.0, -6.0, 0.0, 6.0 })
    {
        row.fader.setDb (db);
        row.meter.addPeak ((float) std::pow (10.0, db / 20.0));
        const auto litEnd = (double) row.meter.getX() + row.meter.litArea().getRight();
        const auto thumb = (double) row.fader.getX() + row.fader.thumbCentre().x;
        CHECK_THAT (litEnd, WithinAbs (thumb, 0.5));
    }
}

TEST_CASE ("an Armed Track's input peak at 0.999 lights the clip dot until it is clicked or the next Pass starts", "[ui][record_meter][clip]")
{
    RecordMeterFixture f;
    auto& row = f.rowNamed ("Guitar");
    auto& clip = row.clip;

    f.feed (guitarInput, impulse (deviceBlockSize, 100, 0.998f));
    f.runBlocks (1);
    CHECK (! clip.isLit());

    f.feed (guitarInput, impulse (deviceBlockSize, 100, clipPeak));
    f.runBlocks (1);
    CHECK (clip.isLit());

    // Silence: the level drops, the dot stays.
    f.runBlocks (4);
    CHECK (row.meter.level() < clipPeak);
    CHECK (clip.isLit());

    click (clip);
    CHECK (! clip.isLit());

    f.feed (guitarInput, impulse (deviceBlockSize, 50, -1.0f));
    f.runBlocks (1);
    REQUIRE (clip.isLit());

    REQUIRE (pressKey (*f.window, 'r'));
    CHECK (f.openSong->isRecording());
    CHECK (! clip.isLit());

    // During the Pass a clip lights it again, and pressing record again starts no Pass.
    f.feed (guitarInput, impulse (deviceBlockSize, 50, 1.0f));
    f.runBlocks (1);
    REQUIRE (clip.isLit());
    REQUIRE (pressKey (*f.window, 'r'));
    CHECK (clip.isLit());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (clip.isLit());
}
