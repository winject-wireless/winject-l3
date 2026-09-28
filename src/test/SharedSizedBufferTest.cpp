#include <bfcext/shared_sized_buffer.hpp>

#include <gtest/gtest.h>

#include <cstring>

TEST(SharedSizedBufferTest, CopyFromAndSubviewShareStorage)
{
    const uint8_t raw[] = {0, 1, 2, 3, 4, 5, 6, 7};
    bfcext::shared_sized_buffer whole =
        bfcext::shared_sized_buffer::copy_from(raw, sizeof(raw));
    EXPECT_EQ(whole.size(), sizeof(raw));
    EXPECT_FALSE(whole.empty());

    bfcext::shared_sized_buffer slice = whole.subview(2, 3);
    EXPECT_EQ(slice.size(), 3u);
    EXPECT_EQ(slice.data()[0], std::byte{2});
    EXPECT_EQ(slice.data()[2], std::byte{4});

    EXPECT_EQ(whole.storage(), slice.storage());

    bfcext::shared_sized_buffer moved = std::move(slice);
    EXPECT_TRUE(slice.empty());
    EXPECT_EQ(moved.size(), 3u);
    EXPECT_EQ(moved.view().size(), 3u);
}

TEST(SharedSizedBufferTest, ReserveResizeForRecv)
{
    bfcext::shared_sized_buffer buf;
    buf.reserve(64);
    EXPECT_GE(buf.capacity(), 64u);
    buf.resize(8);
    EXPECT_EQ(buf.size(), 8u);
    std::memcpy(buf.data(), "abcdefgh", 8);

    bfcext::shared_sized_buffer moved = std::move(buf);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(moved.size(), 8u);

    moved.clear();
    moved.reserve(32);
    moved.resize(4);
    EXPECT_EQ(moved.capacity(), 32u);
}

TEST(SharedSizedBufferTest, InvalidSubviewIsEmpty)
{
    const uint8_t raw[] = {1, 2, 3};
    bfcext::shared_sized_buffer whole =
        bfcext::shared_sized_buffer::copy_from(raw, sizeof(raw));
    EXPECT_TRUE(whole.subview(4, 1).empty());
    EXPECT_TRUE(whole.subview(1, 3).empty());
}
