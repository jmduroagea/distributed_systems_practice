# Distributed Key-Value Storage System

A distributed tuple storage service built for the Distributed Systems course at Universidad Carlos III de Madrid. Implements a `<key, value1, value2, value3>` store across multiple exercises: one using POSIX message queues and another using TCP sockets.

## Overview

The system follows a client-server architecture where:

- **`libclaves.so`** — the core storage engine, backed by a concurrent hash table
- **`libproxyclaves.so`** — client-side library that transparently forwards API calls to the server over the network
- **`servidor`** — concurrent server handling requests from multiple clients simultaneously

The client application (`app-cliente.c`) calls the same API regardless of whether it's talking to a local library or a remote server — the proxy is a drop-in replacement.

## Architecture

```
app-cliente.c
     │
     └── libproxyclaves.so (proxy-sock.c / proxy-mq.c)
               │
           TCP / MQ
               │
         servidor-sock.c / servidor-mq.c
               │
          libclaves.so (claves.c)
               │
          hash-table.c
```

## Storage Engine

The hash table (`hash-table.c`) uses **Hopscotch Hashing** with a neighborhood size of 64 slots, keeping lookups O(1) and cache-friendly. Key design decisions:

- **Skinny/Fat layout** — control metadata stays dense in cache; heavy tuple data lives on the heap
- **Lock striping** — 1024 logical sectors with seqlocks aligned to cache lines to eliminate false sharing
- **Optimistic reads** — readers validate sequence numbers instead of acquiring locks, enabling high read concurrency
- **Hazard pointers** — safe memory reclamation prevents use-after-free under concurrent deletes
- **Parallel resize** — work-stealing across a thread pool minimizes stop-the-world pauses during rehashing
- **Overflow stash** — 1024-slot fallback for the rare case of a full neighborhood

## Exercise 1 — POSIX Message Queues

Communication between proxy and server uses POSIX MQ with a typed protocol defined in `mensajes.h`:

- Fixed-size request structs with a union payload (read vs. write operations)
- Two response types: `RespSimple` (int result) and `RespGet` (full tuple)
- Per-client reply queues identified by name in the request header
- Server spawns one worker thread per available CPU core (default 4)

**Stress test results:**

| Clients | Ops/client | Total ops     | Time (dist) | Time (local) |
|---------|------------|---------------|-------------|--------------|
| 1       | 100        | 500           | 0.041s      | 0.002s       |
| 50      | 10,000     | 2,500,000     | 7.202s      | 0.465s       |
| 255     | 100        | 127,500       | 0.587s      | 0.210s       |
| 255     | 10,000     | 127,500,000   | 463.144s    | 25.006s      |
| 256+    | —          | —             | Error*      | —            |

*Kernel limit of 256 POSIX queues (255 clients + 1 server queue).

## Exercise 2 — TCP Sockets

Same API, same storage engine, different transport. The proxy and server communicate over TCP using a **language-independent binary protocol** (TLV — Type Length Value):

```
| opcode (1B) | len_field (4B, big-endian) | field_data | ...
```

Integers are always sent in network byte order (`htonl`/`ntohl`). No C structs are sent over the wire.

Server IP and port are configured via environment variables:

```bash
export IP_TUPLAS=localhost
export PORT_TUPLAS=8080
```

### Protocol

#### destroy — `0x01`
```
Request:  | opcode (1B) |
Response: | result (1B) |
```

#### set_value — `0x02`
```
Request:  | opcode (1B) | len_key (4B) | key | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
Response: | result (1B) |
```

#### get_value — `0x03`
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
          (if result != 0, nothing follows)
```

#### modify_value — `0x04`
```
Request:  | opcode (1B) | len_key (4B) | key | len_v1 (4B) | v1 | N_v2 (4B) | v2 (N_v2 * 4B) | v3_x (4B) | v3_y (4B) | v3_z (4B) |
Response: | result (1B) |
```

#### delete_key — `0x05`
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) |
```

#### exist — `0x06`
```
Request:  | opcode (1B) | len_key (4B) | key |
Response: | result (1B) |   1 = exists, 0 = not found, -1 = error
```

Result codes across all operations: `0` OK, `-1` service error, `-2` communication error.

## Build

```bash
make              # builds everything
make clean        # removes binaries and objects
```

Produces:
- `libclaves.so` — storage library
- `libproxyclaves.so` — proxy library
- `servidor_mq` / `servidor` — server executables
- `cliente-local` / `cliente-dist` — client executables
- `cliente_sock`    — socket test client

## Running

**Exercise 1 (message queues):**
```bash
./servidor_mq &
./cliente-dist
```

**Exercise 2 (TCP sockets):**
```bash
./servidor 8080 &
IP_TUPLAS=localhost PORT_TUPLAS=8080 ./cliente_sock
```

## Testing

The `stress_validator.sh` script spins up a server and N concurrent clients, each running M operations:

```bash
./stress_validator.sh --modo dist --clientes 50
./stress_validator.sh --modo local --clientes 1
./stress_validator.sh --modo sock --clientes 10
```

Options:
```
-- modo local|dist|sock transport layer (default: dist)
--clientes N            number of concurrent clients (default: 10)
--cliente ./exec        client executable
--servidor ./exec       server executable
--timeout N             seconds before killing (default: 30)
--limpiar               wipe /dev/mqueue before starting
--no-servidor           skip server startup (local mode)
--ip    address        (default: localhost)
--port  port           (default: 8080)
```

Operations per client are configured via `N_OPS` in `src/app-cliente.c`. Each iteration runs set → exist → get → modify → delete, so total ops = `5 × N_OPS × N_CLIENTS`.

## Project Structure

```
.
├── include/
│   ├── claves.h          # public API (unmodified)
│   ├── hash-table.h      # storage engine interface
│   └── mensajes.h        # MQ protocol definitions
├── src/
│   ├── app-cliente.c     # test client
│   ├── utils/
│   │   ├── claves.c      # API implementation
│   │   └── hash-table.c  # concurrent hash table
│   ├── proxy/
│   │   ├── proxy-mq.c    # MQ proxy
│   │   └── proxy-sock.c      # TCP proxy
│   ├── server/
│   │   ├── servidor-mq.c # MQ server
│   │   └── servidor-sock.c   # TCP server
├── xxhash/               # hash function
├── stress_validator.sh
└── Makefile
```

## Authors

- José María Duro Agea — [100522306@alumnos.uc3m.es](mailto:100522306@alumnos.uc3m.es)
- Mathias Aivasovsky Trotta — [100502359@alumnos.uc3m.es](mailto:100502359@alumnos.uc3m.es)