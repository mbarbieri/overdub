#include "core/Described.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace overdub;

namespace
{
enum class Colour
{
    red,
    blue
};
constexpr std::pair<Colour, std::string_view> colourNames[] { { Colour::red, "red" }, { Colour::blue, "blue" } };

struct Paint
{
    static constexpr std::string_view op = "paint";
    std::string wall;
    Colour colour = Colour::red;
    std::optional<double> coats;
    std::vector<int> layers;
    core::AbsolutePath file;
    bool operator== (const Paint&) const = default;
    static constexpr auto fields = std::tuple { core::field ("wall", &Paint::wall),
                                                core::enumField ("colour", &Paint::colour, core::EnumNames<Colour> { colourNames }),
                                                core::field ("coats", &Paint::coats),
                                                core::field ("layers", &Paint::layers),
                                                core::field ("file", &Paint::file) };
};

struct Erase
{
    static constexpr std::string_view op = "erase";
    std::size_t index = 0;
    std::int64_t at = 0;
    bool operator== (const Erase&) const = default;
    static constexpr auto fields = std::tuple { core::field ("index", &Erase::index), core::field ("at", &Erase::at) };
};

struct Rest
{
    static constexpr std::string_view op = "rest";
    bool operator== (const Rest&) const = default;
    static constexpr auto fields = std::tuple {};
};

using Tool = std::variant<Paint, Erase>;

struct Preview
{
    static constexpr std::string_view op = "preview";
    std::optional<Tool> tool;
    bool operator== (const Preview&) const = default;
    static constexpr auto fields = std::tuple { core::field ("tool", &Preview::tool) };
};

using Command = std::variant<Tool, Rest, Preview>;

struct Twin
{
    static constexpr std::string_view op = "rest";
};

struct SameKeyTwice
{
    int first = 0;
    int second = 0;
    static constexpr auto fields = std::tuple { core::field ("value", &SameKeyTwice::first), core::field ("value", &SameKeyTwice::second) };
};

constexpr auto badField = "test.bad_field";
constexpr auto unknownOp = "test.unknown_op";

const Paint paint { .wall = "north", .colour = Colour::blue, .coats = 2.5, .layers = { 3, 1 }, .file = { "/tmp/plan.txt" } };

template <typename Handlers>
constexpr bool canVisit = requires (const Command& command, Handlers handlers) { core::visitExhaustively (command, handlers); };

const auto onTool = [] (const Tool&) { return 1; };
const auto onRest = [] (const Rest&) { return 2; };
const auto onPreview = [] (const Preview&) { return 3; };
} // namespace

TEST_CASE ("a described struct writes each field under its key and reads the same struct back", "[core][described]")
{
    const auto json = core::toJson (paint);

    CHECK (json == nlohmann::json { { "wall", "north" }, { "colour", "blue" }, { "coats", 2.5 }, { "layers", { 3, 1 } }, { "file", "/tmp/plan.txt" } });
    const auto read = core::fromJson<Paint> (json, badField);
    REQUIRE (read.has_value());
    CHECK (*read == paint);
}

TEST_CASE ("an optional field may be missing or null; every other field must be there with its kind", "[core][described]")
{
    auto json = core::toJson (paint);
    json.erase ("coats");
    CHECK (core::fromJson<Paint> (json, badField)->coats == std::nullopt);
    json["coats"] = nullptr;
    CHECK (core::fromJson<Paint> (json, badField)->coats == std::nullopt);

    const auto failsOn = [] (nlohmann::json changed, const std::string& key) {
        const auto read = core::fromJson<Paint> (changed, badField);
        REQUIRE_FALSE (read.has_value());
        CHECK (read.error().code == badField);
        CHECK (read.error().fields.at ("key") == key);
    };
    const auto paintWith = [] (const std::string& key, nlohmann::json value) {
        auto changed = core::toJson (paint);
        changed[key] = std::move (value);
        return changed;
    };
    auto missing = core::toJson (paint);
    missing.erase ("wall");
    failsOn (missing, "wall");
    failsOn (paintWith ("layers", "3"), "layers");
    failsOn (paintWith ("colour", "green"), "colour");
    failsOn (paintWith ("file", "relative/plan.txt"), "file");
    failsOn (paintWith ("walls", "typo"), "walls");
}

TEST_CASE ("integers are checked against their member's range", "[core][described]")
{
    CHECK (core::fromJson<Erase> (nlohmann::json { { "index", 4 }, { "at", -48000 } }, badField) == Erase { 4, -48000 });
    CHECK_FALSE (core::fromJson<Erase> (nlohmann::json { { "index", -1 }, { "at", 0 } }, badField).has_value());
    CHECK_FALSE (core::fromJson<Erase> (nlohmann::json { { "index", 1.5 }, { "at", 0 } }, badField).has_value());

    const auto pastSigned = core::fromJson<Erase> (nlohmann::json { { "index", 1 }, { "at", std::numeric_limits<std::uint64_t>::max() } }, badField);
    REQUIRE_FALSE (pastSigned.has_value());
    CHECK (pastSigned.error().code == badField);
    CHECK (pastSigned.error().fields.at ("key") == "at");
}

TEST_CASE ("a field table with two equal keys is caught", "[core][described]")
{
    STATIC_REQUIRE (core::fieldKeysAreUnique<Paint>());
    STATIC_REQUIRE (core::fieldKeysAreUnique<Rest>());
    STATIC_REQUIRE_FALSE (core::fieldKeysAreUnique<SameKeyTwice>());
}

TEST_CASE ("an operation's JSON names its op, and parsing it wraps the leaf in every enclosing variant", "[core][described]")
{
    const Command painted { Tool { paint } };
    const auto json = core::operationToJson (painted);

    CHECK (json.at ("op") == "paint");
    CHECK (json.at ("wall") == "north");
    const auto parsed = core::parseOperation<Command> (json, unknownOp, badField);
    REQUIRE (parsed.has_value());
    CHECK (*parsed == painted);
    CHECK (core::operationName (*parsed) == "paint");

    const Command previewed { Preview { Tool { Erase { 2, 7 } } } };
    const auto previewJson = core::operationToJson (previewed);
    CHECK (previewJson == nlohmann::json { { "op", "preview" }, { "tool", { { "op", "erase" }, { "index", 2 }, { "at", 7 } } } });
    CHECK (core::parseOperation<Command> (previewJson, unknownOp, badField) == previewed);
}

TEST_CASE ("an unknown op or a request without one is refused with the caller's codes", "[core][described]")
{
    const auto unknown = core::parseOperation<Command> (nlohmann::json { { "op", "polish" } }, unknownOp, badField);
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == unknownOp);
    CHECK (unknown.error().fields.at ("op") == "polish");

    const auto noOp = core::parseOperation<Command> (nlohmann::json { { "wall", "north" } }, unknownOp, badField);
    REQUIRE_FALSE (noOp.has_value());
    CHECK (noOp.error().code == badField);
    CHECK (noOp.error().fields.at ("key") == "op");
}

TEST_CASE ("op names are listed in declaration order, and a vocabulary with two equal names is caught", "[core][described]")
{
    STATIC_REQUIRE (core::operationNames<Command>() == std::array<std::string_view, 4> { "paint", "erase", "rest", "preview" });
    STATIC_REQUIRE (core::operationNamesAreUnique<Command>());
    STATIC_REQUIRE_FALSE (core::operationNamesAreUnique<std::variant<Command, Twin>>());
}

TEST_CASE ("visitExhaustively takes only one handler per alternative, never a catch-all", "[core][described]")
{
    const auto all = core::Overloaded { onTool, onRest, onPreview };
    STATIC_REQUIRE (canVisit<decltype (all)>);
    STATIC_REQUIRE_FALSE (canVisit<decltype (core::Overloaded { onTool, onRest })>);
    STATIC_REQUIRE_FALSE (canVisit<decltype (core::Overloaded { onTool, onRest, onPreview, [] (const auto&) { return 0; } })>);
    STATIC_REQUIRE_FALSE (canVisit<decltype ([] (const auto&) { return 0; })>);
    STATIC_REQUIRE_FALSE (canVisit<decltype ([] (const Command&) { return 0; })>);
    STATIC_REQUIRE_FALSE (canVisit<decltype (core::Overloaded { onTool, onRest, [] (const Command&) { return 0; } })>);

    CHECK (core::visitExhaustively (Command { Rest {} }, all) == 2);
    CHECK (core::visitExhaustively (Command { Tool { Erase {} } }, all) == 1);
}
