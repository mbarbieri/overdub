#include "ui/MixFocus.h"

#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "ui/MixerGeometry.h"

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

core::Expected<song::Song> withMixValue (const song::Song& song, const MixerControl& control, double value)
{
    if (control.trackId.empty())
        return song::setMasterVolume (song, value);
    if (control.kind == MixerControlKind::volume)
        return song::setTrackVolume (song, control.trackId, value);
    return song::setTrackPan (song, control.trackId, value);
}

std::string mixLabel (const song::Song& song, const MixerControl& control)
{
    if (control.trackId.empty())
        return song::masterVolumeLabel;
    if (control.kind == MixerControlKind::volume)
        return song::volumeLabel (song, control.trackId);
    return song::panLabel (song, control.trackId);
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
    preview (withMixValue (current, control, to));
    return true;
}

void MixFocus::commitKeyBurst()
{
    if (! keyBurst.has_value())
        return;
    const auto burst = *keyBurst;
    keyBurst.reset(); // before reporting: the edit comes back here through show
    report (withMixValue (current, burst.control, burst.value), mixLabel (current, burst.control));
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
    auto carried = withMixValue (changed, keyBurst->control, keyBurst->value);
    return carried.has_value() ? std::move (*carried) : changed;
}

// Every gesture starts from `current`, the Song as shown: a drag's preview never accumulates.
// A gesture that changes the Song commits a pending key burst first, so it builds on the burst.
void MixFocus::drag (const MixerControl& control, double value)
{
    preview (withMixValue (current, control, value));
}

void MixFocus::set (const MixerControl& control, double value)
{
    commitKeyBurst();
    report (withMixValue (current, control, value), mixLabel (current, control));
}

void MixFocus::abandon()
{
    if (onPreview)
        onPreview (current);
}

void MixFocus::preview (core::Expected<song::Song> candidate)
{
    if (candidate.has_value() && onPreview)
        onPreview (*candidate);
}

void MixFocus::report (core::Expected<song::Song> changed, const std::string& label)
{
    // The controls exist only for the shown Song's Tracks, so the Track is there.
    if (! editable || ! OVERDUB_CHECK (changed.has_value(), "ui.mixer_edit_failed", { "label", label }))
        return;
    if (*changed != current && onEdit)
        onEdit (std::move (*changed), label);
}
} // namespace overdub::ui
