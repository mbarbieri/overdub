// PROTOTYPE — issue #6 spike.
#include "SpikeComponent.h"
#include "LoopbackTest.h"

using namespace juce;

SpikeComponent::SpikeComponent (SpikeEngine& e)
    : engine (e),
      deviceSelector (e.deviceManager, 1, 8, 2, 2, false, false, false, false)
{
    addAndMakeVisible (deviceSelector);

    for (int t = 0; t < SpikeEngine::numTracks; ++t)
        setUpTrackRow (t);

    for (auto* button : { &loadBacking, &record, &stop, &loopback })
        addAndMakeVisible (button);

    loadBacking.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Backing Track", File(), "*.wav;*.aif;*.aiff;*.mp3;*.m4a;*.flac");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            if (fc.getResult().existsAsFile())
                if (auto error = engine.loadBackingFile (fc.getResult()); error.isNotEmpty())
                    appendLog ("ERROR " + error);
        });
    };
    record.onClick = [this]
    {
        const bool armed[] = { rows[0].armed.getToggleState(), rows[1].armed.getToggleState() };
        engine.start (armed);
    };
    stop.onClick = [this] { engine.stop(); };
    loopback.onClick = [this]
    {
        loopback.setEnabled (false);
        runLoopbackTest (engine, [this] (LoopbackResult) { loopback.setEnabled (true); });
    };

    status.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
    status.setJustificationType (Justification::topLeft);
    addAndMakeVisible (status);

    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
    addAndMakeVisible (log);

    setSize (1200, 860);
    startTimerHz (4);
}

SpikeComponent::~SpikeComponent()
{
    for (auto& editor : editors)
        editor.reset();
}

void SpikeComponent::setUpTrackRow (int t)
{
    auto& row = rows[t];
    row.label.setText ("Track " + String (t + 1), dontSendNotification);

    for (int ch = 0; ch < jmax (1, engine.getNumGraphInputs()); ++ch)
        row.input.addItem ("in " + String (ch + 1), ch + 1);
    row.input.setSelectedId (engine.getInputChannel (t) + 1, dontSendNotification);
    row.input.onChange = [this, t] { engine.setInputChannel (t, rows[t].input.getSelectedId() - 1); };

    row.plugin.addItemList (engine.listEffectNames(), 1);
    row.plugin.setTextWhenNothingSelected ("choose AU effect");

    row.load.onClick = [this, t]
    {
        closeEditor (t);
        rows[t].load.setEnabled (false);
        engine.loadPlugin (t, rows[t].plugin.getText(), rows[t].outOfProcess.getToggleState(),
                           [this, t] (String error)
                           {
                               rows[t].load.setEnabled (true);
                               if (error.isNotEmpty())
                                   appendLog ("ERROR " + error);
                           });
    };
    row.unload.onClick = [this, t] { closeEditor (t); engine.unloadPlugin (t); };
    row.edit.onClick = [this, t] { openEditor (t); };
    row.monitor.onClick = [this, t] { engine.setMonitoring (t, rows[t].monitor.getToggleState()); };
    row.armed.setToggleState (true, dontSendNotification);

    for (Component* c : std::initializer_list<Component*> { &row.label, &row.input, &row.plugin, &row.outOfProcess,
                                                            &row.load, &row.edit, &row.unload, &row.monitor, &row.armed })
        addAndMakeVisible (c);
}

void SpikeComponent::openEditor (int t)
{
    closeEditor (t);
    if (auto* plugin = engine.getPlugin (t))
        editors[t] = std::make_unique<PluginWindow> (*plugin, [this, t] { closeEditor (t); });
}

void SpikeComponent::closeEditor (int t)
{
    editors[t].reset();
}

void SpikeComponent::appendLog (const String& line)
{
    log.moveCaretToEnd();
    log.insertTextAtCaret (line + "\n");
}

void SpikeComponent::timerCallback()
{
    const auto latency = engine.getLatency();
    const auto health = engine.getHealth();
    const auto ms = [&latency] (int s) { return latency.sampleRate > 0 ? String (1000.0 * s / latency.sampleRate, 2) + "ms" : String ("-"); };

    String text;
    text << "sr=" << latency.sampleRate << "  buffer=" << latency.bufferSize
         << "  in_lat=" << latency.input << "  out_lat=" << latency.output << "  graph_lat(PDC)=" << latency.graph
         << "  compensation=" << latency.total() << " (" << ms (latency.total()) << ")\n"
         << "cpu=" << String (100.0 * health.cpu, 1) << "%  max_callback=" << String (health.maxCallbackMs, 3)
         << "ms / budget " << String (health.budgetMs, 3) << "ms  xruns=" << health.xruns
         << "  transport=" << (engine.isPlaying() ? "PLAYING" : "stopped")
         << "  pos=" << String ((double) engine.getPosition() / jmax (1.0, latency.sampleRate), 2) << "s\n";

    for (int t = 0; t < SpikeEngine::numTracks; ++t)
    {
        auto fields = engine.describeTrack (t);
        auto stats = engine.getTakeStats (t);
        text << "track" << t << ": ";
        for (auto& key : fields.getAllKeys())
            if (key != "track")
                text << key << "=" << fields[key] << " ";
        text << " take: written=" << stats.samplesWritten << " dropped=" << stats.samplesDropped
             << " first_block=" << stats.firstBlockStart << "\n";
    }

    status.setText (text, dontSendNotification);
}

void SpikeComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto top = area.removeFromTop (230);
    deviceSelector.setBounds (top.removeFromLeft (520));

    for (auto& row : rows)
    {
        auto line = top.removeFromTop (30).reduced (2);
        row.label.setBounds (line.removeFromLeft (60));
        row.input.setBounds (line.removeFromLeft (70));
        row.plugin.setBounds (line.removeFromLeft (190));
        row.outOfProcess.setBounds (line.removeFromLeft (120));
        row.load.setBounds (line.removeFromLeft (50));
        row.edit.setBounds (line.removeFromLeft (45));
        row.unload.setBounds (line.removeFromLeft (65));
        row.monitor.setBounds (line.removeFromLeft (80));
        row.armed.setBounds (line.removeFromLeft (70));
    }

    auto buttons = top.removeFromTop (34).reduced (2);
    for (auto* button : { &loadBacking, &record, &stop, &loopback })
        button->setBounds (buttons.removeFromLeft (150).reduced (2, 0));

    status.setBounds (area.removeFromTop (110));
    log.setBounds (area);
}
