// Copyright (C) 2026 GM GLOBAL TECHNOLOGY OPERATIONS LLC ALL RIGHTS RESERVED.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../../implementation/endpoints/include/message_buffer_pool.hpp"

namespace {

using vsomeip_v3::byte_t;
using vsomeip_v3::message_buffer_pool;
using vsomeip_v3::message_buffer_t;
using vsomeip_v3::take_local_ipc_command;
using vsomeip_v3::take_stream_frame;
using vsomeip_v3::take_stream_frame_keep;

TEST(message_buffer_pool_test, lease_recycles_on_last_release) {
    auto pool = message_buffer_pool::create(2, 8);
    ASSERT_EQ(pool->available(), 2U);

    {
        auto a = pool->try_lease(16);
        ASSERT_NE(a, nullptr);
        EXPECT_EQ(pool->available(), 1U);
        EXPECT_GE(a->size(), 16U);
        (*a)[0] = 0xAB;
    }
    EXPECT_EQ(pool->available(), 2U);

    auto b = pool->try_lease(8);
    ASSERT_NE(b, nullptr);
    // Capacity kept warm from prior lease.
    EXPECT_GE(b->capacity(), 16U);
}

TEST(message_buffer_pool_test, try_lease_returns_null_when_empty) {
    auto pool = message_buffer_pool::create(1, 4);
    auto a = pool->try_lease();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(pool->try_lease(), nullptr);
    a.reset();
    EXPECT_NE(pool->try_lease(), nullptr);
}

TEST(message_buffer_pool_test, adopt_and_take_stream_frame_round_trip) {
    auto pool = message_buffer_pool::create(2, 8);
    message_buffer_t window(32, byte_t{0});
    for (std::size_t i = 0; i < 32; ++i) {
        window[i] = static_cast<byte_t>(i + 1);
    }
    std::size_t used = 32;
    bool moved = false;
    auto frame = take_stream_frame(window, used, 0, 32, 8, moved, pool);

    ASSERT_TRUE(moved);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->size(), 32U);
    EXPECT_EQ((*frame)[0], 1);
    EXPECT_EQ(used, 0U);
    EXPECT_EQ(window.size(), 8U);

    frame.reset();
    // Can lease again after release (may or may not be the adopted buffer —
    // adopt wraps external storage; recycle may discard when stack is full).
    EXPECT_NE(pool->try_lease(32), nullptr);
}

TEST(take_stream_frame_test, moves_when_gap_zero_and_fills_used) {
    message_buffer_t window(64, byte_t{0});
    for (std::size_t i = 0; i < 32; ++i) {
        window[i] = static_cast<byte_t>(i + 1);
    }
    std::size_t used = 32;
    bool moved = false;
    auto frame = take_stream_frame(window, used, /*gap=*/0, /*message_size=*/32, /*fresh_capacity=*/8, moved);

    ASSERT_TRUE(moved);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->size(), 32U);
    EXPECT_EQ((*frame)[0], 1);
    EXPECT_EQ((*frame)[31], 32);
    EXPECT_EQ(used, 0U);
    EXPECT_EQ(window.size(), 8U);
}

TEST(take_stream_frame_test, copies_when_leftover_bytes_remain) {
    message_buffer_t window{1, 2, 3, 4, 5, 6};
    std::size_t used = 6;
    const byte_t* original_data = window.data();
    bool moved = false;
    auto frame = take_stream_frame(window, used, /*gap=*/0, /*message_size=*/4, /*fresh_capacity=*/8, moved);

    ASSERT_FALSE(moved);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->size(), 4U);
    EXPECT_EQ((*frame)[0], 1);
    EXPECT_EQ((*frame)[3], 4);
    EXPECT_EQ(used, 6U);
    EXPECT_EQ(window.data(), original_data);
    EXPECT_EQ(window.size(), 6U);
}

TEST(take_stream_frame_test, copies_when_gap_nonzero) {
    message_buffer_t window{1, 2, 3, 4, 5, 6};
    std::size_t used = 4;
    bool moved = false;
    auto frame = take_stream_frame(window, used, /*gap=*/2, /*message_size=*/4, /*fresh_capacity=*/8, moved);

    ASSERT_FALSE(moved);
    ASSERT_EQ(frame->size(), 4U);
    EXPECT_EQ((*frame)[0], 3);
    EXPECT_EQ((*frame)[3], 6);
    EXPECT_EQ(used, 4U);
}

TEST(take_stream_frame_test, move_trims_unused_capacity_bytes) {
    message_buffer_t window(100, byte_t{0xAB});
    std::size_t used = 16;
    bool moved = false;
    auto frame = take_stream_frame(window, used, 0, 16, 8, moved);

    ASSERT_TRUE(moved);
    ASSERT_EQ(frame->size(), 16U);
    EXPECT_EQ(used, 0U);
    EXPECT_EQ(window.size(), 8U);
}

TEST(take_stream_frame_test, pooled_copy_reuses_lease) {
    auto pool = message_buffer_pool::create(2, 4);
    message_buffer_t window{1, 2, 3, 4, 5, 6};
    std::size_t used = 6;
    bool moved = false;
    auto frame = take_stream_frame(window, used, 0, 4, 8, moved, pool);
    ASSERT_FALSE(moved);
    ASSERT_EQ(frame->size(), 4U);
    EXPECT_EQ(pool->available(), 1U);
    const auto* storage = frame->data();
    frame.reset();
    EXPECT_EQ(pool->available(), 2U);
    auto again = pool->try_lease(4);
    EXPECT_EQ(again->data(), storage);
}

TEST(take_stream_frame_test, pooled_move_keeps_recv_window_size_eq_capacity) {
    auto pool = message_buffer_pool::create(2, 8);
    // Simulate a recycled buffer that kept a large capacity after clear().
    {
        auto warm = pool->try_lease(256);
        ASSERT_NE(warm, nullptr);
        warm.reset();
    }
    message_buffer_t window(32, byte_t{0x22});
    std::size_t used = 32;
    bool moved = false;
    auto frame = take_stream_frame(window, used, 0, 32, 8, moved, pool);
    ASSERT_TRUE(moved);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(window.size(), window.capacity());
    EXPECT_GE(window.size(), 8U);
}

TEST(take_stream_frame_test, pool_empty_drops_without_allocating) {
    auto pool = message_buffer_pool::create(1, 4);
    auto held = pool->try_lease(4);
    ASSERT_NE(held, nullptr);
    ASSERT_EQ(pool->available(), 0U);

    message_buffer_t window{1, 2, 3, 4, 5, 6};
    std::size_t used = 6;
    bool moved = false;
    bool dropped = false;
    auto frame = take_stream_frame(window, used, 0, 4, 8, moved, pool, &dropped);
    EXPECT_EQ(frame, nullptr);
    EXPECT_TRUE(dropped);
    EXPECT_FALSE(moved);
    EXPECT_EQ(used, 6U); // caller still advances on copy-drop

    // Move-fill path: drop in place, keep window, no handout.
    message_buffer_t full(16, byte_t{0x11});
    used = 16;
    moved = false;
    dropped = false;
    frame = take_stream_frame(full, used, 0, 16, 8, moved, pool, &dropped);
    EXPECT_EQ(frame, nullptr);
    EXPECT_TRUE(dropped);
    EXPECT_TRUE(moved);
    EXPECT_EQ(used, 0U);
    EXPECT_EQ(full.size(), 16U); // same window retained as drain
}

TEST(take_stream_frame_keep_test, empty_pool_allocates_on_move) {
    auto pool = message_buffer_pool::create(1, 4);
    auto held = pool->try_lease(4);
    ASSERT_EQ(pool->available(), 0U);

    message_buffer_t window{1, 2, 3, 4};
    std::size_t used = 4;
    bool moved = false;
    auto frame = take_stream_frame_keep(window, used, 0, 4, 8, moved, pool);
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(moved);
    EXPECT_EQ(used, 0U);
    ASSERT_EQ(frame->size(), 4U);
    EXPECT_EQ((*frame)[0], byte_t{1});
    EXPECT_EQ((*frame)[3], byte_t{4});
    (void)held;
}

TEST(take_local_ipc_command_test, moves_and_strips_tags_when_frame_fills_window) {
    message_buffer_t window{0x67, 0x37, 0x6d, 0x07, 0x11, 0x22, 0x33, 0x07, 0x6d, 0x37, 0x67};
    std::size_t used = window.size();
    bool moved = false;
    auto slice = take_local_ipc_command(window, used, 0, 4, 3, 8, moved, nullptr);
    EXPECT_TRUE(moved);
    EXPECT_EQ(used, 0U);
    ASSERT_TRUE(slice.valid());
    EXPECT_EQ(slice.length, 3U);
    EXPECT_EQ(slice.offset, 4U);
    EXPECT_EQ(slice.buffer->at(slice.offset), byte_t{0x11});
    EXPECT_EQ(slice.buffer->at(slice.offset + 2), byte_t{0x33});
}

TEST(take_local_ipc_command_test, copies_command_when_bytes_follow) {
    message_buffer_t window{0x67, 0x37, 0x6d, 0x07, 0x11, 0x22, 0x33, 0x07, 0x6d, 0x37, 0x67, 0xAA};
    const std::size_t original = window.size();
    std::size_t used = original;
    bool moved = false;
    auto slice = take_local_ipc_command(window, used, 0, 4, 3, 8, moved, nullptr);
    EXPECT_FALSE(moved);
    EXPECT_EQ(used, original);
    ASSERT_TRUE(slice.valid());
    EXPECT_EQ(slice.length, 3U);
    EXPECT_EQ(slice.buffer->size(), 3U);
    EXPECT_EQ((*slice.buffer)[0], byte_t{0x11});
    EXPECT_EQ(window.back(), byte_t{0xAA});
}

TEST(take_local_ipc_command_test, empty_pool_still_returns_command) {
    auto pool = message_buffer_pool::create(1, 4);
    auto held = pool->try_lease(4);
    ASSERT_EQ(pool->available(), 0U);

    message_buffer_t window{0x67, 0x37, 0x6d, 0x07, 0x11, 0x22, 0x33, 0x07, 0x6d, 0x37, 0x67};
    std::size_t used = window.size();
    bool moved = false;
    auto slice = take_local_ipc_command(window, used, 0, 4, 3, 8, moved, pool);
    ASSERT_TRUE(slice.valid());
    EXPECT_TRUE(moved);
    EXPECT_EQ(slice.length, 3U);
    EXPECT_EQ(slice.buffer->at(slice.offset), byte_t{0x11});
    (void)held;
}

} // namespace
