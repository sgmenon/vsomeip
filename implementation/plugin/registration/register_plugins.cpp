// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// The one plugin list for this tree. Linked into the application, not into
// any vsomeip library. Drop the E2E entry to leave that plugin unregistered.

#include <iterator>

#include <vsomeip/plugin_registry.hpp>
#include <vsomeip/plugins/builtin_plugins.hpp>

namespace {

constexpr vsomeip_v3::static_plugin_entry k_plugins[] = {
        {vsomeip_v3::plugin_type_e::SD_RUNTIME_PLUGIN, &vsomeip_v3::create_sd_runtime_plugin},
        {vsomeip_v3::plugin_type_e::CONFIGURATION_PLUGIN, &vsomeip_v3::create_configuration_plugin},
        {vsomeip_v3::plugin_type_e::E2E_PLUGIN, &vsomeip_v3::create_e2e_plugin},
};

} // namespace

namespace vsomeip_v3 {

static_plugin_list static_plugins() noexcept {
    return {k_plugins, std::size(k_plugins)};
}

} // namespace vsomeip_v3
