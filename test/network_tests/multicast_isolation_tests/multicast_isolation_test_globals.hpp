// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef MULTICAST_ISOLATION_TEST_GLOBALS_HPP_
#define MULTICAST_ISOLATION_TEST_GLOBALS_HPP_

#include <chrono>
#include <cstdint>
#include <string>

#include <vsomeip/vsomeip.hpp>

namespace multicast_isolation_test {

// Both lanes use the same service, instance, eventgroup and event, so a
// datagram leaked from the other lane's group is accepted by the routing
// host and reaches the application.
constexpr vsomeip::service_t SERVICE_ID = 0x1111;
constexpr vsomeip::instance_t INSTANCE_ID = 0x0001;
constexpr vsomeip::major_version_t MAJOR_VERSION = 0x01;
constexpr vsomeip::minor_version_t MINOR_VERSION = 0x00;
constexpr vsomeip::eventgroup_t EVENTGROUP_ID = 0x0001;
constexpr vsomeip::event_t EVENT_ID = 0x8001;

// Payload is { lane marker, counter low byte, counter high byte }.
constexpr std::uint16_t LAST_COUNTER = 0xFFFF;
constexpr std::uint16_t NOTIFICATION_COUNT = 500;
constexpr std::chrono::milliseconds NOTIFICATION_PERIOD{10};

// Time for the other lane to subscribe before this lane starts sending,
// and for the other lane's traffic to arrive after this lane is done.
constexpr std::chrono::seconds OVERLAP{2};
constexpr std::chrono::seconds SUBSCRIPTION_TIMEOUT{30};

inline vsomeip::byte_t lane_marker(const std::string& _lane) {
    return _lane == "A" ? 0xAA : 0xBB;
}

} // namespace multicast_isolation_test

#endif // MULTICAST_ISOLATION_TEST_GLOBALS_HPP_
