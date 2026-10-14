#include "separation/CrossFade.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using overdub::separation::chunkCountFor;
using overdub::separation::CrossFade;
using overdub::separation::strideFor;
using overdub::separation::triangularWeight;

TEST_CASE ("Chunks start every three quarters of a segment until the end of the input", "[separation][crossfade]")
{
    CHECK (strideFor (588800) == 441600);
    CHECK (strideFor (1000) == 750);
    CHECK (chunkCountFor (0, 1000) == 0);
    CHECK (chunkCountFor (1, 1000) == 1);
    CHECK (chunkCountFor (750, 1000) == 1);
    CHECK (chunkCountFor (751, 1000) == 2);
    CHECK (chunkCountFor (1500, 1000) == 2);
}

TEST_CASE ("The triangular weight rises to 1 at the middle and falls back, never reaching 0", "[separation][crossfade]")
{
    const auto weight = triangularWeight (8); // 1 2 3 4 4 3 2 1, divided by 4
    const std::vector<float> expected { 0.25f, 0.5f, 0.75f, 1.0f, 1.0f, 0.75f, 0.5f, 0.25f };
    CHECK (weight == expected);

    const auto odd = triangularWeight (5); // 1 2 3 2 1, divided by 3
    REQUIRE (odd.size() == 5);
    CHECK (odd[2] == 1.0f);
    CHECK (odd[0] == Catch::Approx (1.0f / 3.0f));
    CHECK (odd[4] == Catch::Approx (1.0f / 3.0f));
}

TEST_CASE ("The cross-fade turns constant chunk outputs back into the same constant", "[separation][crossfade]")
{
    const int segment = 1000;
    const int length = 3333; // the last chunk runs past the end
    const float constant = 0.7f;
    CrossFade crossFade (segment, length);
    std::vector<float> left ((std::size_t) length, 0.0f);
    std::vector<float> right ((std::size_t) length, 0.0f);
    const std::vector<float> chunk ((std::size_t) segment, constant);

    const int chunks = chunkCountFor (length, segment);
    REQUIRE (chunks == 5);
    for (int index = 0; index < chunks; ++index)
    {
        const int offset = index * strideFor (segment);
        crossFade.addChunk (left, offset, chunk);
        crossFade.addChunk (right, offset, chunk);
        crossFade.addChunkWeight (offset);
    }
    crossFade.finish (left);
    crossFade.finish (right);

    for (std::size_t i = 0; i < left.size(); ++i)
    {
        REQUIRE (left[i] == Catch::Approx (constant).epsilon (1e-6));
        REQUIRE (right[i] == Catch::Approx (constant).epsilon (1e-6));
    }
}
