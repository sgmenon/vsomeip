# Receive-path copy elision in vsomeip

> **Status.** Implemented in this fork as the receive half of
> [E2E scatter-gather send](e2e-scatter-gather-send.md) and
> [send-path copy elision](send-copy-elision.md). Written as a mainline
> candidate: the socket read stays contiguous, the wire format is
> unchanged, and application handlers still receive
> `shared_ptr<message>`.

## 1. Summary

This is an efficiency improvement. Earlier the vsomeip stack had a lot of
redundant copies that caused the stack to be inefficient. Some of these
copies were just unnecessary, but others required API changes.

The core idea is that we receive a contiguous range from the sockets (UDP,
TCP or Unix Sockets), and then downstream operations operate on spans to
this buffer. A `shared_ptr` manages the lifetime of the buffer. When it goes
out of scope, memory is returned.

## 2. What changed

Counts are **payload-sized copies in this process**. A copy into a
kernel buffer on a **network** socket (UDP or TCP to another node) is
not counted. A copy across the **local** Unix-domain or local-TCP
socket between an application and the routing manager is counted.

That local hop is a property of one routing manager per node. The sender
`writev`s the pinned `buffer_sequence` with the framing tags as extra
`const_buffer`s (**0×** in userspace). The kernel still copies those
bytes into the socket (**1×**). The receiver's `read` copies the socket
into `recv_buffer_` (**1×**). When the framed command fills that window
the endpoint moves it (**0×** more); otherwise it copies the command out
(**1×**). The hop goes away when each process is its own routing
manager and owns its SOME/IP socket directly.

An owned 16-byte SOME/IP header on an E2E forward is called out
separately. It is not a payload copy.

| Path                                    | Before                                                                                             | After                                                                                                                                                                                                                 |
| --------------------------------------- | -------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| UDP → hosting app (no E2E)              | **2–3×** (request: frame `set_data` + payload deserialize; notification adds one `create_payload`) | **0×** (datagram `shared_ptr` passed as `whole` / `slice`)                                                                                                                                                            |
| TCP → hosting app (no E2E)              | **2–3×**, same as the UDP row                                                                       | **0×** when the frame fills the window (`gap == 0`): `take_stream_frame` moves that window into a `message_buffer_pool` lease. Otherwise **1×** copy-out from the same pool. The buffer returns to the pool when the last payload reference drops. An empty pool drops the frame |
| Local UDS/TCP → stub                    | **≥2×** at the sender and **≥2×** at the stub                                                      | **1×** at the sender: kernel `writev` of the pinned sequence (**0×** in userspace). At the stub, **1×** for the kernel `read`. When the framed command fills the used window, `take_local_ipc_command` moves that window into a `message_buffer_pool` lease and returns a slice with the tags removed (**0×** more). Otherwise **1×** copy-out of the command. An empty pool allocates; local commands are not dropped |
| Network → hosting app (with E2E)        | the 2–3× above, plus one buffer with the E2E header removed                                        | **0×**. `check` returns spans into the receive buffer, and the delivered payload is a view of that buffer                                                                                                             |
| Network → other local app (E2E forward) | one new frame, header and payload concatenated without the E2E header                              | **0×** inside the routing manager: the app bytes stay in the receive buffer, and a separate 16-byte SOME/IP header carries the patched length. Delivery to the other application then pays the local-socket hop above |
| Stub SEND → routing manager             | **≥2×** at the sender and **≥2×** at the routing manager                                           | **1×** at the sender (kernel `writev`). At the routing manager, **1×** for the kernel `read`, then **0×** when the framed command fills the window and **1×** copy-out otherwise |
| SOME/IP-TP reassembly                   | **1×** full                                                                                        | **1×** (\*_TODO_ this has not yet been improved)                                                                                                                                                                      |
| Proxy receive (routing manager → app)   | the local-socket hop (**≥2×** at the routing manager and **≥2×** at the app), then 2–3× deserialize | **1×** at the routing manager (kernel `writev`). At the app, **1×** kernel `read`, then **0×** when the framed command fills the window and **1×** copy-out otherwise. The SOME/IP region is then pinned (**0×**, `build_message_from_buffer`) |

E2E check and strip do not copy the payload. The 16-byte header on a
forward is not a payload copy. The local-socket copies above are.

## 3. The type that carries it

`owned_buffer_slice` in
[`implementation/endpoints/include/buffer.hpp`](../implementation/endpoints/include/buffer.hpp)
is a shared buffer plus an explicit byte range. The same type is a
segment of `buffer_sequence`, so a received frame can be forwarded
without a second representation.

```cpp
using message_buffer_t = std::vector<byte_t>;
using message_buffer_ptr_t = std::shared_ptr<message_buffer_t>;
struct owned_buffer_slice {
    message_buffer_ptr_t buffer; // required when valid()
    std::size_t offset{0};
    std::size_t length{0};       // explicit; never "0 means whole buffer"
};
```

Helpers: `whole`, `slice`, `copy_of`, `from_pointer`, `is_whole()`.
`length` is always the number of bytes in the range.

`routing_host::on_message` takes that slice. There is no `const byte_t*`
on the routing-manager data-plane receive path. The slice keeps the
frame buffer alive for processing and for any `payload_impl` or
`buffer_sequence` built from it.

`payload_impl` is view-only: `buffer_` + `offset_` + `length_`. A
handler's `shared_ptr<message>` may therefore point at a pinned receive
buffer. The public `application` / `message` / `payload` types are
unchanged.

## 4. What each edge does

**UDP.** The datagram `shared_ptr` is passed through as `whole` or as a
`slice` of the used bytes. The server does not recycle that buffer for
the next receive.

**Remote TCP.** `take_stream_frame` pulls one complete message out of
the compaction window before any leftover `memmove`. When `gap == 0`
and the message size equals the used region, the window storage is
moved into a `shared_ptr` and the window is replaced. Otherwise the
frame is copy-out. Stream code does not pin into a window that is about
to be compacted.

TCP receive buffers come from `message_buffer_pool`
([`message_buffer_pool.hpp`](../implementation/endpoints/include/message_buffer_pool.hpp)),
a LIFO stack. `lease` / `adopt` return a `shared_ptr` whose deleter
recycles the buffer when the last pin drops. Depth is
`receive-buffer-pool-size` (default 8). An empty pool **drops** the
frame: no fallback allocation under stall. A configured size of 0
disables the pool and allocates with `make_shared` instead.

**Local UDS/TCP.** This is the routing-manager side of the Unix-domain
hop. `read` fills `recv_buffer_` from the kernel. `take_local_ipc_command`
then pulls one complete command out of that window. When the framed
command (start tag, command, end tag) is the entire used region at
offset 0, the window is moved into a `message_buffer_pool` lease and the
returned slice skips both tags. Otherwise only the command bytes are
copied out. Depth is `receive-buffer-pool-size`, one
pool per connection. An empty pool allocates a fresh buffer; a local
command is not dropped the way a remote TCP frame is. The kernel
`writev` and `read` remain for as long as applications reach the
routing manager through this socket. The sender does not copy the
payload.

**Hosting application.** `deliver_message(owned_buffer_slice)` reads
SOME/IP header fields and attaches a viewing `shared_ptr<payload>`. It
does not run `deserializer::set_data` over the full frame. Debounce and
the host handler share that one `shared_ptr<payload>`.

**E2E.** `check()` returns `check_result` spans into the pin, including
on CRC failure, so strip can still see the sections. Local forward uses
`send_local(buffer_sequence)`:

- an owned 16-byte SOME/IP header, with the length patched to the
  hole-free size;
- `append_buffer_slice` of `check_result.app_payload` when those bytes
  lie in the frame buffer, otherwise one owned buffer of the
  application bytes only.

Stock AUTOSAR footers are empty. Profile 01 reports its in-band
CRC/counter/nibble as `e2e_header`, so the same strip drops them.
Details of protect/check live in
[e2e-scatter-gather-send.md](e2e-scatter-gather-send.md).

**Proxy.** A non-routing application (`routing_manager_client`) receives
`SEND` over local IPC, parses the command header in place, and pins the
SOME/IP region with `build_message_from_buffer`. Notifications arrive as
`SEND_ID` with notification message type on that same path.

**Service Discovery.** SD keeps `on_message(const byte_t*, …)`. The
routing manager passes bytes from the frame pin for the duration of the
call. SD does not hold that pointer after return.

## 5. Data path

```
Endpoint recv
  → owned_buffer_slice
       UDP: pin the datagram
       TCP: take_stream_frame (move, copy-out, or drop)
       local: take_local_ipc_command (move when the framed command fills the window, else copy-out)
  → routing_host::on_message(slice)
  → e2e check → check_result spans into the pin
  → host: payload_impl view of the application bytes
    or local forward: 16-byte header + app slice
  → proxy app, if any: SEND header parsed in place, SOME/IP region pinned
```

## 6. Compatibility notes

- **Wire format:** unchanged.
- **Application handlers:** still `shared_ptr<message>`. The payload may
  be a view of a receive buffer. Handlers that copy out via
  `payload::get_data()` behave as before. Handlers that retain the
  `shared_ptr` keep the receive buffer alive until they drop it, which
  is what recycles a pooled TCP buffer.
- **E2E:** successful check + strip still delivers hole-free application
  data. The header bytes are not part of the delivered payload.
- **Back-pressure:** with the TCP pool enabled, a stall drops a remote
  TCP frame instead of allocating. Local UDS/TCP uses the same pool
  depth but allocates when it is empty, so a local command is still
  delivered. Size 0 disables the pool and allocates per frame on every
  path.

## 7. How to verify

```bash
bazel test //test/unit_tests/e2e_tests:e2e_tests
bazel test //test/unit_tests/endpoint_tests:endpoint_tests
```

`e2e_check_strip_pin` in `ut_check_strip_pin.cpp` checks that the
`check_result` spans, the `payload_impl`, and the stripped sequence all
share the frame buffer. The Docker suites `e2e_crc`, `e2e_p04`, and
`e2e_p07` remain wire-CRC smoke; see
[e2e-scatter-gather-send.md](e2e-scatter-gather-send.md).

## 8. Follow-ups

- SOME/IP-TP reassembly still flattens into one buffer.
- SD `on_message` can take an `owned_buffer_slice` if SD ever needs the
  pin to outlive the callback. Today the routing manager passes pin
  bytes only for the call.

## Related

- [send-copy-elision.md](send-copy-elision.md) — pin versus snapshot on send
- [e2e-scatter-gather-send.md](e2e-scatter-gather-send.md) — `check_result` spans and `buffer_sequence`
