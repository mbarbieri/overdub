#include "song/SongJson.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

// Writing builds the document field by field. Reading goes through Reader, which tracks the JSON
// path and records the first problem it finds. Reading does not stop there: later reads return
// placeholder values and record nothing new, so the read functions stay flat field lists, and
// songFromJson discards the result and returns the problem as an Error. Nothing here throws:
// every nlohmann call used is guarded by a type check first.
namespace overdub::song
{
namespace
{
using nlohmann::json;

struct Problem
{
    std::string jsonPath;
    std::string reason;
};

// A view of one JSON object together with its path from the document root ("$.tracks[0]").
class Reader
{
public:
    Reader (const json& objectNode, std::string objectPath, std::optional<Problem>& firstProblem)
        : node (objectNode),
          path (std::move (objectPath)),
          problem (firstProblem)
    {
        if (! node.is_object())
            report (path, "expected an object");
    }

    std::string childPath (std::string_view key) const { return path + "." + std::string (key); }

    // Records a problem unless an earlier one is already recorded.
    void report (std::string atPath, std::string reason) const
    {
        if (! problem.has_value())
            problem = Problem { std::move (atPath), std::move (reason) };
    }

    // The value at `key`, or nullptr (problem recorded) when this is not an object or it is missing.
    const json* value (std::string_view key) const
    {
        if (! node.is_object())
            return nullptr;
        const auto found = node.find (key);
        if (found == node.end())
        {
            report (childPath (key), "missing");
            return nullptr;
        }
        return &*found;
    }

    std::string string (std::string_view key) const
    {
        const auto* found = valueWhere (key, &json::is_string, "expected a string");
        return found != nullptr ? found->get<std::string>() : std::string();
    }

    std::int64_t integer (std::string_view key) const
    {
        const auto* found = valueWhere (key, &json::is_number_integer, "expected an integer");
        return found != nullptr ? found->get<std::int64_t>() : 0;
    }

    int smallInteger (std::string_view key) const { return static_cast<int> (integer (key)); }

    double number (std::string_view key) const
    {
        const auto* found = valueWhere (key, &json::is_number, "expected a number");
        return found != nullptr ? found->get<double>() : 0.0;
    }

    bool boolean (std::string_view key) const
    {
        const auto* found = valueWhere (key, &json::is_boolean, "expected a boolean");
        return found != nullptr && found->get<bool>();
    }

    // A channel count: 1 (mono) or 2 (stereo).
    int channels (std::string_view key) const
    {
        const int count = smallInteger (key);
        if (count != 1 && count != 2)
            report (childPath (key), "expected 1 or 2");
        return count;
    }

    Reader object (std::string_view key) const
    {
        const auto* found = value (key);
        return Reader (found != nullptr ? *found : nullNode(), childPath (key), problem);
    }

    // Reads every element of the array of objects at `key` with readElement (const Reader&).
    template <typename Element, typename ReadElement>
    std::vector<Element> objects (std::string_view key, ReadElement readElement) const
    {
        return elements<Element> (key,
                                  [&] (const json& element, std::string elementPath) { return readElement (Reader (element, std::move (elementPath), problem)); });
    }

    std::vector<int> integers (std::string_view key) const
    {
        return elements<int> (key,
                              [&] (const json& element, std::string elementPath) {
            if (element.is_number_integer())
                return element.get<int>();
            report (std::move (elementPath), "expected an integer");
            return 0;
        });
    }

private:
    static const json& nullNode()
    {
        static const json null;
        return null;
    }

    const json* valueWhere (std::string_view key, bool (json::*hasType)() const noexcept, const char* reason) const
    {
        const auto* found = value (key);
        if (found == nullptr)
            return nullptr;
        if (! (found->*hasType)())
        {
            report (childPath (key), reason);
            return nullptr;
        }
        return found;
    }

    // Reads every element of the array at `key` with readElement (const json&, elementPath).
    template <typename Element, typename ReadElement>
    std::vector<Element> elements (std::string_view key, ReadElement readElement) const
    {
        const auto* found = valueWhere (key, &json::is_array, "expected an array");
        if (found == nullptr)
            return {};

        std::vector<Element> result;
        result.reserve (found->size());
        for (std::size_t index = 0; index < found->size(); ++index)
            result.push_back (readElement ((*found)[index], childPath (key) + "[" + std::to_string (index) + "]"));
        return result;
    }

    const json& node;
    std::string path;
    std::optional<Problem>& problem;
};

// ---- Enum tables: one row per value, used both ways ----

constexpr std::array<std::pair<PreRoll, int>, 4> preRollSeconds { {
    { PreRoll::off, 0 },
    { PreRoll::oneSecond, 1 },
    { PreRoll::twoSeconds, 2 },
    { PreRoll::fourSeconds, 4 },
} };

// Every ClipSourceKind; its song.json name is toString (kind).
constexpr std::array<ClipSourceKind, 2> clipSourceKinds { ClipSourceKind::take, ClipSourceKind::backing };

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
    reader.report (reader.childPath (key), "expected 0, 1, 2 or 4");
    return PreRoll::off;
}

ClipSourceKind readClipSourceKind (const Reader& reader, std::string_view key)
{
    const auto name = reader.string (key);
    for (const auto kind : clipSourceKinds)
        if (toString (kind) == name)
            return kind;
    reader.report (reader.childPath (key), "expected \"take\" or \"backing\"");
    return ClipSourceKind::take;
}

// ---- Writing ----

json insertSlotToJson (const InsertSlot& slot)
{
    return {
        { "id", slot.id },
        { "component", { { "type", slot.component.type }, { "subtype", slot.component.subtype }, { "manufacturer", slot.component.manufacturer } } },
        { "name", slot.name },
        { "version", slot.version },
        { "bypassed", slot.bypassed },
        { "stateHash", slot.stateHash },
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

json clipToJson (const Clip& clip)
{
    return {
        { "id", clip.id },
        { "source", { { "kind", std::string (toString (clip.source.kind)) }, { "id", clip.source.id } } },
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
    return {
        { "id", track.id },
        { "name", track.name },
        { "volumeDb", track.volumeDb },
        { "pan", track.pan },
        { "mute", track.mute },
        { "solo", track.solo },
        { "armed", track.armed },
        { "inputChannels", track.inputChannels },
        { "insertChain", arrayToJson (track.insertChain, insertSlotToJson) },
        { "clips", arrayToJson (track.clips, clipToJson) },
    };
}

json passToJson (const Pass& pass)
{
    return { { "id", pass.id }, { "startSample", pass.startSample }, { "recordedAt", pass.recordedAt } };
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

Clip readClip (const Reader& reader)
{
    const auto source = reader.object ("source");
    return Clip {
        .id = reader.string ("id"),
        .source = { .kind = readClipSourceKind (source, "kind"), .id = source.string ("id") },
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
        .insertChain = reader.objects<InsertSlot> ("insertChain", readInsertSlot),
        .clips = reader.objects<Clip> ("clips", readClip),
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

std::optional<Id> readOptionalId (const Reader& reader, std::string_view key)
{
    const auto* found = reader.value (key);
    if (found == nullptr || found->is_null())
        return std::nullopt;
    if (! found->is_string())
    {
        reader.report (reader.childPath (key), "expected a string or null");
        return std::nullopt;
    }
    return found->get<std::string>();
}

Song readSong (const Reader& reader)
{
    if (reader.integer ("formatVersion") != songFormatVersion)
        reader.report (reader.childPath ("formatVersion"), "expected " + std::to_string (songFormatVersion));

    Song song;
    song.sampleRate = reader.smallInteger ("sampleRate");
    if (song.sampleRate <= 0)
        reader.report (reader.childPath ("sampleRate"), "expected a positive rate");

    song.tracks = reader.objects<Track> ("tracks", readTrack);
    const auto master = reader.object ("master");
    song.master = Master { .volumeDb = master.number ("volumeDb"), .insertChain = master.objects<InsertSlot> ("insertChain", readInsertSlot) };
    song.passes = reader.objects<Pass> ("passes", readPass);
    song.takes = reader.objects<Take> ("takes", readTake);
    song.backingTracks = reader.objects<BackingTrack> ("backingTracks", readBackingTrack);
    song.videoTakes = reader.objects<VideoTake> ("videoTakes", readVideoTake);

    song.preRoll = readPreRoll (reader, "preRollSeconds");
    song.cameraArmed = reader.boolean ("cameraArmed");
    song.cameraDeviceId = reader.string ("cameraDeviceId");
    song.playhead = reader.integer ("playhead");
    song.exportVideoTakeId = readOptionalId (reader, "exportVideoTakeId");
    return song;
}
} // namespace

nlohmann::json songToJson (const Song& song)
{
    return {
        { "formatVersion", songFormatVersion },
        { "sampleRate", song.sampleRate },
        { "tracks", arrayToJson (song.tracks, trackToJson) },
        { "master", { { "volumeDb", song.master.volumeDb }, { "insertChain", arrayToJson (song.master.insertChain, insertSlotToJson) } } },
        { "passes", arrayToJson (song.passes, passToJson) },
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
    std::optional<Problem> problem;
    auto song = readSong (Reader (document, "$", problem));
    if (problem.has_value())
        return std::unexpected (core::Error {
            "song.json_invalid",
            "song document is invalid at " + problem->jsonPath + ": " + problem->reason,
            { { "json_path", problem->jsonPath }, { "reason", problem->reason } },
        });
    return song;
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
