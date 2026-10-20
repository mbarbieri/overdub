#include "ui/MixFocus.h"

#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "ui/MixerGeometry.h"
#include "ui/SongEdits.h"

namespace overdub::ui
{
namespace
{
std::optional<double> mixValue (const song::Song& song, const MixerControl& control)
{
    if (control.trackId.empty())
        return song.master.volumeDb;
    const auto* track = song::findTrack (song, control.trackId);
    if (track == nullptr)
        return std::nullopt;
    return control.kind == MixerControlKind::volume ? track->volumeDb : track->pan;
}

intent::MixEdit mixEditFor (const MixerControl& control, double value)
{
    if (control.trackId.empty())
        return intent::SetMasterVolume { value };
    if (control.kind == MixerControlKind::volume)
        return intent::SetTrackVolume { control.trackId, value };
    return intent::SetTrackPan { control.trackId, value };
}
} // namespace

void MixFocus::show (const song::Song& song, bool isEditable)
{
    current = song;
    editable = isEditable;
    // Nothing has focus while a Pass runs, nor once its Track is gone (a burst was committed first).
    if (focused.has_value() && (! editable || ! mixValue (current, *focused).has_value()))
    {
        focused.reset();
        keyBurst.reset();
    }
}

double MixFocus::shownValue (const MixerControl& control, double songValue) const
{
    if (keyBurst.has_value() && keyBurst->control == control)
        return keyBurst->value;
    return songValue;
}

void MixFocus::focus (const MixerControl& control)
{
    if (focused == control)
        return;
    commitKeyBurst();
    focused = control;
    notifyFocusChanged();
}

void MixFocus::releaseFocus()
{
    commitKeyBurst();
    if (! focused.has_value())
        return;
    focused.reset();
    notifyFocusChanged();
}

void MixFocus::notifyFocusChanged()
{
    if (onFocusChanged)
        onFocusChanged();
}

bool MixFocus::handleKey (const juce::KeyPress& key, Clock::time_point now)
{
    if (! focused.has_value() || ! editable)
        return false;
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        releaseFocus();
        return true;
    }
    const auto modifiers = key.getModifiers();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown())
        return false;

    const auto code = key.getKeyCode();
    const auto fine = modifiers.isShiftDown();
    const auto control = *focused;
    const auto from = keyBurst.has_value() && keyBurst->control == control ? keyBurst->value : mixValue (current, control).value_or (0.0);
    double to = from;
    if (control.kind == MixerControlKind::volume)
    {
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
            to = faderKeyStepDb (from, true, fine);
        else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
            to = faderKeyStepDb (from, false, fine);
        else
            return false;
    }
    else
    {
        if (code == juce::KeyPress::rightKey)
            to = panKeyStep (from, true, fine);
        else if (code == juce::KeyPress::leftKey)
            to = panKeyStep (from, false, fine);
        else
            return false;
    }

    keyBurst = KeyBurst { .control = control, .value = to, .deadline = now + keyBurstDuration };
    if (onKeyValue)
        onKeyValue (control, to);
    preview (mixEditFor (control, to));
    return true;
}

void MixFocus::commitKeyBurst()
{
    if (auto burst = takeKeyBurst(); burst.has_value() && onEdit)
        onEdit (*burst);
}

std::optional<intent::Edit> MixFocus::takeKeyBurst()
{
    if (! keyBurst.has_value())
        return std::nullopt;
    const auto burst = *keyBurst;
    keyBurst.reset(); // before its edit runs: the edit comes back here through show
    return reportable (asEdit (mixEditFor (burst.control, burst.value)));
}

void MixFocus::commitKeyBurstIfDue (Clock::time_point now)
{
    if (keyBurst.has_value() && now >= keyBurst->deadline)
        commitKeyBurst();
}

song::Song MixFocus::withKeyBurstCarried (song::Song changed) const
{
    if (! keyBurst.has_value())
        return changed;
    const auto before = mixValue (current, keyBurst->control);
    const auto edited = mixValue (changed, keyBurst->control);
    if (! before.has_value() || ! edited.has_value() || ! juce::exactlyEqual (*before, *edited))
        return changed;
    auto carried = editedSong (changed, asEdit (mixEditFor (keyBurst->control, keyBurst->value)));
    return carried.has_value() ? std::move (carried->song) : changed;
}

// A gesture that changes the Song commits a pending key burst first, so it builds on the burst.
void MixFocus::drag (const MixerControl& control, double value)
{
    preview (mixEditFor (control, value));
}

void MixFocus::set (const MixerControl& control, double value)
{
    commitKeyBurst();
    report (asEdit (mixEditFor (control, value)));
}

void MixFocus::abandon()
{
    if (onPreview)
        onPreview (intent::PreviewMix {});
}

void MixFocus::preview (const intent::MixEdit& edit)
{
    if (onPreview)
        onPreview (intent::PreviewMix { edit });
}

void MixFocus::report (const intent::Edit& edit)
{
    if (auto reported = reportable (edit); reported.has_value() && onEdit)
        onEdit (*reported);
}

std::optional<intent::Edit> MixFocus::reportable (const intent::Edit& edit) const
{
    if (! editable)
        return std::nullopt;
    const auto changed = editedSong (current, edit);
    // The controls exist only for the shown Song's Tracks, so the Track is there.
    if (! OVERDUB_CHECK (changed.has_value(), "ui.mixer_edit_failed", { "op", std::string (core::operationName (edit)) }))
        return std::nullopt;
    if (changed->song == current)
        return std::nullopt;
    return edit;
}
} // namespace overdub::ui
