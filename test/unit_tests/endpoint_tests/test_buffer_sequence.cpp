// Copyright (C) 2026 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <cstdint>

#include "../../../implementation/endpoints/include/buffer.hpp"
#include "../../../implementation/routing/include/payload_ownership.hpp"
#include "../../../implementation/message/include/message_impl.hpp"
#include "../../../implementation/message/include/payload_impl.hpp"
#include <vsomeip/span.hpp>

#if defined(__has_include)
#if __has_include(<boost/core/span.hpp>)
#include <boost/core/span.hpp>
#include <type_traits>
static_assert(std::is_same<vsomeip_v3::span<const std::uint8_t>, boost::span<const std::uint8_t>>::value,
              "vsomeip span should map to boost::span when available");
#endif
#endif

namespace {

TEST(buffer_sequence_test, flatten_single_buffer_reuses_storage) {
    auto payload = std::make_shared<vsomeip_v3::message_buffer_t>();
    payload->push_back(0x11);
    payload->push_back(0x22);
    const auto* raw = payload.get();

    vsomeip_v3::buffer_sequence sequence(std::move(payload));
    auto flat = sequence.flatten();

    EXPECT_EQ(flat.get(), raw);
    ASSERT_EQ(flat->size(), 2U);
    EXPECT_EQ((*flat)[0], 0x11);
    EXPECT_EQ((*flat)[1], 0x22);
}

TEST(buffer_sequence_test, flatten_multi_buffer_concatenates) {
    auto a = std::make_shared<vsomeip_v3::message_buffer_t>();
    a->push_back(0xAA);
    a->push_back(0xBB);
    auto b = std::make_shared<vsomeip_v3::message_buffer_t>();
    b->push_back(0xCC);

    vsomeip_v3::buffer_sequence sequence;
    sequence.append(std::move(a));
    sequence.append(std::move(b));

    auto flat = sequence.flatten();
    ASSERT_NE(flat, nullptr);
    ASSERT_EQ(flat->size(), 3U);
    EXPECT_EQ((*flat)[0], 0xAA);
    EXPECT_EQ((*flat)[1], 0xBB);
    EXPECT_EQ((*flat)[2], 0xCC);
}

TEST(buffer_sequence_test, read_uint16_be_handles_cross_buffer) {
    auto a = std::make_shared<vsomeip_v3::message_buffer_t>();
    a->push_back(0x12);
    auto b = std::make_shared<vsomeip_v3::message_buffer_t>();
    b->push_back(0x34);
    b->push_back(0x56);

    vsomeip_v3::buffer_sequence sequence;
    sequence.append(std::move(a));
    sequence.append(std::move(b));

    std::uint16_t value = 0;
    ASSERT_TRUE(sequence.read_uint16_be(0U, value));
    EXPECT_EQ(value, 0x1234);

    ASSERT_TRUE(sequence.read_uint16_be(1U, value));
    EXPECT_EQ(value, 0x3456);
}

TEST(buffer_sequence_test, append_buffer_slice_shares_backing_store) {
    auto buf = std::make_shared<vsomeip_v3::message_buffer_t>();
    for (std::uint8_t i = 0; i < 8; ++i) {
        buf->push_back(i);
    }

    vsomeip_v3::buffer_sequence sequence;
    sequence.append_buffer_slice(buf, 0, 2);
    sequence.append_buffer_slice(buf, 2, 6);

    ASSERT_EQ(sequence.size(), 8U);
    ASSERT_EQ(sequence.buffers().size(), 2U);
    EXPECT_EQ(sequence.buffers()[0].size(), 2U);
    EXPECT_EQ(sequence.buffers()[1].size(), 6U);

    std::uint16_t value = 0;
    ASSERT_TRUE(sequence.read_uint16_be(0U, value));
    EXPECT_EQ(value, 0x0001);
    ASSERT_TRUE(sequence.read_uint16_be(6U, value));
    EXPECT_EQ(value, 0x0607);

    // Slices must keep the shared buffer alive after local shared_ptr drops.
    buf.reset();
    ASSERT_TRUE(sequence.read_uint16_be(0U, value));
    EXPECT_EQ(value, 0x0001);
}

TEST(buffer_sequence_test, completion_fires_once_after_pending) {
    int calls = 0;
    bool last_ok = false;
    auto latch = std::make_shared<vsomeip_v3::send_completion_state>([&](bool ok) {
        ++calls;
        last_ok = ok;
    });

    latch->begin();
    EXPECT_TRUE(latch->queue_write([] { return true; }));
    EXPECT_TRUE(latch->queue_write([] { return true; }));

    vsomeip_v3::buffer_sequence a;
    vsomeip_v3::buffer_sequence b;
    a.attach_completion(latch);
    b.attach_completion(latch);

    a.complete(true);
    EXPECT_EQ(calls, 0);
    b.complete(true);
    latch->end();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(last_ok);
}

TEST(buffer_sequence_test, queue_write_waits_for_every_queued_write) {
    int calls = 0;
    auto latch = std::make_shared<vsomeip_v3::send_completion_state>([&](bool) { ++calls; });
    vsomeip_v3::buffer_sequence first;
    vsomeip_v3::buffer_sequence second;
    first.attach_completion(latch);
    second.attach_completion(latch);

    latch->begin();
    // The first write completes before its send returns, as an io thread may do.
    EXPECT_TRUE(latch->queue_write([&] {
        first.complete(true);
        return true;
    }));
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(latch->queue_write([] { return false; }));
    EXPECT_TRUE(latch->queue_write([] { return true; }));
    latch->end();
    EXPECT_EQ(calls, 0);

    second.complete(true);
    EXPECT_EQ(calls, 1);
}

TEST(buffer_sequence_test, append_sequence_merges_completions) {
    int calls = 0;
    auto latch = std::make_shared<vsomeip_v3::send_completion_state>([&](bool) { ++calls; });
    latch->begin();
    EXPECT_TRUE(latch->queue_write([] { return true; }));

    vsomeip_v3::buffer_sequence passenger;
    passenger.attach_completion(latch);

    vsomeip_v3::buffer_sequence train;
    train.append_sequence(passenger);
    train.complete(true);
    latch->end();

    EXPECT_EQ(calls, 1);
}

TEST(buffer_sequence_test, snapshot_payload_if_shared_copies_when_shared) {
    auto original = std::make_shared<vsomeip_v3::payload_impl>(std::vector<vsomeip_v3::byte_t>{1, 2, 3});
    auto kept = original;
    auto result = vsomeip_v3::snapshot_payload_if_shared(original);
    ASSERT_NE(result, nullptr);
    EXPECT_NE(result.get(), kept.get());
    EXPECT_EQ(result->get_length(), 3U);
    EXPECT_EQ(result->get_data()[0], 1);
    kept->set_data(std::vector<vsomeip_v3::byte_t>{9, 9, 9});
    EXPECT_EQ(result->get_data()[0], 1);
}

TEST(buffer_sequence_test, snapshot_payload_if_shared_pins_when_exclusive) {
    auto exclusive = std::make_shared<vsomeip_v3::payload_impl>(std::vector<vsomeip_v3::byte_t>{4, 5});
    auto* raw = exclusive.get();
    auto result = vsomeip_v3::snapshot_payload_if_shared(std::move(exclusive));
    EXPECT_EQ(result.get(), raw);
}

TEST(buffer_sequence_test, ensure_exclusive_message_payload_snapshots_when_message_shared) {
    std::shared_ptr<vsomeip_v3::message> message = std::make_shared<vsomeip_v3::message_impl>();
    auto payload = std::make_shared<vsomeip_v3::payload_impl>(std::vector<vsomeip_v3::byte_t>{1, 2, 3});
    message->set_payload(payload);
    auto kept_message = message;
    auto* original = payload.get();

    vsomeip_v3::ensure_exclusive_message_payload(message);

    EXPECT_NE(message->get_payload().get(), original);
    EXPECT_EQ(message->get_payload()->get_length(), 3U);
    payload->set_data(std::vector<vsomeip_v3::byte_t>{9, 9, 9});
    EXPECT_EQ(message->get_payload()->get_data()[0], 1);
    (void)kept_message;
}

TEST(buffer_sequence_test, ensure_exclusive_message_payload_pins_when_exclusive) {
    std::shared_ptr<vsomeip_v3::message> message = std::make_shared<vsomeip_v3::message_impl>();
    auto payload = std::make_shared<vsomeip_v3::payload_impl>(std::vector<vsomeip_v3::byte_t>{7, 8});
    message->set_payload(std::move(payload));
    auto* raw = message->get_payload().get();

    vsomeip_v3::ensure_exclusive_message_payload(message);

    EXPECT_EQ(message->get_payload().get(), raw);
}

TEST(buffer_sequence_test, ensure_exclusive_message_payload_snapshots_when_payload_kept) {
    std::shared_ptr<vsomeip_v3::message> message = std::make_shared<vsomeip_v3::message_impl>();
    auto payload = std::make_shared<vsomeip_v3::payload_impl>(std::vector<vsomeip_v3::byte_t>{3, 4, 5});
    message->set_payload(payload);
    auto* original = payload.get();

    // Exclusive message, but caller still holds the payload.
    vsomeip_v3::ensure_exclusive_message_payload(message);

    EXPECT_NE(message->get_payload().get(), original);
    payload->set_data(std::vector<vsomeip_v3::byte_t>{0, 0, 0});
    EXPECT_EQ(message->get_payload()->get_data()[0], 3);
}

} // namespace
