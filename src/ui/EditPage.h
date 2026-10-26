#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/Mixer.h"
#include "ui/PassesPanel.h"
#include "ui/PeaksLookup.h"
#include "ui/TakeLanes.h"
#include "ui/Timeline.h"
#include "ui/TransportText.h"
#include "ui/VideoPreview.h"
#include "video/FrameSelection.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Edit mode's page (issues #28, #29, issue #9 resolution, variant C): the Clip toolbar (the
// selected Clip's name, Track, start and length, or the selected Video Take's (issue #128); its fade-in, fade-out and gain fields; Split,
// Delete; zoom; the Mixer toggle) over the Timeline, and under a draggable divider the Mixer
// (ui/Mixer.h, issue #31), which the toggle hides and shows. While the camera is Armed the Video
// Preview (ui/VideoPreview.h, issue #198) sits at the Mixer region's right end and hides with it. Thin: every gesture that changes state
// is an Intent reported through onIntent (ADR 0008), the Timeline's included. Split and Delete are
// split_selected_clip and delete_selection, like the S and backspace keys; a field entry that
// changes the selected Clip is set_clip_fade_in, set_clip_fade_out or set_clip_gain; the zoom
// buttons, the Mixer toggle, the divider and "Passes ▾" are view Intents. The page
// changes what it shows only when an Intent's handler calls back (setMixerVisible, ...).
//
// "Passes ▾" (issue #30) in the timeline's ruler corner opens the Passes panel: one row per Pass,
// newest first, with its heard status and a "↺ Bring back" button that closes the panel and puts
// every surviving Take of the Pass back on top as one undo step ("Bring back Pass N").
namespace overdub::ui
{
struct ClipToolbarState
{
    bool hasSelection = false;
    std::string details; // "Pass 2 on Guitar · start 1.250 s · length 3.000 s", or a hint
    bool canSplit = false;  // the playhead is strictly inside the selected Clip
    bool canDelete = false;
    bool canEditFadesAndGain = false;
    std::string fadeInText;  // "0.250 s"; empty with no Clip
    std::string fadeOutText; // "0.250 s"; empty with no Clip
    std::string gainText;    // "-3.0 dB"; empty with no Clip
};

// A toolbar entry in seconds ("0.5", "0.5 s"), as whole samples at the rate; none when it is not a
// finite number of seconds >= 0.
std::optional<song::SampleCount> parseSeconds (const std::string& text, int sampleRate);

// A toolbar entry in decibels ("-6", "+3.5 dB"); none when it is not a finite number.
std::optional<double> parseDecibels (const std::string& text);

// The toolbar for the selected Clip (none, or one the Song no longer has: the hint).
ClipToolbarState clipToolbarStateFor (const song::Song& song, const std::optional<song::Id>& selectedClip, song::SampleCount playhead, bool editable);

// The toolbar for the selected Video Take (issue #128): "Video Take · Pass 2 · start 1.000 s ·
// length 4.000 s" ("movie missing" in place of the length when its movie cannot be read), Delete
// enabled while editable; no Split, fades or gain. A Video Take the Song or the Camera lane's
// spans no longer have: the hint, as with no selection.
ClipToolbarState videoTakeToolbarStateFor (const song::Song& song, const std::vector<video::VideoTakeSpan>& videoTakes, const song::Id& videoTakeId, bool editable);

class ClipToolbar final : public juce::Component
{
public:
    ClipToolbar();

    void show (const ClipToolbarState& state, bool mixerVisible);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void()> onSplit;
    std::function<void()> onDelete;
    std::function<void()> onZoomIn;
    std::function<void()> onZoomOut;
    std::function<void()> onToggleMixer;
    // The text a field was given (by typing and return, or losing focus).
    std::function<void (const std::string& text)> onFadeInEntered;
    std::function<void (const std::string& text)> onFadeOutEntered;
    std::function<void (const std::string& text)> onGainEntered;

    juce::Label details;
    juce::Label fadeInCaption { {}, "Fade in" };
    juce::Label fadeInField;
    juce::Label fadeOutCaption { {}, "Fade out" };
    juce::Label fadeOutField;
    juce::Label gainCaption { {}, "Gain" };
    juce::Label gainField;
    juce::TextButton splitButton { "Split (S)" };
    juce::TextButton deleteButton { "Delete" };
    juce::TextButton zoomOutButton { "-" };
    juce::TextButton zoomInButton { "+" };
    juce::TextButton mixerButton { "Mixer" };

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipToolbar)
};

// The bar between the timeline and the Mixer region: dragging it resizes the region.
class MixerDivider final : public juce::Component
{
public:
    MixerDivider();

    // Called with the y the divider's top should move to, in the parent's coordinates.
    std::function<void (int top)> onDragTo;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    int grabOffset = 0;
};

class EditPage final : public juce::Component
{
public:
    explicit EditPage (PeaksLookup peaksFor);

    // The Song and transport the timeline and the Clip toolbar show.
    void show (const TimelineState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    bool isMixerVisible() const { return mixerVisible; }
    bool isPassesPanelOpen() const { return passesPanel.isVisible(); }
    void setPassesPanelOpen (bool open);
    void setMixerVisible (bool visible);
    int mixerHeight() const { return mixerHeightRequested; }
    // The Mixer region's height, clamped as the divider's drag clamps it.
    void setMixerHeight (int height);
    // What the Video Preview shows (VideoPreview::show); it shows only while the camera is Armed.
    void showVideoPreview (bool armed, void* previewLayer, const juce::String& message, bool playback);
    bool isVideoPreviewShown() const { return cameraArmed && mixerVisible; }
    // The Video Preview's playback screen while it shows playback, else null.
    VideoScreen* shownPlayback();

    IntentSink onIntent;
    // The × in a Track's header (Timeline::onDeleteTrack).
    std::function<void (const song::Id& trackId)> onDeleteTrack;
    // "+ Insert" on a Mixer strip (Mixer::onAddInsert): the window asks, then runs add_insert.
    std::function<void (const song::Id& trackId)> onAddInsert;

    ClipToolbar toolbar;
    Timeline timeline;
    MixerDivider divider;
    Mixer mixer;
    VideoPreview videoPreview;
    juce::TextButton passesButton;
    PassesPanel passesPanel;

private:
    void showToolbar();
    void showPasses();
    void bringBackPass (const song::Id& passId);
    // Runs a field's edit of the selected Clip (none: the entry did not parse) if it changes the
    // Song; then the toolbar shows the Clip's values again (a rejected or clamped entry included).
    void editSelectedClip (const std::function<std::optional<intent::Edit> (const song::Id& clipId)>& editFor);
    void emit (Intent intent) const;

    TimelineState current;
    bool mixerVisible = true;
    bool cameraArmed = false;
    int mixerHeightRequested = defaultMixerHeight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditPage)
};
} // namespace overdub::ui
