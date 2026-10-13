#include "ui/CalibrationText.h"

#include <catch2/catch_test_macros.hpp>

using overdub::capture::ClapReport;
using overdub::capture::ClapResult;
using overdub::capture::LoopbackMeasurement;
using namespace overdub::ui;

TEST_CASE ("the latency calibration in use reads in samples and ms, or as not calibrated", "[ui][calibration]")
{
    CHECK (latencyCalibrationText (std::nullopt, 48000) == "Not calibrated: Takes are placed by the latency the device reports.");
    CHECK (latencyCalibrationText (-821, 44100) == "Calibrated: -821 samples (-18.6 ms).");
    CHECK (latencyCalibrationText (0, 48000) == "Calibrated: 0 samples (0.0 ms).");
}

TEST_CASE ("a loopback result says how late Takes landed, or why it cannot be used", "[ui][calibration]")
{
    LoopbackMeasurement passed;
    passed.signalFound = true;
    passed.offsetSamples = 240;
    passed.jitterSamples = 2;
    CHECK (latencyResultText (passed, 48000) == "Takes landed 240 samples (5.0 ms) late, jitter 2 samples. Use this calibration to correct it.");

    auto early = passed;
    early.offsetSamples = -48;
    CHECK (latencyResultText (early, 48000) == "Takes landed 48 samples (1.0 ms) early, jitter 2 samples. Use this calibration to correct it.");

    auto exact = passed;
    exact.offsetSamples = 0;
    CHECK (latencyResultText (exact, 48000) == "Takes land where they were heard (jitter 2 samples).");

    LoopbackMeasurement silent;
    CHECK (latencyResultText (silent, 48000) == "No bursts came back. Check the cable from output 1 to input 1 and the input level, then measure again.");

    auto uneven = passed;
    uneven.jitterSamples = 9;
    CHECK (latencyResultText (uneven, 48000) == "The bursts came back unevenly (jitter 9 samples, at most 5). Use a cable rather than speakers, then measure again.");
}

TEST_CASE ("the camera calibration in use reads in ms, or as not calibrated", "[ui][calibration]")
{
    CHECK (cameraCalibrationText (std::nullopt) == "Not calibrated: 0 ms.");
    CHECK (cameraCalibrationText (35.0) == "Calibrated: 35.0 ms (video late).");
    CHECK (cameraCalibrationText (-4.26) == "Calibrated: -4.3 ms (video early).");
}

TEST_CASE ("a clap result says how far the video was from the audio, or that no clap was seen", "[ui][calibration]")
{
    ClapReport seen;
    seen.claps = { ClapResult { .offsetMs = 24.0 }, ClapResult { .offsetMs = 26.0 }, ClapResult {} };
    seen.meanOffsetMs = 25.0;
    seen.frameIntervalMs = 33.3;
    CHECK (clapResultText (seen, "Speakers") == "2 of 3 claps seen: the video was 25.0 ms late on Speakers. Use this calibration to correct it.");

    auto early = seen;
    early.meanOffsetMs = -10.0;
    CHECK (clapResultText (early, "Speakers") == "2 of 3 claps seen: the video was 10.0 ms early on Speakers. Use this calibration to correct it.");

    ClapReport none;
    none.claps = { ClapResult {} };
    CHECK (clapResultText (none, "Speakers") == "No clap was seen. Record a Pass clapping 3 to 5 times in frame, then measure again.");
}

TEST_CASE ("a clap result from a Pass whose output device is not known says no calibration can be stored", "[ui][calibration]")
{
    ClapReport seen;
    seen.claps = { ClapResult { .offsetMs = 25.0 } };
    seen.meanOffsetMs = 25.0;

    CHECK (clapResultText (seen, "")
           == "1 of 1 claps seen: the video was 25.0 ms late. The output device this Pass played through is not known, so no calibration can be stored. "
              "Record a new clap Pass, then measure again.");
}
