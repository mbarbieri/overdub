#include "ui/SongEdits.h"

#include "song/ClipEdits.h"
#include "song/InsertEdits.h"
#include "song/MarkerEdits.h"
#include "song/MixEdits.h"
#include "song/TakeEdits.h"

namespace overdub::ui
{
namespace
{
core::Expected<EditedSong> edited (core::Expected<song::Song> changed, std::string label)
{
    if (! changed.has_value())
        return std::unexpected (changed.error());
    return EditedSong { std::move (*changed), std::move (label) };
}
} // namespace

core::Expected<EditedSong> editedSong (const song::Song& before, const intent::Edit& edit)
{
    using namespace intent;
    return core::visitExhaustively (
        edit,
        core::Overloaded {
            [&] (const MoveClip& e) { return edited (song::moveClip (before, e.clipId, e.start), song::moveClipLabel); },
            [&] (const TrimClipStart& e) { return edited (song::trimClipStart (before, e.clipId, e.start), song::trimClipLabel); },
            [&] (const TrimClipEnd& e) { return edited (song::trimClipEnd (before, e.clipId, e.end), song::trimClipLabel); },
            [&] (const SetClipFadeIn& e) { return edited (song::setClipFadeIn (before, e.clipId, e.length), song::fadeInLabel); },
            [&] (const SetClipFadeOut& e) { return edited (song::setClipFadeOut (before, e.clipId, e.length), song::fadeOutLabel); },
            [&] (const SetClipGain& e) { return edited (song::setClipGain (before, e.clipId, e.gainDb), song::clipGainLabel); },
            [&] (const BringBackPass& e) { return edited (song::bringBackPass (before, e.passId), song::bringBackPassLabel (before, e.passId)); },
            [&] (const SetTrackVolume& e) { return edited (song::setTrackVolume (before, e.trackId, e.volumeDb), song::volumeLabel (before, e.trackId)); },
            [&] (const SetTrackPan& e) { return edited (song::setTrackPan (before, e.trackId, e.pan), song::panLabel (before, e.trackId)); },
            [&] (const SetMasterVolume& e) { return edited (song::setMasterVolume (before, e.volumeDb), song::masterVolumeLabel); },
            [&] (const SetTrackMute& e) { return edited (song::setTrackMute (before, e.trackId, e.mute), song::muteLabel (before, e.trackId, e.mute)); },
            [&] (const SetTrackSolo& e) { return edited (song::setTrackSolo (before, e.trackId, e.solo), song::soloLabel (before, e.trackId, e.solo)); },
            [&] (const RemoveInsert& e) { return edited (song::removeInsert (before, e.slotId), song::removeInsertLabel (before, e.slotId)); },
            [&] (const MoveInsert& e) { return edited (song::moveInsert (before, e.slotId, e.toIndex), song::moveInsertLabel (before, e.slotId)); },
            [&] (const SetInsertBypassed& e) { return edited (song::setInsertBypassed (before, e.slotId, e.bypassed), song::bypassInsertLabel (before, e.slotId, e.bypassed)); },
            [&] (const MoveMarker& e) { return edited (song::moveMarker (before, e.markerId, e.position), song::moveMarkerLabel); },
            [&] (const RenameMarker& e) { return edited (song::renameMarker (before, e.markerId, e.name), song::renameMarkerLabel); },
            [&] (const DeleteMarker& e) { return edited (song::deleteMarker (before, e.markerId), song::deleteMarkerLabel); },
        });
}

intent::Edit asEdit (const intent::MixEdit& edit)
{
    return std::visit ([] (const auto& mixEdit) { return intent::Edit { mixEdit }; }, edit);
}
} // namespace overdub::ui
