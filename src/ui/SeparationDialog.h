#pragma once

#include "song/SeparationRun.h"
#include "ui/ExportPage.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

// The modal Separation dialog (issues #114, #115): while a Separation runs it covers the whole
// main window, dimming it and taking every click, with a panel in the middle that names the Backing
// Track, shows the helper's progress (song::SeparationProgress) and offers Cancel (onCancel). The
// main window also refuses keys while it shows, so the user cannot edit, play or record. Thin: it
// shows state only.
namespace overdub::ui
{
// "Separating Band Mix.mp3".
std::string separationTitleText (const std::string& sourceName);
// "Splitting it into bass, drums, other, vocals, guitar and piano. …": the parts from
// song::separationParts.
std::string separationHintText();
// "Starting…" until the helper says how many chunks, then "Chunk 3 of 12 · 25%".
std::string separationStatusText (const song::SeparationProgress& progress);

class SeparationDialog final : public juce::Component
{
public:
    SeparationDialog();

    void show (const std::string& sourceName, const song::SeparationProgress& progress);
    double progress() const { return progressBar.fraction(); }

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Children are public so tests can check what shows.
    juce::Label title;
    juce::Label hint;
    ExportProgressBar progressBar;
    juce::Label statusLabel;
    juce::TextButton cancelButton { "Cancel" };

    std::function<void()> onCancel;

private:
    juce::Rectangle<int> panelBounds() const;
};
} // namespace overdub::ui
