# Network-condition matrix

Two real ICE agents, in separate Linux network namespaces, with real NATs
between them.

`peer_connection_test` runs both agents in one process on one host. It proves
the state machine, the trickle ordering and the role assignment, and it proves
nothing about NAT traversal: both agents see the same interfaces and settle on
a host pair every time. This lab exists to measure the part that test cannot
reach — **which candidate type actually won**.

Every case asserts a candidate type or an address family. A case that only
checked "did it connect" would be the same test with more scaffolding.

## Running it

```bash
docker build -t shadps4-netns-lab src/shadnet/tests/netns
docker run --rm --privileged -v "$PWD:/repo" shadps4-netns-lab \
    bash /repo/src/shadnet/tests/netns/run-matrix.sh
```

`--privileged` is required: the cases create namespaces, veth pairs and NAT
rules. Useful flags:

| Flag | Effect |
|---|---|
| `--case NAME` | run one case |
| `--list` | print the case names |
| `--keep` | leave the namespaces up afterwards |

Environment: `LAB_EXTRA_ARGS="--juice-log debug"` turns on libjuice's own log,
which is the only way to see why a case nominated no pair. `FORCE_BUILD=1`
rebuilds the agent.

Logs land in `/tmp/netns-run/<case>/`.

## Topology

```text
  hostA1 --- rtA ===\                        /=== rtB --- hostB1
  10.1.0.2  10.1.0.1  \                    /  10.2.0.1  10.2.0.2
                  100.64.0.2 --[ inet ]-- 100.64.0.3
                                  |
                                 srv 100.64.0.10
                          (broker, STUN, TURN)
```

`100.64.0.0/10` is the carrier-grade NAT range, standing in for the public
internet. A rule that ever escapes the lab then names a range that is not
somebody's home network.

Each site's NAT mode is what makes a case different:

- **none** — plain routing; the peer is directly addressable.
- **cone** — `MASQUERADE`. The mapping is reused across destinations, so the
  address STUN reports is the address the peer can reach.
- **symmetric** — `MASQUERADE --random`. A fresh source port per destination,
  so the STUN-learned address is useless to the peer and only a relay works.

Every NAT router also drops unsolicited inbound WAN traffic. That is not
decoration: without it, a peer's punch arriving first creates conntrack state
occupying the external port our own STUN mapping used, our punch then fails to
preserve that port, and an endpoint-independent NAT starts behaving like a
symmetric one. Whether that happened came down to which packet arrived first,
so the matrix passed or failed by timing.

## Cases

| Case | Asserts |
|---|---|
| `same-lan` | host pair, IPv4 — the baseline |
| `two-nats` | srflx both sides: hole punching works through cone NAT |
| `lan-and-external` | one open peer, one NATed; no relay needed |
| `symmetric-turn` | endpoint-dependent NAT, direct path blocked → relayed |
| `overlapping-subnets` | both LANs on `10.1.0.0/24` → srflx, not a wrong-host connection |
| `ipv6-only` | IPv4 stripped entirely → host pair on IPv6 |
| `dual-stack-v4-broken` | IPv4 peer path blocked → finishes over IPv6 |

Two notes on the assertions, both learned by getting them wrong first:

- `symmetric-turn` asserts that **either** side of the nominated pair is a
  relay, not a particular side. Sending to a peer's relay allocation makes your
  own side peer-reflexive, so a relayed connection shows `relay` on one end and
  `prflx` on the other — and which end is which changes between runs.
- `ipv6-only` removes the IPv4 addresses rather than leaving them unrouted. An
  assertion that the path was IPv6 proves nothing if IPv4 was present and
  merely lost the race.

## What is not here

The server's own rules — pairing, generations, authorisation, virtual-address
uniqueness, credential expiry, late packets from a superseded attempt — are
covered in the shadNet repo's `test_peer_sessions`, where `SessionCoordinator`
lives and they can be driven deterministically instead of raced between two
processes. The broker in `peer_netns_agent` is a stand-in that forwards
signaling blobs unread, which keeps a failure in this matrix attributable to
ICE rather than to the server.
