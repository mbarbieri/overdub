#pragma once

#include "core/Error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// A plain struct described by a field table next to its members, from which its JSON form is
// derived (toJson / fromJson / operationSchemas), and vocabularies of operations: a std::variant whose leaves are
// described structs that each name an `op` (nested variants are walked).
//
//     struct MoveClip
//     {
//         static constexpr std::string_view op = "move_clip";
//         song::Id clipId;
//         song::SampleCount start = 0;
//         bool operator== (const MoveClip&) const = default;
//         static constexpr auto fields = std::tuple { core::field ("clip_id", &MoveClip::clipId),
//                                                     core::field ("start", &MoveClip::start) };
//     };
//
// A table may name members of these kinds: bool, int, std::int64_t, std::size_t, double,
// std::string, AbsolutePath, an enum with its name table (enumField), std::optional and
// std::vector of those, and a variant of operations (written as an object with its own "op"; a
// bad field inside it is named by its dotted key path, as in "edit.volume_db" or "edit.op").
// Anything else fails to compile.
//
// Pure; no JUCE. `core` logs nothing: a parse failure is an Error whose code the caller names.
namespace overdub::core
{
// An absolute POSIX path. Parsing rejects a relative one, so nothing resolves a path against the
// app's working directory.
struct AbsolutePath
{
    std::string value;
    bool operator== (const AbsolutePath&) const = default;
};

template <typename Owner, typename Member>
struct Field
{
    std::string_view key;
    Member Owner::* member;
};

template <typename Owner, typename Member>
constexpr Field<Owner, Member> field (std::string_view key, Member Owner::* member)
{
    return { key, member };
}

template <typename Enum>
using EnumNames = std::span<const std::pair<Enum, std::string_view>>;

template <typename Owner, typename Enum>
struct EnumField
{
    std::string_view key;
    Enum Owner::* member;
    EnumNames<Enum> names;
};

template <typename Owner, typename Enum>
constexpr EnumField<Owner, Enum> enumField (std::string_view key, Enum Owner::* member, EnumNames<Enum> names)
{
    return { key, member, names };
}

template <typename T>
concept Described = requires { T::fields; };

template <typename T>
concept Operation = Described<T> && requires { { T::op } -> std::convertible_to<std::string_view>; };

template <typename T>
inline constexpr bool isVariant = false;
template <typename... Alternatives>
inline constexpr bool isVariant<std::variant<Alternatives...>> = true;

template <typename T>
inline constexpr bool isOptional = false;
template <typename T>
inline constexpr bool isOptional<std::optional<T>> = true;

template <typename T>
inline constexpr bool isVector = false;
template <typename T>
inline constexpr bool isVector<std::vector<T>> = true;

// The number of leaves of an operation or a (nested) variant of operations.
template <typename T>
inline constexpr std::size_t leafCount = 1;
template <typename... Alternatives>
inline constexpr std::size_t leafCount<std::variant<Alternatives...>> = (leafCount<Alternatives> + ...);

namespace detail
{
template <typename T>
constexpr void appendLeafNames (std::string_view* names, std::size_t& count)
{
    if constexpr (isVariant<T>)
        [&]<std::size_t... I> (std::index_sequence<I...>) {
            (appendLeafNames<std::variant_alternative_t<I, T>> (names, count), ...);
        }(std::make_index_sequence<std::variant_size_v<T>> {});
    else
        names[count++] = T::op;
}
} // namespace detail

// The op names of every leaf of a variant of operations, in declaration order.
template <typename Variant>
constexpr std::array<std::string_view, leafCount<Variant>> operationNames()
{
    std::array<std::string_view, leafCount<Variant>> names {};
    std::size_t count = 0;
    detail::appendLeafNames<Variant> (names.data(), count);
    return names;
}

// True when no two leaves share an op name. A vocabulary static_asserts it.
template <typename Variant>
constexpr bool operationNamesAreUnique()
{
    const auto names = operationNames<Variant>();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j])
                return false;
    return true;
}

// True when no two fields of a described struct share a key. toJson and fromJson static_assert it.
template <Described T>
constexpr bool fieldKeysAreUnique()
{
    return std::apply (
        [] (const auto&... described) {
        const std::array<std::string_view, sizeof...(described)> keys { described.key... };
        for (std::size_t i = 0; i < keys.size(); ++i)
            for (std::size_t j = i + 1; j < keys.size(); ++j)
                if (keys[i] == keys[j])
                    return false;
        return true;
    },
        T::fields);
}

// The op name of the leaf a variant of operations holds.
template <typename T>
std::string_view operationName (const T& value)
{
    if constexpr (isVariant<T>)
        return std::visit ([] (const auto& alternative) { return operationName (alternative); }, value);
    else
        return T::op;
}

template <Described T>
nlohmann::json toJson (const T& value);

template <Described T>
Expected<T> fromJson (const nlohmann::json& object, std::string_view errorCode);

template <typename Variant>
nlohmann::json operationToJson (const Variant& value);

template <typename Variant>
Expected<Variant> parseOperation (const nlohmann::json& object, std::string_view unknownOpCode, std::string_view fieldCode);

namespace detail
{
template <typename Enum>
std::string_view enumName (Enum value, EnumNames<Enum> names)
{
    for (const auto& [candidate, name] : names)
        if (candidate == value)
            return name;
    return {};
}

template <typename Member>
nlohmann::json encode (const Member& value)
{
    if constexpr (std::is_same_v<Member, AbsolutePath>)
        return value.value;
    else if constexpr (isOptional<Member>)
        return value.has_value() ? encode (*value) : nlohmann::json (nullptr);
    else if constexpr (isVector<Member>)
    {
        auto array = nlohmann::json::array();
        for (const auto& item : value)
            array.push_back (encode (item));
        return array;
    }
    else if constexpr (isVariant<Member>)
        return operationToJson (value);
    else
    {
        static_assert (std::is_same_v<Member, bool> || std::is_same_v<Member, int> || std::is_same_v<Member, std::int64_t>
                           || std::is_same_v<Member, std::size_t> || std::is_same_v<Member, double> || std::is_same_v<Member, std::string>,
                       "a described member must be one of the kinds core/Described.h lists");
        return value;
    }
}

inline Error fieldError (std::string_view code, std::string_view key, std::string_view expected)
{
    return Error { std::string (code), "a field is missing or has the wrong type", { { "key", std::string (key) }, { "expected", std::string (expected) } } };
}

template <typename Member>
std::string_view expectedKind()
{
    if constexpr (std::is_same_v<Member, bool>)
        return "boolean";
    else if constexpr (std::is_same_v<Member, double>)
        return "number";
    else if constexpr (std::is_same_v<Member, std::size_t>)
        return "unsigned integer";
    else if constexpr (std::is_integral_v<Member>)
        return "integer";
    else if constexpr (std::is_same_v<Member, std::string>)
        return "string";
    else if constexpr (std::is_same_v<Member, AbsolutePath>)
        return "absolute path";
    else if constexpr (isOptional<Member>)
        return expectedKind<typename Member::value_type>();
    else if constexpr (isVector<Member>)
        return "array";
    else
        return "operation";
}

// Reads `json` into `member`; false when it has the wrong kind (the caller names the key).
template <typename Member>
bool decode (const nlohmann::json& json, Member& member, std::string_view errorCode, std::optional<Error>& nestedError)
{
    if constexpr (std::is_same_v<Member, bool>)
    {
        if (! json.is_boolean())
            return false;
        member = json.get<bool>();
    }
    else if constexpr (std::is_same_v<Member, double>)
    {
        if (! json.is_number())
            return false;
        member = json.get<double>();
    }
    else if constexpr (std::is_same_v<Member, std::size_t>)
    {
        if (! json.is_number_integer() || json.get<std::int64_t>() < 0)
            return false;
        member = json.get<std::size_t>();
    }
    else if constexpr (std::is_integral_v<Member>)
    {
        if (! json.is_number_integer())
            return false;
        // An unsigned JSON number past INT64_MAX would wrap negative as an int64_t.
        if (json.is_number_unsigned() && json.get<std::uint64_t>() > (std::uint64_t) std::numeric_limits<std::int64_t>::max())
            return false;
        const auto value = json.get<std::int64_t>();
        if (value < std::numeric_limits<Member>::min() || value > std::numeric_limits<Member>::max())
            return false;
        member = (Member) value;
    }
    else if constexpr (std::is_same_v<Member, std::string>)
    {
        if (! json.is_string())
            return false;
        member = json.get<std::string>();
    }
    else if constexpr (std::is_same_v<Member, AbsolutePath>)
    {
        if (! json.is_string() || ! json.get<std::string>().starts_with ('/')
            || std::ranges::any_of (json.get<std::string>(), [] (unsigned char c) { return c < 0x20 || c == 0x7f; }))
            return false;
        member = AbsolutePath { json.get<std::string>() };
    }
    else if constexpr (isOptional<Member>)
    {
        if (json.is_null())
        {
            member.reset();
            return true;
        }
        typename Member::value_type value {};
        if (! decode (json, value, errorCode, nestedError))
            return false;
        member = std::move (value);
    }
    else if constexpr (isVector<Member>)
    {
        if (! json.is_array())
            return false;
        Member items;
        for (const auto& item : json)
        {
            typename Member::value_type value {};
            if (! decode (item, value, errorCode, nestedError))
                return false;
            items.push_back (std::move (value));
        }
        member = std::move (items);
    }
    else if constexpr (isVariant<Member>)
    {
        if (! json.is_object())
            return false;
        if (json.contains ("op") && json.at ("op").is_string() && ! std::ranges::contains (operationNames<Member>(), json.at ("op").get<std::string>()))
        {
            nestedError = fieldError (errorCode, "op", "one of its op names");
            return false;
        }
        auto parsed = parseOperation<Member> (json, errorCode, errorCode);
        if (! parsed.has_value())
        {
            nestedError = parsed.error();
            return false;
        }
        member = std::move (*parsed);
    }
    else
        static_assert (sizeof (Member) == 0, "a described member must be one of the kinds core/Described.h lists");
    return true;
}

template <typename Owner, typename Member>
void writeField (nlohmann::json& object, const Owner& owner, const Field<Owner, Member>& described)
{
    object[std::string (described.key)] = encode (owner.*described.member);
}

template <typename Owner, typename Enum>
void writeField (nlohmann::json& object, const Owner& owner, const EnumField<Owner, Enum>& described)
{
    object[std::string (described.key)] = std::string (enumName (owner.*described.member, described.names));
}

template <typename Owner, typename Member>
std::optional<Error> readField (const nlohmann::json& object, Owner& owner, const Field<Owner, Member>& described, std::string_view errorCode)
{
    const auto found = object.find (described.key);
    if (found == object.end())
    {
        if constexpr (isOptional<Member>)
            return std::nullopt;
        else
            return fieldError (errorCode, described.key, expectedKind<Member>());
    }
    std::optional<Error> nestedError;
    if (! decode (*found, owner.*described.member, errorCode, nestedError))
    {
        if (! nestedError.has_value())
            return fieldError (errorCode, described.key, expectedKind<Member>());
        nestedError->fields["key"] = std::string (described.key) + "." + nestedError->fields.value ("key", std::string());
        return nestedError;
    }
    return std::nullopt;
}

template <typename Owner, typename Enum>
std::optional<Error> readField (const nlohmann::json& object, Owner& owner, const EnumField<Owner, Enum>& described, std::string_view errorCode)
{
    const auto found = object.find (described.key);
    if (found != object.end() && found->is_string())
        for (const auto& [value, name] : described.names)
            if (name == found->template get<std::string>())
            {
                owner.*described.member = value;
                return std::nullopt;
            }
    return fieldError (errorCode, described.key, "one of its names");
}

template <typename Variant, typename Alternative>
std::optional<Expected<Variant>> parseAs (const nlohmann::json& object, std::string_view op, std::string_view fieldCode);

// The leaf named `op` among the variant's leaves (from alternative I on), parsed and wrapped in
// the variant; none when no leaf has that name.
template <typename Variant, std::size_t I = 0>
std::optional<Expected<Variant>> parseLeaf (const nlohmann::json& object, std::string_view op, std::string_view fieldCode)
{
    if constexpr (I == std::variant_size_v<Variant>)
        return std::nullopt;
    else
    {
        if (auto parsed = parseAs<Variant, std::variant_alternative_t<I, Variant>> (object, op, fieldCode))
            return parsed;
        return parseLeaf<Variant, I + 1> (object, op, fieldCode);
    }
}

template <typename Variant, typename Alternative>
std::optional<Expected<Variant>> parseAs (const nlohmann::json& object, std::string_view op, std::string_view fieldCode)
{
    std::optional<Expected<Alternative>> leaf;
    if constexpr (isVariant<Alternative>)
        leaf = parseLeaf<Alternative> (object, op, fieldCode);
    else if (Alternative::op == op)
        leaf = fromJson<Alternative> (object, fieldCode);
    if (! leaf.has_value())
        return std::nullopt;
    if (! leaf->has_value())
        return Expected<Variant> (std::unexpected (leaf->error()));
    return Expected<Variant> (Variant (std::in_place_type<Alternative>, std::move (**leaf)));
}
} // namespace detail

// Writes every field of the table. An empty table writes {}.
template <Described T>
nlohmann::json toJson (const T& value)
{
    static_assert (fieldKeysAreUnique<T>(), "two fields of a described struct share a key");
    auto object = nlohmann::json::object();
    std::apply ([&] (const auto&... described) { (detail::writeField (object, value, described), ...); }, T::fields);
    return object;
}

// Reads every field of the table from `object`. A missing key is allowed only for a std::optional
// member; a missing, mistyped or unknown key fails with `errorCode` and the fields `key` and
// `expected`, so a typo is never ignored. "op" is skipped: the vocabulary's parser read it.
template <Described T>
Expected<T> fromJson (const nlohmann::json& object, std::string_view errorCode)
{
    static_assert (fieldKeysAreUnique<T>(), "two fields of a described struct share a key");
    if (! object.is_object())
        return std::unexpected (detail::fieldError (errorCode, "", "object"));
    for (const auto& [key, item] : object.items())
    {
        const auto known = key == "op" || std::apply ([&] (const auto&... described) { return ((described.key == key) || ...); }, T::fields);
        if (! known)
            return std::unexpected (detail::fieldError (errorCode, key, "no such key"));
    }
    T value {};
    std::optional<Error> failed;
    std::apply ([&] (const auto&... described) { (void) ((failed = detail::readField (object, value, described, errorCode)).has_value() || ...); }, T::fields);
    if (failed.has_value())
        return std::unexpected (*failed);
    return value;
}

// {"op": "<leaf op>", ...fields}.
template <typename Variant>
nlohmann::json operationToJson (const Variant& value)
{
    if constexpr (isVariant<Variant>)
        return std::visit ([] (const auto& alternative) { return operationToJson (alternative); }, value);
    else
    {
        auto object = toJson (value);
        object["op"] = std::string (Variant::op);
        return object;
    }
}

// Parses {"op": "<leaf op>", ...fields} into the variant, wrapping the leaf in every enclosing
// variant. No such op: `unknownOpCode` with the field `op`; bad fields: `fieldCode` (fromJson).
template <typename Variant>
Expected<Variant> parseOperation (const nlohmann::json& object, std::string_view unknownOpCode, std::string_view fieldCode)
{
    if (! object.is_object() || ! object.contains ("op") || ! object.at ("op").is_string())
        return std::unexpected (detail::fieldError (fieldCode, "op", "string"));
    const auto op = object.at ("op").get<std::string>();
    auto parsed = detail::parseLeaf<Variant> (object, op, fieldCode);
    if (! parsed.has_value())
        return std::unexpected (Error { std::string (unknownOpCode), "no operation has this name", { { "op", op } } });
    return std::move (*parsed);
}

template <typename Variant>
nlohmann::json operationSchemas();

namespace detail
{
template <typename Member>
nlohmann::json memberSchema()
{
    if constexpr (std::is_same_v<Member, bool>)
        return { { "type", "boolean" } };
    else if constexpr (std::is_same_v<Member, double>)
        return { { "type", "number" } };
    else if constexpr (std::is_same_v<Member, std::size_t>)
        return { { "type", "integer" }, { "minimum", 0 } };
    else if constexpr (std::is_integral_v<Member>)
        return { { "type", "integer" } };
    else if constexpr (std::is_same_v<Member, std::string>)
        return { { "type", "string" } };
    else if constexpr (std::is_same_v<Member, AbsolutePath>)
        return { { "type", "string" }, { "format", "absolute_path" } };
    else if constexpr (isOptional<Member>)
    {
        auto schema = memberSchema<typename Member::value_type>();
        schema["optional"] = true;
        return schema;
    }
    else if constexpr (isVector<Member>)
        return { { "type", "array" }, { "items", memberSchema<typename Member::value_type>() } };
    else if constexpr (isVariant<Member>)
        return { { "type", "object" }, { "ops", operationSchemas<Member>() } };
    else
        static_assert (sizeof (Member) == 0, "a described member must be one of the kinds core/Described.h lists");
}

template <typename Owner, typename Member>
nlohmann::json fieldSchema (const Field<Owner, Member>& described)
{
    auto schema = memberSchema<Member>();
    schema["key"] = std::string (described.key);
    return schema;
}

template <typename Owner, typename Enum>
nlohmann::json fieldSchema (const EnumField<Owner, Enum>& described)
{
    auto names = nlohmann::json::array();
    for (const auto& [value, name] : described.names)
        names.push_back (std::string (name));
    return { { "key", std::string (described.key) }, { "type", "string" }, { "enum", std::move (names) } };
}

template <typename T>
void appendLeafSchemas (nlohmann::json& schemas)
{
    if constexpr (isVariant<T>)
        [&]<std::size_t... I> (std::index_sequence<I...>) {
            (appendLeafSchemas<std::variant_alternative_t<I, T>> (schemas), ...);
        }(std::make_index_sequence<std::variant_size_v<T>> {});
    else
    {
        auto fields = nlohmann::json::array();
        std::apply ([&] (const auto&... described) { (fields.push_back (fieldSchema (described)), ...); }, T::fields);
        schemas.push_back ({ { "op", std::string (T::op) }, { "fields", std::move (fields) } });
    }
}
} // namespace detail

// What each leaf of a variant of operations is parsed from, in declaration order:
// [{"op", "fields": [{"key", "type", ...}]}]. "type" is the JSON type; an enum field lists its
// names under "enum", an optional one has "optional": true (it may be missing or null), a
// std::size_t has "minimum": 0, an AbsolutePath "format": "absolute_path", an array its "items"
// and a nested variant of operations its "ops".
template <typename Variant>
nlohmann::json operationSchemas()
{
    auto schemas = nlohmann::json::array();
    detail::appendLeafSchemas<Variant> (schemas);
    return schemas;
}

struct NotAnAlternative
{
};

template <typename Handlers, typename Variant>
inline constexpr bool handlesEach = false;
template <typename Handlers, typename... Alternatives>
inline constexpr bool handlesEach<Handlers, std::variant<Alternatives...>> = (std::is_invocable_v<Handlers&, const Alternatives&> && ...);

// Handlers that take each alternative of the variant and are not a catch-all: a handler that would
// also take NotAnAlternative (a lambda taking `auto`) or the variant itself (which every
// alternative converts to) would hide an alternative left unhandled.
template <typename Handlers, typename Variant>
concept HandlesEachAlternative = handlesEach<Handlers, Variant> && ! std::is_invocable_v<Handlers&, const NotAnAlternative&>
                              && ! std::is_invocable_v<Handlers&, const Variant&>;

// std::visit that does not compile unless every alternative has its own handler.
template <typename Variant, typename Handlers>
    requires HandlesEachAlternative<Handlers, Variant>
decltype (auto) visitExhaustively (const Variant& value, Handlers&& handlers)
{
    return std::visit (handlers, value);
}

template <typename... Handlers>
struct Overloaded : Handlers...
{
    using Handlers::operator()...;
};
template <typename... Handlers>
Overloaded (Handlers...) -> Overloaded<Handlers...>;
} // namespace overdub::core
