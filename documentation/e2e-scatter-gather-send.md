# E2E scatter-gather send in vsomeip

> **Status.** Implemented in this fork for stock AUTOSAR E2E
> (`libvsomeip3-e2e`). Written as a mainline candidate: the wire format is
> unchanged, the receive path stays contiguous, and apps that already sent
> hole-free payloads keep working. Apps that still pre-allocate E2E holes
> will double-size on the wire — which is a feature if you like oversized
> CRCs, and a bug if you do not.

## 1. Summary

E2E protection is supposed to be a stack concern. In practice, old vsomeip
asked applications to know header sizes, leave placeholder bytes in the
payload, and hope the plugin wrote into the right holes. That is the
middleware equivalent of asking the passenger to bring their own seatbelts.

The redesign has three goals that mainline benefits from equally:

1. **Apps send user data only.** No pre-sized E2E holes. Profile/config
   changes stop leaking into every client and service sample.
2. **The plugin owns the E2E header buffer.** `protect` returns
   owned pieces instead of mutating a caller buffer in place.
3. **Send stays scatter-gather to the socket.** Routing and endpoints
   pass a multi-buffer sequence into Asio (`async_write` /
   `async_send`) instead of concatenating everything into one
   `vector<byte_t>` first.

Item (3) matters: if you only invent a nicer protect API and then
`memcpy` the result back into a single train buffer, you have improved
the paperwork and kept the copy tax. The queue type and the socket type
had to change together.

Receive is intentionally still contiguous at the socket: UDP datagrams and
TCP/UDS stream windows already yield one byte range per message. What changed
is **ownership after that**: routing ingress takes an `owned_buffer_slice`
(pin or one copy-out), `check()` returns spans into that buffer, and strip /
deliver / proxy SEND build `payload_impl` views or a 16B header + app slice —
no full-frame re-materialize. Details:
[`receive-copy-elision.md`](receive-copy-elision.md).

## 2. What changed

| Layer                  | Before                                      | After                                                                                                 |
| ---------------------- | ------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| App payload            | Often pre-sized with E2E holes              | Hole-free user data                                                                                   |
| E2E plugin             | In-place `protect(e2e_buffer&)`             | Public `protect` → `protect_result`                                                                   |
| E2E check / strip      | `check` + `get_unprotected_payload`         | `check` → `check_result` spans; strip via views / `compose_e2e_stripped_sequence` (16B hdr + app pin) |
| Routing send           | Protect, maybe flatten, `send(byte*, size)` | Compose `buffer_sequence`, always `send(sequence)`                                                    |
| Endpoint queue / train | One contiguous `message_buffer_ptr_t`       | Owning `buffer_sequence`                                                                              |
| TCP / UDP sockets      | Single `const_buffer`                       | `vector<const_buffer>` (`buffers()`)                                                                  |

JSON E2E configuration is unchanged. Profiles, offsets, and
`e2e_enabled` keep working the same way — only the _application payload
contract_ changed.

## 3. Application contract (migration)

**New rule:** the SOME/IP payload you hand to vsomeip is unprotected
application data only. The stack inserts the E2E header (and updates the
SOME/IP length field) before the frame hits the wire.

**Breaking for old hole-prealloc apps:** if your app still pads the
payload with room for the E2E header, the plugin will add _another_
header on top. The CRC will look fine to someone who enjoys chaos; the
peer will not.

Receive delivery to the application is hole-free again after a successful
check + strip. Application handlers should not see the E2E header bytes.
The public `application` / `message` / `payload` APIs are unchanged: handlers
still get `shared_ptr<message>`; the payload may be a view into a pinned
receive buffer rather than a freshly allocated vector.

External E2E plugins should implement the same `protect` / `check` contract when they move onto this path. Stock `libvsomeip3-e2e` **refuses** standard AUTOSAR profiles for SOME/IP-SD (`service_id`, `0xFFFF`): routing may still call protect/check on `send_via_sd` /
SD `on_message`, but only a dedicated plugin should register that ID.

Misconfiguring P01/P04/P05/P07 on SD is rejected at `add_configuration`.

## 4. The type that carries it: `buffer_sequence`

Defined in
[`implementation/endpoints/include/buffer.hpp`](../implementation/endpoints/include/buffer.hpp).

It is an owning multi-buffer send unit:

- **`segments_`** — ordered owned vectors and/or pinned `shared_ptr<payload>`
  entries (lifetime across async completion)
- **`buffers_`** — Asio `const_buffer` views over those segments

Useful operations:

- `append` / `append_sequence` — build or batch without forced concat
- `buffers()` — what the socket write path consumes
- `read_byte` / `read_uint16_be` — peek SOME/IP fields without flattening
- `flatten()` — escape hatch for legacy pointer-arithmetic paths
  (SOME/IP-TP split, tracing). Prefer not to live there.

The nPDU `train` now holds a `buffer_sequence_ptr_t` and appends
passenger messages with `append_sequence` instead of copying bytes into
one blob.

## 5. Protect and check APIs

Public surface
([`e2e_provider.hpp`](../implementation/e2e_protection/include/e2e/profile/e2e_provider.hpp),
[`protector.hpp`](../implementation/e2e_protection/include/e2e/profile/profile_interface/protector.hpp),
[`checker.hpp`](../implementation/e2e_protection/include/e2e/profile/profile_interface/checker.hpp)):

```cpp
protect_result protect(buffer_view app_payload, instance_t instance);
check_result   check(buffer_view message, instance_t instance);
```

Provider `check` takes the **full SOME/IP message**. Profile checkers see
the protected area after the 16-byte SOME/IP header. Returned spans still
point into the caller's receive buffer.

[`protect_result`](../implementation/e2e_protection/include/e2e/profile/protect_result.hpp)
returns scatter pieces for send composition:

- **`e2e_header` / `e2e_footer`** — owned buffers (plugin allocates E2E meta)
- **`app_payload`** — non-owning span into the caller's payload (routing pins
  `shared_ptr<payload>` in `buffer_sequence` for async lifetime)
- **`owned_app_payload`** — Profile 01 only; packed in-band CRC/counter/nibble

See [`send-copy-elision.md`](send-copy-elision.md) for the zero-copy send path.

[`check_result`](../implementation/e2e_protection/include/e2e/profile/protect_result.hpp)
mirrors those three sections as **non-owning spans** plus `status`:

- **`status`** — `E2E_OK` / `E2E_WRONG_CRC` / `E2E_ERROR`
- **`e2e_header` / `app_payload` / `e2e_footer`** — views into the receive
  buffer. Sections are filled even on CRC failure so strip can still run.

Profile 1 is the one mismatch: `protect` packs CRC/counter/nibble into
`app_payload` (empty header). `check` still reports those leading in-band
fields as `e2e_header` so routing can drop them without a second lookup.

Routing composes the on-wire sequence in
`compose_e2e_protected_sequence` inside
[`routing_manager_impl.cpp`](../implementation/routing/src/routing_manager_impl.cpp)
by **appending** pieces into one `buffer_sequence` — no concat:

```
[SOME/IP header with fixed length] [e2e_header] [app_payload] [e2e_footer]
```

Empty pieces are skipped. Each remaining segment stays in `segments_`;
`buffers()` is the matching `vector<const_buffer>` (an Asio
`ConstBufferSequence`). The endpoint queues that object (the nPDU train
`append_sequence`s passengers the same way). On the wire the socket
calls `async_write` / `async_send` with `sequence->buffers()`, which is
`writev`: several iovec entries, one syscall, still one TCP segment /
UDP datagram.

A one-buffer sequence is the same path with iovec length 1. That is
what non-E2E remote sends build at the call site, and what
`send(byte*, size)` (non-virtual; SD-only host, `send_local`, routing
stub) wraps into. Local TCP/UDS prepends/appends the `0x67…` framing
tags as extra `const_buffer`s around the same sequence.

`flatten()` is the escape hatch when pointer arithmetic still wants one
blob (SOME/IP-TP split, tracing). It is not the send path.

## 6. Data path

**Send**

```
App (hole-free payload)
  → serialize SOME/IP
  → protect (plugin owns E2E header / footer)
  → buffer_sequence
  → train.append_sequence (batch without concat)
  → async_write / async_send (buffers())
```

**Receive** (see [`receive-copy-elision.md`](receive-copy-elision.md))

```
Endpoint recv
  → owned_buffer_slice (UDP: pin datagram; TCP/UDS: copy-out complete frame/command)
  → routing_host::on_message(slice)
  → e2e check → check_result spans into the pin
  → host deliver: payload_impl view of app_payload
    or local forward: compose_e2e_stripped_sequence (owned 16B hdr + app slice)
  → proxy app (if any): SEND header-only parse + build_message_from_buffer pin
```

There is still no scatter-gather **read** from the socket. Contiguous bytes
arrive; zero-copy is about not copying them again. `check()` verifies
CRC/counter in place and returns spans for the three sections. Strip no
longer concatenates a hole-free blob for the common path: routing keeps the
SOME/IP header (length patched into a small owned 16B buffer when
forwarding) and pins `app_payload` when it lies in the frame buffer.
Stock AUTOSAR footers are empty. P01 protect packs CRC/counter/nibble into
`app_payload`; check still exposes those leading fields as `e2e_header` so
the same strip path works.

SOME/IP-SD keeps its own `on_message(const byte_t*, …)` API; RM extracts
bytes from the frame pin when calling into SD. Changing SD to
`owned_buffer_slice` is deferred until SD itself needs lifetime beyond the
call.

## 7. Compatibility notes

- **Wire format:** unchanged for standard profiles at `offset_ == 0`.
  Peers that already spoke AUTOSAR E2E keep interoperating.
- **Config:** unchanged (`e2e` block in JSON).
- **Local IPC quirk:** the local send path historically skips E2E
  protect; this redesign does not invent a new story there.
- **Open tech debt:** SOME/IP-TP still flattens multi-buffer sequences
  before splitting. Scatter through TP is a follow-up, not a blocker for
  the E2E framing goal.

## 8. How to verify

```bash
# Unit / in-process
bazel test //test/unit_tests/e2e_tests:e2e_tests
bazel test //test/unit_tests/endpoint_tests:endpoint_tests
bazel test //test/network_tests/fake_socket_tests:fake_socket_tests

# Two-node Docker (host Bazel build + Compose)
./test/network_tests/docker_tests/run-test.sh e2e_crc
./test/network_tests/docker_tests/run-test.sh e2e_p04
./test/network_tests/docker_tests/run-test.sh e2e_p07
```

CI: [`.github/workflows/e2e_docker_tests.yml`](../.github/workflows/e2e_docker_tests.yml)
builds `//test/network_tests/docker_tests:e2e_pkgs` once (per-suite
`pkg_tar`), then runs those three suites in a matrix with `--package`.

See also [`test/network_tests/docker_tests/README.md`](../test/network_tests/docker_tests/README.md).

## 9. Follow-ups

Optional follow-ups: train-batching stress without E2E, SecOC plugins on the same
contract, TP without `flatten()`, SD `on_message` on `owned_buffer_slice` if SD
needs pin lifetime beyond the call.
