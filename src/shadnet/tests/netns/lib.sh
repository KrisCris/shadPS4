#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Topology helpers for the network-condition matrix.
#
# The shape every case builds on:
#
#   hostA --- rtA ===\                       /=== rtB --- hostB
#   10.1.0.2  10.1.0.1 \                   / 10.2.0.1  10.2.0.2
#                  100.64.0.2 --[ inet ]-- 100.64.0.3
#                                  |
#                                 srv 100.64.0.10
#                           (broker, STUN, TURN)
#
# 100.64.0.0/10 is the carrier-grade NAT range. It stands in for the public
# internet here for the same reason the transport uses 198.18.0.0/15 for peer
# identity: if a rule ever leaks out of the lab, it names a range that is not
# somebody's home network.
#
# A router's NAT mode is what makes each case different, and the modes are
# chosen to match what actually breaks hole punching in the field:
#
#   none       plain forwarding; both peers are directly addressable
#   cone       MASQUERADE. Linux reuses one mapping across destinations, so a
#              STUN-discovered address is also the address the peer can reach.
#   symmetric  MASQUERADE --random. A new source port per destination, so the
#              address STUN reports is NOT the address the peer will see. This
#              is the case that must fall back to a relay.

set -u

INET_NS="lab-inet"
SRV_NS="lab-srv"

INET_SUBNET="100.64.0"
SRV_ADDR="100.64.0.10"

# Every namespace this run created, so teardown removes exactly those.
LAB_NAMESPACES=()

log() { printf '  %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

in_ns() { local ns="$1"; shift; ip netns exec "$ns" "$@"; }

lab_require_root() {
    [[ "$(id -u)" == "0" ]] || die "the lab needs root inside a --privileged container"
    ip netns list >/dev/null 2>&1 || die "ip netns unavailable; run with --privileged"
}

lab_teardown() {
    local ns
    for ns in $(ip netns list 2>/dev/null | awk '{print $1}' | grep '^lab-' || true); do
        ip netns del "$ns" 2>/dev/null || true
    done
    LAB_NAMESPACES=()
}

_ns_add() {
    local ns="$1"
    ip netns add "$ns"
    in_ns "$ns" ip link set lo up
    LAB_NAMESPACES+=("$ns")
}

INET_GATEWAY="100.64.0.1"

# Set LAB_IPV6=1 before building a topology to give every segment IPv6 as
# well. IPv6 here is routed, not translated, which is how it is deployed:
# a peer is globally addressable and the only thing in the way is a
# stateful firewall. That is why the IPv6 cases expect a host pair.
LAB_IPV6="${LAB_IPV6:-0}"
INET6_PREFIX="fd00:64::"
INET6_GATEWAY="fd00:64::1"
SRV6_ADDR="fd00:64::10"

# The "internet": one bridge every router and the server hangs off, and a
# router on it.
#
# The bridge alone is not enough. While every site is NATed, all traffic is
# addressed inside 100.64.0.0/24 and bridging suffices -- which is why a
# two-NAT case passes without this. A site with no NAT is addressed by its
# private range, and that has to be routed by something. Without a gateway
# here, such a case hangs in checking and looks like an ICE failure.
lab_make_internet() {
    _ns_add "$INET_NS"
    in_ns "$INET_NS" ip link add br-inet type bridge
    in_ns "$INET_NS" ip link set br-inet up
    in_ns "$INET_NS" ip addr add "${INET_GATEWAY}/24" dev br-inet
    in_ns "$INET_NS" sysctl -qw net.ipv4.ip_forward=1
    if [[ "$LAB_IPV6" == "1" ]]; then
        in_ns "$INET_NS" ip -6 addr add "${INET6_GATEWAY}/64" dev br-inet
        in_ns "$INET_NS" sysctl -qw net.ipv6.conf.all.forwarding=1
    fi
}

# _attach_to_inet <ns> <ifname-in-ns> <addr/prefix>
_attach_to_inet() {
    local ns="$1" ifname="$2" cidr="$3" cidr6="${4:-}"
    local host_side="v-${ns#lab-}"
    # Interface names are capped at 15 characters and a collision is silent.
    host_side="${host_side:0:15}"

    ip link add "$host_side" type veth peer name "$ifname" netns "$ns"
    ip link set "$host_side" netns "$INET_NS"
    in_ns "$INET_NS" ip link set "$host_side" master br-inet
    in_ns "$INET_NS" ip link set "$host_side" up
    in_ns "$ns" ip addr add "$cidr" dev "$ifname"
    in_ns "$ns" ip link set "$ifname" up
    in_ns "$ns" ip route add default via "$INET_GATEWAY" 2>/dev/null || true
    if [[ "$LAB_IPV6" == "1" && -n "$cidr6" ]]; then
        in_ns "$ns" ip -6 addr add "$cidr6" dev "$ifname" nodad
        in_ns "$ns" ip -6 route add default via "$INET6_GATEWAY" 2>/dev/null || true
    fi
}

# The server namespace: broker, and coturn when a case asks for it.
lab_make_server() {
    _ns_add "$SRV_NS"
    _attach_to_inet "$SRV_NS" wan "${SRV_ADDR}/24" "${SRV6_ADDR}/64"
}

# Drops inbound WAN traffic that is not part of a flow this side started.
#
# Without this the lab silently stops being a NAT test. A peer's punch arrives
# before we send ours, nothing is listening on the router, and the packet is
# dropped -- but only after conntrack has recorded it, and that record occupies
# the very external port our STUN mapping used. Our own punch then cannot
# preserve that port, so the address we advertised is no longer the address we
# send from, and an endpoint-independent NAT behaves like a symmetric one.
#
# Whether that happened came down to which punch arrived first, so the matrix
# passed or failed by timing. A real router drops unsolicited inbound before it
# becomes state, which is also what makes hole punching necessary at all.
_drop_unsolicited_inbound() {
    local rt="$1"
    local chain
    for chain in INPUT FORWARD; do
        in_ns "$rt" iptables -A "$chain" -i wan -m conntrack             --ctstate ESTABLISHED,RELATED -j ACCEPT
        in_ns "$rt" iptables -A "$chain" -i wan -j DROP
    done
}

# lab_make_site <name> <lan-octet> <wan-octet> <nat-mode> [first-host-index]
#
# Creates lab-rt<name> and lab-host<name>1: a router with one foot on the
# internet bridge and a bridged LAN behind it, plus the first host on that LAN.
# The LAN is a bridge rather than a point-to-point link so more hosts can join
# the same segment -- two peers only produce a host pair if they are genuinely
# on one L2 segment.
lab_make_site() {
    local name="$1" lan_octet="$2" wan_octet="$3" nat_mode="$4" first_host="${5:-1}"
    local rt="lab-rt${name}"
    local lan="10.${lan_octet}.0"
    local wan_addr="${INET_SUBNET}.${wan_octet}"

    _ns_add "$rt"
    _attach_to_inet "$rt" wan "${wan_addr}/24" "${INET6_PREFIX}${wan_octet}/64"

    in_ns "$rt" ip link add br-lan type bridge
    in_ns "$rt" ip link set br-lan up
    in_ns "$rt" ip addr add "${lan}.1/24" dev br-lan
    in_ns "$rt" sysctl -qw net.ipv4.ip_forward=1

    if [[ "$LAB_IPV6" == "1" ]]; then
        local lan6="fd00:${lan_octet}::"
        in_ns "$rt" ip -6 addr add "${lan6}1/64" dev br-lan nodad
        in_ns "$rt" sysctl -qw net.ipv6.conf.all.forwarding=1
        # Routed, not translated: the internet side needs to know the way back.
        in_ns "$INET_NS" ip -6 route replace "${lan6}/64" via "${INET6_PREFIX}${wan_octet}"
        in_ns "$SRV_NS" ip -6 route replace "${lan6}/64" via "${INET6_PREFIX}${wan_octet}"
        eval "LAB_SITE_LAN6_${name}=\"${lan6}\""
    fi

    # Remembered so lab_add_host can place further hosts on this LAN.
    eval "LAB_SITE_LAN_${name}=\"${lan}\""

    case "$nat_mode" in
        none)
            # No translation, so the host's own address is what the far side
            # sees -- and both the internet bridge and the server need a route
            # back to it, since nothing rewrote the source.
            in_ns "$INET_NS" ip route replace "${lan}.0/24" via "$wan_addr" 2>/dev/null || true
            in_ns "$SRV_NS" ip route replace "${lan}.0/24" via "$wan_addr" 2>/dev/null || true
            ;;
        cone)
            _drop_unsolicited_inbound "$rt"
            in_ns "$rt" iptables -t nat -A POSTROUTING -o wan -j MASQUERADE
            ;;
        symmetric)
            _drop_unsolicited_inbound "$rt"
            # --random picks a fresh source port per destination, which is what
            # makes a STUN-learned address useless to the peer.
            in_ns "$rt" iptables -t nat -A POSTROUTING -o wan -j MASQUERADE --random
            ;;
        *)
            die "unknown NAT mode: $nat_mode"
            ;;
    esac

    lab_add_host "$name" "$first_host"
}

# lab_add_host <site> <index>
#
# Attaches lab-host<site><index> to that site's LAN bridge at .<index+1>.
lab_add_host() {
    local name="$1" idx="$2"
    local rt="lab-rt${name}" host="lab-host${name}${idx}"
    local lan
    eval "lan=\$LAB_SITE_LAN_${name}"
    [[ -n "$lan" ]] || die "lab_add_host: site $name does not exist"

    _ns_add "$host"
    local rt_side="h${name}${idx}"
    ip link add "$rt_side" type veth peer name lan netns "$host"
    ip link set "$rt_side" netns "$rt"
    in_ns "$rt" ip link set "$rt_side" master br-lan
    in_ns "$rt" ip link set "$rt_side" up
    in_ns "$host" ip addr add "${lan}.$((idx + 1))/24" dev lan
    in_ns "$host" ip link set lan up
    in_ns "$host" ip route add default via "${lan}.1"

    if [[ "$LAB_IPV6" == "1" ]]; then
        local lan6
            eval "lan6=\$LAB_SITE_LAN6_${name}"
        if [[ -n "$lan6" ]]; then
            in_ns "$host" ip -6 addr add "${lan6}$((idx + 1))/64" dev lan nodad
            in_ns "$host" ip -6 route add default via "${lan6}1"
        fi
    fi
}

# Blocks traffic between two sites' WAN addresses while leaving the server
# reachable, so the only remaining path is through the relay. Without this a
# "TURN required" case can pass on a direct path and look like success.
lab_block_direct() {
    local wan_a="${INET_SUBNET}.$1" wan_b="${INET_SUBNET}.$2"
    in_ns "$INET_NS" iptables -I FORWARD -s "$wan_a" -d "$wan_b" -j DROP
    in_ns "$INET_NS" iptables -I FORWARD -s "$wan_b" -d "$wan_a" -j DROP
}

# Breaks the IPv4 path between peers while leaving IPv6 and the server alone.
#
# Addressing this by WAN address only works behind NAT. A site with no NAT
# sends from its private address, so a rule naming the WAN address never
# matches and the case quietly passes over the family it was meant to break.
# Dropping everything that is not to or from the server covers both.
#
# ip6tables is a separate table, so IPv6 is untouched.
lab_break_ipv4_between_peers() {
    in_ns "$INET_NS" iptables -A FORWARD -s "$SRV_ADDR" -j ACCEPT
    in_ns "$INET_NS" iptables -A FORWARD -d "$SRV_ADDR" -j ACCEPT
    in_ns "$INET_NS" iptables -A FORWARD -j DROP
}

# lab_set_mtu <ns> <ifname> <mtu>
#
# Narrows one link the way a tunnel does: Cloudflare WARP presents a 1280-byte
# interface, the IPv6 minimum. libjuice sets DF, so the sender's own stack
# refuses a larger datagram with EMSGSIZE rather than fragmenting it.
lab_set_mtu() {
    in_ns "$1" ip link set "$2" mtu "$3"
}

# Strips IPv4 from a host so nothing but IPv6 is left. A case that asserts an
# IPv6 path is worth little if IPv4 was available and simply lost the race.
lab_make_host_v6_only() {
    local host="$1"
    in_ns "$host" ip -4 addr flush dev lan
}

# --------------------------------------------------------------------------
# Services
# --------------------------------------------------------------------------

TURN_SECRET="lab-secret-not-a-real-one"
TURN_REALM="lab"

# Starts coturn in the server namespace as both STUN and TURN. The credential
# form matches what shadNet mints, so a credential bug shows up here rather
# than only on real hardware.
lab_start_turn() {
    local conf="${LAB_RUN_DIR}/turnserver.conf"
    cat > "$conf" <<EOF
listening-port=3478
listening-ip=${SRV_ADDR}
relay-ip=${SRV_ADDR}
realm=${TURN_REALM}
fingerprint
lt-cred-mech
use-auth-secret
static-auth-secret=${TURN_SECRET}
min-port=49160
max-port=49200
no-cli
no-tlsv1
no-tlsv1_1
no-multicast-peers
EOF
    in_ns "$SRV_NS" turnserver -c "$conf" --simple-log -o \
        > "${LAB_RUN_DIR}/turnserver.log" 2>&1 &
    LAB_TURN_PID=$!

    # Wait for the port rather than sleeping a guessed amount.
    local i
    for i in $(seq 1 50); do
        if in_ns "$SRV_NS" ss -lun 2>/dev/null | grep -q ":3478"; then
            return 0
        fi
        sleep 0.1
    done
    log "coturn did not come up; see ${LAB_RUN_DIR}/turnserver.log"
    return 1
}

# Mints a credential the same way the server does: the username carries its own
# expiry and the password is an HMAC over it.
lab_turn_credential() {
    local npid="$1" ttl="${2:-3600}"
    local expiry=$(( $(date +%s) + ttl ))
    LAB_TURN_USER="${expiry}:${npid}"
    LAB_TURN_PASS="$(printf '%s' "$LAB_TURN_USER" \
        | openssl dgst -sha1 -hmac "$TURN_SECRET" -binary \
        | base64)"
}

lab_stop_services() {
    [[ -n "${LAB_TURN_PID:-}" ]] && kill "$LAB_TURN_PID" 2>/dev/null
    LAB_TURN_PID=""
}
