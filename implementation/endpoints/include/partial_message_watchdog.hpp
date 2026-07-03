// Copyright (C) 2026 GM GLOBAL TECHNOLOGY OPERATIONS LLC ALL RIGHTS RESERVED.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_V3_PARTIAL_MESSAGE_WATCHDOG_HPP_
#define VSOMEIP_V3_PARTIAL_MESSAGE_WATCHDOG_HPP_

#include <chrono>
#include <cstdint>
#include <mutex>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace vsomeip_v3 {

/**
 * Local IPC peers only ever send complete commands, so waiting longer than
 * `timeout` for the remainder of one is a strong hint that the stream is
 * corrupt. The owner arms the watchdog whenever a receive leaves a partial
 * command in its buffer and disarms it otherwise.
 *
 * The expiry handler receives a generation that must be checked with
 * is_current(): an expiry racing with arm()/disarm() is stale.
 */
class partial_message_watchdog {
public:
    static constexpr std::chrono::seconds timeout{5};

    explicit partial_message_watchdog(boost::asio::io_context& _io) : timer_(_io) { }

    template<typename Handler>
    void arm(Handler&& _handler) {
        std::scoped_lock its_lock{mutex_};
        const std::uint64_t its_generation = ++generation_;
        timer_.expires_after(timeout);
        timer_.async_wait([its_generation, handler = std::forward<Handler>(_handler)](const boost::system::error_code& _error) {
            if (!_error) {
                handler(its_generation);
            }
        });
    }

    void disarm() {
        std::scoped_lock its_lock{mutex_};
        ++generation_;
        timer_.cancel();
    }

    bool is_current(std::uint64_t _generation) const {
        std::scoped_lock its_lock{mutex_};
        return _generation == generation_;
    }

private:
    mutable std::mutex mutex_;
    boost::asio::steady_timer timer_;
    std::uint64_t generation_{0};
};

} // namespace vsomeip_v3

#endif // VSOMEIP_V3_PARTIAL_MESSAGE_WATCHDOG_HPP_
