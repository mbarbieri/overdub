#pragma once

// The device input of the block being rendered, for Monitoring (issue #26). The render plan sets
// it before each block; Track nodes read it. Audio thread only.
namespace overdub::engine
{
struct LiveInput
{
    const float* const* channels = nullptr; // numChannels pointers of the device block; null = none
    int numChannels = 0;
    bool monitoring = false; // Monitoring is on: Armed Tracks add their input channels
    // The device block frame the rendered span starts at: a Loop's wrap renders a block in spans
    // (issue #228), and the span's frame i reads channels[c][firstFrame + i].
    int firstFrame = 0;
};
} // namespace overdub::engine
