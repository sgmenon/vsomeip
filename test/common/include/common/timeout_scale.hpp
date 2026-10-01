// Copyright (C) 2014-2026 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <chrono>
#include <cstdlib>

namespace common {

/// Returns the timeout scale factor for test waits.
/// This can be used to scale up timeouts for valgrind or other slow environments,
/// to avoid false positives due to timeouts being too short.
///
/// Reads TEST_TIMEOUT_SCALE from the environment (set by CI for valgrind jobs),
/// defaults to 1 if not set.
inline int get_timeout_scale() {
    static const int scale = [] {
        if (const char* env = std::getenv("TEST_TIMEOUT_SCALE"); env && env[0]) {
            int v = std::atoi(env);
            if (v > 0) {
                return v;
            }
        }
        return 1;
    }();
    return scale;
}

/// Scale a duration by the timeout multiplier.
template<typename Rep, typename Period>
std::chrono::duration<Rep, Period> scaled_timeout(std::chrono::duration<Rep, Period> d) {
    return d * get_timeout_scale();
}

} // namespace common
