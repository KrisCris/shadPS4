#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The network-condition matrix.
#
# Each case asserts which candidate type actually won, not merely that a
# connection formed. That distinction is the whole point: peer_connection_test
# connects two agents on one host every time and tells you nothing about NAT
# traversal, because both agents see the same interfaces and settle on a host
# pair. A case here that only checked "connected" would be the same test with
# more scaffolding.
#
# Run inside the lab image, privileged:
#
#   docker build -t shadps4-netns-lab src/shadnet/tests/netns
#   docker run --rm --privileged -v "$PWD:/repo" shadps4-netns-lab \
#       /repo/src/shadnet/tests/netns/run-matrix.sh
#
# Options:
#   --case NAME   run one case instead of all
#   --list        print the case names and exit
#   --keep        leave the namespaces up after the run, to poke at them

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/lib.sh"

REPO_ROOT="$(cd "${HERE}/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/netns-build}"
AGENT="${BUILD_DIR}/peer_netns_agent"
LAB_RUN_DIR="${LAB_RUN_DIR:-/tmp/netns-run}"
BROKER_PORT=39100

KEEP=0
ONLY_CASE=""

# --------------------------------------------------------------------------

build_agent() {
    if [[ -x "$AGENT" && -z "${FORCE_BUILD:-}" ]]; then
        log "agent already built at ${AGENT}"
        return 0
    fi
    log "building peer_netns_agent"
    cmake -S "${HERE}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo > "${LAB_RUN_DIR}/cmake.log" 2>&1 \
        || { cat "${LAB_RUN_DIR}/cmake.log"; die "cmake configure failed"; }
    cmake --build "${BUILD_DIR}" >> "${LAB_RUN_DIR}/cmake.log" 2>&1 \
        || { tail -40 "${LAB_RUN_DIR}/cmake.log"; die "build failed"; }
    [[ -x "$AGENT" ]] || die "build produced no ${AGENT}"
}

# run_pair <offerer-ns> <answerer-ns> <case-name> [extra agent args...]
#
# Both agents get the same expectations: a case where one side sees a relayed
# candidate and the other does not is a failure worth catching, and only
# checking one side would hide it.
run_pair() {
    local off_ns="$1" ans_ns="$2" name="$3"; shift 3
    local out="${LAB_RUN_DIR}/${name}"
    mkdir -p "$out"

    in_ns "$SRV_NS" "$AGENT" --role broker --broker "0.0.0.0:${BROKER_PORT}" \
        > "${out}/broker.log" 2>&1 &
    local broker_pid=$!

    # A case may point the agents at an IPv6 broker address instead.
    local broker_addr="${SRV_ADDR}:${BROKER_PORT}"
    if [[ "${1:-}" == "--broker-host" ]]; then
        broker_addr="$2:${BROKER_PORT}"
        shift 2
    fi

    in_ns "$off_ns" "$AGENT" --role offerer --broker "$broker_addr" "$@" \
        > "${out}/offerer.log" 2>&1 &
    local off_pid=$!
    in_ns "$ans_ns" "$AGENT" --role answerer --broker "$broker_addr" "$@" \
        > "${out}/answerer.log" 2>&1 &
    local ans_pid=$!

    local off_rc=0 ans_rc=0
    wait "$off_pid" || off_rc=$?
    wait "$ans_pid" || ans_rc=$?
    kill "$broker_pid" 2>/dev/null
    wait "$broker_pid" 2>/dev/null

    grep -h '^RESULT' "${out}/offerer.log" "${out}/answerer.log" 2>/dev/null | sed 's/^/    /' >&2

    if [[ $off_rc -ne 0 || $ans_rc -ne 0 ]]; then
        grep -h '^EXPECT-FAIL' "${out}/offerer.log" "${out}/answerer.log" 2>/dev/null \
            | sed 's/^/    /' >&2
        return 1
    fi
    return 0
}

# Inverts run_pair: the case passes only when the agents fail to connect.
#
# Worth having for exactly one reason. A negative case that is silently
# mis-built -- a rule that does not match, a service that never started --
# passes for the wrong reason and reports nothing. Every case that uses this
# has to be one where a positive twin exists and does connect, so a failure
# here means the one thing that changed is what stopped it.
run_pair_expect_failure() {
    if run_pair "$@"; then
        log "expected no connection, but the peers connected"
        return 1
    fi
    return 0
}

# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

CASES=(same-lan two-nats lan-and-external symmetric-turn overlapping-subnets
       ipv6-only dual-stack-v4-broken turn-credential-expired)

# Both peers on one LAN. The pair must be host: if a case this simple reaches
# for a reflexive or relayed candidate, the agent is ignoring local interfaces
# and every harder case is being measured against the wrong baseline.
case_same_lan() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 none
    lab_add_host A 2

    run_pair lab-hostA1 lab-hostA2 same-lan         --expect-local host --expect-remote host --expect-family ipv4
}

# Two peers behind two different endpoint-independent NATs, with STUN. The
# pair must be srflx: a host pair here would mean the namespaces are not
# actually separated, and a relayed one would mean hole punching failed on the
# easiest NAT there is.
case_two_nats() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 cone
    lab_make_site B 2 3 cone
    lab_start_turn || return 1

    run_pair lab-hostA1 lab-hostB1 two-nats \
        --stun "${SRV_ADDR}:3478" \
        --expect-local srflx --expect-remote srflx --expect-family ipv4
}

# One peer directly addressable, one behind a NAT. The NATed side must still
# reach the open side -- this is the asymmetric case where only one peer needs
# to punch out.
case_lan_and_external() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 none
    lab_make_site B 2 3 cone
    lab_start_turn || return 1

    # Only the family and the absence of a relay are asserted: which side ends
    # up host and which srflx depends on which candidate pair nominates first,
    # and pinning that would be testing ICE's priority table, not our code.
    run_pair lab-hostA1 lab-hostB1 lan-and-external \
        --stun "${SRV_ADDR}:3478" --expect-family ipv4
}

# Endpoint-dependent NAT on both sides with the direct path blocked outright.
# The address STUN reports is not the address the peer would reach, and the
# peers cannot reach each other at all, so traffic must go through the relay.
#
# The assertion is that EITHER side of the nominated pair is a relay, not a
# particular side. Sending to a peer's relay allocation makes your own side
# peer-reflexive, so a relayed connection shows relay on one end and prflx on
# the other -- and which end is which depends on whose allocation ICE
# nominates, which differs between runs. Pinning a side makes this case flaky
# in a way that looks like a transport bug.
case_symmetric_turn() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 symmetric
    lab_make_site B 2 3 symmetric
    lab_block_direct 2 3
    lab_start_turn || return 1
    lab_turn_credential "labpeer"

    run_pair lab-hostA1 lab-hostB1 symmetric-turn \
        --stun "${SRV_ADDR}:3478" \
        --turn "${SRV_ADDR}:3478" \
        --turn-user "$LAB_TURN_USER" --turn-pass "$LAB_TURN_PASS" \
        --expect-relayed --expect-family ipv4 \
        --timeout 45
}

# Both LANs use the same private subnet. Each peer's host candidate names an
# address that is also valid on the other side, so an implementation that
# trusts a host candidate without checking reachability connects to itself or
# to the wrong machine. The pair must be srflx.
case_overlapping_subnets() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 cone
    # Deliberately the same 10.1.0.0/24, with the peer at a different host
    # address on it -- two homes that both use the router default.
    lab_make_site B 1 3 cone 2
    lab_start_turn || return 1

    run_pair lab-hostA1 lab-hostB2 overlapping-subnets \
        --stun "${SRV_ADDR}:3478" ${LAB_EXTRA_ARGS:-} \
        --expect-local srflx --expect-remote srflx --expect-family ipv4
}

# Two peers with no IPv4 address at all, reaching each other over routed
# IPv6. The pair must be a host pair on an IPv6 address.
#
# The IPv4 addresses are stripped rather than merely left unrouted, because an
# assertion that the path was IPv6 proves nothing if IPv4 was available and
# simply lost the race. With no IPv4 present, an implementation that quietly
# preferred it has nowhere to go and the case fails outright -- which is the
# point of it.
case_ipv6_only() {
    export LAB_IPV6=1
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 none
    lab_make_site B 2 3 none
    lab_make_host_v6_only lab-hostA1
    lab_make_host_v6_only lab-hostB1

    run_pair lab-hostA1 lab-hostB1 ipv6-only \
        --broker-host "[${SRV6_ADDR}]" \
        --expect-local host --expect-remote host --expect-family ipv6
}

# Dual stack, with IPv4 blocked between the two sites and IPv6 left alone. The
# peers must notice and finish over IPv6 rather than stalling on the family
# that cannot work.
case_dual_stack_v4_broken() {
    export LAB_IPV6=1
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 none
    lab_make_site B 2 3 none
    lab_break_ipv4_between_peers

    run_pair lab-hostA1 lab-hostB1 dual-stack-v4-broken \
        --expect-family ipv6
}

# The same topology as symmetric-turn, which does connect, with one change:
# the TURN credential expired ten minutes ago. The relay must refuse the
# allocation, and with the direct path blocked there is nothing else left, so
# the peers must NOT connect.
#
# Without this, nothing proves the relay checks the credential at all -- a
# server handing out malformed or stale credentials would look identical to
# one handing out good ones, because the positive case only ever sees fresh
# ones.
case_turn_credential_expired() {
    lab_make_internet
    lab_make_server
    lab_make_site A 1 2 symmetric
    lab_make_site B 2 3 symmetric
    lab_block_direct 2 3
    lab_start_turn || return 1
    lab_turn_credential "labpeer" -600

    run_pair_expect_failure lab-hostA1 lab-hostB1 turn-credential-expired \
        --stun "${SRV_ADDR}:3478" \
        --turn "${SRV_ADDR}:3478" \
        --turn-user "$LAB_TURN_USER" --turn-pass "$LAB_TURN_PASS" \
        --expect-relayed --timeout 15
}

# --------------------------------------------------------------------------

run_case() {
    local name="$1"
    local fn="case_${name//-/_}"
    declare -F "$fn" > /dev/null || die "no such case: $name"

    printf '\n=== %s\n' "$name" >&2
    export LAB_IPV6=0
    lab_teardown
    lab_stop_services
    mkdir -p "${LAB_RUN_DIR}/${name}"

    local rc=0
    "$fn" || rc=$?
    lab_stop_services
    [[ $KEEP -eq 1 ]] || lab_teardown

    if [[ $rc -eq 0 ]]; then
        printf '  PASS %s\n' "$name" >&2
    else
        printf '  FAIL %s  (logs in %s/%s)\n' "$name" "$LAB_RUN_DIR" "$name" >&2
    fi
    return $rc
}

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --case) ONLY_CASE="$2"; shift 2 ;;
            --keep) KEEP=1; shift ;;
            --list) printf '%s\n' "${CASES[@]}"; return 0 ;;
            *) die "unknown argument: $1" ;;
        esac
    done

    lab_require_root
    mkdir -p "$LAB_RUN_DIR"
    build_agent

    local failed=()
    local name
    if [[ -n "$ONLY_CASE" ]]; then
        run_case "$ONLY_CASE" || failed+=("$ONLY_CASE")
    else
        for name in "${CASES[@]}"; do
            run_case "$name" || failed+=("$name")
        done
    fi

    printf '\n' >&2
    if [[ ${#failed[@]} -eq 0 ]]; then
        printf 'all cases passed\n' >&2
        return 0
    fi
    printf 'failed: %s\n' "${failed[*]}" >&2
    return 1
}

main "$@"
