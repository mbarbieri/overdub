#include "ui/Timeline.h"

#include "core/Error.h"
#include "core/Log.h"
#include "ui/FileDrop.h"

#include <algorithm>
#include <string>

// The Timeline's gestures (ui/Timeline.h): mouse clicks and drags on the lanes, the ruler and the
// Camera lane, Take Lane clicks, grip drags, file drags, and cancelling the one in progress.
namespace overdub::ui
{
const TrackDrag& Timeline::trackDrag() const
{
    static const TrackDrag none;
    const auto* drag = std::get_if<TrackDrag> (&gesture);
    return drag != nullptr ? *drag : none;
}

bool Timeline::cancelGesture (std::string_view reason)
{
    if (std::holds_alternative<std::monostate> (gesture) || std::holds_alternative<FileDragHover> (gesture))
        return false;
    if (std::holds_alternative<TrackDrag> (gesture))
        core::logEvent (core::Level::debug, "ui.track_drag_cancelled", { { "page", "edit" }, { "reason", std::string (reason) } });
    else
        core::logEvent (core::Level::debug, "ui.timeline_gesture_cancelled", { { "gesture", std::holds_alternative<SeekGesture> (gesture) ? "seek" : "clip_drag" }, { "reason", std::string (reason) } });
    gesture = std::monostate {};
    repaint();
    return true;
}

void Timeline::beginTrackDrag (TrackHeader& header, juce::Point<int> inGrip)
{
    const auto& tracks = current.song.tracks;
    const auto found = std::ranges::find (tracks, header.trackId(), &song::Track::id);
    if (! current.editable || found == tracks.end() || ! std::holds_alternative<std::monostate> (gesture))
        return;
    const auto from = (std::size_t) (found - tracks.begin());
    const auto row = getLocalArea (&header, header.getLocalBounds());
    const auto pointerY = getLocalPoint (&header.grip, inGrip).y;
    gesture.emplace<TrackDrag>().begin (header.trackId(), from, row, header.createComponentSnapshot (header.getLocalBounds()), pointerY, trackRowSpans());
    core::logEvent (core::Level::debug, "ui.track_drag_started", { { "page", "edit" }, { "track_id", header.trackId() }, { "from", from } });
    repaint();
}

void Timeline::dragTrack (TrackHeader& header, juce::Point<int> inGrip)
{
    auto* drag = std::get_if<TrackDrag> (&gesture);
    if (drag == nullptr)
        return;
    drag->moveTo (getLocalPoint (&header.grip, inGrip).y, trackRowSpans());
    repaint();
}

void Timeline::dropTrack (TrackHeader& header, juce::Point<int> inGrip)
{
    auto* drag = std::get_if<TrackDrag> (&gesture);
    if (drag == nullptr)
        return;
    const auto drop = drag->end (getLocalPoint (&header.grip, inGrip).y, trackRowSpans());
    gesture = std::monostate {};
    repaint();
    if (! drop.has_value())
        return;
    core::logEvent (core::Level::debug, "ui.track_dropped", { { "page", "edit" }, { "track_id", drop->trackId }, { "from", drop->from }, { "to", drop->to } });
    if (drop->to != drop->from)
        trackActions.moveTrack (drop->trackId, drop->to);
}

bool Timeline::isInterestedInFileDrag (const juce::StringArray& files)
{
    const auto free = std::holds_alternative<std::monostate> (gesture) || std::holds_alternative<FileDragHover> (gesture);
    return free && canImportFiles && canImportFiles() && ! importableAudioFiles (files).empty();
}

void Timeline::fileDragEnter (const juce::StringArray& files, int, int)
{
    if (isInterestedInFileDrag (files))
        gesture = FileDragHover {};
    repaint();
}

void Timeline::fileDragExit (const juce::StringArray&)
{
    if (std::holds_alternative<FileDragHover> (gesture))
        gesture = std::monostate {};
    repaint();
}

void Timeline::filesDropped (const juce::StringArray& files, int, int)
{
    if (std::holds_alternative<FileDragHover> (gesture))
        gesture = std::monostate {};
    repaint();
    auto importable = importableAudioFiles (files);
    const auto accepted = isInterestedInFileDrag (files) && onFilesDropped != nullptr;
    core::logEvent (core::Level::debug, "ui.files_dropped", { { "dropped", files.size() }, { "importable", importable.size() }, { "accepted", accepted } });
    if (accepted)
        onFilesDropped (std::move (importable));
}

std::optional<song::Id> Timeline::videoTakeAt (juce::Point<int> position) const
{
    // Later blocks are drawn on top: search from the last.
    for (auto span = current.videoTakes.rbegin(); span != current.videoTakes.rend(); ++span)
        if (videoTakeBounds (span->videoTakeId).contains (position))
            return span->videoTakeId;
    return std::nullopt;
}

std::optional<std::size_t> Timeline::laneAt (juce::Point<int> position) const
{
    if (! lanesArea().contains (position))
        return std::nullopt;
    for (std::size_t i = 0; i < layout.trackLanes.size(); ++i)
        if (layout.trackLanes[i].contains (position))
            return i;
    return std::nullopt;
}

bool Timeline::clickTakeLane (juce::Point<int> position)
{
    for (std::size_t i = 0; i < takeLanes.size() && i < layout.takeLanes.size(); ++i)
    {
        for (std::size_t j = 0; j < takeLanes[i].size() && j < layout.takeLanes[i].size(); ++j)
        {
            if (! layout.takeLanes[i][j].contains (position))
                continue;
            const auto at = sampleForX (timelineView, position.x);
            auto edit = laneEditAt (current.song, takeLanes[i][j].takeId, at, song::makeId());
            core::logEvent (core::Level::debug, "ui.take_lane_clicked", { { "take_id", takeLanes[i][j].takeId }, { "at", at }, { "changes_song", edit.has_value() } });
            if (! edit.has_value())
                return true;
            const auto clipId = edit->clipId;
            if (onEdit)
                onEdit (std::move (edit->song), std::move (edit->label));
            if (song::findClip (current.song, clipId) != nullptr)
                selectClip (clipId);
            return true;
        }
    }
    return false;
}

void Timeline::mouseDown (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();
    if (! current.editable || ! std::holds_alternative<std::monostate> (gesture))
        return;

    if (layout.ruler.contains (position))
    {
        gesture = SeekGesture {};
        trackActions.seek (std::max<song::SampleCount> (0, sampleForX (timelineView, position.x)));
        return;
    }

    if (layout.cameraLane.contains (position))
    {
        const auto videoTakeId = videoTakeAt (position);
        auto fields = core::Fields { { "hit", videoTakeId.has_value() } };
        if (videoTakeId.has_value())
            fields["video_take_id"] = *videoTakeId;
        core::logEvent (core::Level::debug, "ui.camera_lane_clicked", fields);
        selectVideoTake (videoTakeId);
        return;
    }

    if (lanesArea().contains (position) && clickTakeLane (position))
        return;

    const auto lane = laneAt (position);
    if (! lane.has_value() || *lane >= current.song.tracks.size())
        return;

    auto hit = hitTestClips (current.song.tracks[*lane].clips, timelineView, position.x);
    if (! hit.has_value())
    {
        selectClip (std::nullopt);
        return;
    }
    // The Clip's fade handles and dB tag win over its body and edges.
    const auto* clip = song::findClip (current.song, hit->clipId);
    if (! OVERDUB_CHECK (clip != nullptr, "ui.hit_clip_missing", { "clip_id", hit->clipId }))
        return;
    const auto clipTop = clipRectangle (layout.trackLanes[*lane], clipColumns (timelineView, *clip)).getY();
    if (const auto handle = hitTestClipHandles (timelineView, *clip, clipTop, position.x, position.y))
        hit->part = *handle;

    selectClip (hit->clipId);
    gesture = ClipDragGesture { ClipDrag { hit->clipId, hit->part, position.x, position.y }, current.song, std::nullopt };
}

void Timeline::mouseDrag (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();
    if (std::holds_alternative<SeekGesture> (gesture))
    {
        trackActions.seek (std::max<song::SampleCount> (0, sampleForX (timelineView, position.x)));
        return;
    }
    auto* clipDrag = std::get_if<ClipDragGesture> (&gesture);
    if (clipDrag == nullptr)
        return;

    auto dragged = songForDrag (clipDrag->songAtDown, clipDrag->drag, timelineView, position.x, position.y);
    if (dragged.has_value())
        clipDrag->preview = std::move (*dragged);
    repaint();
}

void Timeline::mouseUp (const juce::MouseEvent& event)
{
    if (std::holds_alternative<SeekGesture> (gesture))
    {
        gesture = std::monostate {};
        return;
    }
    auto* clipDrag = std::get_if<ClipDragGesture> (&gesture);
    if (clipDrag == nullptr)
        return;

    const auto finished = std::move (*clipDrag);
    gesture = std::monostate {};
    repaint();

    auto dragged = songForDrag (finished.songAtDown, finished.drag, timelineView, event.getPosition().x, event.getPosition().y);
    if (! dragged.has_value() || *dragged == finished.songAtDown)
        return;
    if (onEdit)
        onEdit (std::move (*dragged), labelForDrag (finished.drag.part));
}
} // namespace overdub::ui
