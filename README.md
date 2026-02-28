# flip-linux

A user-space implementation of the FLIP (Fast Local Internet Protocol) network protocol for Linux hosts, enabling communication with daemons running on Amoeba systems over FLIP.

## Overview

FLIP is the native network protocol of the Amoeba distributed operating system. This project implements the FLIP protocol stack entirely in user space on Linux, using TAP (layer 2) virtual network interfaces to send and receive raw Ethernet frames carrying FLIP packets (Ethertype `0x8146`).

This allows programs on a Linux host to participate in an Amoeba FLIP network — locating remote services, exchanging data with Amoeba daemons, and routing FLIP traffic across network interfaces.

## Architecture

```
┌─────────────────────────────────┐
│         flip_linux (main)       │  Event loop (poll-based)
├──────────┬──────────────────────┤
│  Router  │  Protocol (RPC/AIDL) │  FLIP routing table & packet handling
├──────────┴──────────────────────┤
│        Network Driver (TAP)     │  TAP interface abstraction
└─────────────────────────────────┘
```

| Component | Description |
|---|---|
| **flip_linux.cpp** | Main entry point. Opens one or more TAP devices, runs the `poll()` event loop, dispatches incoming Ethernet frames, and fires a 30-second timer for routing table maintenance. |
| **flip/router.cpp** | FLIP routing table and packet routing logic. Learns routes from incoming packets, handles LOCATE/HEREIS/UNIDATA/NOTHERE/UNTRUSTED message types, and ages stale routes. |
| **flip/protocol.cpp** | Higher-level FLIP protocol handling (RPC layer — work in progress). |
| **driver/tap.cpp** | Linux TAP network driver. Opens `/dev/net/tun` in TAP mode (layer 2, no PI header), reads/writes raw Ethernet frames. |
| **include/flip_proto.hpp** | FLIP protocol definitions — packet header, message types (LOCATE, HEREIS, UNIDATA, MULTIDATA, NOTHERE, UNTRUSTED), flags, and the fragment control header. |
| **include/flip_router.hpp** | Routing table entry and router class declarations. |
| **include/netdrv.hpp** | Abstract `NetDrv` base class for network drivers, plus the `flip_networks` registry that assigns network IDs. |
| **include/tap.hpp** | TAP driver class declaration. |
| **rpc/** | Placeholder for Amoeba RPC protocol implementation. |

## FLIP Packet Types

| Type | Value | Purpose |
|---|---|---|
| LOCATE | 1 | Locate a FLIP address on the network |
| HEREIS | 2 | Response to LOCATE — announces presence |
| UNIDATA | 3 | Unicast data delivery |
| MULTIDATA | 4 | Multicast data delivery |
| NOTHERE | 5 | Destination is not (or no longer) reachable |
| UNTRUSTED | 6 | Packet traversed an untrusted network |

## Building

Requires a C++23-capable compiler (GCC 13+ or Clang 17+).

```sh
make
```

This produces the `flip_linux` binary.

To clean build artifacts:

```sh
make clean
```

## Usage

Create one or more TAP interfaces and pass their names as arguments:

```sh
# Create a TAP device (requires root or CAP_NET_ADMIN)
sudo ip tuntap add dev tap0 mode tap
sudo ip link set tap0 up

# Run flip_linux (may require root for TAP access)
sudo ./flip_linux tap0
```

Multiple TAP interfaces can be specified to bridge FLIP traffic across networks:

```sh
sudo ./flip_linux tap0 tap1
```

The daemon will listen on all specified TAP interfaces, route incoming FLIP packets, maintain a routing table, and age out stale routes every 30 seconds.

## How It Works

1. **Startup** — Opens each TAP device specified on the command line and registers it as a FLIP network interface.
2. **Event loop** — Uses `poll()` to wait for incoming packets on any TAP interface or a periodic 30-second timer.
3. **Packet reception** — Incoming Ethernet frames are filtered by the FLIP Ethertype (`0x8146`). Valid FLIP packets are passed to the router after stripping the Ethernet and fragment control headers.
4. **Routing** — The router learns source routes from incoming packets and makes forwarding decisions based on the FLIP message type:
   - **LOCATE** — If the destination is local, responds with HEREIS; otherwise forwards.
   - **HEREIS** — Updates the routing table.
   - **UNIDATA** — Delivers locally or forwards to the next hop.
   - **NOTHERE** — Removes the route for the unreachable destination.
5. **Route aging** — Every 30 seconds, routing entries are aged and stale routes are pruned.

## Status

This project is a work in progress. Currently implemented:

- [x] TAP network driver
- [x] FLIP packet parsing (Ethertype, fragment control, FLIP header)
- [x] Basic routing table with source-route learning
- [x] LOCATE / HEREIS / NOTHERE handling (partial)
- [x] Route aging timer
- [ ] UNIDATA forwarding and local delivery
- [ ] MULTIDATA support
- [ ] RPC layer for Amoeba service communication
- [ ] HEREIS response generation
- [ ] Full route aging and pruning logic
- [ ] Trusted / untrusted network handling

## License

See repository for license details.
