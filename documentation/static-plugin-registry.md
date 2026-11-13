# Link-time plugin registry for vsomeip

> **Status.** Implemented. The Bazel build in this tree turns the flag on
> in `.bazelrc`.

## 1. Current design and its limitations

`plugin_manager_impl::get_plugin(type, name)` looks `(type, name)` up in a
cache. On a miss it calls `dlopen(name, RTLD_LAZY | RTLD_LOCAL)`, resolves
the single `extern "C" vsomeip_plugin_init` symbol emitted by
`VSOMEIP_PLUGIN(T)`, constructs `T`, and keeps it only if
`get_plugin_type()` matches and the version is 1.

Four plugins exist in tree:

| Library                | Concrete type               | Reported type          | Loaded by                                                                         | Selected by                                |
| ---------------------- | --------------------------- | ---------------------- | --------------------------------------------------------------------------------- | ------------------------------------------ |
| `libvsomeip3-sd.so.3`  | `sd::runtime_impl`          | `SD_RUNTIME_PLUGIN`    | `routing_manager_impl::init`, again in `service_discovery_impl::init`             | `VSOMEIP_SD_MODULE` (fork addition)        |
| `libvsomeip3-e2e.so.3` | `e2e::e2e_provider_impl`    | `APPLICATION_PLUGIN`   | `routing_manager_impl::init`                                                      | `VSOMEIP_E2E_PROTECTION_MODULE` (upstream) |
| `libvsomeip3-cfg.so.3` | `configuration_plugin_impl` | `CONFIGURATION_PLUGIN` | `application_impl::init`, only without `VSOMEIP_ENABLE_MULTIPLE_ROUTING_MANAGERS` | hard-coded                                 |
| user `.so`             | anything                    | `APPLICATION_PLUGIN`   | `application_impl` init/start/stop (five call sites)                              | `applications[].plugins` in JSON           |

The last row is a dead feature. See §1.3.

### 1.1 Plugins don't provide loose-coupling because the plugin interface is not the contract

The public `plugin` base carries only a name, a version, and a type tag.
Every call site immediately `dynamic_pointer_cast`s to the interface it
actually needs: `sd::runtime`, `e2e::e2e_provider`, `configuration_plugin`.
All three are internal headers. `sd::runtime::create_service_discovery`
also hands over `service_discovery_host*` and the full `configuration`
interface (121 pure virtuals).

A replacement SD or E2E library therefore has to be compiled against the
internal headers of the exact vsomeip build it is loaded into. That is a
source-level dependency on implementation types, not a plugin boundary.
The swap-by-env-var freedom does not exist in practice. The second lookup
in `service_discovery_impl::init` is a leftover of a shutdown guard; see
§3.4.

### 1.2 ODR violations

Each plugin is its own link unit. In CMake, `vsomeip3-sd`, `-e2e`, and
`-cfg` each link `${Boost_LIBRARIES}` in addition to `vsomeip3`. In
Bazel, `vsomeip3-core` exports Boost, OpenSSL, and BoringSSL through
`exports_filter`, but `vsomeip3-e2e` takes `dynamic_deps` on `vsomeip3`,
not on `vsomeip3-core`. Any third-party code a plugin does not resolve
against core is statically absorbed into the plugin. `RTLD_LOCAL` then
hides that copy from the rest of the process, and which copy a given call
binds to depends on symbol visibility. Making `vsomeip3-core` the single
shared home for these dependencies is defeated by every plugin that brings
its own.

### 1.3 Application and pre-configuration plugins are dead

`application_plugin` has one method,
`on_application_state_change(application_name, state)`. The callback
receives no `application*`, no message, and no routing object. Nothing in
vsomeip reads a return value from it. The five call sites in
`application_impl` notify the plugin and then continue; init, start, and
stop do not change based on what the plugin does.

The configuration guide says the plugin can, after the notification, call
back into the public application API through `runtime::get()`. That is the
plugin driving vsomeip from outside, not a hook inside the stack. There is
no place for such a plugin to intercept a send, a receive, an offer, or a
subscription. `application::get_additional_data` only echoed the JSON
`additional` object back to whoever asked. No in-tree code asked, and the
method is removed. An application that wants init, start, and stop
notifications already has `register_state_handler`.

`pre_configuration_plugin::get_configuration_path()` is never called.
`configuration_impl` parses `type: configuration_plugin` into
`PRE_CONFIGURATION_PLUGIN` and nothing loads it.

No class in this tree implements either interface. The documented example
(`vsomeip-hello-plugin`, `vsomeip-world-plugin`) does not exist.

## 2. Requirements

1. Built-in plugins (SD, E2E, configuration) are resolved without
   `dlopen` and live in the same link graph as `vsomeip3-core`, so
   third-party dependencies have one copy per process.
2. `vsomeip3-core` does not depend on any plugin library. The application
   (the final link) decides which plugins exist.
3. No source break of `interface/vsomeip/plugin.hpp` or
   `interface/vsomeip/internal/plugin_manager.hpp`. `get_plugin(type, name)`,
   `plugin_impl<T>`, and `VSOMEIP_PLUGIN` keep compiling so the flag-off
   build and the existing `.so` targets still build.
4. The mode is selected at build time through a CMake option and a Bazel
   flag. Both default to today's `dlopen` behaviour, so an unflagged build
   matches 3.6.1. This fork's own binaries flip the flag on.
5. SD, E2E, and configuration each have their own `plugin_type_e` and are
   resolved through the static list when the flag is on. Service discovery
   and configuration are registered together. E2E is registered only when
   the list names it.
6. `application_plugin` and `pre_configuration_plugin` are no longer
   loaded, in either mode. Their headers stay so an existing `#include`
   still compiles, and nothing calls them.

## 3. Functional design

### 3.1 The application-provided plugin list

A new, additive public header `vsomeip/plugin_registry.hpp` declares one
function. The application defines it, in one translation unit:

```cpp
namespace vsomeip_v3 {

struct static_plugin_entry {
    plugin_type_e type_;
    create_plugin_func create_;
};

struct static_plugin_list {
    const static_plugin_entry* entries_;
    std::size_t size_;
};

// Weak, and not constexpr. See below.
static_plugin_list static_plugins() noexcept;

} // namespace vsomeip_v3
```

```cpp
constexpr static_plugin_entry k_plugins[] = {
    {plugin_type_e::SD_RUNTIME_PLUGIN, &create_sd_runtime_plugin},
    {plugin_type_e::CONFIGURATION_PLUGIN, &create_configuration_plugin},
    {plugin_type_e::E2E_PLUGIN, &create_e2e_plugin}, // omit to leave E2E off
};
static_plugin_list static_plugins() noexcept {
    return {k_plugins, std::size(k_plugins)};
}
```

The entries are a constexpr array. There is no registration call, no
mutex, and no later entry that replaces an earlier one. The function that
returns the array is not constexpr: a constexpr function is inline, and
`plugin_manager` has to call one out-of-line symbol. On ELF that symbol is
weak, so a binary that never defines it still links and every lookup
misses.

The list is keyed by type only. There is one SD, one configuration, and at
most one E2E plugin per process, so the name argument of
`get_plugin(type, name)` is ignored in this mode. Service discovery and
configuration are both in the list: a process that needs one needs the
other. E2E is optional by leaving that entry out.

Each built-in library exports its factory as a normal C++ symbol, declared
in `vsomeip/plugins/builtin_plugins.hpp`: `create_sd_runtime_plugin`,
`create_e2e_plugin`, and `create_configuration_plugin`. `VSOMEIP_PLUGIN`
stays next to them for the flag-off build.

The dependency points from the application to the plugin libraries. Each
plugin is an ordinary `DT_NEEDED` of the binary, resolved by the dynamic
linker at start-up, and it shares core's copy of every third-party library.
`vsomeip3-core` does not define the list and does not link the plugins.
On ELF the declaration is weak, so a binary that never defines
`static_plugins` still loads and every lookup misses.

This tree's definition is
`implementation/plugin/registration/register_plugins.cpp`. Nothing else
references that file, so an archive link would drop it. The Bazel target
`//:vsomeip-plugins` is `alwayslink` and passes `-Wl,-u` for
`static_plugins`. No vsomeip library depends on it. CMake has no
equivalent target. A CMake application compiles `register_plugins.cpp`
into its own sources, as `examples/routingmanagerd/CMakeLists.txt` does,
and exports the symbol so the shared library can call it.

With the flag on, lookup is the instance cache, then the list. A miss logs
the type and returns `nullptr`. With the flag off the function is not
called.

### 3.2 Static plugins, selected by a build flag

This is the design. A `dlopen` fallback behind the same map was the
alternative; it keeps a second link unit alive for every plugin someone
names at run time, which is the ODR exposure in §1.2. The flag exists so
an existing build can stay on `dlopen` until it links the factories.

CMake option `VSOMEIP_STATIC_PLUGINS` (default `OFF`). Bazel `bool_flag`
`//:static_plugins` with `config_setting` `:is_static_plugins_enabled`
(default `False`, set to `True` in this tree's `.bazelrc`). Both define
`VSOMEIP_STATIC_PLUGINS` for every vsomeip library.

When the flag is on:

- `plugin_manager_impl` compiles without `dlopen`/`dlsym`. SD and
  configuration already treat `nullptr` as fatal. E2E already treats it
  as protection off.
- `load_library` logs an error and returns `nullptr`. `load_symbol` and
  `unload_library` are no-ops. They stay in the vtable, so the
  `plugin_manager` header does not change.
- `VSOMEIP_SD_MODULE` and `VSOMEIP_E2E_PROTECTION_MODULE` are ignored.
  Setting either logs a warning.
- The plugin `.so` targets are still built and still shipped. Binaries
  link them directly instead of loading them by name.
- A binary that creates an application depends on `//:vsomeip-plugins`.
  That list includes the configuration plugin, which
  `application_impl::init` fetches when
  `VSOMEIP_ENABLE_MULTIPLE_ROUTING_MANAGERS` is off. With the define on
  (the Bazel default) the configuration entry is unused.

`vsomeip3-e2e` now takes `dynamic_deps` on `vsomeip3-core` instead of
`vsomeip3`, which closes the gap described in §1.2. It never needed
anything from the routing layer.

When the flag is off, `get_plugin` is the 3.6.1 `dlopen` path with one fix.
`load_plugin` used to record the library handle before checking the
plugin type, and never closed it if the check failed. It now records the
handle only on success and closes it otherwise. `unload_plugin` also
forgets the handles of the type it unloads. The retirement in §3.3 applies
in both modes.

### 3.3 Retire application and pre-configuration plugins

Delete the five `on_application_state_change` loops in `application_impl`.
Stop parsing `applications[].plugins`. A configuration that still has the
key logs a warning and otherwise loads as before. Update
`documentation/vsomeipConfiguration.md` to drop the `plugins` entry and
the hello/world example. Remove `application::get_additional_data` from
the public interface; it has no remaining caller.
`configuration::get_plugins` and `get_additional_data` are internal and
are removed.

Leave `interface/vsomeip/plugins/application_plugin.hpp` and
`pre_configuration_plugin.hpp` in place so an out-of-tree `#include`
still compiles. Remove the in-tree includes of both headers. Do not
register either type in the static list, and do not `dlopen` them when
the flag is off.

`PRE_CONFIGURATION_PLUGIN` stays in `plugin_type_e` so the values after
it do not shift. Nothing loads it.

### 3.4 E2E plugin type

This is an existing bug. `e2e_provider_impl` reports `APPLICATION_PLUGIN`,
and `routing_manager_impl` requests it under that type.

- Rename `APPLICATION_PLUGIN` to `E2E_PLUGIN` in place, so it keeps value 0.
  An existing E2E `.so` reports that value from `get_plugin_type()`, and
  the routing manager now requests `E2E_PLUGIN`. Appending a new enumerator
  would make the reported value and the requested value disagree.
  `e2e_provider_impl` reports it; the static list uses the same type, so
  the flag-on and flag-off lookups name the same kind of plugin.
- `unload_plugin` erases a whole type bucket. E2E no longer shares its
  bucket with application plugins, and after §3.3 nothing unloads
  application plugins anyway.
- Drop the second SD lookup in `service_discovery_impl::init`, and the
  `weak_ptr<sd::runtime>` it fills. From vSomeIP 1.3.0 that member was
  `runtime::get()`: each send and subscribe path did `runtime_.lock()` and
  then `its_runtime->create_message()`. The weak pointer is the
  destruction-order half of the static initialization fiasco. It does not
  keep the process runtime alive, and a callback that runs after the
  runtime is destroyed no-ops instead of using a dead singleton. In 2.7.0
  the assignment was retargeted at the SD plugin, and message construction
  moved to `std::make_shared<message_impl>()`. `sd::runtime` has no
  `create_message`. Every remaining `lock()` ignores the pointer it just
  obtained, and `send_multicast_offer_service` does not lock at all. The
  guard no longer watches the object it was written for. With the plugin
  registered at link time its lifetime is the process, so the `dlclose`
  case this rebinding accidentally covered goes away too. Removing the
  lookup also drops a plugin-manager dependency from standalone SD hosts.

## 4. Non-goals

- Removing `VSOMEIP_PLUGIN`, `plugin_impl<T>`, or the `dlopen` path from
  the flag-off build.
- Deleting `application_plugin.hpp` or `pre_configuration_plugin.hpp`.
- Turning `sd::runtime` or `e2e::e2e_provider` into a stable public ABI.
