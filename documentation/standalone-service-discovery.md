# Standalone Service Discovery in vsomeip

> **Status.** Everything below is implemented in this fork of vsomeip and
> shipped as the reference example `examples/sd_only_host/`. The document
> is written as a proposal to the upstream maintainers: the design is
> minimally invasive on the public surface (no header break), reuses
> vsomeip's own SD state machine unchanged, and is validated end-to-end
> under Bazel, CMake, TSan, and LSan.

## 1. Why this belongs in mainline vsomeip

vsomeip is architected around a single `routing_manager` process per
node: applications register with it over Unix-domain sockets, offer or
subscribe through it, and the routing manager owns every SOME/IP data
socket on the machine. That model is a good fit for compact ECUs where
one process serves several small applications. It becomes limiting for
four use cases we and other consumers are hitting today.

### 1.1 High-throughput data plane, minimally-copied

In the routing-manager model every SOME/IP frame an application sends
or receives crosses the app-to-RM Unix-socket boundary at least once,
plus the RM's internal buffer management on top of that. For low-rate
control traffic — the domain SOME/IP was originally designed for — that
cost is invisible. For the bulk data flows increasingly carried by
SOME/IP-TP or SOME/IP-over-TCP in modern vehicle architectures (camera
frames, radar / lidar cubes, HD maps, over-the-air payloads) each extra
copy or hop can cause unacceptable latency and CPU load.

Letting the consuming application own the wire socket collapses that
hop. The application binds the port itself, drains it directly into its
own memory management, and no application data ever crosses the
control-plane IPC. Standalone SD is the piece of vsomeip that makes
this possible without also giving up SOME/IP-SD compliance.

### 1.2 Removing the routing manager as a single point of failure

Automotive safety architectures ask "what happens if
process X crashes?" The routing manager is a shared dependency: if it
dies, every SOME/IP-speaking application on the node loses its
control-plane path, and every publisher loses the ability to reach a
new subscriber. For ASIL-B-and-higher signal flows that shared
dependency is difficult to justify, because the RM is not itself
hardened to those integrity levels — it is a general-purpose
multiplexer.

Standalone SD lets a safety application host its own SD state and its
own service socket, sharing nothing but the wire multicast group with
its neighbours. A crashing neighbour cannot take the safety app down;
a crashing SD authority in one process does not stop offers announced
by a different process. The distributed model has stronger failure
containment than the "everything through one broker" model, without
requiring changes to the SOME/IP wire protocol.

### 1.3 Aligning socket ownership with data ownership

Even outside safety and throughput concerns, there is a natural
argument that the process which knows what to do with a SOME/IP
service's payloads should also own its receive socket. Socket
buffer sizing, `SO_RCVBUF` tuning, DSCP marking, thread affinity of
the receive completion, back-pressure policy — these all depend on
what the payload is and what the consumer intends to do with it. The
routing manager cannot make those choices well across a heterogeneous
mix of clients. Delegating the socket to the owner brings those knobs
back where the domain knowledge lives.

### 1.4 Composition with modern zero-copy IPC frameworks

Users are increasingly pairing SOME/IP with dedicated intra-process
and inter-process zero-copy frameworks — projects like Eclipse
[iceoryx](https://github.com/eclipse-iceoryx/iceoryx2), [dalison/subspace](https://github.com/dallison/subspace), or in-house shared-memory buses. Those
frameworks specialise in efficient local distribution; vsomeip
specialises in SOME/IP wire semantics. The natural division of labour
is "SD in vsomeip, local distribution in the shmem framework of
choice". That requires the SD layer to be usable without being forced
to also route local traffic through vsomeip's UDS IPC.

Standalone SD makes that split feasible without changing vsomeip's
SOME/IP semantics or breaking the routing-manager model for consumers
who still want it.

---

## 2. What "standalone SD" means concretely

A **standalone SD host** is a process that runs `service_discovery_impl`
directly against an application-supplied
`vsomeip_v3::sd::service_discovery_host` implementation, without
constructing a `routing_manager_impl`, an `application_impl`, or any of
their sibling machinery. It:

- opens exactly one UDP socket — the SD multicast/unicast socket;
- speaks the SOME/IP-SD wire protocol as a full peer (OfferService,
  FindService, SubscribeEventgroup on the emit side; the same
  entries plus StopOffer/Unsubscribe on the receive side);
- exposes the parsed remote-side view (which services are offered by
  which address:port tuples) to the process that consumes it;
- never binds any data-plane socket, never creates the local
  Unix-domain endpoints used by the routing manager, and never opens
  an `AF_NETLINK` socket.

The reference implementation lives at
[`examples/sd_only_host/sd_only_host.cpp`](../examples/sd_only_host/sd_only_host.cpp).

---

## 3. The interface, and what it hides

The public-looking surface is
[`implementation/service_discovery/include/service_discovery.hpp`](../implementation/service_discovery/include/service_discovery.hpp):

```cpp
class service_discovery {
public:
    virtual void init() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

    virtual void request_service(...);
    virtual void release_service(...);
    virtual void subscribe(...);
    virtual void unsubscribe(...);
    virtual bool send(bool _is_announcing) = 0;
    virtual void on_message(const byte_t* _data, length_t _length,
                            const boost::asio::ip::address& _sender,
                            bool _is_multicast) = 0;
    virtual void offer_service(const std::shared_ptr<serviceinfo>& _info) = 0;
    virtual bool stop_offer_service(...);
    ...
};
```

That looks promising: feed it inbound multicast bytes via `on_message`,
call `offer_service` / `request_service`, drive `send`. But the
constructor of the only implementation in
[`service_discovery_impl.hpp`](../implementation/service_discovery/include/service_discovery_impl.hpp)
takes two interfaces that were, until this proposal, not usable
without dragging in most of vsomeip:

```cpp
service_discovery_impl(service_discovery_host* _host,
                      const std::shared_ptr<configuration>& _configuration);
```

| Interface                                | Pure virtuals | Where                                                                                                  |
| ---------------------------------------- | ------------- | ------------------------------------------------------------------------------------------------------ |
| `vsomeip_v3::configuration`              | 121           | [`configuration.hpp`](../implementation/configuration/include/configuration.hpp)                       |
| `vsomeip_v3::sd::service_discovery_host` | 16            | [`service_discovery_host.hpp`](../implementation/service_discovery/include/service_discovery_host.hpp) |

Only a small fraction of both are reached from the SD code path.

---

## 4. Friction points a standalone host has to deal with

Four things in
[`service_discovery_impl.cpp`](../implementation/service_discovery/src/service_discovery_impl.cpp)
force a standalone host to be more than a couple of stubs. None of
them require patching vsomeip — they require reusing the pieces of
vsomeip that already do the work.

### 4.1 The plugin-manager lookup in `init()`

`service_discovery_impl::init()` used to look up its own SD runtime
factory through `plugin_manager::get()->get_plugin(SD_RUNTIME_PLUGIN, ...)`,
which ended in `dlopen("libvsomeip3-sd.so.3", RTLD_LAZY|RTLD_LOCAL)`.
That lookup is gone; see
[static-plugin-registry.md §3.4](static-plugin-registry.md). A standalone
host that constructs `service_discovery_impl` directly no longer touches
the plugin manager for SD.

### 4.2 The host callback surface

[`service_discovery_host.hpp`](../implementation/service_discovery/include/service_discovery_host.hpp)
declares sixteen pure virtuals. The ones actually reached from
`service_discovery_impl` are:

- `get_io()`
- `create_service_discovery_endpoint(addr, port, reliable)`
- `get_offered_services()` / `get_offered_service()` / `get_offered_service_instances()`
- `find_eventgroup(service, instance, eventgroup)`
- `find_or_create_remote_client(service, instance, reliable)`
- `add_routing_info(...)` / `del_routing_info(...)` / `update_routing_info(...)`
- `on_remote_subscribe(...)` / `on_remote_unsubscribe(...)`
- `on_subscribe_ack(...)` / `on_subscribe_ack_with_multicast(...)` / `on_subscribe_nack(...)`
- `expire_subscriptions(...)` (overloaded) / `expire_services(...)` (overloaded)
- `get_subscribed_eventgroups(service, instance)`
- `send(client, message, force)` / `send_via_sd(target, data, size, port)`

The signatures use `serviceinfo`, `eventgroupinfo`, `endpoint`,
`endpoint_definition`, `remote_subscription`,
`remote_subscription_callback_t`, `message`, `vsomeip_sec_client_t`
and friends. Every one of these types has to be reachable — a
standalone host cannot invent its own — so the split has to preserve
them, not hide them.

The endpoint returned from `create_service_discovery_endpoint` is
dereferenced through the abstract `endpoint` base and the abstract
`multicast_endpoint` interface (`endpoint.hpp`). There is no concrete
downcast requirement, so the standalone host is free to return
whatever concrete implementation it wants — the reference example
returns the stock `udp_server_endpoint_impl` for the standard
multicast behaviour, but a wrapper that satisfies the two abstract
interfaces would also work.

### 4.3 The configuration interface

`service_discovery_impl` reads roughly twenty `configuration::get_sd_*()`
values and a handful of general accessors: unicast address, SD
multicast address, SD port, SD protocol, SD TTL, initial-delay
min/max, repetitions base delay, repetitions max, cyclic offer delay,
find initial-debounce reps & time, offer debounce time, find debounce
time, TTL factor for offers, TTL factor for subscribes, stop-offer
watchdog time, buffer shrink threshold, netmask, reliability-type
lookup. Trying to stub this against a hand-rolled `configuration`
derived class means providing throwing stubs for the 100-plus pure
virtuals we don't touch — most of which are reached from adjacent
code paths (`get_reliability_type`, security clients, routing
read-set) at unpredictable times.

The standalone host reuses `cfg::configuration_impl` directly.

### 4.4 Logging and other process-global singletons

`VSOMEIP_INFO` / `VSOMEIP_ERROR` / `VSOMEIP_WARNING` expand into calls
on `vsomeip_v3::logger::logger_impl::get()`, itself initialised from
`configuration`. Similar story for `plugin_manager::get()` and the
runtime singleton. These have to be **one instance per process** —
duplicated singletons (e.g. from statically linking two copies) break
plugin registration.

---

## 5. The split

The core idea: partition vsomeip's shared libraries so that everything
`service_discovery_impl` needs at run time is reachable from a smaller
library, and everything that only a `routing_manager_impl` needs is
optional.

### 5.1 Three libraries instead of one

Instead of the historical single `libvsomeip3.so`, this proposal
produces three:

| Library               | Contents                                                                                                                                                                                                                                                                                                                                    | Depends on                          |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| `libvsomeip3-core.so` | SOME/IP wire framing (message, payload, header, serializer, deserializer), endpoints (client + server, TCP + UDP + TP), plugin manager, logger, configuration, security, tracing, utility, the "routing data" carriers (`serviceinfo`, `eventgroupinfo`, `event`, `remote_subscription`), and `runtime_impl`'s message / payload factories. | third-party only (Boost, spdlog)    |
| `libvsomeip3-sd.so.3` | The SD state machine (`service_discovery_impl` and its wire-format helpers). Unchanged from today.                                                                                                                                                                                                                                          | `libvsomeip3-core.so` (`DT_NEEDED`) |
| `libvsomeip3.so`      | `routing_manager_*`, `application_impl`, the UDS IPC endpoints, `netlink_connector`, `endpoint_manager_impl`, and the routing half of `runtime_impl` (see §5.2).                                                                                                                                                                            | `libvsomeip3-core.so` (`DT_NEEDED`) |

A traditional routing-manager-based vsomeip application depends on
`libvsomeip3.so` and pulls in the whole stack, unchanged. A
standalone-SD host depends on `libvsomeip3-core.so` +
`libvsomeip3-sd.so.3`, gets the SD stack, and is free of every
routing-layer symbol.

The `libvsomeip3-core.so` / `libvsomeip3.so` split uses `DT_NEEDED`
stacking. Singletons — `plugin_manager::get()`, the logger, the
configuration, the runtime `shared_ptr<runtime>` — live in
`libvsomeip3-core.so` and are visible to both the routing manager (via
`libvsomeip3.so`'s `DT_NEEDED`) and to standalone hosts that don't
load `libvsomeip3.so` at all. There is exactly one copy of each
singleton per process regardless of which combination is loaded.

### 5.2 `runtime_impl` and the application factory

The historical `runtime_impl` (in `implementation/runtime/`) contains
both the message / payload factories (which no code path outside
`libvsomeip3-core.so` needs) and the application factory (which lives
above `application_impl`, purely in the routing layer). Keeping them
in one translation unit forces the `runtime_impl` vtable and its
`application_impl` uses to live in the same shared object.

The split addresses this without introducing a public-API break by
delegating the application-related vtable slots through an
[`internal::application_factory`](../implementation/runtime/include/application_factory.hpp)
interface. `runtime_impl`'s vtable slots for `create_application`,
`get_application`, and `remove_application` all have concrete bodies
defined in `libvsomeip3-core.so`; they forward through a
statically-stored `application_factory*` that the routing layer
installs at library-load time via
`runtime_impl::set_application_factory(...)`.

The install path is a namespace-scope registrar in
[`implementation/runtime/src/runtime_impl_apps.cpp`](../implementation/runtime/src/runtime_impl_apps.cpp)
(only compiled into `libvsomeip3.so`). Its constructor runs during
library init and hands `libvsomeip3-core.so` a factory whose
`create` / `get` / `remove` methods construct and manage
`application_impl` instances. A process that never loads
`libvsomeip3.so` has no factory installed, and any call to
`runtime::create_application` throws `std::logic_error` with a message
naming the missing library — the correct failure mode for an SD-only
host, and immediately actionable when it hits.

The factory slot is a `std::shared_ptr<internal::application_factory>` accessed via
atomic operations, keeping reads lock-free and safe under TSan without locking on the hot path.

### 5.3 Files that moved

- `implementation/runtime/include/application_factory.hpp` — new
  interface header (declared in `namespace vsomeip_v3::internal`).
- `implementation/runtime/src/runtime_impl.cpp` — application-related
  methods now delegate to the injected factory; message / payload
  methods unchanged. Compiled into `libvsomeip3-core.so`.
- `implementation/runtime/src/runtime_impl_apps.cpp` — new file.
  Owns the applications registry, provides the factory implementation,
  and hosts the namespace-scope registrar. Compiled into
  `libvsomeip3.so`.

No public headers changed; every existing consumer of the `runtime`
interface continues to work bit-for-bit.

### 5.4 Deleted intermediate files

Earlier iterations of the split produced two "surgical extraction"
files (`event_ctor.cpp` and `deserializer_message.cpp`) that carried a
single method body each to keep an `#include` chain from leaking
routing-layer symbols into core. Once `runtime_impl` moved to core
those intermediaries became unnecessary and were folded back into
their parent files:

- `implementation/routing/src/event.cpp` — one file again; its
  constructor (which calls `runtime::get()->create_notification()`)
  resolves inside `libvsomeip3-core.so`.
- `implementation/message/src/deserializer.cpp` — one file again; its
  `deserialize_message()` returns `std::unique_ptr<message_impl>`,
  same as before, and `message_impl.hpp` is visible in core.

---

## 6. The reference example

[`examples/sd_only_host/sd_only_host.cpp`](../examples/sd_only_host/sd_only_host.cpp)
is a single-file program that:

- links against `libvsomeip3-core.so` + `libvsomeip3-sd.so.3`;
- implements `endpoint_host`, `routing_host`, and
  `service_discovery_host` as small local classes;
- takes a stripped-down JSON configuration whose `services[]` block
  drives outbound OfferService PDUs and whose `clients[]` block
  drives outbound FindService PDUs;
- prints inbound OfferService entries into a live routing table,
  updated on every SD receive callback;
- returns real `udp_client_endpoint_impl` / `tcp_client_endpoint_impl`
  objects from `find_or_create_remote_client` — constructed with the
  learned peer's address:port and a configured client port — but
  **never calls `start()`** on them, so no data socket is bound;
- verifiably opens exactly one UDP socket (the SD port) during its
  lifetime (`strace -f -e trace=socket,bind,connect`).

The example is deliberately not the "GM broker" or any other
downstream shape. It is the smallest thing that exercises the split.

---

## 7. Netlink

The upstream `netlink_connector` is constructed exactly once, inside
`routing_manager_impl::init()`. Because a standalone SD host never
instantiates `routing_manager_impl`, no `AF_NETLINK` socket is opened
and none of the `RTMGRP_*` subscriptions happen — even without the
library split, this was always true at run time. The split also
excludes `netlink_connector.cpp` from `libvsomeip3-core.so` for
belt-and-braces, so it is not even reachable at link time in an
SD-only process.

---

## 8. Comparison with the `protocol=external` path

vsomeip already offers a `protocol=external` mode: a full
`application_impl` registers with the routing manager, declares its
services as externally implemented, and rides the SD-plugin contract
through the routing manager. Nothing this proposal does forecloses
that path.

The differences that motivate having _both_ modes available are the
ones §1 lists. `protocol=external` retains every routing-manager
surface — the fake application registration, the UDS IPC endpoints
the RM creates regardless, the RM's security gates, the netlink
connector for an interface the app does not care about, the JSON
schema's expectation that the process is a real vsomeip app — while
merely inhibiting the data plane. For the four use cases in §1 the
whole family of routing-manager surfaces is exactly what we need to
avoid, not merely mute.

Both modes have their place. A single-application node with control
traffic and no safety exposure should probably continue to use the
routing manager. A high-bandwidth application, or one on a safety
boundary, or one composing SD with a dedicated zero-copy IPC
framework, should be able to reach `service_discovery_impl` without
paying the routing-manager cost. This proposal adds the second
option without removing the first.

---

## 9. Build system status

- **Bazel.** The split is expressed in `BUILD.bazel` as three
  `cc_shared_library` targets (`vsomeip3-core`, `vsomeip3-sd`,
  `vsomeip3`) plus their intermediate `cc_library` targets. The
  `runtime_impl_apps.cpp` file goes into `VSOMEIP_ROUTING_LAYER_RUNTIME_SRCS`;
  `runtime_impl.cpp` (with delegation) goes into a new
  `VSOMEIP_CORE_RUNTIME_SRCS`. Both are added to their respective
  `VSOMEIP_CORE_SRCS` / `VSOMEIP_ROUTING_LAYER_SRCS` aggregates.
  `libvsomeip3.so` declares `libvsomeip3-core.so` as `dynamic_deps`
  (i.e. `DT_NEEDED`) rather than duplicating its sources.
- **CMake.** The upstream CMake source lists are already glob-driven
  (`file(GLOB "implementation/*/src/*.cpp")`), so the CMake build
  automatically picked up `runtime_impl_apps.cpp` and dropped the
  deleted intermediate files with **zero edits to `CMakeLists.txt`**.
  It produces a single monolithic `libvsomeip3.so` just as before —
  the core / routing split is a Bazel-side concern, and the CMake
  build keeps the historical layout for consumers who prefer it.

Both builds pass their respective test suites; the Bazel build has
been validated under `--config=tsan` and `--config=lsan` on top of
the default configuration.

---

## 10. Summary of proposed changes to upstream

If accepted upstream in whole, these are the diffs:

1. Add `implementation/runtime/include/application_factory.hpp` — new
   interface header.
2. Modify `implementation/runtime/include/runtime_impl.hpp` — drop
   the internal applications map / mutex, add
   `static void set_application_factory(...)`.
3. Rewrite `implementation/runtime/src/runtime_impl.cpp` — application
   methods delegate to the injected factory; everything else
   unchanged.
4. Add `implementation/runtime/src/runtime_impl_apps.cpp` — factory
   implementation and namespace-scope registrar.
5. Update `CMakeLists.txt` — no changes needed; source globs already
   cover the new file (verified).
6. Optionally: `BUILD.bazel` for consumers who want the core / routing
   split as separate shared libraries. Not required for the CMake
   consumers.
7. `examples/sd_only_host/` — reference program demonstrating a
   standalone SD host, ~500 lines including comments.

No changes to any public header in `interface/vsomeip/`. No behaviour
change for existing applications built against `libvsomeip3.so`. Every
existing test continues to pass.

The proposal is deliberately conservative: it adds capability without
removing any, keeps the routing-manager path as the default, and
respects the existing plugin manager and configuration mechanisms.
The only opinionated change is the introduction of the
`application_factory` indirection inside `runtime_impl` — and that
change is required regardless of how the split is expressed, because
without it a `libvsomeip3-core.so` that instantiates `runtime_impl`
would need the whole application layer as a link-time dependency.
