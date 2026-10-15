#include "ui/Timeline.h"

#include "core/Error.h"
#include "song/ClipEdits.h"
#include "song/TakeEdits.h"
#include "ui/Colours.h"
#include "ui/FileDrop.h"
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
const juce::Colour takeLaneColour { 0xff18181a };

// "▸ 3 Takes" folded, "▾ 3 Takes" unfolded.
juce::String takesToggleText (int numTakes, bool unfolded)
{
    const auto arrow = unfolded ? "\xE2\x96\xBE" : "\xE2\x96\xB8";
    return juce::String::fromUTF8 (std::format ("{} {} {}", arrow, numTakes, numTakes == 1 ? "Take" : "Takes").c_str());
}

// Seconds between labelled ruler ticks: the first that puts them at least 70 pixels apart.
double rulerStepSeconds (song::SampleCount samplesPerPixel, int sampleRate)
{
    constexpr std::array steps { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
    for (const auto step : steps)
        if (step * sampleRate / (double) samplesPerPixel >= 70.0)
            return step;
    return steps.back();
}

// A Clip's rectangle in its lane: its columns, inset 3 pixels from the lane's top and bottom.
juce::Rectangle<int> clipRectangle (juce::Rectangle<int> lane, ClipColumns columns)
{
    return { columns.left, lane.getY() + 3, columns.right - columns.left, lane.getHeight() - 6 };
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

std::string rulerLabel (double seconds)
{
    return std::format ("{:g} s", seconds); // "0.25 s", "1.5 s", "10 s"
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
    addAndMakeVisible (armButton);
    addAndMakeVisible (nameLabel);
    addChildComponent (takesButton);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
}

void TrackHeader::show (const song::Track& track, bool enabled, int numTakes, bool unfolded)
{
    id = track.id;
    armed = track.armed;
    nameLabel.setText (juce::String::fromUTF8 (track.name.c_str()), juce::dontSendNotification);
    armButton.setToggleState (track.armed, juce::dontSendNotification);
    armButton.setEnabled (enabled);
    muted = track.mute;
    soloed = track.solo;
    muteButton.setToggleState (track.mute, juce::dontSendNotification);
    soloButton.setToggleState (track.solo, juce::dontSendNotification);
    muteButton.setEnabled (enabled);
    soloButton.setEnabled (enabled);
    takesButton.setVisible (numTakes > 0);
    takesButton.setButtonText (takesToggleText (numTakes, unfolded));
    takesButton.setToggleState (unfolded, juce::dontSendNotification);
}

void TrackHeader::resized()
{
    const auto headerLayout = trackHeaderLayout (getLocalBounds());
    armButton.setBounds (headerLayout.arm);
    nameLabel.setBounds (headerLayout.name);
    takesButton.setBounds (headerLayout.takesToggle);
    muteButton.setBounds (headerLayout.mute);
    soloButton.setBounds (headerLayout.solo);
}

void TrackHeader::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::line);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

//==============================================================================

Timeline::Timeline()
{
    cameraHeader.setText ("Camera", juce::dontSendNotification);
    cameraHeader.setColour (juce::Label::textColourId, colours::dimText);
    cameraHeader.setFont (juce::FontOptions (12.0f));
    cameraHeader.setInterceptsMouseClicks (false, false);
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    scrollBar.setWantsKeyboardFocus (false);
    addAndMakeVisible (cameraHeader);
    addAndMakeVisible (scrollBar);
}

Timeline::~Timeline()
{
    scrollBar.removeListener (this);
}

void Timeline::show (const TimelineState& state)
{
    const auto songChanged = state.song != current.song;
    const auto changed = songChanged || state.playhead != current.playhead || state.playing != current.playing || state.editable != current.editable
                      || state.videoTakes != current.videoTakes;
    current = state;
    if (! changed)
        return;

    if (songChanged)
    {
        if (selected.has_value() && song::findClip (current.song, *selected) == nullptr)
            selectClip (std::nullopt);
        rebuildHeaders();
        refreshTakeLanes();
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
        headers[i]->show (track, current.editable, numTakes, areTakeLanesUnfolded (track.id));
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

void Timeline::rebuildHeaders()
{
    const auto& tracks = current.song.tracks;
    const auto sameTracks = headers.size() == tracks.size()
                         && std::equal (headers.begin(), headers.end(), tracks.begin(), [] (const auto& header, const song::Track& track) { return header->trackId() == track.id; });
    if (sameTracks)
        return;

    headers.clear();
    for (const auto& track : tracks)
    {
        auto header = std::make_unique<TrackHeader>();
        const auto trackId = track.id;
        header->onArm = [this, trackId] (bool armed) {
            if (onArm)
                onArm (trackId, armed);
        };
        header->onToggleTakes = [this, trackId] { setTakeLanesUnfolded (trackId, ! areTakeLanesUnfolded (trackId)); };
        header->onMute = [this, trackId] (bool mute) {
            if (onMute)
                onMute (trackId, mute);
        };
        header->onSolo = [this, trackId] (bool solo) {
            if (onSolo)
                onSolo (trackId, solo);
        };
        addAndMakeVisible (*header);
        headers.push_back (std::move (header));
    }
    resized();
}

void Timeline::setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel)
{
    timelineView.firstSample = std::max<song::SampleCount> (0, firstSample);
    timelineView.samplesPerPixel = std::clamp (samplesPerPixel, minSamplesPerPixel, maxSamplesPerPixel);
    updateScrollBar();
    repaint();
}

void Timeline::zoomIn()
{
    setView (timelineView.firstSample, zoomedIn (timelineView).samplesPerPixel);
}

void Timeline::zoomOut()
{
    setView (timelineView.firstSample, zoomedOut (timelineView).samplesPerPixel);
}

void Timeline::selectClip (std::optional<song::Id> clipId)
{
    if (clipId == selected)
        return;
    selected = std::move (clipId);
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
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

juce::Rectangle<int> Timeline::lanesArea() const
{
    return getLocalBounds().withTop (layout.cameraLane.getBottom()).withBottom (layout.scrollBar.getY()).withLeft (layout.ruler.getX());
}

bool Timeline::isInterestedInFileDrag (const juce::StringArray& files)
{
    return canImportFiles && canImportFiles() && ! importableAudioFiles (files).empty();
}

void Timeline::fileDragEnter (const juce::StringArray& files, int, int)
{
    dropHighlighted = isInterestedInFileDrag (files);
    repaint();
}

void Timeline::fileDragExit (const juce::StringArray&)
{
    dropHighlighted = false;
    repaint();
}

void Timeline::filesDropped (const juce::StringArray& files, int, int)
{
    dropHighlighted = false;
    repaint();
    auto importable = importableAudioFiles (files);
    const auto accepted = isInterestedInFileDrag (files) && onFilesDropped != nullptr;
    core::logEvent (core::Level::debug, "ui.files_dropped", { { "dropped", files.size() }, { "importable", importable.size() }, { "accepted", accepted } });
    if (accepted)
        onFilesDropped (std::move (importable));
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

void Timeline::mouseDown (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();
    if (! current.editable)
        return;

    if (layout.ruler.contains (position))
    {
        seeking = true;
        if (onSeek)
            onSeek (std::max<song::SampleCount> (0, sampleForX (timelineView, position.x)));
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
    activeDrag = ActiveDrag { ClipDrag { hit->clipId, hit->part, position.x, position.y }, current.song };
}

void Timeline::mouseDrag (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();
    if (seeking)
    {
        if (onSeek)
            onSeek (std::max<song::SampleCount> (0, sampleForX (timelineView, position.x)));
        return;
    }
    if (! activeDrag.has_value())
        return;

    auto dragged = songForDrag (activeDrag->songAtDown, activeDrag->drag, timelineView, position.x, position.y);
    if (dragged.has_value())
        preview = std::move (*dragged);
    repaint();
}

void Timeline::mouseUp (const juce::MouseEvent& event)
{
    seeking = false;
    if (! activeDrag.has_value())
        return;

    const auto finished = std::move (*activeDrag);
    activeDrag.reset();
    preview.reset();
    repaint();

    auto dragged = songForDrag (finished.songAtDown, finished.drag, timelineView, event.getPosition().x, event.getPosition().y);
    if (! dragged.has_value() || *dragged == finished.songAtDown)
        return;
    if (onEdit)
        onEdit (std::move (*dragged), labelForDrag (finished.drag.part));
}

void Timeline::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto delta = wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY;
    const auto pixels = (song::SampleCount) std::lround (-delta * 400.0f);
    setView (timelineView.firstSample + pixels * timelineView.samplesPerPixel, timelineView.samplesPerPixel);
}

void Timeline::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    timelineView.firstSample = std::max<song::SampleCount> (0, (song::SampleCount) newRangeStart);
    repaint();
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
    std::vector<int> takeLanesPerTrack;
    for (std::size_t i = 0; i < headers.size(); ++i)
        takeLanesPerTrack.push_back (i < takeLanes.size() ? (int) takeLanes[i].size() : 0);
    layout = timelineLayout (getLocalBounds(), takeLanesPerTrack);
    timelineView.laneX = layout.ruler.getX();
    cameraHeader.setBounds (layout.cameraHeader.reduced (8, 0));
    scrollBar.setBounds (layout.scrollBar);
    const auto lanesBottom = layout.scrollBar.getY();
    for (std::size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (layout.trackHeaders[i].withBottom (std::min (layout.trackHeaders[i].getBottom(), lanesBottom)));
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
                paintTakeLane (g, takeLanes[i][j], layout.takeLanes[i][j]);
    }
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (getLocalBounds().withTop (layout.cameraLane.getBottom()).withBottom (layout.scrollBar.getY()));
        for (std::size_t i = 0; i < takeLanes.size() && i < layout.takeLaneHeaders.size(); ++i)
            for (std::size_t j = 0; j < takeLanes[i].size() && j < layout.takeLaneHeaders[i].size(); ++j)
                paintTakeLaneHeader (g, takeLanes[i][j], layout.takeLaneHeaders[i][j]);
    }
    paintPlayhead (g);
}

void Timeline::paintOverChildren (juce::Graphics& g)
{
    // Over the Track headers too, so the whole Timeline reads as the drop target.
    if (! dropHighlighted)
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
        g.setColour (span.readable ? colours::videoTake : colours::videoTake.withAlpha (0.4f));
        g.fillRoundedRectangle (block.toFloat(), 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (block.toFloat().reduced (0.5f), 4.0f, 1.0f);
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
        paintClip (g, clip, clipRectangle (lane, columns));
    }
    paintCoveredSpans (g, track, lane);
}

void Timeline::paintClip (juce::Graphics& g, const song::Clip& clip, juce::Rectangle<int> clipArea) const
{
    const auto& song = shownSong();
    const auto isSelected = selected == clip.id;
    const auto area = clipArea.toFloat();
    g.setColour (clip.source.kind == song::ClipSourceKind::backing ? colours::backingClip : colours::takeClip);
    g.fillRoundedRectangle (area, 4.0f);

    // Waveform: one column per visible pixel, under the name strip.
    const auto waveArea = clipArea.withTrimmedTop (14).reduced (0, 2);
    if (peaksFor && waveArea.getHeight() > 2)
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
            const auto gain = (float) song::linearGain (clip.gainDb);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                const auto top = centre - std::clamp (columns[i].maximum * gain, -1.0f, 1.0f) * halfHeight;
                const auto bottom = centre - std::clamp (columns[i].minimum * gain, -1.0f, 1.0f) * halfHeight;
                g.drawVerticalLine (firstX + (int) i, top, std::max (top + 1.0f, bottom));
            }
        }
    }

    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const auto nameRight = clipArea.getRight() - clipEdgeGrabWidth - gainTagWidth - 4;
    g.drawText (juce::String::fromUTF8 (clipName (song, clip).c_str()), clipArea.withHeight (14).withRight (std::max (clipArea.getX(), nameRight)).reduced (4, 0), juce::Justification::centredLeft, true);

    // Over the name, so a fade handle near the Clip's start stays visible.
    paintFadesAndGain (g, clip, clipArea);

    g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
    g.drawRoundedRectangle (area.reduced (isSelected ? 1.0f : 0.5f), 4.0f, isSelected ? 2.0f : 1.0f);
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
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    if (clip.fadeInLength > 0)
        g.drawLine (area.getX(), area.getBottom(), fadeInEnd, area.getY(), 1.0f);
    if (clip.fadeOutLength > 0)
        g.drawLine (fadeOutStart, area.getY(), area.getRight(), area.getBottom(), 1.0f);

    const auto handles = clipHandleBounds (timelineView, clip, clipArea.getY());
    const auto isSelected = selected == clip.id;
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

void Timeline::paintTakeLane (juce::Graphics& g, const TakeLaneRow& row, juce::Rectangle<int> lane) const
{
    g.setColour (takeLaneColour);
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
    g.setColour (colours::takeClip.withAlpha (0.35f));
    g.fillRoundedRectangle (block.toFloat(), 3.0f);
    g.setColour (colours::takeClip.brighter (0.4f));
    for (const auto& heard : row.heard)
        g.fillRect (spanRect (heard));
    g.setColour (colours::takeClip.brighter (0.2f));
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
