<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Bloodborne co-op for shadPS4

This fork connects shadPS4 to a private shadNet server so Bloodborne players can
use either the game's traditional online co-op flow or the experimental seamless
mode. The host rings the Beckoning Bell and a guest rings the Small Resonant Bell
in both modes.

This README is written for players. The person running the server should also
follow the [shadNet server guide](https://github.com/Wozzardman/shadnet-p2p).

> [!IMPORTANT]
> Use the matching releases of this shadPS4 fork and the shadNet fork. The
> upstream public builds do not contain all of this project's P2P changes.

## Choose a co-op mode

All players and the shadNet server must use the same mode. Completely close the
launchers, shadPS4 processes, and server before changing modes.

### Traditional co-op

Traditional mode keeps Bloodborne's normal area, boss, level, bell, death, and
session restrictions. Start QtLauncher or BBLauncher normally. Do not set
`SHADPS4_BLOODBORNE_SEAMLESS_COOP`, and have the server owner set
`BloodborneSeamlessCoop=false` in `shadnet.cfg`.

If the variable was set in the current terminal, remove it before starting the
launcher:

```bash
unset SHADPS4_BLOODBORNE_SEAMLESS_COOP
```

In Windows PowerShell, close the terminal that enabled seamless mode or run:

```powershell
Remove-Item Env:SHADPS4_BLOODBORNE_SEAMLESS_COOP -ErrorAction Ignore
```

### Experimental seamless co-op

Seamless mode bypasses the known bell availability checks and lets shadNet match
players in different maps. For a cross-map summon, the guest is moved to the
host's map before Bloodborne completes its normal room join. The server owner
must enable seamless mode as described in the shadNet README, and every player
must start their launcher with this environment variable:

```text
SHADPS4_BLOODBORNE_SEAMLESS_COOP=1
```

On Linux, start the launcher from a terminal. Replace the path with the actual
QtLauncher or BBLauncher executable:

```bash
SHADPS4_BLOODBORNE_SEAMLESS_COOP=1 /path/to/QtLauncher.AppImage
```

On Windows, open PowerShell in the launcher folder and run:

```powershell
$env:SHADPS4_BLOODBORNE_SEAMLESS_COOP = "1"
./QtLauncher.exe
```

Substitute the BBLauncher executable when using BBLauncher. On macOS, run the
executable inside the application bundle so it inherits the variable:

```bash
SHADPS4_BLOODBORNE_SEAMLESS_COOP=1 /Applications/QtLauncher.app/Contents/MacOS/QtLauncher
```

Keep that terminal open while playing. Starting the launcher normally from a
desktop icon will not inherit a variable set only in another terminal. The
reverse-engineering trace and capture variables are optional diagnostics and
are not required for seamless mode.

Seamless mode is still experimental. Bell use and cross-map guest placement are
working, but lantern travel after a co-op session has already formed is not yet
complete because Bloodborne can still suppress the lantern interaction prompt.
Back up saves before testing it.

## What every player needs

- A Windows or Linux PC capable of running Bloodborne in shadPS4.
- A legal dump of Bloodborne and any required system files. This project does
  not provide game or PlayStation firmware files.
- The same compatible game version as the other players. The currently tested
  Bloodborne target is CUSA03173 version 01.09.
- This fork's shadPS4 build and its QtLauncher.
- A unique account created on the group's shadNet server.
- Tailscale installed and joined to the server owner's tailnet.
- Normal Internet access for the Bloodborne community HTTP service.

## Load the custom build into a launcher

The downloaded `shadps4.exe`, AppImage, or native `shadps4` binary is the
emulator core. Double-clicking it directly may only show a message that it is a
CLI application. Add it to a launcher and start Bloodborne through that
launcher.

Extract or move the custom build to a permanent folder first. Do not move it
after adding it because the launcher saves its full path. On Windows, keep all
DLL files supplied with the build beside `shadps4.exe`.

### QtLauncher

1. Open QtLauncher.
2. Click **Version Manager**.
3. Click **Add Custom**.
4. Select this fork's `shadps4.exe` on Windows, AppImage on Linux, or native
   `shadps4` executable.
5. Enter a clear name such as `Bloodborne P2P`.
6. Close Version Manager and confirm `Bloodborne P2P` is selected in the version
   selector on QtLauncher's main window.
7. Launch the existing Bloodborne entry normally.

On Linux, make an AppImage executable before adding it:

```bash
chmod +x /path/to/shadps4-bloodborne-p2p.AppImage
```

Dragging the executable onto QtLauncher also opens the custom-version flow.

### BBLauncher

[BBLauncher](https://github.com/rainmakerv3/BB_Launcher) can also use a local
custom core:

1. Open BBLauncher and click **Manage Builds**.
2. Click **Add Local Build**. Do not download an upstream build for this setup.
3. Select this fork's `shadps4.exe`, AppImage, or native `shadps4` executable.
4. Name it `Bloodborne P2P`.
5. In **Installed Builds**, check the box beside `Bloodborne P2P`. Only one
   build should be checked.
6. Close Manage Builds and confirm the selected build path on the main window.
7. Set the Bloodborne install folder if BBLauncher has not already found it.

Do not create an empty `user` folder beside the custom executable. Such a folder
enables portable mode and can make the launcher use different users and saves.
Existing portable users should keep using their existing `user` folder.

## Which IP address goes where?

The server owner runs this on the shadNet machine:

```bash
tailscale ip -4
```

It prints an address beginning with `100.`, such as `100.101.102.103`. That is
the **server Tailscale IP**. Replace the example address throughout this guide
with the real one.

| Place | Value |
| --- | --- |
| shadPS4 **Server** | `100.101.102.103:31313` |
| shadPS4 **WebAPI Server** | `http://100.101.102.103:31315` |
| `host_overrides.json` summon URL | `http://100.101.102.103:31315` |
| shadNet `Host` setting, on the server | `0.0.0.0` |

Do not put `0.0.0.0` in shadPS4. Do not use the server's public Internet IP.
`127.0.0.1` only works when shadPS4 and shadNet run on the same computer.

## 1. Join the Tailscale network

The server owner must invite every player to the **same tailnet**. Bloodborne's
game traffic is peer-to-peer, so sharing only the server device is not enough.

### Server owner

1. Open the [Tailscale admin console](https://login.tailscale.com/admin/users).
2. Select **Users**, then **Invite external users**.
3. Enter each player's email address or copy an invitation link.
4. Give players the **Member** role.
5. Send the invitation and the server Tailscale IP to the players.

Invitation links expire, so create a new one if an old link no longer works.

### Each player

1. Install Tailscale from the [official download page](https://tailscale.com/download).
2. Accept the server owner's invitation.
3. Sign in to Tailscale on the computer that will run shadPS4.
4. Confirm that the server appears in `tailscale status`.
5. Test it with `tailscale ping 100.101.102.103`, using the real server IP.

No router port forwarding is needed. If the operating system asks for firewall
access, allow Tailscale and shadPS4 on private networks.

## 2. Configure the shadNet account

Ask the server owner for your own NP ID and password. Do not reuse another
player's account; two players cannot sign in with the same account at once.

### Server owner: register the player profiles

There is no web signup page. First, the server owner chooses a registration
key. shadNet does **not** generate it. It is simply a private signup code you
invent, such as a long phrase with no spaces. It is not a player's password and
is not needed after that player's account has been created.

In `build/shadnet.cfg`, set your own value:

```ini
RegistrationSecretKey=YOUR_OWN_LONG_PRIVATE_CODE
```

Replace `YOUR_OWN_LONG_PRIVATE_CODE` with a value only the server owner knows.
Leave that value in the configuration to prevent uninvited account creation.

On the shadNet computer, open a second terminal while the server is running and
build its bundled registration tool:

```bash
cd /path/to/shadnet-p2p/clientsample
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Create one account for each player. Replace `YOUR_OWN_LONG_PRIVATE_CODE` below
with the exact value entered in `build/shadnet.cfg`:

```bash
./build/shadnet-sample 127.0.0.1 31313 register HunterOne CHOOSE_A_PASSWORD hunter1@example.com YOUR_OWN_LONG_PRIVATE_CODE
./build/shadnet-sample 127.0.0.1 31313 register HunterTwo CHOOSE_ANOTHER_PASSWORD hunter2@example.com YOUR_OWN_LONG_PRIVATE_CODE
```

NP IDs must be unique and contain 3 to 16 letters, numbers, hyphens, or
underscores. Emails must also be unique. The created profiles are stored in
`build/db/shadnet.db`; the server owner should back up this file while shadNet
is stopped. Full server-side instructions are in the
[shadNet README](https://github.com/Wozzardman/shadnet-p2p#5-create-one-account-per-player).

If `RegistrationSecretKey` is left empty, omit the final argument from the
registration command. That opens registration to anyone who can reach the
server, so it is not recommended for an ongoing server.

### Player: enter the profile

The local shadPS4 username and the online shadNet account are different. Enter
the registered **NP ID** and that player's **player password**. Never enter the
server registration key in a launcher.

#### QtLauncher

1. Open **User Manager**.
2. Select the user that will launch Bloodborne. The default user is shown in
   bold; click **Set Default User** first if the wrong row is bold.
3. Click **ShadNet...**, or right-click the row and choose **ShadNet Settings...**.
4. Enable **Enable ShadNet for this user**.
5. Enter the registered name in **Account ID (NPID)**.
6. Enter that account's player password in **Password**.
7. Click **Save**, then close User Manager.

The ShadNet column should now show `On` followed by the NP ID.

#### BBLauncher

1. Select Bloodborne, then click the **shadPS4 Settings** icon.
2. Open the **User / shadNet Settings** tab.
3. Confirm **Username** is the intended local/default shadPS4 user.
4. Enable **Enable shadnet for current user**.
5. Enter the registered NP ID in **shadNet ID**.
6. Enter that account's player password in **shadNet Password**.
7. Click **Save**.

The NP ID is the online name shadNet uses. It is separate from the local
shadPS4 profile name.

## 3. Configure shadPS4 networking

Use the server owner's Tailscale IP. In QtLauncher, open **Settings**, then its
network/shadNet settings. In BBLauncher, open **shadPS4 Settings**, then
**User / shadNet Settings**; the network controls are on the right side.

| Setting | Value |
| --- | --- |
| Network connected | Enabled |
| shadNet | Enabled |
| Server | `100.101.102.103:31313` |
| WebAPI Server | `http://100.101.102.103:31315` |
| Signaling Info | Leave blank |
| UPnP | Disabled for the Tailscale setup |

Use the same server values on every player's PC, including the PC that also
runs shadNet. Restart shadPS4 after changing network or user settings.

BBLauncher's **Create Host Override File** button currently creates only the
community-service override and can overwrite an existing file. Do not use that
button for this private server. Create the complete two-entry file in the next
section manually. Also ignore **Open shadNet Registration Page**; accounts for
this private server are created by its owner as described above.

## 4. Create `host_overrides.json`

Close shadPS4 completely before editing this file. The filename uses an
underscore: **`host_overrides.json`**.

Create it in the shadPS4 user-data folder for your operating system:

| System | File location |
| --- | --- |
| Linux | `~/.local/share/shadPS4/host_overrides.json` |
| Linux with `XDG_DATA_HOME` | `$XDG_DATA_HOME/shadPS4/host_overrides.json` |
| Windows | `%APPDATA%\shadPS4\host_overrides.json` |
| macOS | `~/Library/Application Support/shadPS4/host_overrides.json` |
| Portable setup | `user/host_overrides.json` beside the launcher |

The directory is created after shadPS4 has been run once. Put this exact JSON
in the file, replacing the example `100.` address with the server Tailscale IP:

```json
{
  "https://ss4.scej-network.jp:20443": "http://thehuntersdream.com",
  "http://thehuntersdream.com:18671/summon_messenger": "http://100.101.102.103:31315"
}
```

Do not change the first URL or remove `:18671/summon_messenger` from the second
key. Every player needs this file. Restart shadPS4 after creating or changing
it; host overrides are loaded once when the emulator starts.

## 5. Check the connection

Before starting Bloodborne, keep Tailscale connected and make sure shadNet is
running. Open this address in a browser, with the real server IP:

```text
http://100.101.102.103:31315/status
```

A working server returns:

```json
{"ok":true,"service":"shadnet-webapi"}
```

Start Bloodborne and choose online play. The shadNet server console should show
your NP ID authenticating rather than `Login failed`.

## 6. Play co-op

1. Set the same matchmaking password in Bloodborne on all players. This is
   strongly recommended for testing with a specific friend.
2. Make sure all players are in compatible, normally co-op-enabled areas.
3. The world host rings the **Beckoning Bell**.
4. The guest rings the **Small Resonant Bell**.
5. Wait for Bloodborne to match and connect the peers.

In traditional mode, a greyed bell or crossed-out bell icon means Bloodborne
considers the current state or area ineligible. The Sinister Resonant Bell also
keeps the game's normal invasion rules.

In seamless mode, the host and guest may begin on different maps. Ring the
host's Beckoning Bell first, then the guest's Small Resonant Bell. Leave both
bells active while the server prepares the guest's destination and completes
the normal matchmaking flow. If the host travels before discovery, wait for the
host to finish loading before the guest rings. Established-session lantern
travel is not yet supported.

## Troubleshooting

### The server cannot be reached

- Confirm all PCs appear in `tailscale status` and can `tailscale ping` one
  another.
- Confirm shadNet reports listeners on `0.0.0.0:31313`, UDP `0.0.0.0:31314`,
  and `0.0.0.0:31315`. A `127.0.0.1` listener is not reachable by friends.
- Allow shadNet through the server firewall: TCP `31313`, UDP `31314`, and TCP
  `31315` on the Tailscale/private network.
- Allow shadPS4 through each player's firewall. Its P2P UDP port is assigned at
  runtime unless `SHADPS4_P2P_PORT` is explicitly set.

### Login fails

- Use a unique account that exists in this server's database.
- Check NP ID spelling and password capitalization.
- Confirm the shadPS4 and shadNet builds use the same protocol version.
- Ask the owner whether the server database was moved or replaced.

### Online works but summoning does not

- Recheck the underscore in `host_overrides.json` and validate the JSON.
- Confirm `/status` works from every player's PC.
- Restart shadPS4 after editing the override file.
- Confirm all players use the same game version, matchmaking password, and a
  normally eligible in-game area.
- Look for `loaded 2 host override entries` in the shadPS4 log.

## Building this shadPS4 fork

End users should use a paired release build when one is available. Developers
can clone this fork with its submodules:

```bash
git clone --recursive https://github.com/Wozzardman/shadp2p.git
cd shadp2p
```

Then follow the platform build guide:

- [Linux](documents/building-linux.md)
- [Windows](documents/building-windows.md)
- [macOS](documents/building-macos.md)

The `shadps4` executable built by this repository is the emulator core and may
identify itself as a CLI application. Use the compatible QtLauncher for the GUI
and network/user settings.

## Project status and credits

The Bloodborne P2P and seamless work is experimental. Keep backups of saves and
expect incomplete emulator or network behavior. Reverse-engineering notes live
in [`documents/bloodborne-seamless-re.md`](documents/bloodborne-seamless-re.md).

This fork is based on the [shadPS4 emulator](https://github.com/shadps4-emu/shadPS4)
and remains licensed under GPL-2.0-or-later. Bloodborne is owned by its
respective rights holders.

> [!NOTE]
> This README was generated by ChatGPT and reviewed against the configuration
> and networking code in this repository.

This fork's README documents co-op only. For general shadPS4 documentation —
keybindings, firmware setup, build requirements on other platforms, the main
team and the project's credits — see the
[upstream README](https://github.com/shadps4-emu/shadPS4/blob/main/README.md).
