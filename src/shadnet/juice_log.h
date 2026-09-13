// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace ShadNet {

// Sends libjuice's own log lines into the emulator log, from INFO up. Without
// it libjuice's account of a failure -- consent expiry, a refused TURN refresh,
// a datagram from an unexpected address -- goes nowhere, and a dropped peer
// connection cannot be traced to the hop that failed.
//
// Installed once, and from the emulator only: the handler is process-wide, and
// the tests install their own to watch for particular libjuice messages.
void InstallJuiceLogForwarding();

// A few libjuice warnings quote the ICE username fragment, the username or the
// password. Everything from the first quote on is replaced, so no credential
// can reach the log.
std::string RedactJuiceLogMessage(std::string_view message);

} // namespace ShadNet
