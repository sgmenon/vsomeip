// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_V3_BUILTIN_PLUGINS_HPP_
#define VSOMEIP_V3_BUILTIN_PLUGINS_HPP_

#include <memory>

#include <vsomeip/plugin.hpp>

namespace vsomeip_v3 {

// Factories for the plugins shipped with vsomeip. static_plugins()
// (see <vsomeip/plugin_registry.hpp>) names the ones a binary registers.
// Each factory is defined in the library named next to it.

// libvsomeip3-sd, plugin_type_e::SD_RUNTIME_PLUGIN
VSOMEIP_IMPORT_EXPORT_PLUGIN std::shared_ptr<plugin> create_sd_runtime_plugin();

// libvsomeip3-e2e, plugin_type_e::E2E_PLUGIN
VSOMEIP_IMPORT_EXPORT_PLUGIN std::shared_ptr<plugin> create_e2e_plugin();

// libvsomeip3-cfg, plugin_type_e::CONFIGURATION_PLUGIN
VSOMEIP_IMPORT_EXPORT_PLUGIN std::shared_ptr<plugin> create_configuration_plugin();

} // namespace vsomeip_v3

#endif // VSOMEIP_V3_BUILTIN_PLUGINS_HPP_
