#include "ui/Timeline.h"

#include "core/Decibels.h"
#include "core/Error.h"
#include "core/Log.h"
#include "song/ClipEdits.h"
#include "song/TakeEdits.h"
#include "song/VideoTakeEdits.h"
#include "ui/Colours.h"
#include "ui/MuteSoloButtons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace overdub::ui
{
namespace
{
const juce::Colour coveredColour { 0xffb8c4ff };
const juce::Colour cameraLaneColour { 0xff1c171c };
const juce::Colour takeLaneBackground { 0xff18181a };

// "▸ 3 Takes" folded, "▾ 3 Takes" unfolded.
juce::String takesToggleText (int numTakes, bool unfolded)
{
    const auto arrow = unfolded ? "\xE2\x96\xBE" : "\xE2\x96\xB8";
    return juce::String::fromUTF8 (std::format ("{} {} {}", arrow, numTakes, numTakes == 1 ? "Take" : "Takes").c_str());
}

PixelRect handleRect (const ClipHandleBounds& handles, ClipPart part)
{
    switch (part)
    {
        case ClipPart::fadeInHandle:
            return handles.fadeIn;
        case ClipPart::fadeOutHandle:
            return handles.fadeOut;
        case ClipPart::gainTag:
            return handles.gainTag;
        case ClipPart::body:
        case ClipPart::startEdge:
        case ClipPart::endEdge:
            break;
    }
    return {};
}

juce::Rectangle<int> toRectangle (PixelRect rect)
{
    return { rect.x, rect.y, rect.width, rect.height };
}

} // namespace

std::string clipName (const song::Song& song, const song::Clip& clip)
{
    if (clip.source.kind == song::ClipSourceKind::backing)
    {
        const auto backing = std::ranges::find (song.backingTracks, clip.source.id, &song::BackingTrack::id);
        return backing != song.backingTracks.end() ? backing->originalFilename : "Missing";
    }
    const auto take = std::ranges::find (song.takes, clip.source.id, &song::Take::id);
    if (take == song.takes.end())
        return "Missing";
    const auto pass = std::ranges::find (song.passes, take->passId, &song::Pass::id);
    if (pass == song.passes.end())
        return "Take";
    return std::format ("Pass {}", (pass - song.passes.begin()) + 1);
}

//==============================================================================

TrackHeader::TrackHeader()
{
    armButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    armButton.setWantsKeyboardFocus (false);
    armButton.setTooltip ("Arm");
    armButton.onClick = [this] {
        if (onArm)
            onArm (! armed);
    };
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setInterceptsMouseClicks (false, false);
    takesButton.setWantsKeyboardFocus (false);
    takesButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    takesButton.onClick = [this] {
        if (onToggleTakes)
            onToggleTakes();
    };
    setUpMuteSoloButtons (muteButton, soloButton, [this] { return muted; }, [this] { return soloed; }, onMute, onSolo);
    deleteButton.setWantsKeyboardFocus (false);
    deleteButton.setTooltip ("Delete Track");
    deleteButton.onClick = [this] {
        if (onDelete)
            onDelete();
    };
    addAndMakeVisible (grip);
    addAndMakeVisible (armButton);
    addAndMakeVisible (nameLabel);
    addChildComponent (takesButton);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (deleteButton);
}

void TrackHeader::show (const song::Track& track, bool enabled, bool deletable, int numTakes, bool unfolded)
{
    id = track.id;
    if (const auto colour = trackColour (track); colour != stripeColour)
    {
        stripeColour = colour;
        repaint();
    }
    armed = track.armed;
    nameLabel.setText (juce::String::fromUTF8 (track.name.c_str()), juce::dontSendNotification);
    armButton.setToggleState (track.armed, juce::dontSendNotification);
    armButton.setEnabled (enabled);
    grip.setEnabled (enabled);
    muted = track.mute;
    soloed = track.solo;
    muteButton.setToggleState (track.mute, juce::dontSendNotification);
    soloButton.setToggleState (track.solo, juce::dontSendNotification);
    muteButton.setEnabled (enabled);
    soloButton.setEnabled (enabled);
    deleteButton.setEnabled (deletable);
    takesButton.setVisible (numTakes > 0);
    takesButton.setButtonText (takesToggleText (numTakes, unfolded));
    takesButton.setToggleState (unfolded, juce::dontSendNotification);
}

void TrackHeader::resized()
{
    const auto headerLayout = trackHeaderLayout (getLocalBounds());
    grip.setBounds (headerLayout.grip);
    armButton.setBounds (headerLayout.arm);
    nameLabel.setBounds (headerLayout.name);
    takesButton.setBounds (headerLayout.takesToggle);
    muteButton.setBounds (headerLayout.mute);
    soloButton.setBounds (headerLayout.solo);
    deleteButton.setBounds (headerLayout.deleteButton);
}

void TrackHeader::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (stripeColour);
    g.fillRect (getLocalBounds().removeFromLeft (trackColourStripeWidth));
    g.setColour (colours::line);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

//==============================================================================

Timeline::Timeline (PeaksLookup peaks)
    : peaksFor (std::move (peaks))
{
    cameraHeader.setText ("Camera", juce::dontSendNotification);
    cameraHeader.setColour (juce::Label::textColourId, colours::dimText);
    cameraHeader.setFont (juce::FontOptions (12.0f));
    cameraHeader.setInterceptsMouseClicks (false, false);
    cameraArmButton.setClickingTogglesState (false);
    cameraArmButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    cameraArmButton.setWantsKeyboardFocus (false);
    cameraArmButton.setTooltip ("Arm the camera");
    cameraArmButton.onClick = [this] { emit (SongIntent { intent::SetCameraArmed { ! current.song.cameraArmed } }); };
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    scrollBar.setWantsKeyboardFocus (false);
    verticalScrollBar.setAutoHide (false);
    verticalScrollBar.addListener (this);
    verticalScrollBar.setWantsKeyboardFocus (false);
    trackHeaderColumn.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (cameraHeader);
    addAndMakeVisible (cameraArmButton);
    addAndMakeVisible (trackHeaderColumn);
    addAndMakeVisible (scrollBar);
    addChildComponent (verticalScrollBar);
}

Timeline::~Timeline()
{
    scrollBar.removeListener (this);
    verticalScrollBar.removeListener (this);
}

void Timeline::show (const TimelineState& state)
{
    const auto songChanged = state.song != current.song;
    const auto changed = songChanged || state.playhead != current.playhead || state.playing != current.playing || state.editable != current.editable
                      || state.tracksDeletable != current.tracksDeletable || state.videoTakes != current.videoTakes;
    std::vector<song::Id> previousTrackIds;
    if (songChanged)
        for (const auto& track : current.song.tracks)
            previousTrackIds.push_back (track.id);
    current = state;
    cameraArmButton.setToggleState (current.song.cameraArmed, juce::dontSendNotification);
    cameraArmButton.setEnabled (current.canArmCamera);
    if (! current.editable && std::holds_alternative<TrackDrag> (gesture))
        cancelGesture ("pass_running");
    if (! changed)
        return;

    if (songChanged)
    {
        const auto clipId = selectedClip();
        const auto videoTakeId = selectedVideoTake();
        if ((clipId.has_value() && song::findClip (current.song, *clipId) == nullptr) || (videoTakeId.has_value() && song::findVideoTake (current.song, *videoTakeId) == nullptr))
            setSelection (std::monostate {});
        // A Clip drag carries on over a Song changed by anything else, unless its Clip is gone.
        if (auto* clipDrag = std::get_if<ClipDragGesture> (&gesture))
        {
            if (song::findClip (current.song, clipDrag->drag.clipId) == nullptr)
                cancelGesture ("clip_removed");
            else
                previewClipDrag (*clipDrag);
        }
        // The dragged Track's header, whose grip would end the drag, is about to go.
        if (const auto* drag = std::get_if<TrackDrag> (&gesture); drag != nullptr && song::findTrack (current.song, drag->trackId().value_or ("")) == nullptr)
            cancelGesture ("track_removed");
        rebuildHeaders();
        refreshTakeLanes();
        revealNewTracks (previousTrackIds);
    }
    showHeaders();

    // While playing, page forward when the playhead leaves the visible lanes.
    const auto lanes = lanesArea();
    if (current.playing && ! lanes.isEmpty() && xForSample (timelineView, current.playhead) >= lanes.getRight())
        timelineView.firstSample = current.playhead;

    updateScrollBar();
    repaint();
}

void Timeline::showHeaders()
{
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        const auto& track = current.song.tracks[i];
        const auto numTakes = (int) song::laneTakes (current.song, track.id).size();
        headers[i]->show (track, current.editable, current.tracksDeletable, numTakes, areTakeLanesUnfolded (track.id));
    }
}

void Timeline::refreshTakeLanes()
{
    takeLanes.clear();
    for (const auto& track : current.song.tracks)
        takeLanes.push_back (areTakeLanesUnfolded (track.id) ? takeLaneRows (current.song, track.id) : std::vector<TakeLaneRow> {});
    resized();
    repaint();
}

void Timeline::setTakeLanesUnfolded (const song::Id& trackId, bool unfolded)
{
    if (unfolded)
        unfoldedTracks.insert (trackId);
    else
        unfoldedTracks.erase (trackId);
    core::logEvent (core::Level::debug, "ui.take_lanes_toggled", { { "track_id", trackId }, { "unfolded", unfolded } });
    refreshTakeLanes();
    showHeaders();
}

std::vector<TakeLaneRow> Timeline::takeLanesShown (const song::Id& trackId) const
{
    const auto& tracks = current.song.tracks;
    for (std::size_t i = 0; i < tracks.size() && i < takeLanes.size(); ++i)
        if (tracks[i].id == trackId)
            return takeLanes[i];
    return {};
}

juce::Rectangle<int> Timeline::takeLaneBounds (const song::Id& takeId) const
{
    for (std::size_t i = 0; i < takeLanes.size() && i < layout.takeLanes.size(); ++i)
        for (std::size_t j = 0; j < takeLanes[i].size() && j < layout.takeLanes[i].size(); ++j)
            if (takeLanes[i][j].takeId == takeId)
                return layout.takeLanes[i][j].getIntersection (lanesArea());
    return {};
}

void Timeline::rebuildHeaders()
{
    const auto& tracks = current.song.tracks;
    const auto sameTracks = headers.size() == tracks.size()
                         && std::equal (headers.begin(), headers.end(), tracks.begin(), [] (const auto& header, const song::Track& track) { return header->trackId() == track.id; });
    if (sameTracks)
        return;

    // Headers are kept by Track id, so a reorder moves them, and the grip whose drop reordered
    // the Song survives its own gesture.
    auto previous = std::move (headers);
    headers.clear();
    for (const auto& track : tracks)
    {
        const auto kept = std::ranges::find_if (previous, [&track] (const auto& header) { return header != nullptr && header->trackId() == track.id; });
        if (kept != previous.end())
        {
            headers.push_back (std::move (*kept));
            continue;
        }
        auto header = std::make_unique<TrackHeader>();
        connectHeader (*header, track.id);
        trackHeaderColumn.addAndMakeVisible (*header);
        headers.push_back (std::move (header));
    }
    resized();
}

void Timeline::connectHeader (TrackHeader& header, const song::Id& trackId)
{
    header.grip.onPress = [this, &header] (juce::Point<int> inGrip) { beginTrackDrag (header, inGrip); };
    header.grip.onDrag = [this, &header] (juce::Point<int> inGrip) { dragTrack (header, inGrip); };
    header.grip.onRelease = [this, &header] (juce::Point<int> inGrip) { dropTrack (header, inGrip); };
    header.onArm = [this, trackId] (bool armed) { emit (SongIntent { intent::SetArmed { trackId, armed } }); };
    header.onToggleTakes = [this, trackId] { emit (SongIntent { intent::SetTakeLanesUnfolded { trackId, ! areTakeLanesUnfolded (trackId) } }); };
    header.onMute = [this, trackId] (bool mute) { emit (SongIntent { intent::Edit { intent::SetTrackMute { trackId, mute } } }); };
    header.onSolo = [this, trackId] (bool solo) { emit (SongIntent { intent::Edit { intent::SetTrackSolo { trackId, solo } } }); };
    header.onDelete = [this, trackId] {
        if (onDeleteTrack)
            onDeleteTrack (trackId);
    };
}

std::vector<juce::Range<int>> Timeline::trackRowSpans() const
{
    std::vector<juce::Range<int>> spans;
    const auto top = layout.tracksViewport.getY() - verticalScrollPixels;
    for (const auto& row : layout.trackRows)
        spans.push_back (row + top);
    return spans;
}

void Timeline::revealNewTracks (const std::vector<song::Id>& previousTrackIds)
{
    const auto& tracks = current.song.tracks;
    std::optional<std::size_t> first;
    std::size_t last = 0;
    for (std::size_t i = 0; i < tracks.size() && i < layout.trackRows.size(); ++i)
    {
        if (std::ranges::find (previousTrackIds, tracks[i].id) != previousTrackIds.end())
            continue;
        if (! first.has_value())
            first = i;
        last = i;
    }
    if (! first.has_value())
        return;

    const auto revealed = revealTimelineScroll (verticalScrollPixels, layout.trackRows[*first].getStart(), layout.trackRows[last].getEnd(), layout.tracksViewport.getHeight());
    core::logEvent (core::Level::debug, "ui.timeline_tracks_revealed", { { "first_track_id", tracks[*first].id }, { "new_tracks", last - *first + 1 }, { "from", verticalScrollPixels }, { "to", revealed } });
    setVerticalScroll (revealed);
}

void Timeline::setVerticalScroll (int pixels)
{
    if (pixels == verticalScrollPixels)
        return;
    verticalScrollPixels = pixels;
    layOutRows();
    repaint();
}

void Timeline::setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel)
{
    timelineView.firstSample = std::max<song::SampleCount> (0, firstSample);
    timelineView.samplesPerPixel = std::clamp (samplesPerPixel, minSamplesPerPixel, maxSamplesPerPixel);
    if (! scrollBarReporting)
        updateScrollBar();
    repaint();
}

std::optional<song::Id> Timeline::selectedClip() const
{
    if (const auto* clip = std::get_if<ClipSelection> (&selection))
        return clip->clipId;
    return std::nullopt;
}

std::optional<song::Id> Timeline::selectedVideoTake() const
{
    if (const auto* videoTake = std::get_if<VideoTakeSelection> (&selection))
        return videoTake->videoTakeId;
    return std::nullopt;
}

void Timeline::selectClip (std::optional<song::Id> clipId)
{
    setSelection (clipId.has_value() ? Selection { ClipSelection { std::move (*clipId) } } : Selection {});
}

void Timeline::selectVideoTake (std::optional<song::Id> videoTakeId)
{
    setSelection (videoTakeId.has_value() ? Selection { VideoTakeSelection { std::move (*videoTakeId) } } : Selection {});
}

bool Timeline::isSelectedClip (const song::Id& clipId) const
{
    const auto* clip = std::get_if<ClipSelection> (&selection);
    return clip != nullptr && clip->clipId == clipId;
}

void Timeline::setSelection (Selection next)
{
    if (next == selection)
        return;
    selection = std::move (next);
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

std::optional<juce::Colour> Timeline::clipColour (const song::Id& clipId) const
{
    for (const auto& track : shownSong().tracks)
        if (std::ranges::contains (track.clips, clipId, &song::Clip::id))
            return trackColour (track);
    return std::nullopt;
}

std::optional<juce::Colour> Timeline::takeLaneColourOf (const song::Id& takeId) const
{
    for (std::size_t i = 0; i < takeLanes.size(); ++i)
        if (std::ranges::contains (takeLanes[i], takeId, &TakeLaneRow::takeId))
            return takeLaneColour (current.song.tracks[i]);
    return std::nullopt;
}

juce::Rectangle<int> Timeline::clipBounds (const song::Id& clipId) const
{
    const auto& tracks = shownSong().tracks;
    for (std::size_t i = 0; i < tracks.size() && i < layout.trackLanes.size(); ++i)
    {
        for (const auto& clip : tracks[i].clips)
        {
            if (clip.id != clipId)
                continue;
            const auto lane = layout.trackLanes[i];
            return clipRectangle (lane, clipColumns (timelineView, clip)).getIntersection (lanesArea());
        }
    }
    return {};
}

juce::Rectangle<int> Timeline::videoTakeBounds (const song::Id& videoTakeId) const
{
    const auto span = std::ranges::find (current.videoTakes, videoTakeId, &video::VideoTakeSpan::videoTakeId);
    if (span == current.videoTakes.end())
        return {};
    const auto left = xForSample (timelineView, span->start);
    const auto right = span->readable ? std::max (left + 1, xForSample (timelineView, span->end)) : left + unreadableVideoTakeWidth;
    const auto lane = layout.cameraLane;
    return juce::Rectangle<int> (left, lane.getY() + 4, right - left, lane.getHeight() - 8).getIntersection (lane);
}

juce::Rectangle<int> Timeline::handleBounds (const song::Id& clipId, ClipPart part) const
{
    const auto& tracks = shownSong().tracks;
    for (std::size_t i = 0; i < tracks.size() && i < layout.trackLanes.size(); ++i)
    {
        for (const auto& clip : tracks[i].clips)
        {
            if (clip.id != clipId)
                continue;
            const auto clipTop = clipRectangle (layout.trackLanes[i], clipColumns (timelineView, clip)).getY();
            const auto handles = clipHandleBounds (timelineView, clip, clipTop);
            const auto rect = handleRect (handles, part);
            return toRectangle (rect).getIntersection (lanesArea());
        }
    }
    return {};
}

juce::Rectangle<int> Timeline::clipRectangle (juce::Rectangle<int> lane, ClipColumns columns)
{
    return { columns.left, lane.getY() + 3, columns.right - columns.left, lane.getHeight() - 6 };
}

const song::Song& Timeline::shownSong() const
{
    if (const auto* clipDrag = std::get_if<ClipDragGesture> (&gesture); clipDrag != nullptr && clipDrag->preview.has_value())
        return *clipDrag->preview;
    return current.song;
}

juce::Rectangle<int> Timeline::lanesArea() const
{
    return layout.tracksViewport.withLeft (layout.ruler.getX());
}

void Timeline::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto scroll = wheelScroll (wheel.deltaX, wheel.deltaY, event.mods.isShiftDown());
    if (scroll.timePixels != 0)
        emit (SongIntent { intent::SetTimelineView { timelineView.firstSample + (song::SampleCount) scroll.timePixels * timelineView.samplesPerPixel, timelineView.samplesPerPixel } });
    if (scroll.verticalPixels != 0)
        emit (SongIntent { intent::SetVerticalScroll { verticalScrollPixels + scroll.verticalPixels } });
}

void Timeline::mouseMagnify (const juce::MouseEvent& event, float scaleFactor)
{
    const auto pinch = pinchZoomed (timelineView, pinchPendingLevels, scaleFactor, event.x);
    pinchPendingLevels = pinch.pendingLevels;
    emit (SongIntent { intent::SetTimelineView { pinch.view.firstSample, pinch.view.samplesPerPixel } });
}

void Timeline::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &verticalScrollBar)
    {
        emit (SongIntent { intent::SetVerticalScroll { (int) std::lround (newRangeStart) } });
        return;
    }
    const juce::ScopedValueSetter<bool> reporting (scrollBarReporting, true);
    emit (SongIntent { intent::SetTimelineView { std::max<song::SampleCount> (0, (song::SampleCount) newRangeStart), timelineView.samplesPerPixel } });
}

void Timeline::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

void Timeline::updateScrollBar()
{
    const auto visibleSamples = (double) std::max (1, layout.ruler.getWidth()) * (double) timelineView.samplesPerPixel;
    song::SampleCount songEnd = 0;
    for (const auto& track : current.song.tracks)
        for (const auto& clip : track.clips)
            songEnd = std::max (songEnd, clip.timelineStart + clip.length);
    const auto margin = (song::SampleCount) std::max (1, current.song.sampleRate) * 10;
    const auto total = std::max ((double) (songEnd + margin), (double) timelineView.firstSample + visibleSamples);
    scrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    scrollBar.setCurrentRange ((double) timelineView.firstSample, visibleSamples, juce::dontSendNotification);
}

void Timeline::resized()
{
    layOutRows();
}

void Timeline::layOutRows()
{
    std::vector<int> takeLanesPerTrack;
    for (std::size_t i = 0; i < headers.size(); ++i)
        takeLanesPerTrack.push_back (i < takeLanes.size() ? (int) takeLanes[i].size() : 0);
    layout = timelineLayout (getLocalBounds(), takeLanesPerTrack, verticalScrollPixels);
    const auto clamped = clampTimelineScroll (verticalScrollPixels, layout.tracksHeight, layout.tracksViewport.getHeight());
    if (clamped != verticalScrollPixels)
    {
        verticalScrollPixels = clamped;
        layout = timelineLayout (getLocalBounds(), takeLanesPerTrack, verticalScrollPixels);
    }

    timelineView.laneX = layout.ruler.getX();
    cameraHeader.setBounds (layout.cameraHeader.withRight (layout.cameraArm.getX()).reduced (8, 0));
    cameraArmButton.setBounds (layout.cameraArm);
    scrollBar.setBounds (layout.scrollBar);
    const auto headerColumn = layout.tracksViewport.withWidth (laneHeaderWidth);
    trackHeaderColumn.setBounds (headerColumn);
    for (std::size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (layout.trackHeaders[i] - headerColumn.getPosition());

    verticalScrollBar.setVisible (! layout.verticalScrollBar.isEmpty());
    verticalScrollBar.setBounds (layout.verticalScrollBar);
    verticalScrollBar.setRangeLimits (0.0, (double) std::max (layout.tracksHeight, layout.tracksViewport.getHeight()), juce::dontSendNotification);
    verticalScrollBar.setCurrentRange ((double) verticalScrollPixels, (double) layout.tracksViewport.getHeight(), juce::dontSendNotification);
    updateScrollBar();
}

//==============================================================================

void Timeline::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    paintRuler (g);
    paintCameraLane (g);

    const auto& song = shownSong();
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (lanesArea());
        for (std::size_t i = 0; i < song.tracks.size() && i < layout.trackLanes.size(); ++i)
            paintLane (g, song.tracks[i], layout.trackLanes[i]);
        for (std::size_t i = 0; i < takeLanes.size() && i < layout.takeLanes.size(); ++i)
            for (std::size_t j = 0; j < takeLanes[i].size() && j < layout.takeLanes[i].size(); ++j)
                paintTakeLane (g, takeLanes[i][j], takeLaneColour (current.song.tracks[i]), layout.takeLanes[i][j]);
    }
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (layout.tracksViewport);
        for (std::size_t i = 0; i < takeLanes.size() && i < layout.takeLaneHeaders.size(); ++i)
            for (std::size_t j = 0; j < takeLanes[i].size() && j < layout.takeLaneHeaders[i].size(); ++j)
                paintTakeLaneHeader (g, takeLanes[i][j], layout.takeLaneHeaders[i][j]);
    }
    paintPlayhead (g);
}

void Timeline::paintOverChildren (juce::Graphics& g)
{
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (layout.tracksViewport);
        trackDrag().paint (g, layout.tracksViewport.getX(), layout.tracksViewport.getRight());
    }
    // Over the Track headers too, so the whole Timeline reads as the drop target.
    if (! isDropHighlighted())
        return;
    g.setColour (colours::accent);
    g.drawRect (getLocalBounds(), 3);
}

void Timeline::paintRuler (juce::Graphics& g) const
{
    g.setColour (colours::panel);
    g.fillRect (layout.rulerCorner);
    g.fillRect (layout.ruler);
    g.setColour (colours::line);
    g.drawHorizontalLine (layout.ruler.getBottom() - 1, (float) layout.rulerCorner.getX(), (float) layout.ruler.getRight());

    const auto sampleRate = std::max (1, current.song.sampleRate);
    const auto step = rulerStepSeconds (timelineView.samplesPerPixel, sampleRate);
    const auto firstSecond = (double) timelineView.firstSample / sampleRate;
    const auto lastSecond = (double) sampleForX (timelineView, layout.ruler.getRight()) / sampleRate;

    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (layout.ruler);
    g.setFont (juce::FontOptions (10.0f));
    for (auto tick = std::floor (firstSecond / step); tick * step <= lastSecond; tick += 1.0)
    {
        const auto seconds = tick * step;
        const auto x = xForSample (timelineView, (song::SampleCount) std::llround (seconds * sampleRate));
        g.setColour (colours::line.brighter (0.3f));
        g.drawVerticalLine (x, (float) layout.ruler.getY(), (float) layout.ruler.getBottom());
        g.setColour (colours::dimText);
        g.drawText (juce::String (rulerLabel (seconds)), x + 3, layout.ruler.getY() + 2, 60, 12, juce::Justification::centredLeft);
    }
}

void Timeline::paintCameraLane (juce::Graphics& g) const
{
    g.setColour (colours::panel);
    g.fillRect (layout.cameraHeader);
    g.setColour (cameraLaneColour);
    g.fillRect (layout.cameraLane);
    g.setColour (colours::line);
    g.drawHorizontalLine (layout.cameraLane.getBottom() - 1, (float) layout.cameraHeader.getX(), (float) layout.cameraLane.getRight());
    g.setFont (juce::FontOptions (11.0f));
    juce::Graphics::ScopedSaveState clipToLane (g);
    g.reduceClipRegion (layout.cameraLane);
    for (const auto& span : current.videoTakes)
    {
        const auto block = videoTakeBounds (span.videoTakeId);
        if (block.isEmpty())
            continue;
        const auto isSelected = selectedVideoTake() == span.videoTakeId;
        g.setColour (span.readable ? colours::videoTake : colours::videoTake.withAlpha (0.4f));
        g.fillRoundedRectangle (block.toFloat(), 4.0f);
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (block.toFloat().reduced (isSelected ? 1.0f : 0.5f), 4.0f, isSelected ? 2.0f : 1.0f);
        g.setColour (colours::text);
        if (span.readable)
            g.drawText (juce::String::fromUTF8 (span.title.c_str()), block.reduced (6, 0), juce::Justification::centredLeft, true);
        else
            g.drawText (juce::String::fromUTF8 ((span.title + " (movie missing)").c_str()), block.withX (block.getRight() + 4).withWidth (200), juce::Justification::centredLeft, true);
    }
    if (current.videoTakes.empty())
    {
        g.setColour (colours::dimText);
        g.drawText ("Arm the camera to record a Video Take with each Pass.", layout.cameraLane.reduced (8, 0), juce::Justification::centredLeft);
    }
}

void Timeline::paintLane (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const
{
    g.setColour (colours::background.brighter (0.03f));
    g.fillRect (lane);
    g.setColour (colours::line);
    g.drawHorizontalLine (lane.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());

    for (const auto& clip : track.clips)
    {
        const auto columns = clipColumns (timelineView, clip);
        if (columns.right <= lane.getX() || columns.left >= lane.getRight())
            continue;
        paintClip (g, clip, trackColour (track), clipRectangle (lane, columns));
    }
    paintCoveredSpans (g, track, lane);
}

void Timeline::paintClip (juce::Graphics& g, const song::Clip& clip, juce::Colour body, juce::Rectangle<int> clipArea) const
{
    const auto& song = shownSong();
    const auto isSelected = isSelectedClip (clip.id);
    const auto area = clipArea.toFloat();
    g.setColour (body);
    g.fillRoundedRectangle (area, 4.0f);

    // Waveform: one column per visible pixel, under the name strip.
    const auto waveArea = clipArea.withTrimmedTop (14).reduced (0, 2);
    if (waveArea.getHeight() > 2)
    {
        const auto& peaks = peaksFor (clip.source);
        const auto firstX = std::max (clipArea.getX(), layout.ruler.getX());
        const auto lastX = std::min (clipArea.getRight(), layout.ruler.getRight());
        if (! peaks.channels.empty() && lastX > firstX)
        {
            const auto sourceStart = clip.sourceOffset + (sampleForX (timelineView, firstX) - clip.timelineStart);
            const auto columns = waveformColumns (peaks, sourceStart, timelineView.samplesPerPixel, lastX - firstX);
            const auto centre = (float) waveArea.getCentreY();
            const auto halfHeight = (float) waveArea.getHeight() * 0.5f;
            const auto gain = (float) core::decibelsToGain (clip.gainDb);
            g.setColour (colours::onTrackColour.withAlpha (0.55f));
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                const auto top = centre - std::clamp (columns[i].maximum * gain, -1.0f, 1.0f) * halfHeight;
                const auto bottom = centre - std::clamp (columns[i].minimum * gain, -1.0f, 1.0f) * halfHeight;
                g.drawVerticalLine (firstX + (int) i, top, std::max (top + 1.0f, bottom));
            }
        }
    }

    g.setColour (colours::onTrackColour.withAlpha (0.9f));
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const auto nameRight = clipArea.getRight() - clipEdgeGrabWidth - gainTagWidth - 4;
    g.drawText (juce::String::fromUTF8 (clipName (song, clip).c_str()), clipArea.withHeight (14).withRight (std::max (clipArea.getX(), nameRight)).reduced (4, 0), juce::Justification::centredLeft, true);

    // Over the name, so a fade handle near the Clip's start stays visible.
    paintFadesAndGain (g, clip, clipArea);

    g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
    g.drawRoundedRectangle (area.reduced (isSelected ? 1.0f : 0.5f), 4.0f, isSelected ? 2.0f : 1.0f);
    if (isSelected)
    {
        g.setColour (colours::onTrackColour);
        g.drawRoundedRectangle (area.reduced (2.5f), 3.0f, 1.0f);
    }
}

void Timeline::paintFadesAndGain (juce::Graphics& g, const song::Clip& clip, juce::Rectangle<int> clipArea) const
{
    const auto area = clipArea.toFloat();
    const auto fadeInEnd = (float) xForSample (timelineView, clip.timelineStart + clip.fadeInLength);
    const auto fadeOutStart = (float) xForSample (timelineView, clip.timelineStart + clip.length - clip.fadeOutLength);

    // Shade above each fade line, the line rising from silence (bottom) to full level (top).
    juce::Path shade;
    if (clip.fadeInLength > 0)
        shade.addTriangle (area.getX(), area.getY(), fadeInEnd, area.getY(), area.getX(), area.getBottom());
    if (clip.fadeOutLength > 0)
        shade.addTriangle (fadeOutStart, area.getY(), area.getRight(), area.getY(), area.getRight(), area.getBottom());
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillPath (shade);
    g.setColour (colours::onTrackColour.withAlpha (0.9f));
    if (clip.fadeInLength > 0)
        g.drawLine (area.getX(), area.getBottom(), fadeInEnd, area.getY(), 1.0f);
    if (clip.fadeOutLength > 0)
        g.drawLine (fadeOutStart, area.getY(), area.getRight(), area.getBottom(), 1.0f);

    const auto handles = clipHandleBounds (timelineView, clip, clipArea.getY());
    const auto isSelected = isSelectedClip (clip.id);
    g.setColour (juce::Colours::white.withAlpha (isSelected ? 0.95f : 0.55f));
    for (const auto& handle : { handles.fadeIn, handles.fadeOut })
        g.fillRoundedRectangle (toRectangle (handle).toFloat(), 2.0f);

    if (! handles.gainTag.isEmpty())
    {
        const auto tag = toRectangle (handles.gainTag);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (tag.toFloat(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::FontOptions (9.0f));
        g.drawText (juce::String (decibelsText (clip.gainDb)), tag, juce::Justification::centred, false);
    }
}

void Timeline::paintCoveredSpans (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const
{
    for (const auto& span : song::coveredSpans (track))
    {
        const auto left = xForSample (timelineView, span.start);
        const auto right = std::max (left + 1, xForSample (timelineView, span.end));
        const juce::Rectangle<int> strip { left, lane.getBottom() - 9, right - left, 5 };
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (strip);
        g.setColour (coveredColour.withAlpha (0.8f));
        for (int x = strip.getX() - strip.getHeight(); x < strip.getRight(); x += 5)
            g.drawLine ((float) x, (float) strip.getBottom(), (float) (x + strip.getHeight()), (float) strip.getY(), 1.5f);
    }
}

void Timeline::paintPlayhead (juce::Graphics& g) const
{
    const auto x = xForSample (timelineView, current.playhead);
    if (x < layout.ruler.getX() || x >= layout.ruler.getRight())
        return;
    g.setColour (current.editable ? juce::Colours::white : colours::rec);
    g.drawVerticalLine (x, (float) layout.ruler.getY(), (float) layout.scrollBar.getY());
}

void Timeline::paintTakeLane (juce::Graphics& g, const TakeLaneRow& row, juce::Colour colour, juce::Rectangle<int> lane) const
{
    g.setColour (takeLaneBackground);
    g.fillRect (lane);
    g.setColour (colours::line);
    g.drawHorizontalLine (lane.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());
    if (! row.recorded.has_value())
        return;

    // The recorded span: a dim block. Where the Take is heard now: filled in.
    const auto strip = lane.reduced (0, 3);
    const auto spanRect = [&] (song::SampleSpan span) {
        const auto left = xForSample (timelineView, span.start);
        const auto right = std::max (left + 2, xForSample (timelineView, span.end));
        return juce::Rectangle<int> (left, strip.getY(), right - left, strip.getHeight());
    };
    const auto block = spanRect (*row.recorded);
    g.setColour (colour.withAlpha (0.4f));
    g.fillRoundedRectangle (block.toFloat(), 3.0f);
    g.setColour (colour);
    for (const auto& heard : row.heard)
        g.fillRect (spanRect (heard));
    g.setColour (colour.brighter (0.3f));
    g.drawRoundedRectangle (block.toFloat().reduced (0.5f), 3.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (juce::String (row.title), block.reduced (4, 0), juce::Justification::centredLeft, true);
}

void Timeline::paintTakeLaneHeader (juce::Graphics& g, const TakeLaneRow& row, juce::Rectangle<int> header) const
{
    g.setColour (colours::panel.darker (0.15f));
    g.fillRect (header);
    g.setColour (colours::line);
    g.drawHorizontalLine (header.getBottom() - 1, (float) header.getX(), (float) header.getRight());
    g.drawVerticalLine (header.getRight() - 1, (float) header.getY(), (float) header.getBottom());

    auto area = header.reduced (8, 0).withTrimmedLeft (10);
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (juce::String (row.title), area.removeFromLeft (54), juce::Justification::centredLeft, true);
    g.setColour (row.status == song::HeardStatus::notHeard ? colours::dimText : colours::text.withAlpha (0.8f));
    g.setFont (juce::FontOptions (10.5f));
    g.drawText (juce::String (std::string (song::toString (row.status))), area, juce::Justification::centredRight, true);
}
} // namespace overdub::ui
