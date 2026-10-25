#pragma once

#include "control/Controller.h"
#include "ui/Intent.h"

#include <array>
#include <string_view>

namespace overdub::ui
{
class MainComponent;
}

// The ops the app answers beside the Controller's own (ADR 0008), on the message thread:
//   ops             {"ops": [{"op", "group", "fields"}]}: every Intent's schema, derived from the
//                   ui::Intent variant (core::operationSchemas); group is "app", "window" or "song"
//   insert_plugins  {"plugins": [{"type", "subtype", "manufacturer", "name", "version", "is_auv3"}]}:
//                   the AU effects on offer (MainComponent::listInsertPlugins), which add_insert names
//   any Intent      parsed from the request (core::parseOperation: control.unknown_op,
//                   control.bad_field), run through MainComponent::run with Origin::control and
//                   answered by replyFor
namespace overdub::control
{
inline constexpr std::string_view opsOp = "ops";
inline constexpr std::string_view insertPluginsOp = "insert_plugins";
// The ops intentOps answers beside the Intents.
inline constexpr std::array appOps { opsOp, insertPluginsOp };

// {"created_id": <id or null>, "awaiting_answer": <bool>}, or the Error the handler handled with
// the field "refused": whether the running activity refused the Intent (ui::isRefusal).
Reply replyFor (const ui::IntentResult& result);

// `content` outlives the returned handler.
OpHandler intentOps (ui::MainComponent& content);
} // namespace overdub::control
