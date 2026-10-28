// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <future>
#include <thread>

#include <gtest/gtest.h>
#include <vsomeip/internal/logger.hpp>

#include "multicast_isolation_test_globals.hpp"

using namespace multicast_isolation_test;

namespace {
std::string g_lane;
}

TEST(MulticastIsolationTest, ServiceNotifiesOnItsOwnGroup) {
    auto runtime = vsomeip::runtime::get();
    ASSERT_TRUE(runtime);

    auto app = runtime->create_application("multicast_isolation_service_" + g_lane);
    ASSERT_TRUE(app);
    ASSERT_TRUE(app->init());

    app->offer_service(SERVICE_ID, INSTANCE_ID, MAJOR_VERSION, MINOR_VERSION);
    app->offer_event(SERVICE_ID, INSTANCE_ID, EVENT_ID, {EVENTGROUP_ID}, vsomeip::event_type_e::ET_EVENT);

    std::promise<void> subscribed;
    std::once_flag subscribed_once;
    app->register_subscription_handler(SERVICE_ID, INSTANCE_ID, EVENTGROUP_ID,
                                       [&](vsomeip::client_t, std::uint32_t, std::uint32_t, const std::string&, bool _subscribed) {
                                           if (_subscribed) {
                                               std::call_once(subscribed_once, [&] { subscribed.set_value(); });
                                           }
                                           return true;
                                       });

    std::thread worker([app] { app->start(); });

    if (subscribed.get_future().wait_for(SUBSCRIPTION_TIMEOUT) != std::future_status::ready) {
        ADD_FAILURE() << "Lane " << g_lane << " client never subscribed";
        app->stop();
        worker.join();
        return;
    }
    std::this_thread::sleep_for(OVERLAP);

    auto payload = runtime->create_payload();
    const auto marker = lane_marker(g_lane);
    for (std::uint16_t i = 1; i <= NOTIFICATION_COUNT; ++i) {
        const vsomeip::byte_t data[3] = {marker, static_cast<vsomeip::byte_t>(i & 0xFF), static_cast<vsomeip::byte_t>(i >> 8)};
        payload->set_data(data, sizeof(data));
        app->notify(SERVICE_ID, INSTANCE_ID, EVENT_ID, payload);
        std::this_thread::sleep_for(NOTIFICATION_PERIOD);
    }

    // Repeat the last notification in case one is dropped on the way.
    const vsomeip::byte_t last[3] = {marker, 0xFF, 0xFF};
    payload->set_data(last, sizeof(last));
    for (int i = 0; i < 5; ++i) {
        app->notify(SERVICE_ID, INSTANCE_ID, EVENT_ID, payload);
        std::this_thread::sleep_for(NOTIFICATION_PERIOD);
    }
    VSOMEIP_INFO << "Lane " << g_lane << " sent all notifications";

    std::this_thread::sleep_for(OVERLAP);
    app->stop_offer_service(SERVICE_ID, INSTANCE_ID, MAJOR_VERSION, MINOR_VERSION);
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
