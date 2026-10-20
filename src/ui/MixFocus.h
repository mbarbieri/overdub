#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// The mix controls' behaviour apart from how they are drawn (issues #129, #130): the gestures on
// a Track's fader or pan or the Master fader as Intents (ui/Intent.h), keyboard focus, and the
// arrow keys' bursts. The Mixer (ui/Mixer.h) and the Record page's rows (ui/RecordPage.h) each own
// one, so a Record row's fader behaves exactly like its Mixer fader.
//
// A drag is previewed while it moves (onPreview: the mix edit the drag would make) and reported
// once, on mouse up, as one edit (onEdit: set_track_volume, set_track_pan, set_master_volume, one
// undo step each); a drag that ends where it started previews the Song as it is (abandon). A
// double-click sets 0 dB or C (set), one undo step (none when already there).
//
// Keyboard focus: grabbing a control focuses it; the window sends it keys first (handleKey). The
// arrows adjust it by faderKeyStepDb / panKeyStep (ui/MixerGeometry.h), each press previewed at
// once and shown through onKeyValue; a burst of presses on one control is one undo step, labelled
// like its drag, committed keyBurstDuration after the last press (commitKeyBurstIfDue), or sooner by
// commitKeyBurst (focus moving or released, another gesture, or the window's own triggers). Esc
// releases focus. The focus is this object's state, not JUCE's keyboard focus, which stays on the
// window.
//
// While not editable (a Pass runs) nothing is reported and nothing has focus.
namespace overdub::ui
{
enum class MixerControlKind
{
    volume,
    pan
};

// One focusable control: a Track's fader or pan, or (empty trackId) the Master fader.
struct MixerControl
{
    song::Id trackId;
    MixerControlKind kind = MixerControlKind::volume;

    bool operator== (const MixerControl&) const = default;
};

class MixFocus
{
public:
    using Clock = std::chrono::steady_clock;
    // How long after the last arrow press a burst of presses becomes its undo step.
    static constexpr auto keyBurstDuration = std::chrono::seconds (1);

    // The Song the controls show; not editable while a Pass runs. Nothing has focus while not
    // editable, nor once the focused control's Track is gone. Does not call onFocusChanged.
    void show (const song::Song& song, bool editable);
    const song::Song& song() const { return current; }
    bool isEditable() const { return editable; }
    // What a control shows: the burst's value while a burst on it is pending, else `songValue`.
    double shownValue (const MixerControl& control, double songValue) const;

    // Keyboard focus: the control that takes the arrow keys, if any.
    const std::optional<MixerControl>& focusedControl() const { return focused; }
    // Focuses a control (committing a burst on another one).
    void focus (const MixerControl& control);
    // Commits the burst, if any, and leaves no control focused.
    void releaseFocus();
    // A key press while a control has focus: the arrows adjust it, Esc releases it. Returns
    // whether the key was used; every other key is left to the window.
    bool handleKey (const juce::KeyPress& key, Clock::time_point now);

    bool hasKeyBurst() const { return keyBurst.has_value(); }
    // Reports the burst of arrow presses (if any) as one undo step now.
    void commitKeyBurst();
    // Commits the burst once keyBurstDuration has passed since its last press.
    void commitKeyBurstIfDue (Clock::time_point now);
    // An edit made from the Song before the burst was committed, with the burst's value carried into
    // it, unless the edit set that control itself (or removed its Track).
    song::Song withKeyBurstCarried (song::Song changed) const;

    // A drag in progress at `value`: previewed.
    void drag (const MixerControl& control, double value);
    // A finished drag or a double-click: the control set to `value` as one undo step (after a
    // pending burst's own step).
    void set (const MixerControl& control, double value);
    // A drag that ended where it started: the Song as it is is previewed.
    void abandon();
    // Reports an edit unless it changes song(); nothing while not editable.
    void report (const intent::Edit& edit);

    // The focused control changed (focus or releaseFocus).
    std::function<void()> onFocusChanged;
    // An arrow press set this control's value, ahead of the Song.
    std::function<void (const MixerControl& control, double value)> onKeyValue;
    // A gesture in progress: the mix it would make, heard and not committed.
    std::function<void (const intent::PreviewMix& preview)> onPreview;
    // A finished gesture: one undo step.
    std::function<void (const intent::Edit& edit)> onEdit;

private:
    struct KeyBurst
    {
        MixerControl control;
        double value = 0.0;
        Clock::time_point deadline;
    };

    void preview (const intent::MixEdit& edit);
    void notifyFocusChanged();

    song::Song current;
    bool editable = true;
    std::optional<MixerControl> focused;
    std::optional<KeyBurst> keyBurst;
};
} // namespace overdub::ui
