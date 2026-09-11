# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# The network-condition lab. Everything the matrix needs in one image, so a
# case is reproducible on any machine with a Linux container runtime rather
# than only on the host it was written on.
#
# Must be run with --privileged: the cases create network namespaces, veth
# pairs and NAT rules, none of which a default container may do.

FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        iproute2 \
        iptables \
        iputils-ping \
        coturn \
        procps \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# coturn's package installs a service that would fight the per-case instances
# the lab starts by hand.
RUN rm -f /etc/default/coturn /etc/turnserver.conf

WORKDIR /lab
ENTRYPOINT ["/bin/bash"]
