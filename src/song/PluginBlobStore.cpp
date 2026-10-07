#include "song/PluginBlobStore.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>

namespace overdub::song
{
namespace
{
bool isSha256Hex (const std::string& hash)
{
    return hash.size() == 2 * CC_SHA256_DIGEST_LENGTH
           && hash.find_first_not_of ("0123456789abcdef") == std::string::npos;
}

juce::File blobFile (const juce::File& pluginsDirectory, const std::string& hash)
{
    return pluginsDirectory.getChildFile (hash + ".state");
}
} // namespace

std::string sha256Hex (const juce::MemoryBlock& bytes)
{
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest {};
    CC_SHA256 (bytes.getData(), static_cast<CC_LONG> (bytes.getSize()), digest.data());
    return juce::String::toHexString (digest.data(), static_cast<int> (digest.size()), 0).toStdString();
}

core::Expected<std::string> storePluginBlob (const juce::File& pluginsDirectory, const juce::MemoryBlock& state, const BeforeRenameHook& beforeRename)
{
    const auto hash = sha256Hex (state);
    const auto file = blobFile (pluginsDirectory, hash);
    if (file.existsAsFile())
        return hash;

    const std::string_view bytes (static_cast<const char*> (state.getData()), state.getSize());
    if (auto written = writeFileAtomically (file, bytes, beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    return hash;
}

core::Expected<juce::MemoryBlock> loadPluginBlob (const juce::File& pluginsDirectory, const std::string& hash)
{
    if (! isSha256Hex (hash))
        return std::unexpected (core::Error { "song.plugin_state_invalid_hash", "not a plugin state hash", { { "hash", hash } } });

    juce::MemoryBlock state;
    if (! blobFile (pluginsDirectory, hash).loadFileAsData (state))
        return std::unexpected (core::Error { "song.plugin_state_missing", "plugin state file is missing", { { "hash", hash } } });
    return state;
}

int deleteUnreferencedPluginBlobs (const juce::File& pluginsDirectory, const std::set<std::string>& referenced)
{
    int deleted = 0;
    for (const auto& file : pluginsDirectory.findChildFiles (juce::File::findFiles, false, "*.state"))
    {
        if (referenced.contains (file.getFileNameWithoutExtension().toStdString()))
            continue;
        if (file.deleteFile())
            ++deleted;
        else
            core::logEvent (core::Level::warn, "song.plugin_blob_delete_failed", { { "path", pathOf (file) } });
    }
    return deleted;
}
} // namespace overdub::song
