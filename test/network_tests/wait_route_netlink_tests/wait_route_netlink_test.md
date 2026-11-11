# `wait_route_netlink` network tests

Behavioral coverage for `service-discovery.wait_route_netlink_notification` and
`routing_manager_impl::is_external_routing_ready()`.

Zuul containers normally add `ip route add multicast 224.0.0.0/4 dev eth0`.
These tests move the IPv4 multicast route onto a **dummy** interface while the
routing manager’s unicast address stays on **eth0**, matching the production case
where the kernel may still reach SD multicast but netlink reports the route on
another `RTA_OIF`.

| CTest name | Config | Expectation |
|------------|--------|-------------|
| `wait_route_netlink_wait_false_test` | `wait_route_netlink_notification: false` | Log contains `SOME/IP routing ready.` without an eth0 SD route |
| `wait_route_netlink_wait_true_test` | default wait (`true`) | No routing ready for ~12s, then ready after `224.0.0.0/4 dev eth0` |

Requires `NET_ADMIN` (provided in the Zuul network-test images).
