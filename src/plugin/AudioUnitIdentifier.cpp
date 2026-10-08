#include "plugin/AudioUnitIdentifier.h"

#include <vector>

namespace overdub::plugin
{
std::string componentText (const song::AudioUnitComponent& component)
{
    return component.type + "," + component.subtype + "," + component.manufacturer;
}

std::string identifierFor (const song::AudioUnitComponent& component)
{
    return "AudioUnit:Effects/" + componentText (component);
}

std::optional<song::AudioUnitComponent> componentFromIdentifier (const std::string& identifier)
{
    const auto codesStart = identifier.find_last_of (":/");
    const auto codes = codesStart == std::string::npos ? identifier : identifier.substr (codesStart + 1);

    std::vector<std::string> parts;
    std::string::size_type from = 0;
    while (true)
    {
        const auto comma = codes.find (',', from);
        parts.push_back (codes.substr (from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos)
            break;
        from = comma + 1;
    }

    if (parts.size() != 3)
        return std::nullopt;
    for (const auto& part : parts)
        if (part.size() != 4)
            return std::nullopt;
    return song::AudioUnitComponent { parts[0], parts[1], parts[2] };
}
} // namespace overdub::plugin
