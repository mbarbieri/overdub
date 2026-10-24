#include "ui/ExportPage.h"

#include "ui/Colours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace overdub::ui
{
namespace
{
constexpr int formWidth = 560;
constexpr int formatRadioGroup = 3701;
constexpr int lengthRadioGroup = 3702;

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

ExportSpanBarState exportSpanBar (exporting::ExportFormat format, exporting::VideoLength length, song::SampleCount clipsEnd, const std::optional<video::VideoTakeSpan>& videoTake)
{
    if (format != exporting::ExportFormat::mp4 || ! videoTake.has_value())
        return { .scaleEnd = clipsEnd, .exported = { 0, clipsEnd } };
    const auto span = exporting::SampleRange { std::max<song::SampleCount> (0, videoTake->start), std::max<song::SampleCount> (0, videoTake->end) };
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

std::string exportStatusText (const ExportPageState& state)
{
    if (state.waitingForAudio)
        return "Preparing\xe2\x80\xa6";
    if (state.rendering)
        return "Exporting\xe2\x80\xa6 " + std::to_string ((int) std::floor (state.progress * 100.0)) + "%";
    if (state.finishedFile.has_value())
        return "Exported to " + state.finishedFile->getFullPathName().toStdString();
    return {};
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

ExportPage::ExportPage()
{
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
    statusLabel.setColour (juce::Label::textColourId, colours::dimText);

    const std::pair<juce::ToggleButton*, exporting::ExportFormat> formatButtons[] {
        { &wavButton, exporting::ExportFormat::wav },
        { &m4aButton, exporting::ExportFormat::m4a },
        { &mp4Button, exporting::ExportFormat::mp4 },
    };
    for (const auto& [button, buttonFormat] : formatButtons)
    {
        button->setRadioGroupId (formatRadioGroup, juce::dontSendNotification);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, format = buttonFormat] { emit (SongIntent { intent::SetExportFormat { format } }); };
        addAndMakeVisible (button);
    }
    wavButton.setToggleState (true, juce::dontSendNotification);

    const std::pair<juce::ToggleButton*, exporting::VideoLength> lengthButtons[] {
        { &trimButton, exporting::VideoLength::trimToVideoTake },
        { &fullSongButton, exporting::VideoLength::fullSong },
    };
    for (const auto& [button, buttonLength] : lengthButtons)
    {
        button->setRadioGroupId (lengthRadioGroup, juce::dontSendNotification);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, length = buttonLength] { emit (SongIntent { intent::SetExportLength { length } }); };
        addChildComponent (button);
    }
    trimButton.setToggleState (true, juce::dontSendNotification);

    videoTakePicker.setWantsKeyboardFocus (false);
    videoTakePicker.onChange = [this] { pickVideoTake(); };

    chooseButton.onClick = [this] {
        if (onChooseDestination)
            onChooseDestination (chosenDestination);
    };
    exportButton.onClick = [this] { emit (SongIntent { intent::Export {} }); };
    exportButton.setColour (juce::TextButton::buttonColourId, colours::accent);
    revealButton.onClick = [this] { emit (SongIntent { intent::RevealExport {} }); };

    for (auto* button : { &chooseButton, &exportButton, &revealButton })
        button->setWantsKeyboardFocus (false);
    for (juce::Component* child : { (juce::Component*) &title, (juce::Component*) &formatHeading, (juce::Component*) &spanBar, (juce::Component*) &spanLabel, (juce::Component*) &destinationHeading, (juce::Component*) &destinationLabel, (juce::Component*) &chooseButton, (juce::Component*) &exportButton, (juce::Component*) &progressBar, (juce::Component*) &statusLabel })
        addAndMakeVisible (child);
    for (juce::Component* child : { (juce::Component*) &videoTakeHeading, (juce::Component*) &videoTakePicker, (juce::Component*) &noVideoTakesLabel, (juce::Component*) &lengthHeading, (juce::Component*) &revealButton })
        addChildComponent (child);
    updateSpan();
    updateControls();
}

void ExportPage::show (const ExportPageState& state)
{
    const auto videoTakesChanged = state.videoTakes != shown.videoTakes || state.sampleRate != shown.sampleRate;
    shown = state;
    progressBar.setFraction (state.rendering ? state.progress : (state.finishedFile.has_value() ? 1.0 : 0.0));
    statusLabel.setText (juce::String::fromUTF8 (exportStatusText (state).c_str()), juce::dontSendNotification);
    if (videoTakesChanged)
        updatePicker();
    updateSpan();
    updateControls();
}

exporting::ExportRequest ExportPage::request() const
{
    if (chosenFormat != exporting::ExportFormat::mp4)
        return { chosenFormat, chosenDestination };
    const auto videoTake = shownVideoTake();
    return {
        chosenFormat,
        chosenDestination,
        videoTake.has_value() ? std::optional<song::Id> (videoTake->videoTakeId) : std::nullopt,
        chosenLength,
    };
}

void ExportPage::setDestination (const juce::File& file)
{
    chosenDestination = file == juce::File() ? file : exporting::withFormatExtension (file, chosenFormat);
    destinationLabel.setText (chosenDestination.getFullPathName(), juce::dontSendNotification);
    updateControls();
}

void ExportPage::chooseFormat (exporting::ExportFormat format)
{
    chosenFormat = format;
    wavButton.setToggleState (format == exporting::ExportFormat::wav, juce::dontSendNotification);
    m4aButton.setToggleState (format == exporting::ExportFormat::m4a, juce::dontSendNotification);
    mp4Button.setToggleState (format == exporting::ExportFormat::mp4, juce::dontSendNotification);
    setDestination (chosenDestination);
    updateSpan();
    updateControls();
    resized();
}

void ExportPage::chooseLength (exporting::VideoLength length)
{
    chosenLength = length;
    trimButton.setToggleState (length == exporting::VideoLength::trimToVideoTake, juce::dontSendNotification);
    fullSongButton.setToggleState (length == exporting::VideoLength::fullSong, juce::dontSendNotification);
    updateSpan();
}

void ExportPage::chooseVideoTake (const song::Id& videoTakeId)
{
    pickedVideoTakeId = videoTakeId;
    updateSpan();
}

void ExportPage::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

void ExportPage::pickVideoTake()
{
    const auto index = videoTakePicker.getSelectedItemIndex();
    if (index < 0 || index >= (int) shown.videoTakes.size())
        return;
    emit (SongIntent { intent::SetExportVideoTake { shown.videoTakes[(std::size_t) index].videoTakeId } });
}

std::optional<video::VideoTakeSpan> ExportPage::shownVideoTake() const
{
    const auto id = pickedVideoTakeId.has_value() && std::ranges::contains (shown.videoTakes, *pickedVideoTakeId, &video::VideoTakeSpan::videoTakeId)
                      ? pickedVideoTakeId
                      : defaultExportVideoTake (shown.videoTakes, shown.storedVideoTakeId);
    if (! id.has_value())
        return std::nullopt;
    const auto found = std::ranges::find (shown.videoTakes, *id, &video::VideoTakeSpan::videoTakeId);
    return *found;
}

// Fills the picker's items; updateSpan() selects the one shown.
void ExportPage::updatePicker()
{
    videoTakePicker.clear (juce::dontSendNotification);
    for (std::size_t i = 0; i < shown.videoTakes.size(); ++i)
        videoTakePicker.addItem (juce::String::fromUTF8 (videoTakeChoiceText (shown.videoTakes[i], shown.sampleRate).c_str()), (int) i + 1);
}

void ExportPage::updateSpan()
{
    const auto videoTake = shownVideoTake();
    const auto bar = exportSpanBar (chosenFormat, chosenLength, shown.clipsEnd, videoTake);
    spanBar.setState (bar);
    const auto text = bar.videoTake.has_value() && videoTake.has_value()
                        ? movieSpanText (chosenLength, *videoTake, bar.exported, shown.sampleRate)
                        : exportSpanText (shown.clipsEnd, shown.sampleRate);
    spanLabel.setText (juce::String::fromUTF8 (text.c_str()), juce::dontSendNotification);

    // The picker shows the Video Take shown (the stored choice until the user picks).
    if (videoTake.has_value())
    {
        const auto found = std::ranges::find (shown.videoTakes, videoTake->videoTakeId, &video::VideoTakeSpan::videoTakeId);
        videoTakePicker.setSelectedItemIndex ((int) (found - shown.videoTakes.begin()), juce::dontSendNotification);
    }
}

void ExportPage::updateControls()
{
    const auto busy = shown.waitingForAudio || shown.rendering;
    const auto mp4 = chosenFormat == exporting::ExportFormat::mp4;
    const auto hasVideoTakes = ! shown.videoTakes.empty();
    for (auto* button : { &wavButton, &m4aButton, &mp4Button })
        button->setEnabled (! busy);

    videoTakeHeading.setVisible (mp4);
    videoTakePicker.setVisible (mp4 && hasVideoTakes);
    noVideoTakesLabel.setVisible (mp4 && ! hasVideoTakes);
    lengthHeading.setVisible (mp4 && hasVideoTakes);
    trimButton.setVisible (mp4 && hasVideoTakes);
    fullSongButton.setVisible (mp4 && hasVideoTakes);
    for (juce::Component* child : { (juce::Component*) &videoTakePicker, (juce::Component*) &trimButton, (juce::Component*) &fullSongButton })
        child->setEnabled (! busy);

    chooseButton.setEnabled (! busy);
    exportButton.setEnabled (! busy && chosenDestination != juce::File() && (! mp4 || hasVideoTakes));
    revealButton.setVisible (! busy && shown.finishedFile.has_value());
}

void ExportPage::resized()
{
    auto area = getLocalBounds().reduced (30);
    area = area.withSizeKeepingCentre (std::min (formWidth, area.getWidth()), area.getHeight());

    title.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    formatHeading.setBounds (area.removeFromTop (18));
    for (auto* button : { &wavButton, &m4aButton, &mp4Button })
        button->setBounds (area.removeFromTop (24));
    if (chosenFormat == exporting::ExportFormat::mp4)
    {
        area.removeFromTop (8);
        videoTakeHeading.setBounds (area.removeFromTop (18));
        const auto pickerRow = area.removeFromTop (26);
        videoTakePicker.setBounds (pickerRow);
        noVideoTakesLabel.setBounds (pickerRow);
        area.removeFromTop (8);
        lengthHeading.setBounds (area.removeFromTop (18));
        trimButton.setBounds (area.removeFromTop (24));
        fullSongButton.setBounds (area.removeFromTop (24));
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
    area.removeFromTop (14);
    auto actions = area.removeFromTop (28);
    exportButton.setBounds (actions.removeFromRight (100));
    actions.removeFromRight (6);
    revealButton.setBounds (actions.removeFromRight (140));
    area.removeFromTop (12);
    progressBar.setBounds (area.removeFromTop (10));
    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (22));
}

void ExportPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
