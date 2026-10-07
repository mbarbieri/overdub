// PROTOTYPE — issue #7 spike.
#pragma once
#include <cstdint>
#include <vector>

// One audio callback: the song sample at the start of the output buffer, and the
// host time (mach_absolute_time in ns) at which that buffer passes the device IO.
// JUCE only exposes the output timestamp in duplex mode, so all mapping goes through it.
struct ClockAnchor
{
    int64_t songSample = 0;
    uint64_t hostNs = 0;
};

// Least-squares line hostNs = interceptNs + nsPerSample * songSample.
// nsPerSample differs from 1e9 / sampleRate by the audio clock's drift against host time.
struct ClockFit
{
    double interceptNs = 0.0;
    double nsPerSample = 0.0;
    double nominalNsPerSample = 0.0;
    double maxResidualNs = 0.0;
    double rmsResidualNs = 0.0;
    int anchorCount = 0;

    double ppm() const { return (nsPerSample / nominalNsPerSample - 1.0) * 1.0e6; }
};

ClockFit fitClock (const std::vector<ClockAnchor>& anchors, double sampleRate);

// Song sample whose sound is *heard* at hostNs: output IO time plus output latency.
double songSampleHeardAtHostNs (const ClockFit& fit, double hostNs, double outputLatencyNs);

// Same mapping, but from the first anchor at the nominal rate: what you get if drift is ignored.
double songSampleHeardAtHostNsNaive (const ClockAnchor& first, double sampleRate, double hostNs, double outputLatencyNs);

uint64_t hostNowNs();
