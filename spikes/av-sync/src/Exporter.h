// PROTOTYPE — issue #7 spike.
#pragma once
#include "Session.h"
#include <functional>

struct ExportResult
{
    bool ok = false;
    juce::String error;
    juce::File file;
    double elapsedSeconds = 0;
};

// Option E1 from research/av-sync.md: mix.wav at 0 plus the Video Take at its sample-exact
// offset in an AVMutableComposition, exported with AVAssetExportSession as H.264/AAC MP4.
//   fullSong == false: trimmed to the Video Take's span.
//   fullSong == true:  the whole song, black where there is no video.
// The "mix" here is the single dry Take; the real app renders the graph offline first.
void exportSession (const SessionInfo& session, bool fullSong, std::function<void (ExportResult)> onDoneOnMessageThread);
