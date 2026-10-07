#pragma once

#include "core/Error.h"
#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

// The launch log files (ADR 0006): one JSON Lines file per launch in ~/Library/Logs/Overdub/,
// named overdub-<UTC timestamp>-<pid>.jsonl, the last 20 launches kept, every line flushed.
namespace overdub::core
{
constexpr int launchLogsKept = 20;

juce::File defaultLogDirectory();

// e.g. overdub-2026-10-07T09-41-05Z-4321.jsonl.
std::string launchLogFileName (std::chrono::system_clock::time_point launchTime, int processId);

// Deletes the oldest overdub-*.jsonl files in `directory` until at most `keep` remain. Launch order
// is the file name's timestamp, then its pid as a number. A file that cannot be deleted is logged
// as `app.log_prune_failed` (warn) and skipped.
void pruneLaunchLogs (const juce::File& directory, int keep);

// Appends each line plus '\n' to a file and flushes it; optionally echoes to stderr (debug builds).
class FileLogSink final : public LogSink
{
public:
    static Expected<std::shared_ptr<FileLogSink>> open (const juce::File& file, bool echoToStderr);
    ~FileLogSink() override;

    FileLogSink (const FileLogSink&) = delete;
    FileLogSink& operator= (const FileLogSink&) = delete;

    void writeLine (std::string_view line) override;

private:
    FileLogSink (std::FILE* openedFile, bool echoToStderr);

    std::FILE* file;
    bool echoToStderr;
};

struct ProcessLoggingOptions
{
    juce::File directory;
    std::chrono::system_clock::time_point launchTime;
    int processId = 0;
    Level minLevel = Level::info;
    bool echoToStderr = false;
};

// Creates `directory`, prunes it to the last launchLogsKept - 1 launches, creates this launch's file,
// and installs it as the process logger's sink at `minLevel`. Returns the new file's path.
// On failure it installs a StderrLogSink at `minLevel` instead and returns the Error
// (`app.log_dir_create_failed` or `app.log_open_failed`), so a sink is installed either way.
Expected<juce::File> startProcessLogging (const ProcessLoggingOptions& options);
} // namespace overdub::core
