// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_V3_PLUGIN_REGISTRY_HPP_
#define VSOMEIP_V3_PLUGIN_REGISTRY_HPP_

#include <cstddef>

#include <vsomeip/plugin.hpp>

namespace vsomeip_v3 {

struct static_plugin_entry {
    plugin_type_e type_;
    create_plugin_func create_;
};

// Pointer and length of a fixed list. The list itself never changes.
struct static_plugin_list {
    const static_plugin_entry* entries_;
    std::size_t size_;
};

/**
 * The plugins linked into this process.
 *
 * Define this once, in the application. The entries are a constexpr array,
 * so the list is fixed at compile time: nothing adds to it and nothing
 * replaces an entry. A build with VSOMEIP_STATIC_PLUGINS reads it from
 * plugin_manager::get_plugin and does not dlopen. Without that define the
 * function is not called.
 *
 * The function itself is not constexpr. A constexpr function is inline, and
 * this has to be one out-of-line symbol so the plugin manager can call it.
 * On ELF the declaration is weak: a binary that does not define it has an
 * empty list.
 *
 * Service discovery and configuration belong in the list together. Leave a
 * type out, E2E in particular, and that plugin is not registered.
 *
 *   constexpr static_plugin_entry k_plugins[] = {
 *       {plugin_type_e::SD_RUNTIME_PLUGIN, &create_sd_runtime_plugin},
 *       {plugin_type_e::CONFIGURATION_PLUGIN, &create_configuration_plugin},
 *       // {plugin_type_e::E2E_PLUGIN, &create_e2e_plugin},
 *   };
 *   static_plugin_list static_plugins() noexcept {
 *       return {k_plugins, std::size(k_plugins)};
 *   }
 */
static_plugin_list static_plugins() noexcept
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((weak))
#endif
        ;

} // namespace vsomeip_v3

#endif // VSOMEIP_V3_PLUGIN_REGISTRY_HPP_
