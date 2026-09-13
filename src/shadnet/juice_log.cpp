// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/juice_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>

#include <juice/juice.h>

#include "common/logging/log.h"

namespace ShadNet {

namespace {

// libjuice calls this from its own threads.
void ForwardJuiceLog(juice_log_level_t level, const char* message) {
    if (message == nullptr) {
        return;
    }
    const std::string text = RedactJuiceLogMessage(message);
    switch (level) {
    case JUICE_LOG_LEVEL_VERBOSE:
    case JUICE_LOG_LEVEL_DEBUG:
        LOG_DEBUG(ShadNet, "juice: {}", text);
        break;
    case JUICE_LOG_LEVEL_INFO:
        LOG_INFO(ShadNet, "juice: {}", text);
        break;
    case JUICE_LOG_LEVEL_WARN:
        LOG_WARNING(ShadNet, "juice: {}", text);
        break;
    default:
        LOG_ERROR(ShadNet, "juice: {}", text);
        break;
    }
}

} // namespace

void InstallJuiceLogForwarding() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        // The handler first, so nothing logged in between goes to stdout.
        juice_set_log_handler(&ForwardJuiceLog);
        // INFO is where libjuice reports consent expiry and failed TURN
        // allocations, and it logs per session there, not per packet.
        juice_set_log_level(JUICE_LOG_LEVEL_INFO);
    });
}

std::string RedactJuiceLogMessage(std::string_view message) {
    static constexpr std::array<std::string_view, 4> kCredentialWords = {"password", "pwd", "ufrag",
                                                                         "username"};
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool names_credential = std::any_of(
        kCredentialWords.begin(), kCredentialWords.end(),
        [&lower](std::string_view word) { return lower.find(word) != std::string::npos; });
    const size_t quote = message.find('"');
    if (!names_credential || quote == std::string_view::npos) {
        return std::string(message);
    }
    return std::string(message.substr(0, quote)) + "<redacted>";
}

} // namespace ShadNet
