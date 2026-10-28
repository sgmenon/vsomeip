// Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include <common/utility.hpp>

#include "mocked_vsomeip_dependencies.hpp"

struct usei_fixture : public ::testing::Test {
    std::shared_ptr<boost::asio::io_context> context_;
    std::shared_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::thread mainloop_;
    std::shared_ptr<vsomeip_v3::udp_server_endpoint_impl> server_;
    std::shared_ptr<mock_endpoint_host> endpoint_;
    std::shared_ptr<mock_routing_host> routing_;
    boost::asio::ip::udp::endpoint unicast_parameters_{boost::asio::ip::make_address("127.0.0.1"), 5000};
    boost::asio::ip::udp::endpoint multicast_parameters_{boost::asio::ip::make_address("0.0.0.0"), 5000};
    boost::asio::ip::udp::endpoint tester_parameters_{boost::asio::ip::make_address("127.0.0.1"), 5002};
    int udp_socket_{-1};

    void SetUp() override {
        // Boost ASIO main loop
        context_ = std::make_shared<boost::asio::io_context>();
        work_guard_ = std::make_shared<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(context_->get_executor());

        mainloop_ = std::thread([this] { context_->run(); });

        // VSOMEIP objects
        endpoint_ = std::make_shared<mock_endpoint_host>();
        routing_ = std::make_shared<mock_routing_host>();
        server_ = std::make_shared<vsomeip_v3::udp_server_endpoint_impl>(
                endpoint_, routing_, *context_, std::make_shared<vsomeip_v3::cfg::configuration_impl>("usei_fixture.json"));

        // UDP tester socket
        udp_socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(udp_socket_, 0);

        struct sockaddr_in tester_address { };
        tester_address.sin_family = AF_INET;
        tester_address.sin_addr.s_addr = htonl(tester_parameters_.address().to_v4().to_uint());
        tester_address.sin_port = htons(tester_parameters_.port());

        int result = ::bind(udp_socket_, reinterpret_cast<const struct sockaddr*>(&tester_address), sizeof(tester_address));
        ASSERT_EQ(result, 0);
    }

    void TearDown() override {
        // UDP tester socket
        ::close(udp_socket_);

        // VSOMEIP objects
        server_.reset();
        routing_.reset();
        endpoint_.reset();

        // Boost ASIO main loop
        context_->stop();
        mainloop_.join();
        work_guard_.reset();
        context_.reset();
    }

    void send(const boost::asio::ip::udp::endpoint& target, const std::byte* buffer, std::size_t len) {
        struct sockaddr_in target_address { };

        target_address.sin_family = AF_INET;
        target_address.sin_addr.s_addr = htonl(target.address().to_v4().to_uint());
        target_address.sin_port = htons(target.port());

        ssize_t result =
                ::sendto(udp_socket_, buffer, len, 0, reinterpret_cast<const struct sockaddr*>(&target_address), sizeof(target_address));
        ASSERT_EQ(result, len);
    }

    template<size_t N>
    void send(const boost::asio::ip::udp::endpoint& target, const std::array<std::byte, N>& data) {
        send(target, data.data(), N);
    }
};

// https://stackoverflow.com/questions/45172052/correct-way-to-initialize-a-container-of-stdbyte
template<typename... Ts>
std::array<std::byte, sizeof...(Ts)> make_bytes(Ts&&... args) noexcept {
    return {std::byte(std::forward<Ts>(args))...};
}

extern "C" int __real_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
extern "C" int __wrap_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);

int __wrap_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    if (level == IPPROTO_IP && (optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP || optname == IP_PKTINFO)) {
        return 0;
    }

    return __real_setsockopt(sockfd, level, optname, optval, optlen);
}

using namespace testing;

TEST_F(usei_fixture, basic) {
    using namespace std::chrono_literals;

    // A promise/future pair replaces the mutex + condition_variable + bool
    // trio: cleaner ownership semantics across the io_context thread and the
    // test thread, and avoids a spurious TSan v2 "double lock" warning that
    // libtsan reports for the cv.wait_for pattern in this configuration.
    std::promise<void> received_promise;
    auto received_future = received_promise.get_future();

    EXPECT_CALL(*routing_, on_message)
            .WillOnce([&](vsomeip_v3::owned_buffer_slice, vsomeip_v3::endpoint*, bool, vsomeip_v3::client_t, const vsomeip_sec_client_t*,
                          const boost::asio::ip::address&, uint16_t) { received_promise.set_value(); });

    boost::system::error_code error;
    server_->init(unicast_parameters_, error);
    server_->start();

    send(unicast_parameters_, make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C));

    EXPECT_EQ(received_future.wait_for(5s), std::future_status::ready);

    server_->stop(false);
}

TEST_F(usei_fixture, corrupted_data) {
    using namespace std::chrono_literals;

    auto good_data = make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
                                0x0F, 0x10, 0x00, 0x00, 0x00, 0x09, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19);
    auto end_message = make_bytes(0x1A, 0x1B, 0x1C, 0x1D, 0x00, 0x00, 0x00, 0x08, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x025);

    constexpr size_t MESSAGE_SENT_COUNT = 10000;

    // WillRepeatedly may match many times; std::call_once guarantees the
    // promise is only satisfied once (calling set_value twice would throw
    // std::future_error(promise_already_satisfied)).
    std::promise<void> received_promise;
    auto received_future = received_promise.get_future();
    std::once_flag received_once;

    EXPECT_CALL(*endpoint_, on_error).Times(AtLeast(MESSAGE_SENT_COUNT / 20));
    EXPECT_CALL(*routing_, on_message)
            .WillRepeatedly([&](vsomeip_v3::owned_buffer_slice frame, vsomeip_v3::endpoint*, bool, vsomeip_v3::client_t,
                                const vsomeip_sec_client_t*, const boost::asio::ip::address&, uint16_t) {
                const auto* data = frame.data();
                const auto len = frame.length;
                if (len == 16 && data && data[0] == 0x1A && data[1] == 0x1B && data[2] == 0x1C && data[3] == 0x1D) {
                    std::call_once(received_once, [&] { received_promise.set_value(); });
                }
            });

    boost::system::error_code error;
    server_->init(unicast_parameters_, error);
    server_->start();

    for (size_t i = 0; i < MESSAGE_SENT_COUNT; ++i) {
        auto bad_data = good_data;
        bad_data[i % bad_data.size()] = static_cast<std::byte>(i);
        send(unicast_parameters_, bad_data);
        std::this_thread::sleep_for(10us);
    }

    send(unicast_parameters_, end_message);

    EXPECT_EQ(received_future.wait_for(5s), std::future_status::ready);

    server_->stop(false);
}

TEST_F(usei_fixture, basic_multicast) {
    using namespace std::chrono_literals;

    std::promise<void> received_promise;
    auto received_future = received_promise.get_future();

    EXPECT_CALL(*routing_, on_message)
            .WillOnce([&](vsomeip_v3::owned_buffer_slice, vsomeip_v3::endpoint*, bool, vsomeip_v3::client_t, const vsomeip_sec_client_t*,
                          const boost::asio::ip::address&, uint16_t) { received_promise.set_value(); });

    boost::system::error_code error;
    server_->init(unicast_parameters_, error);
    server_->start();
    server_->set_multicast_option(multicast_parameters_.address(), true, error);

    send(multicast_parameters_,
         make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x09, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D));

    EXPECT_EQ(received_future.wait_for(5s), std::future_status::ready);

    server_->joined_.clear(); // We don't want to call `leave` method, so we do its job
    server_->set_multicast_option(multicast_parameters_.address(), false, error);
    server_->stop(false);
}

// Feeds one SOME/IP notification to on_multicast_received as if IP_PKTINFO
// reported `_destination`, and returns how many times it reached the routing host.
struct usei_multicast_destination_fixture : public usei_fixture {
    void SetUp() override {
        usei_fixture::SetUp();
        EXPECT_CALL(*endpoint_, add_multicast_option).Times(AnyNumber());
    }

    void use_local(const char* _address) {
        server_->local_ = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(_address), 50000);
        server_->is_v4_ = server_->local_.address().is_v4();
        server_->netmask_ = boost::asio::ip::make_address("255.255.255.0");
        server_->prefix_ = 64;
    }

    int deliver(const char* _sender, const char* _destination) {
        int its_count{0};
        EXPECT_CALL(*routing_, on_message).WillRepeatedly([&](auto&&...) { ++its_count; });

        // service 0x1111, event 0x8001, length 8, protocol 1, interface 1, NOTIFICATION, E_OK
        auto its_buffer = std::make_shared<vsomeip_v3::message_buffer_t>(vsomeip_v3::message_buffer_t{
                0x11, 0x11, 0x80, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x02, 0x00});
        server_->on_multicast_received(boost::system::error_code(), its_buffer->size(),
                                       boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(_sender), 30509),
                                       boost::asio::ip::make_address(_destination), its_buffer);

        Mock::VerifyAndClearExpectations(routing_.get());
        return its_count;
    }
};

TEST_F(usei_multicast_destination_fixture, group_joined_by_another_socket_is_dropped) {
    use_local("127.0.0.1");
    server_->join("239.195.233.1");

    EXPECT_EQ(deliver("127.0.0.2", "239.195.233.0"), 0);
    EXPECT_EQ(deliver("127.0.0.2", "239.195.233.1"), 1);
}

TEST_F(usei_multicast_destination_fixture, unicast_destination_is_delivered) {
    use_local("127.0.0.1");
    server_->join("239.195.233.1");

    EXPECT_EQ(deliver("127.0.0.2", "127.0.0.1"), 1);
}

// "multicast": "ff02::4:c%1" in the configuration. IP_PKTINFO reports the
// destination without the interface, so the group must match without it.
TEST_F(usei_multicast_destination_fixture, group_with_interface_suffix_is_delivered) {
    use_local("fd00::10");
    server_->join("ff02::4:c%1");

    EXPECT_TRUE(server_->is_joined("ff02::4:c"));
    EXPECT_EQ(deliver("fd00::20", "ff02::4:c"), 1);
}

// "multicast": "FF0E::4:C" in the configuration.
TEST_F(usei_multicast_destination_fixture, group_written_in_uppercase_is_one_entry) {
    use_local("fd00::10");
    server_->join("FF0E::4:C");
    server_->join("ff0e::4:c");

    EXPECT_EQ(server_->joined_.size(), 1u);
    EXPECT_EQ(deliver("fd00::20", "ff0e::4:c"), 1);

    server_->leave("ff0e::4:c");
    EXPECT_TRUE(server_->joined_.empty());
}

TEST_F(usei_fixture, no_overwrite_during_restart) {
    constexpr size_t MESSAGE_SENT_COUNT = 10000;
    using namespace std::chrono_literals;
    std::atomic<bool> finished{false};

    EXPECT_CALL(*endpoint_, on_error).Times(0);
    EXPECT_CALL(*routing_, on_message).Times(AtLeast(MESSAGE_SENT_COUNT / 3));

    boost::system::error_code error;
    server_->init(unicast_parameters_, error);
    server_->start();

    auto producer = std::thread([&] {
        for (size_t i = 0; i < MESSAGE_SENT_COUNT; ++i) {
            send(unicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C));
            send(unicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x09, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D));
            std::this_thread::sleep_for(10us);
        }
        finished.store(true);
    });

    while (!finished.load()) {
        server_->restart(true);
        std::this_thread::sleep_for(1ms);
    }

    producer.join();
    server_->stop(false);
}

TEST_F(usei_fixture, no_overwrite_during_restart_with_multicast) {
    constexpr size_t MESSAGE_SENT_COUNT = 1000000;
    using namespace std::chrono_literals;
    std::atomic<bool> finished{false};

    EXPECT_CALL(*endpoint_, on_error).Times(0);
    EXPECT_CALL(*endpoint_, add_multicast_option).Times(AtLeast(5));
    EXPECT_CALL(*routing_, on_message).Times(AtLeast(MESSAGE_SENT_COUNT / 3));

    boost::system::error_code error;
    server_->init(unicast_parameters_, error);
    server_->start();
    server_->set_multicast_option(multicast_parameters_.address(), true, error);

    auto producer = std::thread([&] {
        for (size_t i = 0; i < MESSAGE_SENT_COUNT; ++i) {
            send(unicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C));
            send(multicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x09, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D));
            send(unicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E));
            send(multicast_parameters_,
                 make_bytes(0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x0B, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
                            0x0F));
        }
        finished.store(true);
    });

    while (!finished.load()) {
        server_->restart(true);
        std::this_thread::sleep_for(1ms);
    }

    server_->joined_.clear();
    server_->set_multicast_option(multicast_parameters_.address(), false, error);
    producer.join();
    server_->stop(false);
}
