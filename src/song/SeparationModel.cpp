#include "song/SeparationModel.h"

#include "song/AtomicWrite.h"
#include "song/Separation.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace overdub::song
{
namespace
{
constexpr int metadataPropsField = 14; // ModelProto.metadata_props (StringStringEntryProto)
constexpr std::uint64_t maxMetadataEntryBytes = 1 << 20;

core::Error modelInvalid (const juce::File& model, core::Fields fields)
{
    fields["path"] = pathOf (model);
    return core::Error { "song.separation_model_invalid", "the installed Separation model is not the expected graph", std::move (fields) };
}

// A protobuf varint from the stream; nullopt at its end or past 10 bytes.
std::optional<std::uint64_t> readVarint (juce::InputStream& in)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7)
    {
        if (in.isExhausted())
            return std::nullopt;
        const auto byte = (std::uint8_t) in.readByte();
        value |= (std::uint64_t) (byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return value;
    }
    return std::nullopt;
}

// A varint from a byte buffer at `pos`, advancing it.
std::optional<std::uint64_t> varintAt (const std::string& bytes, std::size_t& pos)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64 && pos < bytes.size(); shift += 7)
    {
        const auto byte = (std::uint8_t) bytes[pos++];
        value |= (std::uint64_t) (byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return value;
    }
    return std::nullopt;
}

// StringStringEntryProto: key (1), value (2). nullopt when malformed.
std::optional<std::pair<std::string, std::string>> parseEntry (const std::string& bytes)
{
    std::pair<std::string, std::string> entry;
    std::size_t pos = 0;
    while (pos < bytes.size())
    {
        const auto tag = varintAt (bytes, pos);
        if (! tag.has_value() || (*tag & 7) != 2)
            return std::nullopt;
        const auto length = varintAt (bytes, pos);
        if (! length.has_value() || *length > bytes.size() - pos)
            return std::nullopt;
        const auto text = bytes.substr (pos, (std::size_t) *length);
        pos += (std::size_t) *length;
        if ((*tag >> 3) == 1)
            entry.first = text;
        else if ((*tag >> 3) == 2)
            entry.second = text;
    }
    return entry;
}

std::string canonicalSources (const std::string& raw)
{
    const auto parsed = nlohmann::json::parse (raw, nullptr, false);
    return parsed.is_discarded() ? raw : parsed.dump();
}
} // namespace

core::Expected<std::map<std::string, std::string>> readOnnxMetadata (const juce::File& model)
{
    juce::FileInputStream in (model);
    if (! in.openedOk())
        return std::unexpected (modelInvalid (model, { { "reason", "unreadable" } }));
    const auto notOnnx = [&model] { return std::unexpected (modelInvalid (model, { { "reason", "not_onnx" } })); };

    std::map<std::string, std::string> metadata;
    const auto total = (std::uint64_t) in.getTotalLength();
    while (! in.isExhausted())
    {
        const auto tag = readVarint (in);
        if (! tag.has_value() || (*tag >> 3) == 0)
            return notOnnx();
        const auto remaining = total - (std::uint64_t) in.getPosition();
        switch (*tag & 7)
        {
            case 0:
                if (! readVarint (in).has_value())
                    return notOnnx();
                break;
            case 1:
            case 5:
            {
                const std::uint64_t size = (*tag & 7) == 1 ? 8 : 4;
                if (size > remaining)
                    return notOnnx();
                in.skipNextBytes ((juce::int64) size);
                break;
            }
            case 2:
            {
                const auto length = readVarint (in);
                if (! length.has_value() || *length > total - (std::uint64_t) in.getPosition())
                    return notOnnx();
                if ((*tag >> 3) != metadataPropsField)
                {
                    in.setPosition (in.getPosition() + (juce::int64) *length);
                    break;
                }
                if (*length > maxMetadataEntryBytes)
                    return notOnnx();
                std::string bytes ((std::size_t) *length, '\0');
                if (in.read (bytes.data(), (int) bytes.size()) != (int) bytes.size())
                    return notOnnx();
                const auto entry = parseEntry (bytes);
                if (! entry.has_value())
                    return notOnnx();
                metadata[entry->first] = entry->second;
                break;
            }
            default:
                return notOnnx();
        }
    }
    return metadata;
}

core::Expected<void> checkSeparationModel (const juce::File& model)
{
    if (! model.existsAsFile())
        return std::unexpected (core::Error { "song.separation_model_missing", "the Separation model is not installed", { { "path", pathOf (model) } } });
    const auto metadata = readOnnxMetadata (model);
    if (! metadata.has_value())
        return std::unexpected (metadata.error());

    std::vector<std::string> stems;
    for (const auto& part : separationParts)
        stems.emplace_back (part.fileStem);
    const std::vector<std::pair<std::string, std::string>> expected {
        { "architecture", "bs_roformer" },
        { "sources", nlohmann::json (stems).dump() },
    };
    for (const auto& [key, value] : expected)
    {
        const auto found = metadata->find (key);
        if (found == metadata->end())
            return std::unexpected (modelInvalid (model, { { "key", key }, { "expected", value }, { "actual", "missing" } }));
        const auto actual = key == "sources" ? canonicalSources (found->second) : found->second;
        if (actual != value)
            return std::unexpected (modelInvalid (model, { { "key", key }, { "expected", value }, { "actual", found->second } }));
    }
    return {};
}
} // namespace overdub::song
