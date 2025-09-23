#include <gtest/gtest.h>

#include "ck/find/cli_buffer_utils.hpp"

#include <array>
#include <string>

TEST(CkFindCliBufferUtils, CopyToArrayHandlesNullAndTruncation)
{
    std::array<char, 8> buffer{};
    ck::find::copyToArray(buffer, nullptr);
    EXPECT_STREQ(buffer.data(), "");

    ck::find::copyToArray(buffer, "abcdefghijk");
    EXPECT_EQ(buffer.back(), '\0');
    EXPECT_STREQ(buffer.data(), "abcdefg");
}

TEST(CkFindCliBufferUtils, BufferToStringStopsAtNullTerminator)
{
    std::array<char, 6> buffer{};
    ck::find::copyToArray(buffer, "abc");
    std::string value = ck::find::bufferToString(buffer);
    EXPECT_EQ(value, "abc");

    buffer = {'n', 'u', 'l', 'l', 's', 's'};
    value = ck::find::bufferToString(buffer);
    EXPECT_EQ(value, "nullss");
}
