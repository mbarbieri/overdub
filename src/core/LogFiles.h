#pragma once

#include "core/Error.h"
#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <string>

// The launch log files (ADR 0006): one JSON Lines file per launch in ~/Library/Logs/Overdub/,
// named overdub-<UTC timestamp>-<pid>.jsonl, the last 20 launches kept, every line flushed.
namespace overdub::core
{
constexpr int launchLogsKept = 20;

// ~/Library/Logs/Overdub
juce::File defaultLogDirectory();

// e.g. overdub-2026-10-07T09-41-05Z-4321.jsonl. Names sort in launch order.
std::string launchLogFileName (std::chrono::system_clock::time_point launchTime, int processId);

// Deletes the oldest overdub-*.jsonl files in `directory` until at most `keep` remain.
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

// Creates `directory`, prunes it to the last launchLogsKept - 1 launches, creates this launch's file,
// and installs it as the process logger's sink at `minLevel`. Returns the new file's path.
Expected<juce::File> startProcessLogging (const juce::File& directory,
                                          std::chrono::system_clock::time_point launchTime,
                                          int processId,
                                          Level minLevel,
                                          bool echoToStderr);
} // namespace overdub::core
