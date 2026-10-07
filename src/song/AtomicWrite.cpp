#include "song/AtomicWrite.h"

#include "song/SongJson.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace overdub::song
{
std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

core::Error writeFailed (const juce::File& file, const char* step, int savedErrno)
{
    return core::Error {
        "song.write_failed",
        std::string ("could not write a package file (") + step + ")",
        { { "path", pathOf (file) }, { "step", step }, { "errno", savedErrno } },
    };
}

namespace
{
// Writes all of `bytes` to a new file at `file` and fsyncs it.
core::Expected<void> writeAndSync (const juce::File& file, std::string_view bytes)
{
    const int descriptor = ::open (file.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0)
        return std::unexpected (writeFailed (file, "open", errno));

    std::size_t written = 0;
    while (written < bytes.size())
    {
        const auto result = ::write (descriptor, bytes.data() + written, bytes.size() - written);
        if (result < 0)
        {
            const int writeErrno = errno;
            ::close (descriptor);
            return std::unexpected (writeFailed (file, "write", writeErrno));
        }
        written += static_cast<std::size_t> (result);
    }

    if (::fsync (descriptor) != 0)
    {
        const int syncErrno = errno;
        ::close (descriptor);
        return std::unexpected (writeFailed (file, "fsync", syncErrno));
    }
    ::close (descriptor);
    return {};
}
} // namespace

core::Expected<void> writeFileAtomically (const juce::File& destination, std::string_view bytes, const BeforeRenameHook& beforeRename)
{
    const auto tempFile = destination.getSiblingFile (destination.getFileName() + ".tmp");

    if (auto written = writeAndSync (tempFile, bytes); ! written.has_value())
        return written;

    if (beforeRename)
        if (auto injected = beforeRename (destination); injected.has_value())
            return std::unexpected (*injected);

    if (std::rename (tempFile.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8()) != 0)
        return std::unexpected (writeFailed (destination, "rename", errno));
    return {};
}

core::Expected<void> syncFile (const juce::File& file)
{
    const int descriptor = ::open (file.getFullPathName().toRawUTF8(), O_RDONLY);
    if (descriptor < 0)
        return std::unexpected (writeFailed (file, "fsync", errno));
    const int synced = ::fsync (descriptor);
    const int syncErrno = errno;
    ::close (descriptor);
    if (synced != 0)
        return std::unexpected (writeFailed (file, "fsync", syncErrno));
    return {};
}

core::Expected<std::size_t> writeJsonAtomically (const juce::File& destination, const nlohmann::json& document, const BeforeRenameHook& beforeRename)
{
    std::string text;
    try
    {
        text = document.dump (1);
    }
    catch (const nlohmann::json::exception& exception)
    {
        return std::unexpected (core::Error {
            "song.json_unserializable",
            "could not serialize a package file",
            { { "path", pathOf (destination) }, { "reason", exception.what() } },
        });
    }

    if (auto written = writeFileAtomically (destination, text, beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    return text.size();
}

core::Expected<std::string> readTextFile (const juce::File& file)
{
    juce::MemoryBlock contents;
    if (! file.existsAsFile() || ! file.loadFileAsData (contents))
        return std::unexpected (core::Error {
            "song.read_failed",
            "could not read a package file",
            { { "path", pathOf (file) } },
        });
    return contents.toString().toStdString();
}

core::Expected<nlohmann::json> readJsonFile (const juce::File& file)
{
    const auto text = readTextFile (file);
    if (! text.has_value())
        return std::unexpected (text.error());
    auto parsed = parseJsonText (*text);
    if (! parsed.has_value())
        return std::unexpected (withFilePath (parsed.error(), file));
    return parsed;
}

core::Error withFilePath (core::Error error, const juce::File& file)
{
    error.fields["path"] = pathOf (file);
    return error;
}

int deleteTempFiles (const juce::File& directory)
{
    int deleted = 0;
    for (const auto& file : directory.findChildFiles (juce::File::findFiles, true, "*.tmp"))
    {
        if (file.deleteFile())
            ++deleted;
        else
            core::logEvent (core::Level::warn, "song.temp_delete_failed", { { "path", pathOf (file) } });
    }
    return deleted;
}
} // namespace overdub::song
