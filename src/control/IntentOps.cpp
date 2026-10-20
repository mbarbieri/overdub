#include "control/IntentOps.h"

#include "core/Described.h"
#include "ui/Activity.h"
#include "ui/MainComponent.h"

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace overdub::control
{
namespace
{
constexpr bool noIntentIsNamed (std::string_view name)
{
    for (const auto op : core::operationNames<ui::Intent>())
        if (op == name)
            return false;
    return true;
}

constexpr bool reservedOpsAreFree()
{
    for (const auto op : Controller::ownOps)
        if (! noIntentIsNamed (op) || std::ranges::contains (appOps, op))
            return false;
    for (const auto op : appOps)
        if (! noIntentIsNamed (op))
            return false;
    return true;
}

static_assert (reservedOpsAreFree(), "an Intent's op name is taken by a control op, or the Controller and intentOps share one");

constexpr std::string_view groupName (std::type_identity<ui::AppIntent>)
{
    return "app";
}
constexpr std::string_view groupName (std::type_identity<ui::WindowIntent>)
{
    return "window";
}
constexpr std::string_view groupName (std::type_identity<ui::SongIntent>)
{
    return "song";
}

template <typename Group>
void appendGroupSchemas (nlohmann::json& schemas)
{
    for (auto schema : core::operationSchemas<Group>())
    {
        schema["group"] = std::string (groupName (std::type_identity<Group> {}));
        schemas.push_back (std::move (schema));
    }
}

template <typename... Groups>
nlohmann::json intentSchemas (std::type_identity<std::variant<Groups...>>)
{
    auto schemas = nlohmann::json::array();
    (appendGroupSchemas<Groups> (schemas), ...);
    return schemas;
}

nlohmann::json insertPlugins (const std::vector<plugin::AvailableAudioUnit>& plugins)
{
    auto list = nlohmann::json::array();
    for (const auto& plugin : plugins)
        list.push_back ({ { "type", plugin.component.type },
                          { "subtype", plugin.component.subtype },
                          { "manufacturer", plugin.component.manufacturer },
                          { "name", plugin.name },
                          { "version", plugin.version },
                          { "is_auv3", plugin.isAUv3 } });
    return list;
}

Reply runIntent (ui::MainComponent& content, const Request& request)
{
    auto object = request.fields;
    object["op"] = request.op;
    const auto intent = core::parseOperation<ui::Intent> (object, "control.unknown_op", "control.bad_field");
    if (! intent.has_value())
    {
        core::logError (intent.error(), core::Level::debug);
        return std::unexpected (intent.error());
    }
    return replyFor (content.run (*intent, ui::Origin::control));
}
} // namespace

Reply replyFor (const ui::IntentResult& result)
{
    if (! result.has_value())
    {
        auto error = result.error();
        if (! error.fields.is_object())
            error.fields = nlohmann::json::object();
        error.fields["refused"] = ui::isRefusal (result.error());
        return std::unexpected (std::move (error));
    }
    return nlohmann::json {
        { "created_id", result->createdId.has_value() ? nlohmann::json (*result->createdId) : nlohmann::json (nullptr) },
        { "awaiting_answer", result->awaitingAnswer },
    };
}

OpHandler intentOps (ui::MainComponent& content)
{
    return [&content] (const Request& request) -> Reply {
        if (request.op == opsOp)
            return nlohmann::json { { "ops", intentSchemas (std::type_identity<ui::Intent> {}) } };
        if (request.op == insertPluginsOp)
            return nlohmann::json { { "plugins", insertPlugins (content.listInsertPlugins ? content.listInsertPlugins() : std::vector<plugin::AvailableAudioUnit> {}) } };
        return runIntent (content, request);
    };
}
} // namespace overdub::control
