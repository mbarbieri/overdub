#include "core/Error.h"

#include <string>

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
    // Invariant, as in formatLogLine: `condition` and `where` always mean what OVERDUB_CHECK says,
    // so a caller field with either name is renamed to field_<key>.
    Fields allFields = Fields::object();
    if (fields.is_object())
        for (const auto& [key, value] : fields.items())
            allFields[(key == "condition" || key == "where") ? "field_" + key : key] = value;

    allFields["condition"] = condition;
    allFields["where"] = juce::File::createFileWithoutCheckingPath (file).getFileName().toStdString() + ":" + std::to_string (line);

    logEvent (Level::error, event, allFields);
    jassertfalse;
    return false;
}
} // namespace detail
} // namespace overdub::core
