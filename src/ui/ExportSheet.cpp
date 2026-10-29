#include "ui/ExportSheet.h"

#include "song/LoopEdits.h"
#include "ui/Availability.h"
#include "ui/Colours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace overdub::ui
{
namespace
{
constexpr int formWidth = 560;
constexpr int panelPadding = 24;
// The form's rows (resized): without the MP4's Video Take rows and the length rows, those, and
// the length's heading and each of its choices.
constexpr int formHeight = 292;
constexpr int videoTakeRowsHeight = 52;
constexpr int lengthHeadingHeight = 26;
constexpr int lengthChoiceHeight = 24;
constexpr int formatRadioGroup = 3701;

// "1:23.4": minutes and seconds to a tenth, rounded down.
std::string minutesText (song::SampleCount samples, int sampleRate)
{
    const auto tenths = sampleRate > 0 && samples > 0 ? samples * 10 / sampleRate : 0;
    char text[32];
    std::snprintf (text, sizeof (text), "%lld:%02lld.%lld", (long long) (tenths / 600), (long long) (tenths / 10 % 60), (long long) (tenths % 10));
    return text;
}

std::string rangeText (exporting::SampleRange range, int sampleRate)
{
    return minutesText (range.start, sampleRate) + " \xe2\x80\x93 " + minutesText (range.end, sampleRate);
}

void styleHeading (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, colours::dimText);
    label.setFont (juce::FontOptions (11.0f));
}
} // namespace

ExportSpanBarState exportSpanBar (exporting::ExportFormat format,
                                  exporting::VideoLength length,
                                  song::SampleCount clipsEnd,
                                  const std::optional<video::VideoTakeSpan>& videoTake,
                                  const std::optional<exporting::SampleRange>& loop)
{
    const auto movie = format == exporting::ExportFormat::mp4 && videoTake.has_value();
    if (! movie)
    {
        if (loop.has_value())
            return { .scaleEnd = std::max (clipsEnd, loop->end), .exported = *loop };
        return { .scaleEnd = clipsEnd, .exported = { 0, clipsEnd } };
    }
    const auto span = exporting::SampleRange { std::max<song::SampleCount> (0, videoTake->start), std::max<song::SampleCount> (0, videoTake->end) };
    if (loop.has_value())
        return { .scaleEnd = std::max ({ clipsEnd, span.end, loop->end }), .exported = *loop, .videoTake = span, .blackOutsideVideo = true };
    return {
        .scaleEnd = std::max (clipsEnd, span.end),
        .exported = exporting::movieExportRange (length, clipsEnd, { videoTake->start, videoTake->end }),
        .videoTake = span,
        .blackOutsideVideo = length == exporting::VideoLength::fullSong,
    };
}

juce::Range<int> spanBarPixels (exporting::SampleRange range, song::SampleCount scaleEnd, int width)
{
    if (scaleEnd <= 0 || width <= 0)
        return {};
    const auto toX = [scaleEnd, width] (song::SampleCount sample) {
        return (int) std::llround ((double) std::clamp<song::SampleCount> (sample, 0, scaleEnd) * width / (double) scaleEnd);
    };
    return { toX (range.start), toX (range.end) };
}

std::string exportSpanText (song::SampleCount clipsEnd, int sampleRate)
{
    if (clipsEnd <= 0)
        return "Nothing to export: the Song has no Clips";
    return "Whole mix: 0:00.0 \xe2\x80\x93 " + minutesText (clipsEnd, sampleRate) + ", plus effect tails";
}

std::string movieSpanText (exporting::VideoLength length, const video::VideoTakeSpan& videoTake, exporting::SampleRange exported, int sampleRate)
{
    if (length == exporting::VideoLength::fullSong)
        return "Full song: " + rangeText (exported, sampleRate) + ", black outside " + videoTake.title;
    return "Trimmed to " + videoTake.title + ": " + rangeText (exported, sampleRate);
}

std::string loopSpanText (const song::Loop& loop, const std::optional<video::VideoTakeSpan>& videoTake, int sampleRate)
{
    auto text = loopChoiceText (loop) + ": " + rangeText ({ loop.start, loop.end }, sampleRate);
    if (videoTake.has_value())
        text += ", black outside " + videoTake->title;
    return text;
}

std::string loopChoiceText (const song::Loop& loop)
{
    return "Loop: " + loop.name;
}

std::string videoTakeChoiceText (const video::VideoTakeSpan& videoTake, int sampleRate)
{
    return videoTake.title + " \xc2\xb7 " + rangeText ({ videoTake.start, videoTake.end }, sampleRate);
}

std::optional<song::Id> defaultExportVideoTake (const std::vector<video::VideoTakeSpan>& videoTakes, const std::optional<song::Id>& stored)
{
    if (videoTakes.empty())
        return std::nullopt;
    if (stored.has_value() && std::ranges::contains (videoTakes, *stored, &video::VideoTakeSpan::videoTakeId))
        return stored;
    return videoTakes.front().videoTakeId;
}

juce::File exportDestinationFor (const song::Song& song, const juce::File& exportFolder, const std::string& songName, bool fallBackWhenFolderGone)
{
    if (juce::File::isAbsolutePath (song.exportDestination))
    {
        const auto stored = exporting::withFormatExtension (juce::File (song.exportDestination), song.exportFormat);
        if (! fallBackWhenFolderGone || stored.getParentDirectory().isDirectory())
            return stored;
    }
    return exporting::defaultExportFile (exportFolder, songName, song.exportFormat);
}

exporting::ExportRequest exportRequestFor (const song::Song& song, const std::vector<video::VideoTakeSpan>& videoTakes, const juce::File& destination)
{
    const auto mp4 = song.exportFormat == exporting::ExportFormat::mp4;
    std::optional<exporting::ExportLoop> loop;
    if (const auto* found = song.exportLoopId.has_value() ? song::findLoop (song, *song.exportLoopId) : nullptr)
        loop = exporting::ExportLoop { found->id, { found->start, found->end } };
    return { song.exportFormat, destination, mp4 ? defaultExportVideoTake (videoTakes, song.exportVideoTakeId) : std::nullopt, song.exportLength, loop };
}

std::string replacesExistingText (const juce::File& destination)
{
    return "Replaces the existing " + destination.getFileName().toStdString();
}

void ExportSpanBar::setState (const ExportSpanBarState& state)
{
    if (state == shown)
        return;
    shown = state;
    repaint();
}

void ExportSpanBar::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds();
    g.setColour (juce::Colour (0xff111111));
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    const auto columns = [this, &area] (exporting::SampleRange range) {
        const auto pixels = spanBarPixels (range, shown.scaleEnd, area.getWidth());
        return juce::Rectangle<int> (area.getX() + pixels.getStart(), area.getY(), pixels.getLength(), area.getHeight());
    };
    if (shown.blackOutsideVideo)
    {
        const auto exported = columns (shown.exported).toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (exported);
        g.setColour (juce::Colour (0xff555555));
        g.drawRect (exported, 1.0f);
    }
    else
    {
        g.setColour (colours::accent);
        g.fillRoundedRectangle (columns (shown.exported).reduced (0, 2).toFloat(), 3.0f);
    }
    if (shown.videoTake.has_value())
    {
        g.setColour (colours::videoTake);
        g.fillRoundedRectangle (columns (*shown.videoTake).reduced (0, 2).toFloat(), 3.0f);
    }
}

ExportSheet::ExportSheet()
{
    // Covers the window: every click lands here, none on the page under it.
    setInterceptsMouseClicks (true, true);
    title.setText ("Export", juce::dontSendNotification);
    title.setColour (juce::Label::textColourId, colours::text);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    styleHeading (formatHeading, "Format");
    styleHeading (videoTakeHeading, "Video Take");
    styleHeading (lengthHeading, "Length");
    styleHeading (destinationHeading, "Save as");
    noVideoTakesLabel.setText ("No Video Takes yet. Arm the camera and record a Pass.", juce::dontSendNotification);
    noVideoTakesLabel.setColour (juce::Label::textColourId, colours::dimText);
    spanLabel.setColour (juce::Label::textColourId, colours::dimText);
    destinationLabel.setColour (juce::Label::textColourId, colours::text);
    destinationLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.4f));
    destinationLabel.setMinimumHorizontalScale (0.7f);
    replacesLabel.setColour (juce::Label::textColourId, colours::dimText);
    replacesLabel.setFont (juce::FontOptions (11.0f));

    const std::pair<juce::ToggleButton*, exporting::ExportFormat> formatButtons[] {
        { &wavButton, exporting::ExportFormat::wav },
        { &m4aButton, exporting::ExportFormat::m4a },
        { &mp4Button, exporting::ExportFormat::mp4 },
    };
    for (const auto& [button, buttonFormat] : formatButtons)
    {
        button->setRadioGroupId (formatRadioGroup, juce::dontSendNotification);
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, format = buttonFormat] { emit (SongIntent { intent::SetExportFormat { format } }); };
        addAndMakeVisible (button);
    }

    // The length choices are in no radio group: show() sets each one's state, since an MP4's
    // stored length and a Loop are kept apart.
    const std::pair<juce::ToggleButton*, exporting::VideoLength> lengthButtons[] {
        { &trimButton, exporting::VideoLength::trimToVideoTake },
        { &fullSongButton, exporting::VideoLength::fullSong },
    };
    for (const auto& [button, buttonLength] : lengthButtons)
    {
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, length = buttonLength] { emit (SongIntent { intent::SetExportLength { length } }); };
        addChildComponent (button);
    }
    // The Whole Song keeps the MP4's stored length.
    wholeSongButton.setClickingTogglesState (false);
    wholeSongButton.setWantsKeyboardFocus (false);
    wholeSongButton.onClick = [this] { emit (SongIntent { intent::SetExportLength { shown.request.videoLength } }); };
    addChildComponent (wholeSongButton);

    videoTakePicker.setWantsKeyboardFocus (false);
    videoTakePicker.onChange = [this] { pickVideoTake(); };

    chooseButton.onClick = [this] {
        if (onChooseDestination)
            onChooseDestination (shown.request.destination);
    };
    cancelButton.onClick = [this] { emit (SongIntent { intent::ShowExport { false } }); };
    exportButton.onClick = [this] { emit (SongIntent { intent::Export {} }); };
    exportButton.setColour (juce::TextButton::buttonColourId, colours::accent);

    for (auto* button : { &chooseButton, &cancelButton, &exportButton })
        button->setWantsKeyboardFocus (false);
    for (juce::Component* child : { (juce::Component*) &title, (juce::Component*) &formatHeading, (juce::Component*) &spanBar, (juce::Component*) &spanLabel, (juce::Component*) &destinationHeading, (juce::Component*) &destinationLabel, (juce::Component*) &chooseButton, (juce::Component*) &cancelButton, (juce::Component*) &exportButton })
        addAndMakeVisible (child);
    for (juce::Component* child : { (juce::Component*) &videoTakeHeading, (juce::Component*) &videoTakePicker, (juce::Component*) &noVideoTakesLabel, (juce::Component*) &lengthHeading, (juce::Component*) &replacesLabel })
        addChildComponent (child);
    show (shown);
}

void ExportSheet::show (const ExportSheetState& state)
{
    const auto videoTakesChanged = state.videoTakes != shown.videoTakes || state.sampleRate != shown.sampleRate;
    const auto loopsChanged = state.loops != shown.loops || loopButtons.size() != shown.loops.size();
    const auto layoutBefore = std::pair { shown.request.format, lengthChoiceCount() };
    shown = state;
    if (videoTakesChanged)
        updatePicker();
    if (loopsChanged)
        updateLoopButtons();

    const auto format = shown.request.format;
    wavButton.setToggleState (format == exporting::ExportFormat::wav, juce::dontSendNotification);
    m4aButton.setToggleState (format == exporting::ExportFormat::m4a, juce::dontSendNotification);
    mp4Button.setToggleState (format == exporting::ExportFormat::mp4, juce::dontSendNotification);
    const auto length = shown.request.videoLength;
    const auto loop = shownLoop();
    wholeSongButton.setToggleState (! loop.has_value(), juce::dontSendNotification);
    trimButton.setToggleState (! loop.has_value() && length == exporting::VideoLength::trimToVideoTake, juce::dontSendNotification);
    fullSongButton.setToggleState (! loop.has_value() && length == exporting::VideoLength::fullSong, juce::dontSendNotification);
    for (std::size_t i = 0; i < loopButtons.size(); ++i)
        loopButtons[i]->setToggleState (loop.has_value() && loop->id == shown.loops[i].id, juce::dontSendNotification);
    destinationLabel.setText (shown.request.destination.getFullPathName(), juce::dontSendNotification);
    replacesLabel.setText (shown.replacesExisting ? juce::String::fromUTF8 (replacesExistingText (shown.request.destination).c_str()) : juce::String(), juce::dontSendNotification);

    updateSpan();
    updateControls();
    if (loopsChanged || layoutBefore != std::pair { shown.request.format, lengthChoiceCount() })
    {
        resized();
        repaint();
    }
}

void ExportSheet::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

void ExportSheet::pickVideoTake()
{
    const auto index = videoTakePicker.getSelectedItemIndex();
    if (index < 0 || index >= (int) shown.videoTakes.size())
        return;
    emit (SongIntent { intent::SetExportVideoTake { shown.videoTakes[(std::size_t) index].videoTakeId } });
}

std::optional<video::VideoTakeSpan> ExportSheet::shownVideoTake() const
{
    const auto& id = shown.request.videoTakeId;
    if (! id.has_value())
        return std::nullopt;
    const auto found = std::ranges::find (shown.videoTakes, *id, &video::VideoTakeSpan::videoTakeId);
    if (found == shown.videoTakes.end())
        return std::nullopt;
    return *found;
}

std::optional<song::Loop> ExportSheet::shownLoop() const
{
    if (! shown.request.loop.has_value())
        return std::nullopt;
    const auto found = std::ranges::find (shown.loops, shown.request.loop->loopId, &song::Loop::id);
    if (found == shown.loops.end())
        return std::nullopt;
    return *found;
}

int ExportSheet::lengthChoiceCount() const
{
    const auto loops = (int) shown.loops.size();
    if (shown.request.format == exporting::ExportFormat::mp4)
        return shown.videoTakes.empty() ? 0 : 2 + loops;
    return loops == 0 ? 0 : 1 + loops;
}

// One length choice per Loop; show() sets which is on.
void ExportSheet::updateLoopButtons()
{
    for (auto& button : loopButtons)
        removeChildComponent (button.get());
    loopButtons.clear();
    for (const auto& loop : shown.loops)
    {
        auto button = std::make_unique<juce::ToggleButton> (juce::String::fromUTF8 (loopChoiceText (loop).c_str()));
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, loopId = loop.id] { emit (SongIntent { intent::SetExportLength { shown.request.videoLength, loopId } }); };
        addChildComponent (*button);
        loopButtons.push_back (std::move (button));
    }
}

// Fills the picker's items; updateSpan() selects the one shown.
void ExportSheet::updatePicker()
{
    videoTakePicker.clear (juce::dontSendNotification);
    for (std::size_t i = 0; i < shown.videoTakes.size(); ++i)
        videoTakePicker.addItem (juce::String::fromUTF8 (videoTakeChoiceText (shown.videoTakes[i], shown.sampleRate).c_str()), (int) i + 1);
}

void ExportSheet::updateSpan()
{
    const auto videoTake = shownVideoTake();
    const auto loop = shownLoop();
    const auto loopRange = loop.has_value() ? std::optional<exporting::SampleRange> ({ loop->start, loop->end }) : std::nullopt;
    const auto bar = exportSpanBar (shown.request.format, shown.request.videoLength, shown.clipsEnd, videoTake, loopRange);
    spanBar.setState (bar);
    const auto movie = bar.videoTake.has_value() && videoTake.has_value();
    std::string text;
    if (loop.has_value() && (movie || shown.clipsEnd > 0))
        text = loopSpanText (*loop, movie ? videoTake : std::nullopt, shown.sampleRate);
    else if (movie)
        text = movieSpanText (shown.request.videoLength, *videoTake, bar.exported, shown.sampleRate);
    else
        text = exportSpanText (shown.clipsEnd, shown.sampleRate);
    spanLabel.setText (juce::String::fromUTF8 (text.c_str()), juce::dontSendNotification);

    if (videoTake.has_value())
    {
        const auto found = std::ranges::find (shown.videoTakes, videoTake->videoTakeId, &video::VideoTakeSpan::videoTakeId);
        videoTakePicker.setSelectedItemIndex ((int) (found - shown.videoTakes.begin()), juce::dontSendNotification);
    }
}

void ExportSheet::updateControls()
{
    const auto mp4 = shown.request.format == exporting::ExportFormat::mp4;
    const auto hasVideoTakes = ! shown.videoTakes.empty();

    videoTakeHeading.setVisible (mp4);
    videoTakePicker.setVisible (mp4 && hasVideoTakes);
    noVideoTakesLabel.setVisible (mp4 && ! hasVideoTakes);
    const auto lengthShown = lengthChoiceCount() > 0;
    lengthHeading.setVisible (lengthShown);
    wholeSongButton.setVisible (lengthShown && ! mp4);
    trimButton.setVisible (lengthShown && mp4);
    fullSongButton.setVisible (lengthShown && mp4);
    for (auto& button : loopButtons)
        button->setVisible (lengthShown);
    replacesLabel.setVisible (shown.replacesExisting);
    exportButton.setEnabled (canExport());
    exportButton.setTooltip (shown.exportRefusal.has_value() ? juce::String::fromUTF8 (unavailableReason (*shown.exportRefusal).c_str()) : juce::String());
}

bool ExportSheet::canExport() const
{
    return ! shown.exportRefusal.has_value() && shown.request.destination != juce::File() && (shown.request.format != exporting::ExportFormat::mp4 || ! shown.videoTakes.empty());
}

juce::Rectangle<int> ExportSheet::panelBounds() const
{
    const auto choices = lengthChoiceCount();
    const auto height = formHeight + (shown.request.format == exporting::ExportFormat::mp4 ? videoTakeRowsHeight : 0)
                      + (choices > 0 ? lengthHeadingHeight + choices * lengthChoiceHeight : 0) + 2 * panelPadding;
    const auto width = std::min (formWidth + 2 * panelPadding, getWidth() - 32);
    return getLocalBounds().withSizeKeepingCentre (width, std::min (height, getHeight() - 32));
}

void ExportSheet::resized()
{
    auto area = panelBounds().reduced (panelPadding);

    title.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    formatHeading.setBounds (area.removeFromTop (18));
    for (auto* button : { &wavButton, &m4aButton, &mp4Button })
        button->setBounds (area.removeFromTop (24));
    if (shown.request.format == exporting::ExportFormat::mp4)
    {
        area.removeFromTop (8);
        videoTakeHeading.setBounds (area.removeFromTop (18));
        const auto pickerRow = area.removeFromTop (26);
        videoTakePicker.setBounds (pickerRow);
        noVideoTakesLabel.setBounds (pickerRow);
    }
    if (lengthChoiceCount() > 0)
    {
        area.removeFromTop (8);
        lengthHeading.setBounds (area.removeFromTop (18));
        const auto lengthButtons = shown.request.format == exporting::ExportFormat::mp4 ? std::vector<juce::ToggleButton*> { &trimButton, &fullSongButton }
                                                                                        : std::vector<juce::ToggleButton*> { &wholeSongButton };
        for (auto* button : lengthButtons)
            button->setBounds (area.removeFromTop (lengthChoiceHeight));
        for (auto& button : loopButtons)
            button->setBounds (area.removeFromTop (lengthChoiceHeight));
    }
    area.removeFromTop (10);
    spanBar.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    spanLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (10);
    destinationHeading.setBounds (area.removeFromTop (18));
    auto destinationRow = area.removeFromTop (26);
    chooseButton.setBounds (destinationRow.removeFromRight (90));
    destinationRow.removeFromRight (6);
    destinationLabel.setBounds (destinationRow);
    replacesLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (14);
    auto actions = area.removeFromTop (28);
    exportButton.setBounds (actions.removeFromRight (100));
    actions.removeFromRight (6);
    cancelButton.setBounds (actions.removeFromRight (90));
}

void ExportSheet::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    const auto panel = panelBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (panel, 10.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (panel.reduced (0.5f), 10.0f, 1.0f);
}
} // namespace overdub::ui
