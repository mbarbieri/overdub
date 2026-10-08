#pragma once

#include "core/Error.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

// The state an Insert's plugin is saved with (issue #33, ADR 0004): the bytes behind an Insert
// Chain slot's stateHash (plugins/<sha256>.state).
//
// - An AU: its fullStateForDocument (kAudioUnitProperty_ClassInfoFromDocument, which AUv3 maps to
//   fullStateForDocument), or its ClassInfo (fullState) when the AU has no document state, as a
//   binary property list. Restoring sets the same property (document first, then ClassInfo).
//   JUCE's cached parameter values are not refreshed by a restore; read parameters from the AU.
// - Any other processor (test-only ones): getStateInformation / setStateInformation.
//
// Message thread. Capture and restore talk to the AU over IPC when it runs out-of-process.
namespace overdub::plugin
{
// Empty when the processor has no state. Errors: plugin.state_capture_failed (os_status).
core::Expected<juce::MemoryBlock> captureState (juce::AudioProcessor& processor);

// Errors: plugin.state_unreadable (the bytes are not a property list), plugin.state_restore_failed
// (os_status).
core::Expected<void> restoreState (juce::AudioProcessor& processor, const juce::MemoryBlock& state);
} // namespace overdub::plugin
