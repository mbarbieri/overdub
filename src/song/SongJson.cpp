#include "song/SongJson.h"

#include <array>
#include <string_view>
#include <utility>

// Writing builds the document field by field. Reading goes through Reader, which tracks the JSON
// path and throws InvalidSongJson on the first problem; songFromJson catches it (and any
// nlohmann exception) at this file's boundary and returns an Error. No exception leaves this file.
namespace overdub::song
{
namespace
{
using nlohmann::json;

struct InvalidSongJson
{
    std::string path;
    std::string reason;
};

// A view of one JSON object together with its path from the document root ("$.tracks[0]").
class Reader
{
public:
    Reader (const json& objectNode, std::string objectPath)
        : node (objectNode),
          path (std::move (objectPath))
    {
        if (! node.is_object())
            throw InvalidSongJson { path, "expected an object" };
    }

    const json& value (std::string_view key) const
    {
        const auto found = node.find (key);
        if (found == node.end())
            throw InvalidSongJson { childPath (key), "missing" };
        return *found;
    }

    std::string childPath (std::string_view key) const { return path + "." + std::string (key); }

    std::string string (std::string_view key) const
    {
        const auto& found = value (key);
        if (! found.is_string())
            throw InvalidSongJson { childPath (key), "expected a string" };
        return found.get<std::string>();
    }

    std::int64_t integer (std::string_view key) const
    {
        const auto& found = value (key);
        if (! found.is_number_integer())
            throw InvalidSongJson { childPath (key), "expected an integer" };
        return found.get<std::int64_t>();
    }

    int smallInteger (std::string_view key) const { return static_cast<int> (integer (key)); }

    double number (std::string_view key) const
    {
        const auto& found = value (key);
        if (! found.is_number())
            throw InvalidSongJson { childPath (key), "expected a number" };
        return found.get<double>();
    }

    bool boolean (std::string_view key) const
    {
        const auto& found = value (key);
        if (! found.is_boolean())
            throw InvalidSongJson { childPath (key), "expected a boolean" };
        return found.get<bool>();
    }

    Reader object (std::string_view key) const { return Reader (value (key), childPath (key)); }

    // Reads every element of the array at `key` with readElement (const Reader&).
    template <typename Element, typename ReadElement>
    std::vector<Element> array (std::string_view key, ReadElement readElement) const
    {
        const auto& found = value (key);
        if (! found.is_array())
            throw InvalidSongJson { childPath (key), "expected an array" };

        std::vector<Element> elements;
        elements.reserve (found.size());
        for (std::size_t index = 0; index < found.size(); ++index)
            elements.push_back (readElement (Reader (found[index], childPath (key) + "[" + std::to_string (index) + "]")));
        return elements;
    }

    std::vector<int> integers (std::string_view key) const
    {
        const auto& found = value (key);
        if (! found.is_array())
            throw InvalidSongJson { childPath (key), "expected an array" };

        std::vector<int> elements;
        for (std::size_t index = 0; index < found.size(); ++index)
        {
            if (! found[index].is_number_integer())
                throw InvalidSongJson { childPath (key) + "[" + std::to_string (index) + "]", "expected an integer" };
            elements.push_back (found[index].get<int>());
        }
        return elements;
    }

    // A channel count: 1 (mono) or 2 (stereo).
    int channels (std::string_view key) const
    {
        const int count = smallInteger (key);
        if (count != 1 && count != 2)
            throw InvalidSongJson { childPath (key), "expected 1 or 2" };
        return count;
    }

private:
    const json& node;
    std::string path;
};

// ---- Pre-roll: whole seconds {0, 1, 2, 4} ----

constexpr std::array<std::pair<PreRoll, int>, 4> preRollSeconds { {
    { PreRoll::off, 0 },
    { PreRoll::oneSecond, 1 },
    { PreRoll::twoSeconds, 2 },
    { PreRoll::fourSeconds, 4 },
} };

int preRollToSeconds (PreRoll preRoll)
{
    for (const auto& [value, seconds] : preRollSeconds)
        if (value == preRoll)
            return seconds;
    return 0;
}

PreRoll readPreRoll (const Reader& reader, std::string_view key)
{
    const auto seconds = reader.integer (key);
    for (const auto& [value, valueSeconds] : preRollSeconds)
        if (valueSeconds == seconds)
            return value;
    throw InvalidSongJson { reader.childPath (key), "expected 0, 1, 2 or 4" };
}

// ---- Writing ----

json insertChainToJson (const std::vector<InsertSlot>& insertChain)
{
    json slots = json::array();
    for (const auto& slot : insertChain)
        slots.push_back ({
            { "id", slot.id },
            { "component", { { "type", slot.component.type }, { "subtype", slot.component.subtype }, { "manufacturer", slot.component.manufacturer } } },
            { "name", slot.name },
            { "version", slot.version },
            { "bypassed", slot.bypassed },
            { "stateHash", slot.stateHash },
        });
    return slots;
}

json clipToJson (const Clip& clip)
{
    return {
        { "id", clip.id },
        { "source", { { "kind", clip.source.kind == ClipSourceKind::take ? "take" : "backing" }, { "id", clip.source.id } } },
        { "timelineStart", clip.timelineStart },
        { "sourceOffset", clip.sourceOffset },
        { "length", clip.length },
        { "gainDb", clip.gainDb },
        { "fadeInLength", clip.fadeInLength },
        { "fadeOutLength", clip.fadeOutLength },
    };
}

json trackToJson (const Track& track)
{
    json clips = json::array();
    for (const auto& clip : track.clips)
        clips.push_back (clipToJson (clip));

    return {
        { "id", track.id },
        { "name", track.name },
        { "volumeDb", track.volumeDb },
        { "pan", track.pan },
        { "mute", track.mute },
        { "solo", track.solo },
        { "armed", track.armed },
        { "inputChannels", track.inputChannels },
        { "insertChain", insertChainToJson (track.insertChain) },
        { "clips", clips },
    };
}

json takeToJson (const Take& take)
{
    return {
        { "id", take.id },
        { "trackId", take.trackId },
        { "passId", take.passId },
        { "file", take.file },
        { "channels", take.channels },
        { "lengthSamples", take.lengthSamples },
        { "origin", take.origin },
        { "compensation",
          { { "reportedLatencySamples", take.compensation.reportedLatencySamples },
            { "calibrationOffsetSamples", take.compensation.calibrationOffsetSamples } } },
    };
}

json backingTrackToJson (const BackingTrack& backing)
{
    return {
        { "id", backing.id },
        { "originalFilename", backing.originalFilename },
        { "originalExtension", backing.originalExtension },
        { "originalSampleRate", backing.originalSampleRate },
        { "channels", backing.channels },
        { "formatName", backing.formatName },
        { "lengthSamples", backing.lengthSamples },
    };
}

json videoTakeToJson (const VideoTake& videoTake)
{
    return {
        { "id", videoTake.id },
        { "passId", videoTake.passId },
        { "file", videoTake.file },
        { "startSample", videoTake.startSample },
        { "songSamplesPerMovieSecond", videoTake.songSamplesPerMovieSecond },
        { "cameraId", videoTake.cameraId },
        { "calibrationOffsetSamples", videoTake.calibrationOffsetSamples },
    };
}

template <typename Element, typename ToJson>
json arrayToJson (const std::vector<Element>& elements, ToJson toJson)
{
    json array = json::array();
    for (const auto& element : elements)
        array.push_back (toJson (element));
    return array;
}

// ---- Reading ----

InsertSlot readInsertSlot (const Reader& reader)
{
    const auto component = reader.object ("component");
    return InsertSlot {
        .id = reader.string ("id"),
        .component = { .type = component.string ("type"), .subtype = component.string ("subtype"), .manufacturer = component.string ("manufacturer") },
        .name = reader.string ("name"),
        .version = reader.string ("version"),
        .bypassed = reader.boolean ("bypassed"),
        .stateHash = reader.string ("stateHash"),
    };
}

ClipSource readClipSource (const Reader& reader)
{
    const auto kind = reader.string ("kind");
    if (kind != "take" && kind != "backing")
        throw InvalidSongJson { reader.childPath ("kind"), "expected \"take\" or \"backing\"" };
    return ClipSource { .kind = kind == "take" ? ClipSourceKind::take : ClipSourceKind::backing, .id = reader.string ("id") };
}

Clip readClip (const Reader& reader)
{
    return Clip {
        .id = reader.string ("id"),
        .source = readClipSource (reader.object ("source")),
        .timelineStart = reader.integer ("timelineStart"),
        .sourceOffset = reader.integer ("sourceOffset"),
        .length = reader.integer ("length"),
        .gainDb = reader.number ("gainDb"),
        .fadeInLength = reader.integer ("fadeInLength"),
        .fadeOutLength = reader.integer ("fadeOutLength"),
    };
}

Track readTrack (const Reader& reader)
{
    return Track {
        .id = reader.string ("id"),
        .name = reader.string ("name"),
        .volumeDb = reader.number ("volumeDb"),
        .pan = reader.number ("pan"),
        .mute = reader.boolean ("mute"),
        .solo = reader.boolean ("solo"),
        .armed = reader.boolean ("armed"),
        .inputChannels = reader.integers ("inputChannels"),
        .insertChain = reader.array<InsertSlot> ("insertChain", readInsertSlot),
        .clips = reader.array<Clip> ("clips", readClip),
    };
}

Pass readPass (const Reader& reader)
{
    return Pass { .id = reader.string ("id"), .startSample = reader.integer ("startSample"), .recordedAt = reader.string ("recordedAt") };
}

Take readTake (const Reader& reader)
{
    const auto compensation = reader.object ("compensation");
    return Take {
        .id = reader.string ("id"),
        .trackId = reader.string ("trackId"),
        .passId = reader.string ("passId"),
        .file = reader.string ("file"),
        .channels = reader.channels ("channels"),
        .lengthSamples = reader.integer ("lengthSamples"),
        .origin = reader.integer ("origin"),
        .compensation = { .reportedLatencySamples = compensation.integer ("reportedLatencySamples"),
                          .calibrationOffsetSamples = compensation.integer ("calibrationOffsetSamples") },
    };
}

BackingTrack readBackingTrack (const Reader& reader)
{
    return BackingTrack {
        .id = reader.string ("id"),
        .originalFilename = reader.string ("originalFilename"),
        .originalExtension = reader.string ("originalExtension"),
        .originalSampleRate = reader.smallInteger ("originalSampleRate"),
        .channels = reader.channels ("channels"),
        .formatName = reader.string ("formatName"),
        .lengthSamples = reader.integer ("lengthSamples"),
    };
}

VideoTake readVideoTake (const Reader& reader)
{
    return VideoTake {
        .id = reader.string ("id"),
        .passId = reader.string ("passId"),
        .file = reader.string ("file"),
        .startSample = reader.integer ("startSample"),
        .songSamplesPerMovieSecond = reader.number ("songSamplesPerMovieSecond"),
        .cameraId = reader.string ("cameraId"),
        .calibrationOffsetSamples = reader.integer ("calibrationOffsetSamples"),
    };
}

Song readSong (const Reader& reader)
{
    const auto formatVersion = reader.integer ("formatVersion");
    if (formatVersion != songFormatVersion)
        throw InvalidSongJson { reader.childPath ("formatVersion"), "expected " + std::to_string (songFormatVersion) };

    Song song;
    song.sampleRate = reader.smallInteger ("sampleRate");
    if (song.sampleRate <= 0)
        throw InvalidSongJson { reader.childPath ("sampleRate"), "expected a positive rate" };

    song.tracks = reader.array<Track> ("tracks", readTrack);
    const auto master = reader.object ("master");
    song.master = Master { .volumeDb = master.number ("volumeDb"), .insertChain = master.array<InsertSlot> ("insertChain", readInsertSlot) };
    song.passes = reader.array<Pass> ("passes", readPass);
    song.takes = reader.array<Take> ("takes", readTake);
    song.backingTracks = reader.array<BackingTrack> ("backingTracks", readBackingTrack);
    song.videoTakes = reader.array<VideoTake> ("videoTakes", readVideoTake);

    song.preRoll = readPreRoll (reader, "preRollSeconds");
    song.cameraArmed = reader.boolean ("cameraArmed");
    song.cameraDeviceId = reader.string ("cameraDeviceId");
    song.playhead = reader.integer ("playhead");

    const auto& exportChoice = reader.value ("exportVideoTakeId");
    if (exportChoice.is_string())
        song.exportVideoTakeId = exportChoice.get<std::string>();
    else if (! exportChoice.is_null())
        throw InvalidSongJson { reader.childPath ("exportVideoTakeId"), "expected a string or null" };
    return song;
}
} // namespace

nlohmann::json songToJson (const Song& song)
{
    return {
        { "formatVersion", songFormatVersion },
        { "sampleRate", song.sampleRate },
        { "tracks", arrayToJson (song.tracks, trackToJson) },
        { "master", { { "volumeDb", song.master.volumeDb }, { "insertChain", insertChainToJson (song.master.insertChain) } } },
        { "passes", arrayToJson (song.passes, [] (const Pass& pass) -> json
                                 { return { { "id", pass.id }, { "startSample", pass.startSample }, { "recordedAt", pass.recordedAt } }; }) },
        { "takes", arrayToJson (song.takes, takeToJson) },
        { "backingTracks", arrayToJson (song.backingTracks, backingTrackToJson) },
        { "videoTakes", arrayToJson (song.videoTakes, videoTakeToJson) },
        { "preRollSeconds", preRollToSeconds (song.preRoll) },
        { "cameraArmed", song.cameraArmed },
        { "cameraDeviceId", song.cameraDeviceId },
        { "playhead", song.playhead },
        { "exportVideoTakeId", song.exportVideoTakeId.has_value() ? json (*song.exportVideoTakeId) : json (nullptr) },
    };
}

core::Expected<Song> songFromJson (const nlohmann::json& document)
{
    try
    {
        return readSong (Reader (document, "$"));
    }
    catch (const InvalidSongJson& invalid)
    {
        return std::unexpected (core::Error {
            "song.json_invalid",
            "song document is invalid at " + invalid.path + ": " + invalid.reason,
            { { "path", invalid.path }, { "reason", invalid.reason } },
        });
    }
    catch (const nlohmann::json::exception& exception)
    {
        return std::unexpected (core::Error {
            "song.json_invalid",
            "song document could not be read",
            { { "path", "$" }, { "reason", exception.what() } },
        });
    }
}

core::Expected<nlohmann::json> parseJsonText (const std::string& text)
{
    try
    {
        return nlohmann::json::parse (text);
    }
    catch (const nlohmann::json::parse_error& error)
    {
        return std::unexpected (core::Error {
            "song.json_unparsable",
            "text is not valid JSON",
            { { "byte", error.byte }, { "reason", error.what() } },
        });
    }
}
} // namespace overdub::song
