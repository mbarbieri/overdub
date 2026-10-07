// PROTOTYPE — issue #6 spike. Logs raw CoreAudio latency properties for every device visible to
// this process (including JUCE's private aggregate), to explain JUCE's reported latency.
#pragma once

void logHalLatencies();
