# Benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Release, or the numbers mean nothing
cmake --build build -j
./build/benchmarks/bench_codec                   # encode / parse / verify, ns per op
./build/benchmarks/bench_send                    # the send path taken apart
taskset -c 2 ./build/benchmarks/bench_receive    # a player's connection receiving
```

`bench_e2e_latency` measures echo round-trip against a deployment and needs the
`CROWDY_E2E_*` variables; it is not part of the offline set.

## bench_receive (0.95.0)

What it costs a player's `Connection` to receive: a manual-pump connection drains loopback
datagrams that were all sent before the clock starts, and the round times `pump()` (the socket
read and the HMAC verification) and `poll()` (the dispatch to a handler) apart. A datagram is a
plain `MESSAGE_BUNDLE` of 6 actor updates of 64 bytes, each with its own HMAC, or with `signed`
one `MESSAGE_BUNDLE_SIGNED` of the same 6. The file uses nothing newer than 0.93.0, so the same
source measures an older tree.

On the CKS builder (8 cores shared with other builds, so the fastest of 300 rounds of 1,000
datagrams is the number to compare), Release, pinned with `taskset -c 2`, each tree run in turn:

| Tree | Plain bundles, 5 runs | Signed bundles, 3 runs |
| --- | --- | --- |
| 0.93.0 | 464 to 483 ns a notification, 2.07 to 2.16 million a second | 259 to 263 ns, 3.81 to 3.87 million |
| 0.94.1 | 551 to 582 ns, 1.72 to 1.82 million | 294 to 299 ns, 3.34 to 3.40 million |
| 0.95.0 | 484 to 495 ns, 2.02 to 2.07 million | 271 to 274 ns, 3.65 to 3.70 million |

0.94.0 put two things on the path every notification takes: a clock read for each one that
verified (about 30 ns) and the handling of a fleet session's three frames inside the
per-message code (about 40 ns). 0.95.0 reads the clock once a datagram and reads the fleet
frames out of line. The two checks 0.95.0 adds for a player (signed, and for its app) cost
nothing that these runs can see.

## bench_chunk_store (0.82.0)

What a region load costs the game thread, against the Unreal team's measurements (C3, C6):
the Game API's source answered by an in-process fake, the parse on the store's I/O thread, and
`tick()` applying within `Options::adoptBudgetUs`. On the CKS builder, Release:

| Load | I/O thread | Longest tick | Held |
| --- | --- | --- | --- |
| 125 chunks x 256 edits, 64-byte states (5.5 MB JSON) | 26 ms read + parse | 1.1 ms (2 ms budget), 0.6 ms (500 us) | |
| Range 8, 4,913 chunks, 4,298 of them air | | 2.1 ms (2 ms budget) | 2.8 MB (20.1 MB as 4 KiB grids) |

Before 0.82.0 the first load ran inside one `ensureAround()` call on the game thread (57 ms for the
same 125 chunks in the team's own test), and every chunk kept a 4 KiB grid.

0.84.0 dates each entry as ck-api v2.69.0 does (`updatedAt` after the row's write), so every edit's
voxel keeps its time (16 bytes), and a tick checks its budget after every chunk rather than every
16th. Three runs on the same builder:

| Load | I/O thread | Longest tick | Held |
| --- | --- | --- | --- |
| 125 chunks x 256 edits, 64-byte states, dated (6.7 MB JSON) | 28-31 ms read + parse | 1.6-2.1 ms (2 ms budget), 0.56-0.78 ms (500 us) | 3.46 MB, 0.51 MB of it times |
| Range 8, 4,913 chunks, 4,298 of them air | | 2.1-2.6 ms (2 ms budget) | 3.0 MB |

## What bench_send is for

`bench_codec` reports encode+sign as a single number, which is enough to see a
regression and not enough to act on one. `bench_send` splits the per-datagram
cost into encode, MAC and socket write, and prices the alternatives for each
side by side, so an optimisation can be aimed at the part that costs something.

It also cross-checks every MAC variant against the one-shot result before
timing any of them. A faster MAC that disagrees with the one on the wire is not
a faster MAC.

## Results, 0.24.0

Recorded on the CKS builder: AWS `b3` class, OpenSSL 3.0.13, CPU SHA extensions
present, no KPTI/Meltdown mitigations. Absolute numbers move with hardware —
the ratios are the part that travels, and the ratios are what the change was
judged on. A machine without hardware SHA will be several times slower on every
MAC line, and one with syscall mitigations enabled will be slower on the writes.

### The change in 0.24.0: a pre-keyed MAC

| | one-shot (0.23.0) | pre-keyed (0.24.0) | |
|---|---|---|---|
| encode + sign, 88-byte payload | 1225 ns | **319 ns** | 3.84x |
| verify a notification | 1276 ns | **337 ns** | 3.79x |
| 200-entity frame, encode+sign+write | 2922 ns/entity | **1824 ns/entity** | 1.60x |
| 200-entity frame through `Connection::sendActorUpdate` | — | **1912 ns/entity** | |

Encoding itself is 4.2 ns. Before the change the MAC was essentially 100% of
`encode+sign`, because the one-shot `HMAC()` rebuilt a context and re-imported
the 64-byte key for every datagram while the key changes only on token refresh.

The last row is the number a game pays: the public path adds about 90 ns of
locking and counter bookkeeping over the raw encode-and-write, which is why the
send-side mutexes were left alone.

### Why the MAC is implemented the way it is

| variant | ns | |
|---|---|---|
| one-shot `HMAC()` | 1236 | what 0.23.0 did |
| pre-keyed `EVP_MAC` + `EVP_MAC_CTX_dup` per call | 574 | 2.15x |
| the same, fed in two parts instead of concatenated | 595 | no measurable difference |
| pre-keyed, duplicated once per thread then reset | 313 | 3.95x, and what ships |
| legacy `HMAC_CTX` reset | 260 | 4.76x, but deprecated in OpenSSL 3 |

Duplicating the keyed context on every call throws away half the benefit, so
each thread duplicates once and then resets. Feeding the prefix and token
separately rather than concatenating them saves no measurable time; it is done
anyway because it removes a 1296-byte stack buffer from a call a game makes
hundreds of times a frame.

### Why there is no sendmmsg

Batching writes was the other obvious lever and it was measured before being
built. It is not worth it:

| | ns per datagram |
|---|---|
| `send()` per datagram, loopback | 1446 |
| `sendmmsg` batch of 8 / 16 / 32 / 64 | 1390 / 1430 / 1445 / 1440 |
| `send()` per datagram, recipient discarded before delivery | 758 |
| `sendmmsg` batch of 64, same destination | 732 |

Between 1.00x and 1.04x. The second pair removes loopback delivery from the
measurement, so what is left is the syscall and the route lookup — and batching
still gains only about 3%, because the kernel does the same per-datagram work
either way and only the syscall boundary is amortised. That boundary is a small
part of the cost on this machine, which has no KPTI. It would be worth more on
a machine that does, so this is a decision to revisit with a measurement rather
than a permanent no; a public batch API was not worth adding for 3%.
