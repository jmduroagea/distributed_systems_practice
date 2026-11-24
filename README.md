# Distributed Key-Value Store in C

A concurrent `<key, value1, value2, value3>` store built for the Distributed Systems course at Universidad Carlos III de Madrid. The same C API (`claves.h`) runs locally or over three transports (POSIX message queues, TCP sockets and ONC RPC), on top of a hopscotch hash table with optimistic, lock-free reads.

## Highlights

- **50.7 million operations per second** with 16 threads on a laptop CPU, scaling **5.9×** from 1 to 16 threads.
- **About 1,350× faster** than a mutex-protected linked list under the same workload, which gets *slower* as threads are added.
- **One API, four deployments**: link against `libclaves.so` for local use, or swap in a proxy library to reach a remote server. The client code does not change.
- **Measured, not assumed**: an in-process benchmark (`make bench`) found two scalability bottlenecks in the engine; both are fixed and documented below.

## Project history

The version submitted for grading replaced this storage engine with a simple mutex-protected linked list to fit the course requirements. This repository restores the original design and includes the performance work done afterwards. The linked list is kept as a benchmark baseline (`make ENGINE=list`).

## Architecture

```
                         app-cliente.c   (same code for every transport)
                               │
      ┌────────────────┬───────┴────────┬──────────────────┐
      │                │                │                  │
 libclaves.so   libproxyclaves.so  libproxyclaves-   libproxyclaves-
   (local)        (POSIX MQ)        sock.so (TCP)     rpc.so (ONC RPC)
      │                │                │                  │
      │          servidor_mq        servidor         clavesRPC_server
      │                │                │                  │
      └────────────────┴───────┬────────┴──────────────────┘
                               │
                         libclaves.so  (claves.c)
                               │
              hash-table.c (hopscotch)  |  linked-list.c (baseline)
```

Every server links the same `libclaves.so`, so all transports share the same storage engine and the same concurrency guarantees.

## Storage engine

`src/utils/hash-table.c` implements a concurrent **hopscotch hash table**.

### Design

- **Hopscotch hashing** with a 64-slot neighborhood: every key lives within 64 slots of its ideal position, so a lookup touches a small, contiguous, cache-friendly range. A 64-bit `hop_info` bitmap per bucket says which slots of the neighborhood belong to it.
- **xxHash3** as the hash function. The low 32 bits are cached in each slot, so most non-matching slots are rejected without dereferencing the tuple or comparing strings.
- **Skinny/fat layout**: the slot array holds only metadata and a pointer; tuples live on the heap. Scans stay dense in cache.
- **Optimistic reads with seqlocks**: the table is split into 1,024 segments, each with a sequence lock on its own cache line. Readers never lock a segment: they read, then check that the sequence number did not change, and retry if it did. Writers lock only the one or two segments covering the neighborhood they modify.
- **Hazard pointers** for safe memory reclamation: a reader publishes the pointer it is about to use, and a writer that replaces or deletes a tuple only frees it once no reader has it published. Retired pointers are freed in batches, comparing them against a single snapshot of the active hazard pointers.
- **Overflow stash**: if a whole neighborhood is full, the tuple goes to a 1,024-slot stash instead of failing the insert.
- **Parallel stop-the-world resize**: when the load factor passes 0.9 (or the stash is half full), the table doubles and the migration is split into chunks shared by a pool of worker threads (one per core, up to 32). The old table is freed by a background thread.
- **Non-blocking `destroy()`**: the old table is swapped out instantly and its tuples are released by a background thread through the hazard pointer system.

### Scalability fixes (found with the benchmark)

The first benchmark run showed throughput **dropping** when going from 1 to 2 threads, and flat at about 15M ops/s from 4 threads on, even with 16 hardware threads available. Three causes were found and fixed:

1. **Global reader-writer lock.** Every operation took a `pthread_rwlock_t` in read mode to stay out of a concurrent resize. Even in read mode, each acquire and release writes the lock's shared counter, so that single cache line bounced between all cores on every operation. It was replaced with a **distributed reader indicator**: each thread marks "I am reading" on its *own* cache line, and only a resize (rare) walks all the marks to wait for readers. A Dekker-style ordering (mark, then check the writer flag, with `seq_cst` on both sides) guarantees a reader and a resize never run together.
2. **Stash checked on every insert.** The stash is almost always empty, but every insert, and every lookup of a missing key, took the stash mutex and scanned its 1,024 slots, serializing all writers. Now `stash_count` is incremented *before* a tuple is published in the stash and decremented *after* it is removed, so `stash_count == 0` safely means nothing is there and the mutex and scan are skipped. An insert + delete pair went from 520 ns to 205 ns, and a lookup of a missing key from 380 ns to 55 ns.
3. **False sharing on `total_items`.** The item counter, written by every insert and delete, shared a cache line with `entries` and `capacity`, which every operation reads. It now has its own cache line.

| Threads | Original (rwlock) | + distributed reader lock | + stash fast path and no false sharing |
|---:|---:|---:|---:|
| 1  | 6.97M | 7.49M  | **8.54M**  |
| 2  | 10.37M | 10.48M | **15.93M** |
| 4  | 13.86M | 14.79M | **27.95M** |
| 8  | 14.12M | 14.40M | **40.20M** |
| 16 | n/a    | 14.98M | **50.74M** |

*Operations per second, same machine and workload as below.*

## Performance

`make bench` builds `src/bench/bench-engine.c` against each engine and runs the same workload on both. The benchmark calls the API in-process, with no network in between, so it measures the engine alone.

**Workload**: 10,000 pre-loaded keys; each thread runs for 3 s doing 45% `get_value`, 45% `exist`, 5% `modify_value` and 5% `set_value` + `delete_key` on a thread-private key (so the key count stays constant). Keys are pre-formatted, each thread has its own RNG, and per-thread counters sit on separate cache lines, so the benchmark adds no contention of its own. Every result is checked: the benchmark doubles as a concurrency test and fails if any operation returns an unexpected result.

**Machine**: Intel Core i7-13620H (6 performance cores with Hyper-Threading + 4 efficiency cores, 16 threads), WSL2 on Windows.

| Threads | Hopscotch (ops/s) | Linked list (ops/s) | Speedup |
|---:|---:|---:|---:|
| 1  | 8,540,553  | 66,922 | 128× |
| 2  | 15,925,253 | 39,164 | 407× |
| 4  | 27,952,861 | 38,584 | 724× |
| 8  | 40,204,069 | 36,992 | 1,087× |
| 16 | 50,737,857 | 37,522 | 1,352× |

All runs: 0 errors. Hopscotch reaches 1.9× with 2 threads, 3.3× with 4 and 4.7× with 8; beyond the 6 performance cores, threads share physical cores (Hyper-Threading) or run on the slower efficiency cores, so each extra thread adds less. The linked list loses 41% of its throughput as soon as a second thread competes for its single mutex, and stays flat after that.

To reproduce:

```bash
make bench                                  # 1, 2, 4, 8 threads, both engines
make bench BENCH_ARGS="-t 1,2,4,8,16"       # custom thread counts
make bench BENCH_ARGS="-k 50000 -d 5"       # 50k keys, 5 s per measurement
```

Results are also written to `bench_results.csv`.

## Transports

All proxies implement `claves.h` exactly, so a client switches transport by linking a different library.

### Exercise 1: POSIX message queues

- One server queue (`/claves_server_req`) and one reply queue per client process (`/cq_<pid>`).
- Typed messages defined in `mensajes.h`: a request header plus a union payload (read vs. write operations). Each request is sent with only the bytes its operation needs.
- Two reply types: `RespSimple` (result code) and `RespGet` (the full tuple).
- The server runs one worker thread per CPU core.
- Queues are created with `mq_maxmsg = 1` to minimise kernel memory per client.

### Exercise 2: TCP sockets

Proxy and server talk over TCP using a **language-independent binary protocol**. No C structs are sent over the wire, and every integer travels in network byte order (`htonl`/`ntohl`).

```
| opcode (1B) | len_field (4B, big-endian) | field_data | ...
```

The server accepts connections into a bounded work queue served by a pool of worker threads (one per core). Nagle's algorithm is disabled (`TCP_NODELAY`) because every request is a small message that waits for its reply.

Server address and port are read from environment variables:

```bash
export IP_TUPLAS=localhost
export PORT_TUPLAS=8080
```

#### Protocol

**destroy (`0x01`)**
```
Request:  | opcode (1B) |
Response: | result (1B) |
```

**set_value (`0x02`)**
```
Request:  | opcode (1B) | len_key (4B) | key | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
Response: | result (1B) |
```

**get_value (`0x03`)**
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
          (if result != 0, nothing follows)
```

**modify_value (`0x04`)**
```
Request:  | opcode (1B) | len_key (4B) | key | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
Response: | result (1B) |
```

**delete_key (`0x05`)**
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) |
```

**exist (`0x06`)**
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) |   1 = exists, 0 = not found, -1 = error
```

Floats are sent as their IEEE 754 bit pattern in network byte order.

### Exercise 3: ONC RPC

The interface is defined in `src/rpc/clavesRPC.x` (program `0x20000001`, version 1, six procedures) and compiled with `rpcgen -NM`. The generated stubs are not versioned: `make` regenerates them whenever the `.x` file changes.

- **Single source of truth for limits**: the maximum string and vector sizes are `const` declarations in the `.x` file. rpcgen turns them into `#define`s, so the XDR types and the proxy's input validation can never disagree.
- **One RPC client per thread**: a `CLIENT` handle is not thread-safe, so each thread lazily opens its own (thread-local storage). A `pthread_key_t` destructor closes it when the thread exits, so short-lived threads do not leak connections.
- **Validation before sending**: lengths and `N_value2` are checked against the wire limits before any network traffic.
- **Recovery**: after a transport error the handle is discarded and the next call on that thread reconnects.

The server needs `rpcbind` running. Clients find it through the `IP_TUPLAS` variable.

### Result codes

All operations return `0` on success and `-1` on a service error (missing key, duplicate key, out-of-range `N_value2`). `exist` returns `1` / `0`. On a communication error, the MQ and socket proxies return `-2`; the RPC proxy returns `-1`, as `claves.h` specifies.

## Building and running

**Requirements**: Linux (or WSL2), `gcc`, `make`, `rpcgen`, `libtirpc-dev` and `rpcbind`.

```bash
sudo apt install build-essential libtirpc-dev rpcbind
make                # optimised build (-O2, LTO)
make debug          # -g -O0
make clean
```

| Artifact | Description |
|---|---|
| `libclaves.so` | Storage engine + API |
| `libproxyclaves.so` | MQ proxy (Exercise 1) |
| `libproxyclaves-sock.so` | Socket proxy (Exercise 2) |
| `libproxyclaves-rpc.so` | RPC proxy (Exercise 3) |
| `servidor_mq` / `servidor` / `clavesRPC_server` | Servers for each transport |
| `cliente_local` / `cliente_dist` / `cliente_sock` / `cliente_rpc` | The same test client linked against each library |
| `bench-engine` | Engine benchmark |

Build options:

| Option | Effect |
|---|---|
| `make ENGINE=list` | Build `libclaves.so` with the linked-list baseline instead of hopscotch (default `ENGINE=hopscotch`). Switching engines rebuilds automatically. |
| `make clean && make DEBUG_LOGS=1` | Enable the servers' debug logs. |

**Running each transport:**

```bash
# Exercise 1: message queues
./servidor_mq &
./cliente_dist

# Exercise 2: TCP sockets
./servidor 8080 &
IP_TUPLAS=localhost PORT_TUPLAS=8080 ./cliente_sock

# Exercise 3: ONC RPC
sudo service rpcbind start
./clavesRPC_server &
IP_TUPLAS=127.0.0.1 ./cliente_rpc
```

## Testing

`stress_validator.sh` starts the server for the chosen transport, launches N clients in parallel and checks every result. Each client runs `set → exist → get → modify → delete` on its own keys (`N_OPS` in `src/app-cliente.c`), so total operations = `5 × N_OPS × clients`.

```bash
./stress_validator.sh --modo local --no-servidor --cliente ./cliente_local
./stress_validator.sh --modo dist  --clientes 50 --limpiar
./stress_validator.sh --modo sock  --clientes 50
./stress_validator.sh --modo rpc   --clientes 50 --ip 127.0.0.1
```

Shortcuts: `make stress-local`, `make stress-dist`, `make stress-sock`, `make stress-rpc`.

Options:
```
--modo local|dist|sock|rpc   transport (default: dist)
--clientes N                 concurrent clients (default: 10)
--cliente ./exec             client executable
--servidor ./exec            server executable
--timeout N                  seconds before killing (default: 30)
--limpiar                    wipe /dev/mqueue before starting
--no-servidor                skip server startup (local mode)
--ip address                 (default: localhost)
--port port                  (default: 8080)
```

All four transports pass with 50 concurrent clients, and the build has zero compiler warnings. The engine changes were also checked with a dedicated test that runs readers verifying values while other threads force several concurrent resizes, with the stash forced into use, and under ThreadSanitizer.

## Known limitations

- **Duplicate key race involving the stash.** If a key sits in the stash, a concurrent re-insert of the same key can land in its neighborhood when another thread frees a slot there at the same moment, leaving the key twice. It requires a completely full 64-slot neighborhood plus that exact interleaving, so it is extremely unlikely, and the stash fast path was designed not to widen it. A complete fix needs insert and the stash to be coordinated differently.
- **At most 256 threads** using the engine at the same time, the size of the hazard pointer table (the engine's background cleanup threads also take a slot while they run).
- **Message queues**: Linux limits the number of POSIX queues per user (256 by default, `/proc/sys/fs/mqueue/queues_max`). With one reply queue per client plus the server queue, the MQ transport supports up to 255 concurrent client processes unless that limit is raised.

## Project structure

```
.
├── include/
│   ├── claves.h              # Public API (given by the course, unmodified)
│   ├── hash-table.h          # Internal engine interface
│   ├── log.h                 # Debug logging macros for the servers
│   └── mensajes.h            # Message queue protocol
├── src/
│   ├── app-cliente.c         # Test client (same code for every transport)
│   ├── proxy-rpc.c           # RPC proxy
│   ├── bench/
│   │   └── bench-engine.c    # Engine benchmark
│   ├── proxy/
│   │   ├── proxy-mq.c        # Message queue proxy
│   │   └── proxy-sock.c      # TCP proxy
│   ├── rpc/
│   │   ├── clavesRPC.x       # RPC interface (stubs generated by make)
│   │   └── clavesRPC_server.c
│   ├── server/
│   │   ├── servidor-mq.c     # Message queue server
│   │   └── servidor-sock.c   # TCP server
│   └── utils/
│       ├── claves.c          # API implementation over the engine
│       ├── hash-table.c      # Hopscotch engine
│       └── linked-list.c     # Baseline engine (make ENGINE=list)
├── xxhash/                   # xxHash3 (BSD-2-Clause)
├── stress_validator.sh
├── Makefile
└── LICENSE
```

## Authors

- José María Duro Agea ([@jmduroagea](https://github.com/jmduroagea))
- Mathias Aivasovsky Trotta ([@mathiasaiva](https://github.com/mathiasaiva))

## License

MIT, see [LICENSE](LICENSE). xxHash is distributed under its own BSD-2-Clause license.
