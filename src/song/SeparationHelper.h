#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The one seam through which Overdub reaches the overdub-separate helper (issue #114, ADR 0007):
// start it with the model, the input file and an output folder; read its progress lines; learn
// how it ended. The protocol it speaks is src/separation/SeparateCommand.h. Tests substitute a fake
// StartSeparationHelper; the app uses separationHelperProcess.
namespace overdub::song
{
// How the helper ended. Success is exactly: it exited (not by a signal) with code 0.
struct HelperExit
{
    int exitCode = 0;          // the exit code; -1 when a signal ended it
    int signal = 0;            // the signal that ended it; 0 when it exited
    std::string errorLine;     // its stderr (one JSON line on a failure), trimmed; may be empty

    bool succeeded() const { return signal == 0 && exitCode == 0; }
};

// What the helper said since the last poll: its complete stdout lines in order, then, once it has
// ended and every line has been read, how it ended.
struct HelperOutput
{
    std::vector<std::string> lines;
    std::optional<HelperExit> exit;
};

// One running helper. Polled from one thread (the message thread); never blocks.
class SeparationHelperRun
{
public:
    virtual ~SeparationHelperRun() = default;

    // The lines since the last call and, once it has ended, its exit (reported once).
    virtual HelperOutput poll() = 0;
    // Ends a helper that still runs (SIGKILL) and reaps it; nothing once it has ended.
    virtual void kill() = 0;
};

using StartSeparationHelper = std::function<core::Expected<std::unique_ptr<SeparationHelperRun>> (const juce::File& model, const juce::File& input, const juce::File& outputFolder)>;

// The real helper: `executable` started as a child process (posix_spawn) with its stdout and
// stderr on non-blocking pipes. Destroying the run kills a helper that still runs.
// Errors when starting: song.separation_start_failed (path, errno).
StartSeparationHelper separationHelperProcess (juce::File executable);

// Where the app finds the helper: overdub-separate next to the running executable
// (Overdub.app/Contents/MacOS).
juce::File defaultSeparationHelperFile();

// Where `make separation-model` installs the model: $OVERDUB_MODELS_DIR when set, otherwise
// ~/Library/Application Support/Overdub/models; the file is bs_roformer_sw.fp32.onnx.
juce::File defaultSeparationModelFile();

// The model's name a Separation records (SeparationOrigin::model): its file name without ".onnx".
std::string separationModelName (const juce::File& model);
} // namespace overdub::song
