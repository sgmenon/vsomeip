// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <atomic>
#include <future>
#include <iomanip>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <vsomeip/internal/logger.hpp>

#include "multicast_isolation_test_globals.hpp"

using namespace multicast_isolation_test;

namespace {
std::string g_lane;
}

// Each lane's routing host listens on port 50000 and joins only its own
// group. Every notification it delivers must carry its own lane's marker.
TEST(MulticastIsolationTest, ClientOnlyReceivesItsOwnGroup) {
    auto runtime = vsomeip::runtime::get();
    ASSERT_TRUE(runtime);

    auto app = runtime->create_application("multicast_isolation_client_" + g_lane);
    ASSERT_TRUE(app);
    ASSERT_TRUE(app->init());

    const auto own_marker = lane_marker(g_lane);
    std::atomic<int> own_count{0};
    std::atomic<int> foreign_count{0};
    std::promise<void> received_last;
    std::once_flag received_last_once;

    app->register_state_handler([&](vsomeip::state_type_e _state) {
        if (_state == vsomeip::state_type_e::ST_REGISTERED) {
            app->request_service(SERVICE_ID, INSTANCE_ID, MAJOR_VERSION, MINOR_VERSION);
            app->request_event(SERVICE_ID, INSTANCE_ID, EVENT_ID, {EVENTGROUP_ID}, vsomeip::event_type_e::ET_EVENT);
        }
    });

    app->register_availability_handler(SERVICE_ID, INSTANCE_ID, [&](vsomeip::service_t, vsomeip::instance_t, bool _is_available) {
        if (_is_available) {
            app->subscribe(SERVICE_ID, INSTANCE_ID, EVENTGROUP_ID, MAJOR_VERSION);
        }
    });

    app->register_message_handler(SERVICE_ID, INSTANCE_ID, EVENT_ID, [&](const std::shared_ptr<vsomeip::message>& _message) {
        const auto payload = _message->get_payload();
        if (payload->get_length() != 3) {
            ADD_FAILURE() << "Unexpected payload length " << payload->get_length();
            return;
        }
        const auto* data = payload->get_data();
        if (data[0] != own_marker) {
            if (foreign_count++ == 0) {
                VSOMEIP_ERROR << "Lane " << g_lane << " received the other lane's notification, marker 0x" << std::hex
                              << static_cast<int>(data[0]);
            }
            return;
        }
        ++own_count;
        if (static_cast<std::uint16_t>(data[1] | (data[2] << 8)) == LAST_COUNTER) {
            std::call_once(received_last_once, [&] { received_last.set_value(); });
        }
    });

    std::thread worker([app] { app->start(); });

    const auto timeout = SUBSCRIPTION_TIMEOUT + OVERLAP + NOTIFICATION_COUNT * NOTIFICATION_PERIOD;
    EXPECT_EQ(received_last.get_future().wait_for(timeout), std::future_status::ready)
            << "Lane " << g_lane << " never received its last notification";

    // Keep listening while the other lane may still be sending.
    std::this_thread::sleep_for(OVERLAP);

    VSOMEIP_INFO << "Lane " << g_lane << " own=" << own_count << " foreign=" << foreign_count;
    EXPECT_GT(own_count.load(), 0);
    EXPECT_EQ(foreign_count.load(), 0) << "Lane " << g_lane << " received notifications sent to the other lane's group";

    app->unsubscribe(SERVICE_ID, INSTANCE_ID, EVENTGROUP_ID);
    app->stop();
    worker.join();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc < 2 || (std::string(argv[1]) != "A" && std::string(argv[1]) != "B")) {
        std::cerr << "usage: " << argv[0] << " A|B" << std::endl;
        return 1;
    }
    g_lane = argv[1];
    return RUN_ALL_TESTS();
}
