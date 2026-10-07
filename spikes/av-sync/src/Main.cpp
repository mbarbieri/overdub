// PROTOTYPE — issue #7 spike: record camera + audio, play back in sync, export MP4.
#include "AudioEngine.h"
#include "CameraRecorder.h"
#include "SpikeComponent.h"
#include "SpikeLog.h"

using namespace juce;

class MainWindow final : public DocumentWindow
{
public:
    explicit MainWindow (SpikeComponent* component)
        : DocumentWindow ("Overdub AV Spike (PROTOTYPE)", Colours::black, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true); // before setContentOwned (AU spike finding 2)
        setContentOwned (component, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
};

// Usage:
//   (no args)                         interactive: Record / Stop / Play / Export / Analyse
//   --session <dir> [--play | --export | --analyse] [--quit]   re-run one step on a recorded session
//   --rt-calibration <samples>        default 821 (ADR 0002, built-in devices)
//   --camera-calibration-ms <ms>      subtracted from the video offset of new recordings
class SpikeApplication final : public JUCEApplication
{
public:
    const String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const String&) override
    {
        spikelog::install();
        const auto args = getCommandLineParameterArray();
        spikelog::line ("app_start", { { "args", args.joinIntoString (" ") },
                                       { "cpu", SystemStats::getCpuModel() },
                                       { "os", SystemStats::getOperatingSystemName() } });

        SpikeOptions options;
        if (auto i = args.indexOf ("--rt-calibration"); i >= 0)
            options.rtCalibrationSamples = args[i + 1].getIntValue();
        if (auto i = args.indexOf ("--camera-calibration-ms"); i >= 0)
            options.cameraCalibrationMs = args[i + 1].getDoubleValue();

        const auto sessionIndex = args.indexOf ("--session");
        if (sessionIndex >= 0)
        {
            runHeadless (File (args[sessionIndex + 1]), args, options);
            return;
        }

        requestCaptureAccess ([this, options] (bool cameraGranted, bool micGranted)
        {
            spikelog::line ("capture_access", { { "camera", cameraGranted ? "yes" : "no" }, { "microphone", micGranted ? "yes" : "no" } });

            engine = std::make_unique<AudioEngine>();
            if (auto error = engine->openDefaultDevice(); error.isNotEmpty())
                spikelog::line ("startup_error", { { "error", error } });

            camera = std::make_unique<CameraRecorder>();
            if (auto error = camera->start(); error.isNotEmpty())
                spikelog::line ("startup_error", { { "error", error } });

            openWindow (new SpikeComponent (engine.get(), camera.get(), options));
        });
    }

    void shutdown() override
    {
        spikelog::setUiSink (nullptr);
        window.reset();
        camera.reset();
        engine.reset();
        spikelog::line ("app_shutdown");
    }

    void systemRequestedQuit() override { quit(); }

private:
    void openWindow (SpikeComponent* component)
    {
        spikelog::setUiSink ([component] (const String& line) { component->appendLog (line); });
        window = std::make_unique<MainWindow> (component);
    }

    void runHeadless (const File& dir, const StringArray& args, SpikeOptions options)
    {
        // --play needs the audio device (no camera); export and analysis need neither.
        if (args.contains ("--play"))
        {
            engine = std::make_unique<AudioEngine>();
            if (auto error = engine->openDefaultDevice(); error.isNotEmpty())
                spikelog::line ("startup_error", { { "error", error } });
        }

        auto* component = new SpikeComponent (engine.get(), nullptr, options);
        openWindow (component);
        if (! component->loadExistingSession (dir))
        {
            setApplicationReturnValue (1);
            quit();
            return;
        }

        const bool quitAfter = args.contains ("--quit");
        auto done = [quitAfter] { if (quitAfter) JUCEApplication::getInstance()->systemRequestedQuit(); };

        if (args.contains ("--play"))
            component->startPlayback (done);
        else if (args.contains ("--export"))
            component->exportThenAnalyse (done);
        else if (args.contains ("--analyse"))
            component->analyse (done);
    }

    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<CameraRecorder> camera;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SpikeApplication)
