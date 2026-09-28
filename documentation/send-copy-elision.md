# Send-path copy elision in vsomeip

> **Status.** Implemented in this fork on top of
> [E2E scatter-gather send](e2e-scatter-gather-send.md). Written as a
> mainline candidate: the wire format is unchanged, existing `send` /
> `notify` callers keep compiling, and a payload-sized copy happens only
> when the caller still shares the bytes.

## 1. Why this belongs in mainline

Scatter-gather stopped the stack from concatenating an E2E header and an
application payload into one buffer before `async_send`. The copies in
front of that queue were still there. On upstream COVESA 3.6.1 a remote
send with E2E and nPDU debouncing typically copied the application bytes
several times:

1. **App** padded a hole for the E2E header (the old contract).
2. **Serialize** copied the payload into a pooled serializer vector that
   held a full SOME/IP frame.
3. **E2E routing** assigned that frame into another buffer and ran
   `protect` in place
   ([upstream `routing_manager_impl.cpp`](https://github.com/COVESA/vsomeip/blob/master/implementation/routing/src/routing_manager_impl.cpp#L669)).
4. **Train** inserted the result into `train_->buffer_` (removed once
   the train holds a `buffer_sequence`).

Hole-free E2E removed (1). Scatter-gather removed (4). This change
removes (2) and (3) on the routing-host `send(message)` / `notify` path,
and stops the E2E plugin from copying application bytes into its own
buffer.

The goal for mainline is one rule: application bytes are pinned from the
`shared_ptr<payload>` the caller already owns, through E2E and the
endpoint queue, until the socket completion runs. A copy is a deliberate
snapshot at the public API, taken when the caller might still mutate
those bytes.

## 2. What changed

Counts are **payload-sized copies** on the routing-host
`application::send(message)` path. The 16-byte SOME/IP header and any
owned E2E meta are separate small buffers.

| Stage                          | Before                         | After                                                |
| ------------------------------ | ------------------------------ | ---------------------------------------------------- |
| App `set_data(ptr)`            | 1× into `shared_ptr<payload>`  | unchanged (the caller's own copy into the payload)   |
| App `set_data(move(vec))`      | 0×                             | 0×                                                   |
| Serialize full frame           | 1×                             | skipped for non-SD `send(message)`                   |
| E2E plugin app copy            | 1×                             | 0× (span into the pinned payload)                    |
| Routing into `buffer_sequence` | 1×                             | 0× (`append_message_payload` pins the backing store) |
| nPDU train                     | concat into one train buffer   | 0× (`append_sequence` of the same pins)              |
| Socket queue                   | holds buffers until `send_cbk` | holds the same `shared_ptr`s until `send_cbk`        |

Profile 01 is the exception: CRC, counter, and nibble sit in-band, so
`protect` packs once into `owned_app_payload`. Header/footer profiles
allocate only the E2E meta.

Service Discovery is the other exception. SD messages store entries and
options rather than a payload, so that path still serializes into one
owned buffer.

## 3. Application contract

`send`, `notify`, and `notify_one` return after the writes are queued.
The endpoint holds the `buffer_sequence` until `async_send` /
`async_write` completes. Pins on `shared_ptr<payload>` (and on owned
header / E2E vectors) keep the bytes alive. There is no free callback
unless the caller asks for one.

At the public API the stack decides pin versus snapshot:

- **Exclusive.** The caller passes the only remaining `shared_ptr`,
  typically with `std::move`. The payload is pinned. No payload-sized
  copy.
- **Shared, no completion.** The caller still holds a live `shared_ptr`
  and did not pass a completion handler. Bytes are snapshotted once, so
  a later `set_data` or free cannot change an in-flight write.
- **Shared, with completion.** The payload is pinned as-is. The caller
  keeps it alive and unchanged until the handler runs. That handler is
  the natural place to free or recycle. No payload-sized copy.

Do not poll `use_count()` from application code. Move the pointer when
you want the pin, or pass a completion handler when you want to reuse
the buffer after the local write.

```cpp
app->send(std::move(msg));
app->send(msg, [](bool ok) { /* local writes done */ });
app->notify(service, instance, event, std::move(payload), false);
app->notify(service, instance, event, payload, false, [](bool ok) { /* ... */ });
```

The handler type is `send_completion_handler_t` in
[`interface/vsomeip/handler.hpp`](../interface/vsomeip/handler.hpp). It
runs **once**, posted through the application dispatcher, when every
async write started by that call **in this process** has finished. `ok`
is the AND of those results.

- **Proxy client:** the handler covers the IPC write to the routing
  manager only.
- **Routing host:** the handler covers the remote and local-subscriber
  writes started here (first hop is the last hop).
- Debounce that later sends the same field again is outside this call.
- If nothing is queued (unchanged field, or no subscribers), the handler
  runs immediately with `true`.

`notify` / `notify_one` use `snapshot_payload_if_shared` unless
`_completion` is set. `send(message)` uses
`ensure_exclusive_message_payload` unless `_completion` is set. Both
live in
[`payload_ownership.hpp`](../implementation/routing/include/payload_ownership.hpp).
Routing always pins after that boundary. The exclusive-or-not flag is
not threaded into the routing manager.

The optional handler is a defaulted parameter on the existing virtuals.
Callers that already call `send` / `notify` / `notify_one` keep
compiling and keep the snapshot-if-shared behavior.

## 4. How the pin is built

`append_message_payload` takes a `shared_ptr<payload>`, downcasts to
`payload_impl`, and calls `append_buffer_slice` on the backing vector.
That is the same `owned_buffer_slice` receive uses. A payload that is
not a `payload_impl` falls back to `append_bytes` (one copy).

`compose_e2e_protected_sequence` in
[`routing_manager_impl.cpp`](../implementation/routing/src/routing_manager_impl.cpp)
appends pieces into one `buffer_sequence`. Wire layout is unchanged
from scatter-gather:

```
[SOME/IP header] [e2e_header] [app_payload] [e2e_footer]
```

`protect_result::app_payload` is a non-owning span into the pinned
payload. `protect_result::owned_app_payload` exists for Profile 01 only.
`send_completion_state` is a latch on the sequence, fired from the
endpoint `send_cbk`.

The internal data-plane entry points are `send(message)`,
`send(message_buffer_ptr_t)`, and `send(buffer_sequence)`. There is no
raw `const byte_t*` send into routing. A caller that only has a
temporary pointer allocates a `shared_ptr<vector>` at the edge.
`send_via_sd` takes an owned frame.

Local UDS/TCP `send_local` scatters `[IPC meta | SOME/IP header | payload]`.
The on-wire bytes, including the `0x67…` framing tags, are unchanged.
A routing client skips the full-frame serializer for non-SD messages and
pins `shared_ptr<payload>` the same way. The stub then slices that IPC
frame into the remote send sequence. E2E still allocates a writable
16-byte SOME/IP header so the length field can be patched.

## 5. Data path

```
App send / notify
  → pin, or one snapshot if the payload is still shared and no completion was passed
  → routing_manager_impl::send(message)
       non-SD: buffer_sequence from message + payload (no full-frame serialize)
       SD:     serializer into one owned buffer
  → protect (span into the pin; P01 packs once)
  → train.append_sequence
  → async_write / async_send (buffers())
  → send_cbk fires the completion latch
```

Local subscribers take `send_local` on the same sequence shape. A proxy
application's completion covers only the write to the routing manager.

## 6. Compatibility notes

- **Wire format:** unchanged, including local IPC tags.
- **Public API:** `send` / `notify` / `notify_one` gain an optional
  trailing `send_completion_handler_t`. The default is `nullptr`, which
  preserves snapshot-if-shared.
- **E2E contract:** hole-free application data, as in
  [e2e-scatter-gather-send.md](e2e-scatter-gather-send.md). Local send
  still skips E2E protect.
- **Internal:** `send(const byte_t*, size)` is gone. In-tree callers
  pass an owned buffer or a `buffer_sequence`.

## 7. How to verify

```bash
bazel test //test/unit_tests/endpoint_tests:endpoint_tests
bazel test //test/unit_tests/e2e_tests:e2e_tests
```

`endpoint_tests` covers `snapshot_payload_if_shared` and
`ensure_exclusive_message_payload` (pin when exclusive, copy when the
caller still holds the pointer). `e2e_tests` covers protect pieces and
the pinned check/strip path.

## 8. Follow-ups

The stub `send_command` path still copies a message when it only needs
to parse SEND metadata. SOME/IP-TP still flattens a multi-buffer
sequence before splitting; that sits with the scatter-gather follow-up,
not with this pin.

## Related

- [e2e-scatter-gather-send.md](e2e-scatter-gather-send.md) — `buffer_sequence`, protect/check, socket `writev`
- [receive-copy-elision.md](receive-copy-elision.md) — the matching pin on ingress
