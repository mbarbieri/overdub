#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Mixer.h"
#include "ui/TakeLanes.h"
#include "ui/Timeline.h"
#include "ui/TransportText.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Edit mode's page (issues #28, #29, issue #9 resolution, variant C): the Clip toolbar (the
// selected Clip's name, Track, start and length; its fade-in, fade-out and gain fields; Split,
// Delete; zoom; the Mixer toggle) over the Timeline, and under a draggable divider the Mixer
// (ui/Mixer.h, issue #31), which the toggle hides and shows. Thin: Split and Delete are reported to the
// window, which runs them like the S and backspace keys; drags arrive from the Timeline, and field
// entries are made here, as a changed Song and its undo label ("Fade In", "Fade Out", "Clip Gain").
//
// "Passes ▾" (issue #30) in the timeline's ruler corner opens the Passes panel: one row per Pass,
// newest first, with its heard status and a "↺ Bring back" button that puts every surviving Take
// of the Pass back on top as one undo step ("Bring back Pass N"), then closes the panel.
namespace overdub::ui
{
struct ClipToolbarState
{
    bool hasClip = false;
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

// Edit's Passes list: a heading, then one row per Pass (ui/TakeLanes.h passRows).
class PassesPanel final : public juce::Component
{
public:
    PassesPanel();

    // Bring back is enabled only when `editable` and the row can be brought back.
    void show (const std::vector<PassRow>& rows, bool editable);
    void resized() override;
    void paint (juce::Graphics& g) override;

    int numRows() const { return (int) rowComponents.size(); }

    std::function<void (const song::Id& passId)> onBringBack;

    struct Row
    {
        song::Id passId;
        juce::Label title;
        juce::Label status;
        juce::TextButton bringBackButton;
    };

    juce::Label heading;
    std::vector<std::unique_ptr<Row>> rowComponents;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PassesPanel)
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
    EditPage();

    // The Song and transport the timeline and the Clip toolbar show.
    void show (const TimelineState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    bool isMixerVisible() const { return mixerVisible; }
    bool isPassesPanelOpen() const { return passesPanel.isVisible(); }
    void setPassesPanelOpen (bool open);
    void setMixerVisible (bool visible);
    int mixerHeight() const { return mixerHeightRequested; }

    std::function<void (song::Song changed, std::string label)> onEdit;
    // The Song a Mixer gesture in progress would make (OpenSong::previewMix).
    std::function<void (const song::Song& preview)> onMixPreview;
    std::function<void (song::SampleCount sample)> onSeek;
    std::function<void (const song::Id& trackId, bool armed)> onArm;
    // M and S in the Track headers.
    std::function<void (const song::Id& trackId, bool mute)> onMute;
    std::function<void (const song::Id& trackId, bool solo)> onSolo;
    std::function<void()> onSplit;
    std::function<void()> onDelete;

    ClipToolbar toolbar;
    Timeline timeline;
    MixerDivider divider;
    Mixer mixer;
    juce::TextButton passesButton;
    PassesPanel passesPanel;

private:
    void showToolbar();
    void showPasses();
    void bringBackPass (const song::Id& passId);
    // Applies `change` to the selected Clip as one undo step with this label, if it changes the
    // Song; then the toolbar shows the Clip's values again (a rejected or clamped entry included).
    void editSelectedClip (const std::function<core::Expected<song::Song> (const song::Song&, const song::Id&)>& change, const char* label);

    TimelineState current;
    bool mixerVisible = true;
    int mixerHeightRequested = defaultMixerHeight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditPage)
};
} // namespace overdub::ui
