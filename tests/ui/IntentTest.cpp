#include "ui/Intent.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace overdub;
using namespace overdub::ui;

namespace
{
// Sets a member to a value unlike its default, so a field that does not round-trip shows.
void fill (bool& value, std::string_view)
{
    value = ! value;
}
void fill (int& value, std::string_view)
{
    value = 7;
}
void fill (std::int64_t& value, std::string_view)
{
    value = -48000;
}
void fill (std::size_t& value, std::string_view)
{
    value = 3;
}
void fill (double& value, std::string_view)
{
    value = -6.5;
}
void fill (std::string& value, std::string_view key)
{
    value = "x-" + std::string (key);
}
void fill (core::AbsolutePath& path, std::string_view key)
{
    path.value = "/tmp/" + std::string (key);
}
template <typename T>
void fill (std::optional<T>& value, std::string_view key);
template <typename T>
void fill (std::vector<T>& values, std::string_view key);
template <typename... Alternatives>
void fill (std::variant<Alternatives...>& value, std::string_view key);

template <typename Owner, typename Member>
void fillField (Owner& owner, const core::Field<Owner, Member>& described)
{
    fill (owner.*described.member, described.key);
}

template <typename Owner, typename Enum>
void fillField (Owner& owner, const core::EnumField<Owner, Enum>& described)
{
    const auto original = owner.*described.member;
    for (const auto& [value, name] : described.names)
        if (value != original)
        {
            owner.*described.member = value;
            return;
        }
}

template <typename T>
void fillFields (T& value)
{
    std::apply ([&] (const auto&... described) { (fillField (value, described), ...); }, T::fields);
}

template <typename T>
void fill (std::optional<T>& value, std::string_view key)
{
    T inner {};
    fill (inner, key);
    value = std::move (inner);
}

template <typename T>
void fill (std::vector<T>& values, std::string_view key)
{
    values.resize (2);
    for (auto& value : values)
        fill (value, key);
}

template <typename... Alternatives>
void fill (std::variant<Alternatives...>& value, std::string_view)
{
    using Last = std::variant_alternative_t<sizeof...(Alternatives) - 1, std::variant<Alternatives...>>;
    Last leaf {};
    fillFields (leaf);
    value = std::move (leaf);
}

template <typename T, typename Wrap, typename CheckLeaf>
void forEachLeaf (Wrap wrap, CheckLeaf& checkLeaf)
{
    if constexpr (core::isVariant<T>)
        [&]<std::size_t... I> (std::index_sequence<I...>) {
            (forEachLeaf<std::variant_alternative_t<I, T>> ([wrap] (auto inner) { return wrap (T { std::in_place_index<I>, std::move (inner) }); }, checkLeaf), ...);
        }(std::make_index_sequence<std::variant_size_v<T>> {});
    else
        checkLeaf.template operator()<T> (wrap);
}

bool isSnakeCase (std::string_view name)
{
    if (name.empty() || name.front() == '_' || name.back() == '_')
        return false;
    for (const auto character : name)
        if (! ((character >= 'a' && character <= 'z') || character == '_'))
            return false;
    return true;
}

// Intents are compared by their JSON form, which names every field.
nlohmann::json songIntent (SongIntent intent)
{
    return core::operationToJson (Intent { std::move (intent) });
}

nlohmann::json windowIntent (WindowIntent intent)
{
    return core::operationToJson (Intent { std::move (intent) });
}

std::optional<nlohmann::json> keyRuns (KeyChord key)
{
    const auto intent = intentForKey (key);
    if (! intent.has_value())
        return std::nullopt;
    return core::operationToJson (*intent);
}
} // namespace

TEST_CASE ("every Intent leaf round-trips through its JSON form, named by its op", "[ui][intent]")
{
    std::size_t leaves = 0;
    auto checkLeaf = [&leaves]<typename Leaf> (const auto& wrap) {
        ++leaves;
        Leaf leaf {};
        fillFields (leaf);
        if constexpr (std::tuple_size_v<std::remove_cvref_t<decltype (Leaf::fields)>> > 0)
            CHECK (core::toJson (leaf) != core::toJson (Leaf {}));
        const Intent intent = wrap (leaf);

        const auto json = core::operationToJson (intent);
        INFO (json.dump());
        CHECK (json.at ("op") == std::string (Leaf::op));
        CHECK (json.size() == std::tuple_size_v<std::remove_cvref_t<decltype (Leaf::fields)>> + 1);
        CHECK (opName (intent) == Leaf::op);
        CHECK (isSnakeCase (Leaf::op));
        const auto parsed = core::parseOperation<Intent> (json, "control.unknown_op", "control.bad_field");
        REQUIRE (parsed.has_value());
        CHECK (core::operationToJson (*parsed) == json);
    };
    forEachLeaf<Intent> ([] (Intent intent) { return intent; }, checkLeaf);

    CHECK (leaves == core::leafCount<Intent>);
    CHECK (leaves >= 70);
}

TEST_CASE ("keys 1 and 2 show the Record and Edit modes, and command-E opens the Export sheet", "[ui][intent][keys]")
{
    CHECK (keyRuns (KeyChord { '1' }) == windowIntent (intent::ShowMode { Mode::record }));
    CHECK (keyRuns (KeyChord { '2' }) == windowIntent (intent::ShowMode { Mode::edit }));
    CHECK (keyRuns (KeyChord { '3' }) == std::nullopt);
    CHECK (keyRuns (KeyChord { 'e', true }) == songIntent (intent::ShowExport { true }));
    CHECK (keyRuns (KeyChord { 'E', true }) == songIntent (intent::ShowExport { true }));
    CHECK (keyRuns (KeyChord { 'e', true, true }) == std::nullopt);
    CHECK (keyRuns (KeyChord { 'e' }) == std::nullopt);
}

TEST_CASE ("R records, Space plays or stops, command-Z undoes and shift-command-Z redoes", "[ui][intent][keys]")
{
    CHECK (keyRuns (KeyChord { 'r' }) == songIntent (intent::Record {}));
    CHECK (keyRuns (KeyChord { 'R' }) == songIntent (intent::Record {}));
    CHECK (keyRuns (KeyChord { ' ' }) == songIntent (intent::PlayOrStop {}));

    CHECK (keyRuns (KeyChord { 'z', true, false }) == songIntent (intent::Undo {}));
    CHECK (keyRuns (KeyChord { 'Z', true, true }) == songIntent (intent::Redo {}));
    CHECK (keyRuns (KeyChord { 'z' }) == std::nullopt);
    CHECK (keyRuns (KeyChord { 'r', true, false }) == std::nullopt);
    CHECK (keyRuns (KeyChord { '1', true, false }) == std::nullopt);
    CHECK (keyRuns (KeyChord { ' ', false, true }) == std::nullopt);
    CHECK (keyRuns (KeyChord { 'R', false, true }) == std::nullopt);
}

TEST_CASE ("S splits the selected Clip, and backspace or forward delete deletes the selection", "[ui][intent][keys]")
{
    CHECK (keyRuns (KeyChord { 's' }) == songIntent (intent::SplitSelectedClip {}));
    CHECK (keyRuns (KeyChord { 'S' }) == songIntent (intent::SplitSelectedClip {}));
    CHECK (keyRuns (KeyChord { backspaceCharacter }) == songIntent (intent::DeleteSelection {}));
    CHECK (keyRuns (KeyChord { forwardDeleteCharacter }) == songIntent (intent::DeleteSelection {}));
    CHECK (keyRuns (KeyChord { 's', true, false }) == std::nullopt);
    CHECK (keyRuns (KeyChord { backspaceCharacter, true, false }) == std::nullopt);
}

TEST_CASE ("the left and right arrows nudge the playhead back and forward", "[ui][intent][keys]")
{
    CHECK (keyRuns (KeyChord { leftArrowCharacter }) == songIntent (intent::Nudge { Direction::back }));
    CHECK (keyRuns (KeyChord { rightArrowCharacter }) == songIntent (intent::Nudge { Direction::forward }));
    CHECK (keyRuns (KeyChord { leftArrowCharacter, true, false }) == std::nullopt);
    CHECK (keyRuns (KeyChord { rightArrowCharacter, false, true }) == std::nullopt);
}
