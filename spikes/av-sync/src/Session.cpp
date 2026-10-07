// PROTOTYPE — issue #7 spike.
#include "Session.h"
#include "SpikeLog.h"
#include <juce_audio_formats/juce_audio_formats.h>

using namespace juce;

File sessionsRoot()
{
    return spikelog::logDirectory().getChildFile ("sessions");
}

File createSessionDirectory()
{
    auto dir = sessionsRoot().getChildFile ("session-" + Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    dir.createDirectory();
    return dir;
}

bool saveSession (const SessionInfo& s)
{
    auto* o = new DynamicObject();
    o->setProperty ("sampleRate", s.sampleRate);
    o->setProperty ("bufferSize", s.bufferSize);
    o->setProperty ("inputLatencySamples", s.inputLatencySamples);
    o->setProperty ("outputLatencySamples", s.outputLatencySamples);
    o->setProperty ("rtCalibrationSamples", s.rtCalibrationSamples);
    o->setProperty ("recordStartSample", (int64) s.recordStartSample);
    o->setProperty ("takeStartSample", (int64) s.takeStartSample);
    o->setProperty ("takeLengthSamples", (int64) s.takeLengthSamples);
    o->setProperty ("clockInterceptNs", s.clock.interceptNs);
    o->setProperty ("clockNsPerSample", s.clock.nsPerSample);
    o->setProperty ("clockNominalNsPerSample", s.clock.nominalNsPerSample);
    o->setProperty ("clockPpm", s.clock.ppm());
    o->setProperty ("clockMaxResidualNs", s.clock.maxResidualNs);
    o->setProperty ("clockRmsResidualNs", s.clock.rmsResidualNs);
    o->setProperty ("clockAnchorCount", s.clock.anchorCount);
    o->setProperty ("driftAtEndMs", s.driftAtEndMs);
    o->setProperty ("cameraName", s.cameraName);
    o->setProperty ("captureClock", s.captureClock);
    o->setProperty ("captureClockRelativeRate", s.captureClockRelativeRate);
    o->setProperty ("videoFirstPtsNs", (int64) s.videoFirstPtsNs);
    o->setProperty ("videoFirstHostNs", (int64) s.videoFirstHostNs);
    o->setProperty ("videoFrameCount", s.videoFrameCount);
    o->setProperty ("videoDroppedFrames", s.videoDroppedFrames);
    o->setProperty ("videoDurationSeconds", s.videoDurationSeconds);
    o->setProperty ("cameraCalibrationMs", s.cameraCalibrationMs);
    o->setProperty ("videoOffsetSamples", s.videoOffsetSamples);
    o->setProperty ("videoOffsetSeconds", s.videoOffsetSamples / s.sampleRate);

    return s.file ("session.json").replaceWithText (JSON::toString (var (o)));
}

bool loadSession (const File& dir, SessionInfo& s, String& error)
{
    auto json = JSON::parse (dir.getChildFile ("session.json"));
    if (! json.isObject())
    {
        error = "no readable session.json in " + dir.getFullPathName();
        return false;
    }

    s = {};
    s.dir = dir;
    s.sampleRate = json["sampleRate"];
    s.bufferSize = json["bufferSize"];
    s.inputLatencySamples = json["inputLatencySamples"];
    s.outputLatencySamples = json["outputLatencySamples"];
    s.rtCalibrationSamples = json["rtCalibrationSamples"];
    s.recordStartSample = (int64) json["recordStartSample"];
    s.takeStartSample = (int64) json["takeStartSample"];
    s.takeLengthSamples = (int64) json["takeLengthSamples"];
    s.clock.interceptNs = json["clockInterceptNs"];
    s.clock.nsPerSample = json["clockNsPerSample"];
    s.clock.nominalNsPerSample = json["clockNominalNsPerSample"];
    s.clock.maxResidualNs = json["clockMaxResidualNs"];
    s.clock.rmsResidualNs = json["clockRmsResidualNs"];
    s.clock.anchorCount = json["clockAnchorCount"];
    s.driftAtEndMs = json["driftAtEndMs"];
    s.cameraName = json["cameraName"].toString();
    s.captureClock = json["captureClock"].toString();
    s.captureClockRelativeRate = json["captureClockRelativeRate"];
    s.videoFirstPtsNs = (int64) json["videoFirstPtsNs"];
    s.videoFirstHostNs = (int64) json["videoFirstHostNs"];
    s.videoFrameCount = json["videoFrameCount"];
    s.videoDroppedFrames = json["videoDroppedFrames"];
    s.videoDurationSeconds = json["videoDurationSeconds"];
    s.cameraCalibrationMs = json["cameraCalibrationMs"];
    s.videoOffsetSamples = json["videoOffsetSamples"];

    if (s.sampleRate <= 0 || s.clock.nsPerSample <= 0)
    {
        error = "session.json is missing sampleRate or clock fit";
        return false;
    }
    return true;
}

bool writeAnchorsCsv (const File& file, const std::vector<ClockAnchor>& anchors)
{
    FileOutputStream out (file);
    if (! out.openedOk())
        return false;

    out.setPosition (0);
    out.truncate();
    out << "song_sample,host_ns\n";
    for (auto& a : anchors)
        out << String ((int64) a.songSample) << "," << String ((int64) a.hostNs) << "\n";
    return true;
}

bool buildSongMix (const SessionInfo& s, AudioBuffer<float>& mix, String& error)
{
    WavAudioFormat wav;
    std::unique_ptr<AudioFormatReader> reader (wav.createReaderFor (s.file ("take.wav").createInputStream().release(), true));
    if (reader == nullptr)
    {
        error = "cannot read take.wav";
        return false;
    }

    const auto takeLength = (int) reader->lengthInSamples;
    const auto start = (int) std::max<int64_t> (0, s.takeStartSample);
    const auto skip = (int) std::max<int64_t> (0, -s.takeStartSample);

    mix.setSize (1, start + takeLength - skip);
    mix.clear();
    reader->read (&mix, start, takeLength - skip, skip, true, false);
    return true;
}

bool writeMonoWav (const File& file, const AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<OutputStream> stream = std::make_unique<FileOutputStream> (file);
    WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                                         .withNumChannels (1)
                                                                         .withBitsPerSample (24));
    if (writer == nullptr)
        return false;
    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}
