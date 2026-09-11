// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "shadnet/peer_address.h"

namespace {

int g_failures = 0;

bool Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("check failed at line %d: %s\n", line, expression);
        ++g_failures;
    }
    return condition;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

// Build the constants at runtime rather than hand-swapping bytes, so the test
// is correct on either endianness and cannot silently agree with a
// byte-order bug in the code it is checking.
u32 Nbo(u32 host_order) {
    return htonl(host_order);
}

} // namespace

int main() {
    using ShadNet::PeerAddressTable;

    const u32 addr_a = Nbo(0xC6120005u); // 198.18.0.5
    const u32 addr_b = Nbo(0xC6120006u); // 198.18.0.6
    const u32 addr_c = Nbo(0xC612012Au); // 198.18.1.42

    // Range membership, including both edges and the address just past the end.
    CHECK(PeerAddressTable::IsVirtual(Nbo(0xC6120000u)));  // 198.18.0.0
    CHECK(PeerAddressTable::IsVirtual(Nbo(0xC613FFFFu)));  // 198.19.255.255
    CHECK(PeerAddressTable::IsVirtual(addr_a));
    CHECK(!PeerAddressTable::IsVirtual(Nbo(0xC611FFFFu))); // 198.17.255.255
    CHECK(!PeerAddressTable::IsVirtual(Nbo(0xC6140000u))); // 198.20.0.0
    CHECK(!PeerAddressTable::IsVirtual(Nbo(0x0A0000F6u))); // 10.0.0.246
    CHECK(!PeerAddressTable::IsVirtual(Nbo(0x7F000001u))); // 127.0.0.1
    CHECK(!PeerAddressTable::IsVirtual(0));

    // Round-trip in both directions.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        CHECK(table.NpidFor(addr_a).value_or("") == "MintCoffeeCat");
        CHECK(table.AddrFor("MintCoffeeCat").value_or(0) == addr_a);
        CHECK(table.SessionFor(addr_a).value_or(0) == 7u);
        CHECK(!table.NpidFor(addr_b).has_value());
        CHECK(!table.AddrFor("nobody").has_value());
        CHECK(!table.SessionFor(addr_b).has_value());
    }

    // Two peers coexist without disturbing each other.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.Insert(addr_b, "SomeoneElse", 7);
        CHECK(table.NpidFor(addr_a).value_or("") == "MintCoffeeCat");
        CHECK(table.NpidFor(addr_b).value_or("") == "SomeoneElse");
    }

    // Re-ringing leases the same peer a new address. The old address must stop
    // resolving -- leaving it behind is the failure that only shows up on a
    // retry, when traffic goes to an address nobody answers on.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.Insert(addr_c, "MintCoffeeCat", 8);
        CHECK(table.AddrFor("MintCoffeeCat").value_or(0) == addr_c);
        CHECK(table.NpidFor(addr_c).value_or("") == "MintCoffeeCat");
        CHECK(!table.NpidFor(addr_a).has_value());
        CHECK(table.SessionFor(addr_c).value_or(0) == 8u);
    }

    // Removing a session clears both directions for that session only.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.Insert(addr_b, "SomeoneElse", 9);
        table.RemoveSession(7);
        CHECK(!table.NpidFor(addr_a).has_value());
        CHECK(!table.AddrFor("MintCoffeeCat").has_value());
        CHECK(table.NpidFor(addr_b).value_or("") == "SomeoneElse");
        CHECK(table.AddrFor("SomeoneElse").value_or(0) == addr_b);
    }

    // Tearing down the superseded session must not unmap the address its peer
    // has already been re-leased under. The old session ends after the new one
    // begins whenever a retry overlaps, so this ordering is the normal case.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.Insert(addr_c, "MintCoffeeCat", 8);
        table.RemoveSession(7);
        CHECK(table.AddrFor("MintCoffeeCat").value_or(0) == addr_c);
        CHECK(table.NpidFor(addr_c).value_or("") == "MintCoffeeCat");
    }

    // Removing a session that was never there changes nothing.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.RemoveSession(999);
        CHECK(table.NpidFor(addr_a).value_or("") == "MintCoffeeCat");
    }

    // Clear empties both directions.
    {
        PeerAddressTable table;
        table.Insert(addr_a, "MintCoffeeCat", 7);
        table.Insert(addr_b, "SomeoneElse", 9);
        table.Clear();
        CHECK(!table.NpidFor(addr_a).has_value());
        CHECK(!table.NpidFor(addr_b).has_value());
        CHECK(!table.AddrFor("MintCoffeeCat").has_value());
        CHECK(!table.AddrFor("SomeoneElse").has_value());
    }

    if (g_failures != 0) {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
