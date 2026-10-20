#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

#include <string>

// The one pairing of each plain edit (intent::Edit) with its undo label: the Song after the edit,
// and the step's label named from the Song before it. SongView's edit handler and the Mixer's
// previews use it, so the Song a gesture previews is the Song its release commits. The edit
// functions themselves stay in song/ClipEdits.h, MixEdits.h, InsertEdits.h and TakeEdits.h.
namespace overdub::ui
{
struct EditedSong
{
    song::Song song;
    std::string label;
};

// The edit function's Error (song.clip_not_found, ...) when the Song cannot take the edit.
core::Expected<EditedSong> editedSong (const song::Song& before, const intent::Edit& edit);

intent::Edit asEdit (const intent::MixEdit& edit);
} // namespace overdub::ui
