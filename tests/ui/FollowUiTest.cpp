#include "ui/Follow.h"

#include "core/Log.h"
#include "ui/MainWindowFixture.h"
#include "ui/TranscribePage.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

// Follow through the real window (issue #266): Edit's Timeline pages and Transcribe's waveform
// scrolls while the Song plays, and a hand-made view change pauses it until the next play, a seek
// or the playhead coming back into view. Driven through MainComponent::run on a 20 s Backing Track,
// with the engine playing device blocks of 256 samples at 48 kHz.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using core::ScopedLogCapture;

namespace
{
constexpr song::SampleCount second = 48'000;

std::unique_ptr<song::testing::FixtureSong> longSong()
{
    return song::testing::FixtureSongBuilder {}.backingTrack ("Band Mix", 20.0).track ("Guitar").build();
}

void run (MainWindowFixture& f, Intent intent)
{
    REQUIRE (f.window->run (std::move (intent), Origin::control).has_value());
}

void showMode (MainWindowFixture& f, Mode mode)
{
    run (f, WindowIntent { intent::ShowMode { mode } });
}

void playOrStop (MainWindowFixture& f)
{
    run (f, SongIntent { intent::PlayOrStop {} });
}

// Stops and plays again. The engine publishes that it stopped from the audio thread, so a block
// runs in between, as a tick always does between two presses.
void stopAndPlay (MainWindowFixture& f)
{
    playOrStop (f);
    f.runBlocks (1);
    REQUIRE_FALSE (f.openSong->isPlaying());
    playOrStop (f);
}

void seek (MainWindowFixture& f, song::SampleCount sample)
{
    run (f, SongIntent { intent::Seek { sample } });
}

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

struct EditFollow : MainWindowFixture
{
    EditFollow() : MainWindowFixture (longSong())
    {
        showMode (*this, Mode::edit);
        setView (0);
        seek (*this, 0); // resumes Follow, paused by setView
    }

    Timeline& timeline() { return window->songView()->editPage.timeline; }
    int lanesWidth() { return timeline().rulerBounds().getWidth(); }

    // A hand-made view change at the finest zoom.
    void setView (song::SampleCount firstSample) { run (*this, SongIntent { intent::SetTimelineView { firstSample, minSamplesPerPixel } }); }

    // Where a page now puts the lanes: the playhead a tenth of their width in, never before 0.
    song::SampleCount pagedTo() { return std::max<song::SampleCount> (0, openSong->position() - (song::SampleCount) (lanesWidth() / 10) * minSamplesPerPixel); }
};

struct TranscribeFollow : MainWindowFixture
{
    TranscribeFollow() : MainWindowFixture (longSong())
    {
        showMode (*this, Mode::transcribe);
        setView (0);
        seek (*this, 0);
    }

    TranscribePage& page() { return window->songView()->transcribePage; }

    // A hand-made view change at the finest zoom (clamped to the page's limit).
    void setView (song::SampleCount firstSample) { run (*this, SongIntent { intent::SetTranscribeView { firstSample, 1 } }); }

    // Where scrolling puts the waveform: the playhead a fifth of its width in.
    song::SampleCount scrolledTo() { return openSong->position() - (song::SampleCount) (page().wave.getWidth() / 5) * page().view().samplesPerPixel; }
};
} // namespace

TEST_CASE ("Edit's Timeline doesn't follow while the Song is stopped", "[ui][follow][edit]")
{
    EditFollow f;
    seek (f, 5 * second);
    f.runBlocks (1);
    CHECK (f.timeline().view().firstSample == 0);
}

TEST_CASE ("Edit's Timeline pages to a playhead past its lanes, a tenth of their width in", "[ui][follow][edit]")
{
    EditFollow f;
    seek (f, 5 * second);
    playOrStop (f);
    f.runBlocks (1);
    CHECK (f.timeline().view().firstSample == f.pagedTo());
    CHECK (f.timeline().view().samplesPerPixel == minSamplesPerPixel);

    // Inside the lanes, the view stays put.
    const auto paged = f.timeline().view().firstSample;
    f.runBlocks (4);
    CHECK (f.timeline().view().firstSample == paged);
}

TEST_CASE ("Edit's Timeline pages back to a playhead before its lanes", "[ui][follow][edit]")
{
    EditFollow f;
    f.setView (10 * second);
    seek (f, second);
    playOrStop (f);
    f.runBlocks (1);
    CHECK (f.timeline().view().firstSample == f.pagedTo());
}

TEST_CASE ("A hand-made view change pauses Edit's Follow until a seek", "[ui][follow][edit]")
{
    EditFollow f;
    const ScopedLogCapture capture;
    playOrStop (f);
    f.runBlocks (1);
    f.setView (10 * second);
    f.runBlocks (2);
    CHECK (f.timeline().isFollowPaused());
    CHECK (f.timeline().view().firstSample == 10 * second);
    const auto paused = eventsNamed (capture, "ui.follow_paused");
    REQUIRE (paused.size() == 1);
    CHECK (paused[0].at ("mode") == "edit");

    seek (f, 5 * second); // pages at once, while playing
    const auto pagedAtSeek = f.pagedTo();
    f.runBlocks (1);
    CHECK_FALSE (f.timeline().isFollowPaused());
    CHECK (f.timeline().view().firstSample == pagedAtSeek);
    const auto resumed = eventsNamed (capture, "ui.follow_resumed");
    REQUIRE (resumed.size() == 1);
    CHECK (resumed[0].at ("mode") == "edit");
    CHECK (resumed[0].at ("reason") == "seek");
}

TEST_CASE ("Edit's Follow resumes when the playhead comes back into the lanes", "[ui][follow][edit]")
{
    EditFollow f;
    playOrStop (f);
    f.runBlocks (1);
    const auto ahead = f.openSong->position() + 512;
    f.setView (ahead);
    f.runBlocks (1);
    CHECK (f.timeline().isFollowPaused());

    f.runBlocks (4); // 1024 samples on: inside the lanes
    CHECK_FALSE (f.timeline().isFollowPaused());
    CHECK (f.timeline().view().firstSample == ahead);
}

TEST_CASE ("Edit's Follow resumes on the next play, and switching modes doesn't resume it", "[ui][follow][edit]")
{
    EditFollow f;
    playOrStop (f);
    f.runBlocks (1);
    f.setView (10 * second);
    showMode (f, Mode::transcribe);
    f.runBlocks (1);
    showMode (f, Mode::edit);
    f.runBlocks (1);
    CHECK (f.timeline().isFollowPaused());
    CHECK (f.timeline().view().firstSample == 10 * second);

    stopAndPlay (f);
    f.runBlocks (1);
    CHECK_FALSE (f.timeline().isFollowPaused());
    CHECK (f.timeline().view().firstSample == f.pagedTo());
}

TEST_CASE ("Follow scrolls Transcribe's waveform while playing, and not while stopped", "[ui][follow][transcribe]")
{
    TranscribeFollow f;
    seek (f, 5 * second);
    f.runBlocks (1);
    CHECK (f.page().view().firstSample == 0); // stopped

    playOrStop (f);
    f.runBlocks (1);
    CHECK (f.page().view().firstSample == f.scrolledTo());
    f.runBlocks (4);
    CHECK (f.page().view().firstSample == f.scrolledTo());
    CHECK (f.page().overview.windowRectangle().getX() > 0.0f);
}

TEST_CASE ("A hand-made view change pauses Transcribe's Follow until the next play", "[ui][follow][transcribe]")
{
    TranscribeFollow f;
    const ScopedLogCapture capture;
    seek (f, 5 * second);
    playOrStop (f);
    f.runBlocks (1);
    f.setView (12 * second);
    f.runBlocks (2);
    CHECK (f.page().isFollowPaused());
    CHECK (f.page().view().firstSample == 12 * second);
    const auto paused = eventsNamed (capture, "ui.follow_paused");
    REQUIRE (paused.size() == 1);
    CHECK (paused[0].at ("mode") == "transcribe");

    stopAndPlay (f);
    f.runBlocks (1);
    CHECK_FALSE (f.page().isFollowPaused());
    CHECK (f.page().view().firstSample == f.scrolledTo());
    const auto resumed = eventsNamed (capture, "ui.follow_resumed");
    REQUIRE (resumed.size() == 1);
    CHECK (resumed[0].at ("reason") == "play");
}

TEST_CASE ("While looping a Loop that fits, Transcribe's waveform stays still through the wrap", "[ui][follow][transcribe]")
{
    TranscribeFollow f;
    const auto added = f.window->run (SongIntent { intent::AddLoop { 8 * second, 8 * second + second / 2 } }, Origin::control);
    REQUIRE (added.has_value());
    run (f, SongIntent { intent::SelectLoop { added->createdId } }); // looping, zoomed to fit
    const auto framed = f.page().view();
    playOrStop (f);
    f.runBlocks (100); // 25 600 samples: past the Loop's end, wrapped to its start
    CHECK (f.openSong->position() < 8 * second + second / 2);
    CHECK (f.page().view() == framed);
    CHECK_FALSE (f.page().isFollowPaused());
}

TEST_CASE ("A hand-made view change that keeps the playhead past its spot stays where it was put", "[ui][follow][transcribe]")
{
    TranscribeFollow f;
    seek (f, 5 * second);
    playOrStop (f);
    f.runBlocks (1);
    // Back by half the width: the playhead shows, but past the fifth Follow holds it at.
    const auto halfWidth = (song::SampleCount) (f.page().wave.getWidth() / 2) * f.page().view().samplesPerPixel;
    const auto putAt = f.page().view().firstSample - halfWidth;
    f.setView (putAt);
    f.runBlocks (4);
    CHECK (f.page().isFollowPaused());
    CHECK (f.page().view().firstSample == putAt);
}

TEST_CASE ("Transcribe's Follow resumes once the playhead reaches its spot in a view put ahead of it", "[ui][follow][transcribe]")
{
    TranscribeFollow f;
    const ScopedLogCapture capture;
    seek (f, 5 * second);
    playOrStop (f);
    f.runBlocks (1);
    const auto putAt = f.openSong->position() + 512;
    f.setView (putAt);
    f.runBlocks (1);
    CHECK (f.page().isFollowPaused());

    // 1024 samples on the playhead is inside the view, before its spot: Follow resumes, the view stays.
    f.runBlocks (4);
    CHECK_FALSE (f.page().isFollowPaused());
    CHECK (f.page().view().firstSample == putAt);
    const auto resumed = eventsNamed (capture, "ui.follow_resumed");
    REQUIRE (resumed.size() == 1);
    CHECK (resumed[0].at ("reason") == "in_view");
}
