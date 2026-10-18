#pragma once

#include "song/SeparationRun.h"
#include "ui/ThinProgressBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

// The modal dialog of a long run on the open Song: a Separation (issues #114, #115) or a sample
// rate conversion (issue #160). While one runs it covers the whole main window, dimming it and
// taking every click, with a panel in the middle that says what runs, shows its progress and offers
// Cancel (onCancel). The main window also refuses keys while it shows, so the user cannot edit, play
// or record. Thin: it shows state only.
namespace overdub::ui
{
// What the dialog says about a run.
struct RunDialogText
{
    std::string title;
    std::string hint;
    std::string status;
};

// "Separating Band Mix.mp3".
std::string separationTitleText (const std::string& sourceName);
// "Splitting it into bass, drums, other, vocals, guitar and piano. …": the parts from
// song::separationParts.
std::string separationHintText();
// "Starting…" until the helper says how many chunks, then "Chunk 3 of 12 · 25%".
std::string separationStatusText (const song::SeparationProgress& progress);
RunDialogText separationDialogText (const std::string& sourceName, const song::SeparationProgress& progress);

// "Converting to 48000 Hz", a hint, and "40%".
RunDialogText conversionDialogText (int toRate, double fraction);

class RunDialog final : public juce::Component
{
public:
    RunDialog();

    void show (const RunDialogText& text, double fraction);
    double progress() const { return progressBar.fraction(); }

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Children are public so tests can check what shows.
    juce::Label title;
    juce::Label hint;
    ThinProgressBar progressBar;
    juce::Label statusLabel;
    juce::TextButton cancelButton { "Cancel" };

    std::function<void()> onCancel;

private:
    juce::Rectangle<int> panelBounds() const;
};
} // namespace overdub::ui
