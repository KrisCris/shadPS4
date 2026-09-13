# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Standalone build of peer_netns_agent, for the network-namespace lab.
#
# The emulator's top-level CMakeLists cannot be used here: its configure step
# looks for Qt, SDL, Vulkan and the rest, none of which a container that only
# needs to run two ICE agents should have to carry. This project compiles the
# same two source files with the same headers, and nothing else.
#
# It is not a second way to build the emulator. If peer_connection.cpp grows a
# dependency, this file should gain it too -- a divergence here shows up as a
# lab that cannot build what the emulator ships.

cmake_minimum_required(VERSION 3.16)
project(peer_netns_agent CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# <repo>/src/shadnet/tests/netns -> <repo>
get_filename_component(SHADPS4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

if (NOT EXISTS "${SHADPS4_ROOT}/externals/libjuice/CMakeLists.txt")
    message(FATAL_ERROR
        "libjuice not found under ${SHADPS4_ROOT}/externals. "
        "Run: git submodule update --init --recursive externals/libjuice")
endif()

set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
add_subdirectory("${SHADPS4_ROOT}/externals/libjuice" libjuice EXCLUDE_FROM_ALL)

# spdlog and fmt are built, not used header-only: spdlog's error helpers live
# in a translation unit, so a header-only build resolves nothing for them even
# though every LOG_ call site references them.
#
# The order and the flags mirror externals/CMakeLists.txt, including adding
# spdlog before fmt so it uses its own bundled copy. Setting
# SPDLOG_FMT_EXTERNAL instead sends spdlog 2.0 looking for an installed fmt
# package, which is not what the emulator links against.
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_DISABLE_GLOBAL_LOGGER ON)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHADPS4_ROOT}/externals/spdlog" spdlog EXCLUDE_FROM_ALL SYSTEM)
target_compile_definitions(spdlog PUBLIC SPDLOG_FUNCTION=__func__)
if (NOT TARGET fmt::fmt)
    add_subdirectory("${SHADPS4_ROOT}/externals/fmt" fmt EXCLUDE_FROM_ALL)
endif()

add_executable(peer_netns_agent
    "${SHADPS4_ROOT}/src/shadnet/tests/peer_netns_agent.cpp"
    "${SHADPS4_ROOT}/src/shadnet/peer_connection.cpp"
    "${SHADPS4_ROOT}/src/shadnet/peer_datagram.cpp"
)

target_include_directories(peer_netns_agent PRIVATE "${SHADPS4_ROOT}/src")

target_link_libraries(peer_netns_agent PRIVATE
    LibJuice::LibJuiceStatic
    spdlog::spdlog
    fmt::fmt
)

find_package(Threads REQUIRED)
target_link_libraries(peer_netns_agent PRIVATE Threads::Threads)
