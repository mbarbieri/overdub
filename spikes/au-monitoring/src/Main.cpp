// PROTOTYPE — issue #6 spike: dry recording while monitoring through AUs, latency-compensated takes.
#include "SpikeEngine.h"
#include "SpikeComponent.h"
#include "AutoRun.h"
#include "SpikeLog.h"

using namespace juce;

class MainWindow final : public DocumentWindow
{
public:
    MainWindow (SpikeEngine& engine)
        : DocumentWindow ("Overdub AU Spike (PROTOTYPE)", Colours::black, DocumentWindow::allButtons)
    {
        auto* component = new SpikeComponent (engine);
        spikelog::setUiSink ([component] (const String& line) { component->appendLog (line); });
        setUsingNativeTitleBar (true);
        setContentOwned (component, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~MainWindow() override { spikelog::setUiSink (nullptr); }
    void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SpikeApplication final : public JUCEApplication
{
public:
    const String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const String&) override
    {
        spikelog::install();
        const auto args = getCommandLineParameterArray();
        spikelog::line ("app_start", { { "args", args.joinIntoString (" ") },
                                       { "arch", SystemStats::getCpuModel() },
                                       { "os", SystemStats::getOperatingSystemName() } });

        RuntimePermissions::request (RuntimePermissions::recordAudio, [this, args] (bool granted)
        {
            spikelog::line ("mic_permission", { { "granted", granted ? "yes" : "no" } });
            engine = std::make_unique<SpikeEngine>();

            if (auto error = engine->openDefaultDevice(); error.isNotEmpty())
                spikelog::line ("startup_error", { { "error", error } });

            window = std::make_unique<MainWindow> (*engine);

            if (args.contains ("--auto"))
            {
                autoRun = std::make_unique<AutoRun> (*engine, args);
                autoRun->start();
            }
        });
    }

    void shutdown() override
    {
        autoRun.reset();
        window.reset();
        engine.reset();
        spikelog::line ("app_shutdown");
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<SpikeEngine> engine;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<AutoRun> autoRun;
};

START_JUCE_APPLICATION (SpikeApplication)
