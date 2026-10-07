#include "core/Error.h"

namespace overdub::core
{
void logError (const Error& error, Level level)
{
    logEvent (level, error.code, error.fields, error.message);
}

namespace detail
{
bool reportFailedCheck (const char* condition, const char* event, const char* file, int line, const Fields& fields)
{
    auto allFields = fields;
    allFields["condition"] = condition;
    allFields["where"] = juce::File::createFileWithoutCheckingPath (file).getFileName().toStdString() + ":" + std::to_string (line);

    logEvent (Level::error, event, allFields);
    jassertfalse;
    return false;
}
} // namespace detail
} // namespace overdub::core
