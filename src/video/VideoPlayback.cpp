#include "video/VideoPlayback.h"

#include "core/Log.h"

#include <algorithm>

namespace overdub::video
{
VideoPlayback::VideoPlayback (juce::File packageRoot)
    : root (std::move (packageRoot))
{
}

VideoPlayback::~VideoPlayback() = default;

const MovieTiming* VideoPlayback::timing (const song::VideoTake& videoTake)
{
    auto found = timings.find (videoTake.id);
    if (found == timings.end())
    {
        auto read = readMovieTiming (root.getChildFile (videoTake.file));
        if (! read.has_value())
        {
            auto error = read.error();
            error.fields["video_take_id"] = videoTake.id;
            core::logError (error, core::Level::warn);
        }
        found = timings.emplace (videoTake.id, read.has_value() ? std::optional<MovieTiming> (std::move (*read)) : std::nullopt).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

TimingLookup VideoPlayback::timingLookup()
{
    return [this] (const song::VideoTake& videoTake) { return timing (videoTake); };
}

std::vector<VideoTakeSpan> VideoPlayback::spans (const song::Song& song)
{
    return videoTakeSpans (song, timingLookup());
}

void VideoPlayback::noteShown (const std::optional<song::Id>& videoTakeId)
{
    if (anyShown && videoTakeId == lastShown)
        return;
    anyShown = true;
    lastShown = videoTakeId;
    core::logEvent (core::Level::debug, "video.video_take_shown", { { "video_take_id", videoTakeId.has_value() ? nlohmann::json (*videoTakeId) : nlohmann::json (nullptr) } });
}

FrameDecoder* VideoPlayback::decoderFor (const song::VideoTake& videoTake, const MovieTiming& movieTiming)
{
    if (decoder != nullptr && decoderVideoTakeId == videoTake.id)
        return decoder.get();
    decoder.reset();
    decoderVideoTakeId = videoTake.id;
    auto opened = FrameDecoder::open (root.getChildFile (videoTake.file), movieTiming);
    if (! opened.has_value())
    {
        if (! decodeFailureLogged[videoTake.id])
        {
            auto error = opened.error();
            error.fields["video_take_id"] = videoTake.id;
            core::logError (error, core::Level::warn);
            decodeFailureLogged[videoTake.id] = true;
        }
        return nullptr;
    }
    decoder = std::move (*opened);
    return decoder.get();
}

VideoPlayback::Shown VideoPlayback::frameAt (const song::Song& song, double heardSample)
{
    Shown shown;
    shown.choice = chooseFrame (song, timingLookup(), heardSample);
    noteShown (shown.choice.videoTakeId);
    if (! shown.choice.videoTakeId.has_value())
        return shown;

    const auto videoTake = std::ranges::find (song.videoTakes, *shown.choice.videoTakeId, &song::VideoTake::id);
    const auto* movieTiming = timing (*videoTake);
    auto* frameDecoder = decoderFor (*videoTake, *movieTiming);
    if (frameDecoder == nullptr)
        return shown;
    auto decoded = frameDecoder->decode (shown.choice.movieSeconds);
    if (! decoded.has_value())
    {
        if (! decodeFailureLogged[videoTake->id])
        {
            auto error = decoded.error();
            error.fields["video_take_id"] = videoTake->id;
            core::logError (error, core::Level::warn);
            decodeFailureLogged[videoTake->id] = true;
        }
        return shown;
    }
    shown.sampleBuffer = decoded->sampleBuffer;
    return shown;
}
} // namespace overdub::video
