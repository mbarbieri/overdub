#include "plugin/AudioUnitIdentifier.h"

#include <catch2/catch_test_macros.hpp>

using overdub::plugin::componentFromIdentifier;
using overdub::plugin::identifierFor;
using overdub::song::AudioUnitComponent;

TEST_CASE ("an AU component becomes JUCE's effect identifier and back", "[plugin][identifier]")
{
    const AudioUnitComponent delay { "aufx", "dely", "appl" };
    CHECK (identifierFor (delay) == "AudioUnit:Effects/aufx,dely,appl");
    CHECK (componentFromIdentifier (identifierFor (delay)) == delay);
    CHECK (componentFromIdentifier ("AudioUnit:aumf,Nam1,Nmdl") == AudioUnitComponent { "aumf", "Nam1", "Nmdl" });
}

TEST_CASE ("an identifier without exactly three four-character codes is no AU component", "[plugin][identifier]")
{
    CHECK_FALSE (componentFromIdentifier ("AudioUnit:Effects/aufx,dely").has_value());
    CHECK_FALSE (componentFromIdentifier ("AudioUnit:Effects/aufx,dely,appl,more").has_value());
    CHECK_FALSE (componentFromIdentifier ("AudioUnit:Effects/aufx,delay,appl").has_value());
    CHECK_FALSE (componentFromIdentifier ("").has_value());
}
