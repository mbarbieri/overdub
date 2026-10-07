#pragma once

// The device input of the block being rendered, for Monitoring (issue #26). The render plan sets
// it before each block; Track nodes read it. Audio thread only.
namespace overdub::engine
{
struct LiveInput
{
    const float* const* channels = nullptr; // numChannels pointers of the block's length; null = none
    int numChannels = 0;
    bool monitoring = false; // Monitoring is on: Armed Tracks add their input channels
};
} // namespace overdub::engine
