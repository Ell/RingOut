# Netplay lobby architecture and UX audit

Audit commit: `ff0ad952980f5083afd21c3d3758208a7a093d72`

Audit date: 2026-08-25

This document describes the lobby and session-setup system that exists at the
audit commit. It separates what the source and tests prove from what still needs
physical or multi-machine validation. Paths and line numbers are relative to the
repository root and refer to that commit.

## Executive assessment

### Rollback branch player-flow update (2026-08-25)

The `codex/rollback-netplay` worktree now has a bounded player-facing network
mode seam on top of the launcher prototype. `config.ini` persists an explicit
`mode=fixed-delay|rollback`, the launcher passes the same choice through
`--netplay-mode`, and the in-game restart path reuses that persisted choice.
Rollback is labelled Experimental and is selectable only when the runtime's
authoritative `NetPlay::IsLiveRollbackProductionReady()` predicate reports
that the audited output matrix and production gate factory are available.
There is no environment-variable path from the normal launcher; the isolated
headless acknowledgement remains test infrastructure.

The branch also corrects the buffer unit to SI samples, maps a manual rollback
value to the negotiated rollback base delay, exposes a two-player side swap,
and requires the exact expected roster plus a mapped pad for every player
before host Start is enabled. A requested rollback match is blocked when any
peer lacks the capability and a guest refuses to boot if the host selects a
disabled rollback session; it never silently presents fixed-delay play as
rollback.

The branch now implements the deliberate Ready flow through authoritative
`Ready`/`NotReady` protocol messages. Each roster row separates game and
controller setup from the player's explicit Ready state. Mapping or host delay
changes clear all Ready flags, and the server refuses Start until every mapped
player is Ready. Headless automation waits for both Same Game and its
controller mapping, then auto-readies through the same protocol rather than
bypassing the gate.

The release entry-point audit originally found these controls were source-tree
only. The rollback branch now removes the helper's compiled checkout path,
resolves module templates, game settings, and the Windows toolchain relative to
the shipped helper, builds `moderngekko-launcher` in both release workflows,
and makes it the top-level Windows and AppImage entry point. Existing wrapper
setup assets remain in each artifact for recovery. Package gates require the
C++ launcher, helper, fonts, runner, and their import/library closures; Windows
Wine and AppImage self-tests execute the top-level launcher and verify that the
helper resolves the exact packaged module-source and game-settings paths.
Tagged artifact and physical
platform validation are still required before calling a release shipped.

### Online Room beta update (2026-08-25)

The current `codex/rollback-netplay` branch adds an Online Room beta on top of
that verified lobby and rollback path. Its preceding committed checkpoint is
`05798a513f34a02ea5983daa65521cd36532cc0f`; the Online Room snapshot described
here was integrated on 2026-08-25. The integrated desktop launcher now
defaults to a short-code room using Dolphin's hosted rendezvous service, while
**Advanced: use Direct IP** retains the LAN/private-VPN path. Online Room fixes
the player selection to rollback; Advanced Direct IP offers both fixed delay and
rollback (`ModernGekko/tools/moderngekko_launcher.cpp:1199-1273`).

The runtime defaults to `stun.dolphin-emu.org`, main UDP port 6262, and alternate
UDP port 6226. Hosts receive an eight-lowercase-hex-digit room code; joiners may
paste upper- or lowercase hex with surrounding whitespace, which is normalized
before dialing (`ModernGekko/tools/netplay_session.hpp:16-64` and
`ModernGekko/tools/frontend_config.cpp:45-61`). The host's traversal server and
its direct loopback client deliberately share the established RingOut lobby and
handshake after rendezvous; a remote joiner passes the room code through the
traversal client (`ModernGekko/tools/netplay_session.cpp:1149-1168,1255-1273`).

This is not a RingOut matchmaking, relay, authentication, encryption, or IP-
privacy service. Dolphin only introduces the direct peers. Strict NATs and
stateful firewalls may fail, the opponent ultimately learns the peer IP, and
gameplay remains plaintext. The launcher and lobby say this explicitly and map
service unavailable, missing/expired code, and peer traversal failure to typed
messages (`ModernGekko/tools/netplay_session.cpp:212-251,757-796,1276-1304` and
`ModernGekko/tools/moderngekko_launcher.cpp:1596-1613`). Use it only with a
trusted friend.

The desktop launcher and direct CLI are implemented. The in-game System tab no
longer lists its obsolete Direct-IP Host/Join/Scan controls, so players cannot
accidentally enter a second flow that lacks Online Room. The dormant one-shot
request reader remains for compatibility with an already-created request file;
normal player UI cannot create a new request.

The current source predicate returns true because all eleven audited capability
bits are set (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:158-178`),
so the branch launcher exposes Experimental rollback. The memory-card snapshot,
teardown-latched suppression, and corrected-frontier hard-fault fixes identified
by the earlier review are now implemented, and the final ordinary production
correction run at `/tmp/ringout-live-rollback.final-correction.OHN0EzDz` passed with matching
confirmed logical states. This validates the current Linux/source launcher path;
it is not a final packaged-player approval because tagged Windows/AppImage and
physical/cross-machine validation remain absent.

### Ordinary integrated-launcher flow on this branch

This flow applies to a package built from the rollback worktree, not to the
published `ell.6` packages:

1. Both players complete Game files setup from the same RingOut source,
   compatible `GRSEAF` disc/DOL, and generated module. Open **Netplay**.
2. Both select the exact same mode. **Fixed delay (stable)** is the compatibility
   path. **Experimental rollback** requires every peer to request and support
   rollback; a mismatch is rejected rather than silently downgraded
   (`ModernGekko/tools/moderngekko_launcher.cpp:1199-1239,1474-1501` and
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:473-499`).
3. For the ordinary beta flow, leave **Advanced: use Direct IP** off. The host
   selects **Host online room**, copies the eight-character code from the lobby,
   and sends it privately to the guest. The guest pastes it and selects **Join
   online room**. For a trusted LAN/private VPN or service outage, both players
   may instead select Advanced Direct IP and use the same hostname/IP and UDP
   port (`ModernGekko/tools/moderngekko_launcher.cpp:1243-1324` and
   `ModernGekko/tools/netplay_session.cpp:757-788`).
4. In the lobby, verify **Same game**, one controller assignment per player, and
   the displayed requested mode. Each player selects **Ready**. A mapping,
   delay/mode, game, join, or disconnect change clears Ready
   (`ModernGekko/tools/netplay_session.cpp:716-834` and
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:708-716,1607-1624`).
5. After every mapped player is Ready, the host selects **Start game**. The
   server checks readiness again at request and launch
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1501-1513,1720-1741`).

If Online Room reports a missing/expired code, have the host keep the lobby open
and copy its current code. If peer traversal fails, use Advanced Direct IP over
a trusted LAN/private VPN; this beta has no relay. If rollback compatibility or
capability fails, Advanced Direct IP can be used to create a deliberate new
fixed-delay session. “Fallback” always means a new session; an already requested
rollback session is never relabelled fixed delay.

Rollback currently quarantines persistence: synchronized memory-card contents
remain guest-visible for play, but `savedata_write` and writable SD are forced
off, serial ports are disabled, and production start requires safe memory-card
slot/device policy (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1127-1146,1644-1697`;
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:143-188`).
Players should expect rollback-session save progress not to persist.

### Historical audit assessment at `ff0ad952`

Ring Out has a useful direct-connect lobby, not a central matchmaking service.
It is reasonably good for two trusted players on one LAN or a VPN: it can find a
local host, display a live roster and ping, verify the selected `main.dol`, assign
controller ports, synchronize host saves and cheats, and then boot both peers
through Dolphin's NetPlay path.

It is not yet a production-grade internet lobby. There is no relay, traversal,
room code, public browser, authentication, or automatic port mapping in the
Ring Out path. Several important error cases terminate silently, player
readiness is conflated with game compatibility, the existing ModernGekko module
fingerprint is not exchanged on the wire, and the roster API exposes live
pointers after releasing its lock.

The most important terminology correction is that this is **fixed-delay
netplay, not rollback**. Ring Out forces `NETPLAY_NETWORK_MODE` to
`"fixeddelay"` and disables host-input authority
(`ModernGekko/tools/netplay_session.cpp:821-824` and `:912-923`). The lobby's
1-20 slot slider is an input-delay buffer. The UI calls the slots frames, but
they are consumed at Dolphin's variable SI input-poll rate, typically 120 Hz.
The default labelled "Automatic" is currently a fixed five samples, not an
adaptive algorithm.

### Ratings

| Area | Rating | Assessment |
| --- | ---: | --- |
| Two-player LAN/VPN setup | 7/10 | Direct connection, discovery, roster, ping, pad mapping, and synchronized boot are present. |
| Direct internet setup | 4/10 | Works with a reachable UDP port, but requires manual forwarding or a VPN and gives little diagnostic help. |
| Compatibility protection | 5/10 | Exact source revision and full `main.dol` hash are checked; recomp-module compatibility is not. |
| Readiness and capacity | 4/10 | Host-controlled start works, but there is no player-ready state and no four-controller admission limit. |
| Keyboard/controller lobby UX | 7/10 | The actual lobby supports mouse, keyboard, and gamepad, with explicit cancel controls. The desktop launcher does not enable ImGui gamepad navigation. |
| Error and recovery UX | 3/10 | Typed connection failures are collapsed, cancellation and failure share a result, and returning to offline play is unsupported. |
| Thread/lifetime safety | 4/10 | The main restart path is careful, but the roster is not copied under lock and LAN scanning uses a detached worker. |
| Test confidence | 6/10 | A real localhost protocol test and manual full-match harness exist; interactive UI, discovery, and physical Windows paths are not automated. |

Overall: a credible engineering prototype with good deterministic-session
foundations, but still one hardening pass short of being a friendly public
lobby.

## Architecture at a glance

```text
desktop launcher                         running offline game
      |                                         |
      | Host / Join button                      | System -> Netplay
      |                                         | optional LAN Scan
      |                                         | Start Netplay
      |                                         v
      |                                netplay-request.ini
      |                                         |
      |                                safe runtime shutdown
      +----------------------+------------------+
                             v
                     moderngekko-run
                             |
                 initialize UICommon + ENet
                             |
                +------------+-------------+
                |                          |
              Host                       Join
       Online: register and       Online: submit room code
        display room code          through Dolphin service
       Direct: bind UDP port      Direct: resolve host/IP
        create local client           and connect
                |                          |
                +------------+-------------+
                             v
              direct peer-to-peer ENet transport
                (Dolphin service is not a relay)
                             |
                             v
                    SDL3 + ImGui lobby
           roster / ping / game status / pad map
                             |
                     host requests Start
                             |
              sync settings, saves, and codes
                             |
             NetPlayClient::StartGame -> boot data
                             |
                  create a fresh Runtime and run
```

The restart is not cosmetic. `NetPlay_Enable` is armed inside
`NetPlayClient::StartGame`, before the core boots. An already-running offline
core therefore cannot simply toggle itself into a network session. This design
constraint is documented in `ModernGekko/tools/moderngekko_run.cpp:397-400` and
`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:208-212`.

## Entry points

### Desktop launcher

The following audit-commit description is historical. On the current branch,
the launcher now presents Online Room by default, asks only a joiner for an
eight-character code, provides Host/Join Online Room buttons, and exposes the
address/port fields only after selecting Advanced Direct IP
(`ModernGekko/tools/moderngekko_launcher.cpp:1199-1324`). It passes
`--netplay-traversal` for Online Room and preserves the existing direct arguments
otherwise (`ModernGekko/tools/moderngekko_launcher.cpp:1474-1505`).

The current launcher always captures basic child output in
`userdata/Logs/RingOut.log`. Its persisted **Detailed netplay diagnostics**
toggle additionally enables Dolphin's NETPLAY transport/handshake category,
rotates the prior run to `RingOut.previous.log`, and provides a Copy log path
button. The privacy notice calls out nicknames, room codes, IP addresses,
controller names, and local paths before players share a file.

The desktop launcher initializes SDL video and gamepad support, but enables only
ImGui keyboard navigation (`ModernGekko/tools/moderngekko_launcher.cpp:340-377`).
Its main screen exposes:

- `Play`, `Host Netplay`, and `Join Netplay` buttons
  (`ModernGekko/tools/moderngekko_launcher.cpp:546-613`).
- Nickname, host/IP, UDP port, and automatic/manual buffer controls
  (`ModernGekko/tools/moderngekko_launcher.cpp:682-695`).
- A controller-profile prerequisite for either Host or Join.

Host or Join saves the complete frontend configuration and builds a child
`moderngekko-run` argument list. Join passes the saved address; both modes pass
port, nickname, buffer, and each selected controller
(`ModernGekko/tools/moderngekko_launcher.cpp:778-801`). The launcher redirects
the child log to `userdata/Logs/KirbyRecomp.log`, hides its window, and waits for
the child (`ModernGekko/tools/moderngekko_launcher.cpp:809-854`). The historical
`KirbyRecomp.log` name makes support instructions less obvious.

The desktop launcher has no LAN Scan button. A desktop joiner must type or reuse
an address even though the in-game menu has discovery.

### In-game System menu

The current branch removes all Netplay, Scan, address, port, and Start rows from
the visible System-tab item list. Online Room and Advanced Direct IP are
available through the integrated desktop launcher; explicit runner CLI remains
for automation and diagnostics.

The remainder of this subsection records the historical audit-commit behavior,
not the current visible player flow.

The in-game menu's System tab contains, in order, Netplay mode, Scan, address,
port, and Start rows
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:146-164`).
Netplay mode cycles from Off to Host to Join. Join mode enables Scan and address
editing; both Host and Join enable the port and Start rows
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:1004-1075`).

The address has two representations:

- `netplay_addr_text` is authoritative and can retain a hostname such as a
  Tailscale MagicDNS name.
- Four integer octets provide a controller-friendly IPv4 editor.

The split is deliberate: merely opening the menu must not replace a saved
hostname with `127.0.0.1`
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:216-230`).
Space enters/exits octet editing; Left/Right chooses an octet; Up/Down changes
it, with accelerated key repeat
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:1987-2046`).
Escape first exits address-edit mode and only then closes the menu
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:2405-2439`).

### Request and restart lifecycle

Start writes `userdata/netplay-request.ini` with mode, port, and—only for a
joiner—a non-empty address
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:1714-1737`).
It deliberately does not write an auto-resume snapshot because a local state
would immediately diverge from the remote peer. It then closes/resumes the core
and uses a generation-guarded terminal callback to quit only after the core is
safe (`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:2287-2322`).

After the offline runtime ends, `moderngekko-run`:

1. Reads `mode`, `address`, and `port` from the request.
2. Deletes the request before acting, preventing a crash from trapping the user
   in a repeated netplay relaunch.
3. Releases the single-runtime guard.
4. Builds `NetplayOptions`, using persisted values when the request omitted one.
5. Uses a 600-second human-oriented wait rather than the 120-second scripted
   default.
6. Persists the chosen address and port for the next launch.
7. Calls `RunNetplayLobby` in the same process.

That sequence is in `ModernGekko/tools/moderngekko_run.cpp:397-478`. Reusing the
same process required a Windows-specific fix: a second runtime accepts the
already-registered render-window class instead of failing startup
(`ModernGekko/vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWin32.cpp:69-98`).

The lifecycle is careful and is one of the strongest parts of the implementation.
Its main UX limitation is that cancellation does not reconstruct offline play;
once the old runtime has been torn down, leaving the lobby ends the application.

## Connection topologies

### Online Room beta

`NetplayOptions` distinguishes `OnlineRoom` from `Direct` and contains the
Dolphin host/main/alternate defaults
(`ModernGekko/tools/netplay_session.hpp:16-64`). The host gives its
traversal-enabled `NetPlayServer` an OS-assigned ephemeral UDP port, while its
own loopback `NetPlayClient` remains direct. This prevents a hidden, stale
Direct-IP port setting from blocking Online Room. The guest gives the normalized
room code and traversal configuration to `NetPlayClient`
(`ModernGekko/tools/netplay_session.cpp:981-999,1149-1168,1255-1273`).

The host lobby transitions from `Creating online room ...` to the assigned code
and a copy button. A joining lobby identifies the submitted code. LAN discovery
beacons are disabled for an Online Room host because the two discovery systems
have different scopes (`ModernGekko/tools/netplay_session.cpp:691-694,757-788`).

Dolphin's service records the observed UDP endpoint, returns a random eight-hex
code, forwards a connect request, and tells each peer where to punch. It does not
carry the ENet lobby or gameplay stream. Consequently Online Room changes how
the initial route is discovered, not RingOut's roster, mode/fingerprint
admission, Ready gate, pad mapping, synchronized start, rollback scheduler, or
desync stop behavior.

### Advanced Direct IP

`RunNetplayLobby` initializes ENet at the lobby boundary for both roles. This is
necessary on Windows because a direct join otherwise reaches `enet_host_create`
before `WSAStartup` (`ModernGekko/tools/netplay_session.cpp:757-771`).

The session then configures Dolphin for strict synchronized fixed-delay play,
stock clocks and 100% speed, deterministic dual-core GPU processing by default,
and background input
(`ModernGekko/tools/netplay_session.cpp:778-853`).

At historical audit commit `ff0ad952`, this was the only topology. It remains
available unchanged as Advanced Direct IP:

- `NetTraversalConfig direct{}` leaves `use_traversal` false
  (`ModernGekko/tools/netplay_session.cpp:872-875` and
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayProto.h:119-132`).
- The public Dolphin index is disabled
  (`ModernGekko/tools/netplay_session.cpp:821-824`).
- Host construction passes `forward_port=false`, so Dolphin's optional UPnP path
  is not requested (`ModernGekko/tools/netplay_session.cpp:880-885` and
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:175-178`).
- The host binds all local interfaces on the chosen UDP port. ENet is configured
  for ten transport peers, even though the game has four controller ports
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:153-163`).
- The host creates a normal client connected to `127.0.0.1`, so host and guests
  share the same client path (`ModernGekko/tools/netplay_session.cpp:924-932`).

Consequences of Advanced Direct IP:

- Same-LAN play normally needs no router setup.
- Internet hosting requires forwarding the selected UDP port or using a routed
  VPN such as Tailscale.
- There is no room code, relay, public room list, or password in this path.
- Address validation accepts IPv4-like text and hostnames, but not IPv6 literals
  because `:` is rejected (`ModernGekko/tools/frontend_config.cpp:80-85`).

The direct client waits up to five seconds for the ENet connection and then up
to another five seconds for the protocol response
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:125-174`
and `:241-269`). `enet_address_set_host`'s return value is ignored at `:145-150`,
so DNS/parse failure cannot be reported distinctly.

## LAN discovery

Discovery is a small Ring Out-specific UDP protocol, independent of Dolphin's
index/traversal system.

### Host beacon

An interactive host starts `DiscoveryBeacon` only after reaching the lobby
(`ModernGekko/tools/netplay_session.cpp:597-610`). Once per second it sends to:

- `255.255.255.255:2627` for the local broadcast domain.
- `127.255.255.255:2627` so a second instance on the same machine can discover
  the host.

The wire payload is unencrypted UTF-8 text:

```text
RINGOUT1 <netplay-port> <nickname>
```

The constants and send loop are at
`ModernGekko/tools/netplay_session.cpp:521-595`. The actual game traffic does not
use 2627; it uses the advertised session port, default UDP 2626.

### Joiner scan

The scan is available only in the in-game Join menu. A worker binds UDP 2627
with `REUSEADDR`, listens for 2.5 seconds, validates the `RINGOUT1` prefix and
port, and takes the address from the packet source rather than trusting the
payload (`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:881-977`).

The first result is adopted automatically; Left/Right over the Scan row cycles
the result list and updates both address and port
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:1443-1458`
and `:2260-2284`). This produces a sensible human flow:

1. Host enters its lobby and begins beaconing.
2. Joiner remains in offline play, chooses Join, and scans.
3. Joiner selects the host and starts netplay.
4. Joiner's offline runtime restarts into a direct connection.

Current limitations:

- Discovery is LAN/loopback only; broadcasts do not cross routers or most VPNs.
- Results deduplicate by address only, not `(address, port)`, so two hosts on one
  machine/IP collapse into one entry (`RecompMenu.cpp:968-972`).
- The beacon carries no build or module fingerprint, capacity, in-game state, or
  authentication. An incompatible or spoofed local beacon can appear in Scan;
  the later connection must reject it.
- The constants and payload layout are duplicated between `tools/` and
  `VideoCommon`; the comments explicitly warn that changing only one silently
  breaks discovery (`netplay_session.cpp:529-531` and `RecompMenu.cpp:881-884`).
- The scan worker is detached. It is bounded, but still writes global menu state
  after its caller may have begun shutdown (`RecompMenu.cpp:2265-2284`).
- There is no discovery test.

## Connection handshake and compatibility

### What is checked

The direct client sends:

1. `Common::GetScmRevGitStr()`.
2. Dolphin's displayed netplay revision string.
3. The nickname.

This is serialized in
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:241-250`.
The server rejects an unequal SCM revision, a game already starting/running, a
full server, or an overlong name
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:443-480`).
The exact SCM comparison is a useful hard gate: independently built binaries
from different commits cannot silently mix.

Once admitted, the server sends its selected `SyncIdentifier`. Ring Out creates
that identifier from `game_root/sys/main.dol`
(`ModernGekko/tools/netplay_session.cpp:855-875` and `:924-932`). Because this is
a DOL file, Dolphin hashes the entire file and compares its size, generated game
ID, revision/disc metadata, and SHA-1
(`ModernGekko/vendor/dolphin/Source/Core/UICommon/GameFile.cpp:677-724` and
`:727-784`). The client reports one of Same Game, Different Hash, Different
Disc, Different Revision, Different Region, Different Game, or Unknown; the
lobby renders that status in its Game column
(`ModernGekko/tools/netplay_session.cpp:297-314` and `:663-700`).

### The unused ModernGekko fingerprint

Ring Out also has `CompatibilityFingerprint`, which contains substantially more
relevant information:

- Ring Out netplay protocol marker.
- SCM revision.
- disc ID and inspected DOL SHA-256.
- ModernGekko module ABI and CPU ABI.
- `CPUState` size.
- module entry point, code/SMC/chunk ranges, and guest chunk hashes.

It is implemented at `ModernGekko/tools/netplay_compatibility.cpp:17-96` and has
pure-computation test coverage. It is **not called by the lobby or the network
protocol**. The protocol test's opening comment explicitly records that the
private fork's `SetCompatibilityFingerprint` handshake is absent from the
vendored Dolphin API
(`ModernGekko/tests/netplay_protocol_test.cpp:1-24`).

This is the largest compatibility hole. Two peers from the same commit with the
same `main.dol` can display `ready` while loading different recomp modules. That
may result in a later desync, a module rejection on one side, or divergent
behavior instead of a clear preflight error.

The correct fix is to add a Ring Out compatibility field to the connect
handshake, not merely display the fingerprint locally. On mismatch, the lobby
should identify the first differing component: release, game/DOL, module ABI,
CPU ABI/state size, or module content descriptor.

## Lobby window, roster, and controls

The lobby is a 760x520 resizable high-DPI SDL3/ImGui window. It initializes video
and gamepad support and enables both ImGui keyboard and gamepad navigation
(`ModernGekko/tools/netplay_session.cpp:316-378`). Mouse interaction is provided
normally by ImGui.

Explicit cancel inputs are:

- window close;
- Escape, ignoring repeats;
- controller East/B;
- controller Back/View.

They are handled at `ModernGekko/tools/netplay_session.cpp:396-415`.

The roster table shows Player, Ping, Controller, and Game. It marks the local
player and host, displays ping in milliseconds, finds the player's mapped GC
port, and renders game compatibility
(`ModernGekko/tools/netplay_session.cpp:631-700`). Dolphin sends a ping once per
second and broadcasts the measured value
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:254-278`
and `:860-879`). A peer with no acknowledgements is considered disconnected
after 30 seconds (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.h:18-21`).

### Roster snapshot defect

`NetPlayClient::GetPlayers()` locks the player map, builds a vector of pointers
to map values, releases the lock, and returns those pointers
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1159-1169`).
The network thread can then update or erase a player while the lobby reads name,
ping, status, and PID. Inserts into the map do not invalidate existing nodes,
but erasing a disconnected player invalidates that player's pointer; concurrent
field reads are also unsynchronized.

The original private API reportedly exposed `GetPlayersSnapshot`; the vendored
API does not (`ModernGekko/tools/netplay_session.cpp:1-24`). Ring Out should add
a by-value snapshot method under the client mutex and make the pad map a copied
part of the same snapshot.

## Pad assignment and capacity

The wrapper assigns one GC controller port to each player in ascending player-ID
order. Player IDs begin at one, making the host port 1 and subsequent joiners
ports 2-4 (`ModernGekko/tools/netplay_session.cpp:248-295`). It reassigns when
the observed player count changes and logs the local routing before boot. This
explicit mapping is important because an all-zero map means no input reaches the
game.

The protocol test verifies the server's ownership guard: the mapped owner may
send pad data, while another peer attempting to send for that port is dropped
(`ModernGekko/tests/netplay_protocol_test.cpp:246-297`). That is good protection
against accidental or malicious cross-port injection.

Capacity is inconsistent:

- `PadMappingArray` has four slots.
- The Ring Out mapping loop assigns only the first four players.
- The ENet server is created for ten peers.
- The server's application-level `ServerFull` check does not trigger until 255
  players (`NetPlayServer.cpp:450-454`), beyond the transport limit.
- The interactive lobby enables Start with one or more players, not with the
  requested/expected count (`netplay_session.cpp:721-728`).

A fifth reachable client can therefore enter the roster with no controller. If
spectators are desired, that needs to be explicit. Otherwise, admission should
reject a fifth active player with the already-defined `RoomFull` result.

## Readiness, delay buffer, and start

The following subsection records the state at the audit commit. It is
superseded on `codex/rollback-netplay` by the branch update above: that branch
has authoritative Ready/NotReady state and an exact-roster start gate.

There is no player Ready/Not Ready state. The code comments explicitly say that
the upstream API lacks the private fork's ready protocol
(`ModernGekko/tools/netplay_session.hpp:35-40` and
`ModernGekko/tools/netplay_session.cpp:13-18`). The lobby's `ready` text means
only `SyncIdentifierComparison::SameGame`; it does not mean the person has
confirmed that they are ready to start.

For an interactive host, Start becomes available when:

- every connected peer reports Same Game; and
- at least one player exists, which is always true for the host's local client.

Thus a host can start alone or before a guest has finished adjusting controls.
The `options.players` expected count is honored only by the headless path.

The host alone controls the input-delay slider. It immediately broadcasts a
1-20 queue target and estimates the delay as if each slot were a 60 fps video
frame (`ModernGekko/tools/netplay_session.cpp:703-717`). In reality input is
consumed at the variable SI rate, documented as typically 120 Hz
(`ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp:551-557`), so the
displayed milliseconds are not authoritative. The configured `auto` mode does
not observe ping or jitter. Both the server and lobby simply begin at five
samples (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:167-173`
and `ModernGekko/tools/netplay_session.cpp:612-619`). It should be renamed
`Default (5 samples)` until there is a real adaptive algorithm.

When Start is requested:

1. The host gathers and freezes synchronized NetSettings.
2. It collects and transfers save data when more than one player is present.
3. It transfers enabled Gecko and AR codes.
4. It waits for peers to acknowledge those synchronization phases.
5. It sends StartGame and boot settings.
6. Each client calls `StartGame`, which arms NetPlay and supplies
   `BootSessionData` to the runtime.

Ring Out enables save load/write, code synchronization, strict settings,
fixed-delay mode, stock overclock, and 100% emulation speed at
`ModernGekko/tools/netplay_session.cpp:813-840`. Dolphin's start coordinator is
at `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1381-1443`,
and the actual active-code transfer is at `:1927-2067`.

The lobby does not expose synchronization progress. `ShowChunkedProgressDialog`
only logs the transfer title, while progress and hide callbacks are empty
(`ModernGekko/tools/netplay_session.cpp:219-233`). The host's
`RequestStartGame()` return and the client's `StartGame()` return are also
ignored in the interactive lobby (`ModernGekko/tools/netplay_session.cpp:639-647`
and `:721-728`). A failure can therefore leave the user staring at an unchanged
lobby or a generic message rather than a clear failed phase.

## Headless lobby path

Headless sessions are intentionally flag-driven for automated two-instance
tests:

- Host waits for `--netplay-players`, default two, up to
  `--netplay-timeout`, default 120 seconds.
- After the player count arrives, host waits a separate 30 seconds for every
  peer to report Same Game.
- Host assigns pads, waits 500 ms for the asynchronous mapping broadcast, and
  requests Start.
- Joiner waits for the host's start signal.
- Every peer explicitly calls `StartGame`, asserts that NetPlay is armed, checks
  for boot data, and only then creates the runtime.

This defensive path is at `ModernGekko/tools/netplay_session.cpp:1006-1091`.
The explicit armed assertion prevents a dangerous false pass in which two
independent single-player sessions run without any desync checker.

The `lobby_timeout` option is not used by the interactive lobby. Consequently,
the 600-second menu-oriented setting in `moderngekko_run.cpp:452-460` has no
effect on an actual windowed lobby; ENet peer liveness and user cancellation are
the only bounds after connection.

### Online Room CLI and harness

The current runner exposes `--netplay-traversal` plus optional
`--traversal-server`, `--traversal-port`, and `--traversal-alt-port` overrides
(`ModernGekko/tools/moderngekko_run.cpp:31-47,157-213,354-379`). A manual
rollback pair can be started as follows from a package root:

```bash
# Host; copy the code shown in the lobby or logged as "online room code ...".
./bin/moderngekko-run --game ./game --module ./bin/gGRSEAF_recomp.so \
  --user-dir /absolute/path/to/host-user --controller "Standard Controller" \
  --netplay-host --netplay-traversal --netplay-mode rollback \
  --nickname Host --buffer auto

# Guest; replace 0123abcd with the host's code.
./bin/moderngekko-run --game ./game --module ./bin/gGRSEAF_recomp.so \
  --user-dir /absolute/path/to/guest-user --controller "Standard Controller" \
  --netplay-join 0123abcd --netplay-traversal --netplay-mode rollback \
  --nickname Guest --buffer auto
```

The existing two-input real-game harness now has an opt-in traversal route:

```bash
PKG=/absolute/path/to/RingOut-package \
RINGOUT_NETPLAY_TRAVERSAL=1 \
RINGOUT_ROLLBACK_PRODUCTION=1 \
bash .github/scripts/netplay-match.sh /tmp/ringout-online-room 60 2640
```

It starts the host, waits up to 30 seconds for the exact logged code, passes that
code to the guest, and still requires `netplay armed` from both peers before the
scripted match (`.github/scripts/netplay-match.sh:181-234`).
`RINGOUT_TRAVERSAL_SERVER`, `RINGOUT_TRAVERSAL_PORT`, and
`RINGOUT_TRAVERSAL_ALT_PORT` select a compatible controlled service. This is an
external-network integration test, not a hermetic per-change CI test.

On 2026-08-25, a separate minimal live check resolved
`stun.dolphin-emu.org`, received a valid version-0 hello and eight-hex code over
UDP 6262, acknowledged it, and received a valid registered-ping acknowledgement
from UDP 6226. It did not send `ConnectPlease`, connect two NAT peers, or boot
RingOut, so it proves current hosted-service wire compatibility rather than an
end-to-end player session. See [the connectivity document's exact
evidence](netplay-connectivity.md#live-hosted-service-compatibility-check).

## Disconnect, cancel, and error behavior

### Before or inside the lobby

Initial connection errors are stored as a string by `SessionUI::OnConnectionError`
(`ModernGekko/tools/netplay_session.cpp:167-189`). If construction fails,
`RunNetplayLobby` logs the string but returns only `HostUnavailable`
(`ModernGekko/tools/netplay_session.cpp:935-943`).

This discards Dolphin's typed reasons—server full, game running, version
mismatch, and nickname too long—even though the launcher contains tailored
messages for Ring Out's corresponding exit codes
(`ModernGekko/tools/moderngekko_launcher.cpp:862-891`). At the audited commit,
`VersionMismatch`, `RoomFull`, `GameRunning`, `ServerFull`, and
`NicknameRejected` are defined in `netplay_session.hpp:16-26` but are never
returned by `RunNetplayLobby`.

Inside the lobby, a lost connection logs an error and returns the same `false`
as Escape, B/Back, Quit, or closing the window
(`ModernGekko/tools/netplay_session.cpp:621-629`). The caller maps every false
result to successful exit code zero (`:950-963`). The desktop parent therefore
does not show its failure dialog, and an in-game-menu start simply ends the
application. Cancellation does not return to the launcher or reconstruct an
offline runtime.

Failure to create the SDL lobby window has another definite bug:
`RunLobbyWindow` logs "falling back to auto-start" and returns true
(`ModernGekko/tools/netplay_session.cpp:601-605`), but no start was requested.
The caller immediately finds that NetPlay is not armed and fails at `:964-973`.

### During a running match

A connection loss sets an error and asks the runtime to stop
(`ModernGekko/tools/netplay_session.cpp:167-175`). A desync records the frame,
stops the runtime, and later forces a failure result (`:151-165` and `:991-995`).

Connection loss does not similarly set the final result. If the runtime exits
without its own error, the session can log `session ended cleanly, no desync
reported` and return zero despite `ui.ConnectionLost()` being true
(`ModernGekko/tools/netplay_session.cpp:982-1003` and `:1107-1120`). A network
failure must be a distinct unsuccessful outcome.

### Busy host port

Busy-port handling is unusually good: interactive hosting offers Join, Retry,
or Quit instead of dropping the user at stderr
(`ModernGekko/tools/netplay_session.cpp:431-494` and `:877-910`). One detail is
wrong: `Join that host instead` uses `options.address`, which may be the last
remote address saved in config. A locally occupied listening port should join
`127.0.0.1` or explicitly let the user choose; it cannot infer that the stale
configured remote address owns the local socket.

## Security and trust boundaries

Both connection methods are appropriate for trusted peers, not an adversarial
public service. Online Room avoids asking players to share an IP manually, but
the Dolphin service and eventual direct opponent still learn the public
endpoint. Its 32-bit room code is not authentication, and there is no relay or
encrypted gameplay envelope. At source level:

- The direct path configures no Ring Out password or authentication handshake.
- Online Room uses Dolphin's unauthenticated version-0 rendezvous protocol and
  then the same direct ENet transport; it does not use Dolphin's public index.
- LAN beacons are unauthenticated and can be spoofed by any local sender.
- The compatibility gate proves a build revision and selected DOL, not a peer's
  identity.
- Invalid pad ownership and unknown protocol messages are rejected by the
  server, which is a meaningful safety property.
- The host controls synchronized settings, save data, and active cheat codes.

This audit did not perform a cryptographic review of ENet/SFML serialization,
fuzz malformed network packets, or expose the port to an untrusted network.
No claim of confidentiality, authentication, or hostile-network hardening is
made.

## Defect register

This table remains the historical defect register for `ff0ad952`. Later branch
sections above record which items have been completed or partially superseded;
do not reinterpret the original rows as current source claims.

| Priority | Defect | User impact | Evidence |
| --- | --- | --- | --- |
| P0 | ModernGekko compatibility fingerprint is not exchanged. | Same build/DOL but different recomp modules can enter and fail later. | `tools/netplay_compatibility.cpp:67-96`; `tests/netplay_protocol_test.cpp:1-24` |
| P0 | Roster returns live pointers after unlocking. | Join/leave can race rendering, producing stale reads or a dangling pointer. | `NetPlayClient.cpp:1159-1169` |
| P0 | Connection loss can return success. | A failed lobby/match may silently close and be reported as clean. | `netplay_session.cpp:167-175`, `:621-629`, `:950-963`, `:991-1003` |
| P0 | Typed connection failures collapse to HostUnavailable. | Version/full/running/name failures all look like firewall trouble. | `netplay_session.cpp:935-943`; `moderngekko_launcher.cpp:862-891` |
| P1 | No real readiness or interactive expected-player count. | Host can start alone or before the guest is ready. | `netplay_session.hpp:35-40`; `netplay_session.cpp:721-728` |
| P1 | Four active slots are not enforced. | Fifth-through-tenth transport peers may join with no controller. | `netplay_session.cpp:271-294`; `NetPlayServer.cpp:153-163`, `:450-454` |
| P1 | "Automatic" buffer is fixed at five SI samples, while the UI calls them 60 fps frames. | Users expect adaptation and see an inaccurate millisecond estimate. | `netplay_session.cpp:612-619`, `:703-717`; `SI.cpp:551-557`; `NetPlayServer.cpp:167-173` |
| P1 | Interactive start return values are ignored. | Save/code/start failure has weak or stuck-looking feedback. | `netplay_session.cpp:639-647`, `:721-728` |
| P1 | No-window "auto-start" fallback does not start. | Display initialization failure becomes a misleading generic failure. | `netplay_session.cpp:601-605`, `:964-973` |
| P1 | Cancellation exits instead of returning to offline/launcher UI. | Escape/B can feel like the game closed unexpectedly. | `netplay_session.cpp:396-410`, `:950-963`; `moderngekko_run.cpp:397-478` |
| P1 | Direct internet path has no traversal, relay, or UPnP. | Manual UDP forwarding/VPN is required. | `netplay_session.cpp:821-824`, `:872-885` |
| P2 | Discovery deduplicates only by IP. | Multiple hosts on one IP collapse into one row. | `RecompMenu.cpp:968-972` |
| P2 | Discovery schema/constants are duplicated. | A one-sided edit silently breaks Scan. | `netplay_session.cpp:529-531`; `RecompMenu.cpp:881-884` |
| P2 | Discovery worker is detached. | Shutdown can race a late write to global menu state. | `RecompMenu.cpp:2265-2284` |
| P2 | Desktop launcher has no Scan and no ImGui gamepad navigation. | Controller-only desktop users cannot complete the whole setup consistently. | `moderngekko_launcher.cpp:369-378`, `:682-695` |
| P2 | DNS resolution result is ignored. | Bad hostname failures cannot be diagnosed precisely. | `NetPlayClient.cpp:145-150` |
| P2 | No synchronization progress UI. | Start may appear idle during save/code transfer. | `netplay_session.cpp:219-233`; `NetPlayServer.cpp:1381-1443` |
| P3 | Port range differs between UIs. | In-game editor clamps to 1024-65535 while launcher accepts 1-65535. | `RecompMenu.cpp:1465-1471`; `frontend_config.cpp:103-111` |
| P3 | Child log retains a historical product name. | Users may not recognize `KirbyRecomp.log` as the Ring Out log. | `moderngekko_launcher.cpp:809-813` |

## Prioritized improvement plan

### P0: correctness and truthful failure handling

1. **Wire the compatibility fingerprint into connect.** Extend the initialization
   packet with a versioned Ring Out handshake payload. Reject before roster
   admission and return a structured mismatch containing release, DOL, ABI, and
   module components.
2. **Add a copied lobby snapshot API.** Copy players and pad mapping under one
   mutex. Never render or assign from pointers into the network-owned map.
3. **Introduce a typed lobby result.** At minimum: Started, Canceled,
   HostUnavailable, VersionMismatch, CompatibilityMismatch, RoomFull,
   GameRunning, NicknameRejected, ConnectionLost, TimedOut, and SyncFailed.
   Preserve Dolphin's typed `ConnectionError` rather than translating to and
   reparsing localized strings.
4. **Treat mid-game connection loss as failure.** Set a nonzero final result and
   show which peer disconnected. Do not print `cleanly` when
   `ConnectionLost()` is true.

### P1: complete the friend-to-friend lobby

5. **Completed on the rollback branch: add Ready/Not Ready.** The table separates
   `game compatible` from player Ready, Start requires the exact configured
   roster to be compatible, mapped, and ready, and mapping/delay mutations
   invalidate Ready.
6. **Define capacity explicitly.** Reject a fifth active player with RoomFull, or
   add an explicit spectator role with no controller and clear start semantics.
7. **Make buffer behavior honest.** Rename the current option to `5 SI samples
   (default)` immediately and derive milliseconds from the measured poll rate.
   If automatic mode is desired, base recommendations on a rolling RTT/jitter
   sample, apply hysteresis, show the chosen value, and let the host override it.
8. **Add connect/start state UI.** Open UI before dialing; show Resolving,
   Connecting, Handshaking, Synchronizing save, Synchronizing codes, and Booting.
   Check every start return value and allow retry/back where safe.
9. **Return from cancellation.** Keep or recreate a parent launcher loop. For an
   in-game request, cancel should offer `Return to offline game` and reconstruct
   a fresh offline runtime rather than ending the process.
10. **Fix the no-window path.** Either enter the actual headless auto-start state
    machine or report a precise lobby-window error. Do not claim a fallback that
    did not occur.

### P2: connectivity and discovery

11. **Partially completed on the rollback branch: offer an Internet convenience
    path.** Online Room beta now supplies Dolphin traversal/NAT punch-through
    and a short host code. It still needs representative NAT validation and a
    relay fallback; the eventual production invite must be versioned and bind
    connection method, room identity, and compatibility rather than reusing the
    unauthenticated 32-bit Dolphin code.
12. **Share and version discovery.** Move magic, schema, and parser into a common
    library used by both tools and VideoCommon. Deduplicate by `(address, port)`,
    include release/fingerprint/capacity/start state, and ignore incompatible
    hosts before presenting them.
13. **Manage the scan worker.** Replace the detached thread with a `std::jthread`
    owned by menu state, cancel/join it during shutdown, and expose bind/listen
    failures instead of displaying only `NONE FOUND`.
14. **Unify launcher and in-game UX.** Add Scan, gamepad navigation, saved-host
    history, local-address display, and a copyable invite to the desktop launcher.

### P3: polish and diagnostics

15. Display ping distribution/jitter and packet-loss counters, not only one RTT.
16. Show each peer's release, compatibility fingerprint prefix, controller role,
    and synchronized-data progress.
17. Add host kick support using the backend's existing `KickPlayer` method.
18. Rename the log to `RingOut.log`, retain bounded previous logs, and link/copy
    the log path from failure dialogs.
19. Standardize allowed port ranges and explain that both session UDP and LAN
    discovery UDP may need local firewall permission.

## Test evidence and missing coverage

### Verified in this audit

The existing `build-appimage/moderngekko_netplay_protocol_test` was run outside
the network sandbox against the audit commit and exited zero. Its source verifies:

- invalid-host failure produces an error;
- a host and two local clients exchange roster data;
- pad-buffer changes make a real protocol round trip;
- mapped pad ownership is accepted;
- a peer attempting to send for another player's port is disconnected.

The relevant assertions are in
`ModernGekko/tests/netplay_protocol_test.cpp:178-305`.

### Existing broader harnesses

`.github/scripts/netplay-local.sh` launches two headless peers, waits for the
load-bearing `netplay armed` marker, can compare every frame's guest-RAM hash,
and reports desyncs (`.github/scripts/netplay-local.sh:89-193`).

`.github/scripts/netplay-match.sh` drives both peers through character select
and into a VS match, requiring useful input from both controller owners before
comparing RAM hashes (`.github/scripts/netplay-match.sh:1-20` and `:89-175`).

The current branch extends that script with
`RINGOUT_NETPLAY_TRAVERSAL=1`: it waits for the host's code, starts the guest
through the same traversal CLI, and retains the existing armed/match/oracle
requirements (`.github/scripts/netplay-match.sh:181-234`). The route is
implemented. `RINGOUT_NETPLAY_DIAGNOSTICS=1` passes the same detailed logging
flag as the launcher. A final rebuilt live run received Dolphin code `f2a7304d`
after one second. Its two process logs prove host registration, host punch,
guest `ConnectReady`, and the subsequent ENet timeout; evidence is retained at
`/tmp/ringout-two-instance-hosted-diagnostics-final-20260825`. This demonstrates
absent same-host reachability without identifying one specific NAT or firewall
cause. The separate
`.github/scripts/test-dolphin-traversal-live.py` smoke completed the hosted
`ConnectPlease`/`PleaseSendPacket`/`ConnectReady` exchange, but also observed no
same-host punch. Neither result is a successful cross-network RingOut match.

The ISO-backed Direct regression after these changes did reach a two-controller
VS Battle, activated production rollback on both peers, accepted scripted input
from both controller owners, and completed 690 retained physical-frame rows.
Both peer logs contain Dolphin's detailed NETPLAY trace. Evidence is retained at
`/tmp/ringout-two-instance-direct-diagnostics-20260825b`.

Exact commands for those two controlled-instance runs were:

```bash
RINGOUT_ROLLBACK_PRODUCTION=1 \
RINGOUT_NETPLAY_DIAGNOSTICS=1 \
PKG=/tmp/ringout-traversal-package.KXrClx57 \
bash .github/scripts/netplay-match.sh \
  /tmp/ringout-two-instance-direct-diagnostics-20260825b 5 32626

RINGOUT_NETPLAY_TRAVERSAL=1 \
RINGOUT_ROLLBACK_PRODUCTION=1 \
RINGOUT_NETPLAY_DIAGNOSTICS=1 \
PKG=/tmp/ringout-traversal-package.KXrClx57 \
bash .github/scripts/netplay-match.sh \
  /tmp/ringout-two-instance-hosted-diagnostics-final-20260825 5 32629
```

Those scripts are valuable manual integration evidence, but neither is invoked
by the release workflows at the audited commit. The CTest protocol test is
automated on Linux/Deck builds; it deliberately does not boot a core or assert
received in-game pad data (`ModernGekko/tests/netplay_protocol_test.cpp:260-264`).

### Required new tests

- Handshake rejects same DOL with different module fingerprint.
- Every structured connect failure survives to the visible launcher message.
- Interactive Cancel and Connection Lost produce different results.
- Mid-game disconnect returns failure and never logs a clean session.
- Four active peers work; a fifth is rejected or explicitly spectates.
- Ready state resets and gates Start correctly.
- Discovery round-trip on LAN and loopback, malformed beacon handling,
  incompatible beacon filtering, and two ports on one IP.
- Address resolution failure and retry.
- Save/code synchronization success, failure, progress, and cancellation.
- Rapid join/leave while the roster is rendered, under ThreadSanitizer where
  supported.
- Gamepad-only navigation from launcher through lobby cancellation and boot.
- Native Windows two-machine test with real controllers, firewall prompts,
  focus changes, suspend/resume, and host/client disconnects.
- Online Room across two permissive consumer NATs, port-restricted NAT, strict
  stateful NAT, double NAT/CGNAT, and blocked UDP, with typed expected outcomes.
- Invalid, expired, and unknown room codes; hosted-service outage before join;
  service loss after a direct peer route is established.
- Packet capture proving the rendezvous server is not on the gameplay path and
  that the UI never claims relay or peer-IP privacy.

## Claim boundaries

The body remains a source-led historical audit of commit
`ff0ad952980f5083afd21c3d3758208a7a093d72`, plus one successful run of the
existing localhost protocol test. The explicitly labelled branch-update
sections describe committed checkpoint
`05798a513f34a02ea5983daa65521cd36532cc0f` plus the Online Room snapshot
integrated on 2026-08-25.

The historical `ff0ad952` audit does **not** claim:

- that rollback exists at that audit commit; the later branch sections supersede
  that historical mode verdict;
- physical Windows controller, firewall, GPU/audio, or two-machine validation;
- a complete two-peer hosted gameplay or representative NAT-matrix pass; the
  main/alternate-port handshake and full hosted rendezvous exchange passed, but
  same-host UDP hairpinning did not;
- adversarial-network security or encryption;
- automated coverage of the SDL/ImGui lobby, LAN discovery, request/restart UX,
  save/code transfer UI, or native Windows lifecycle;
- that historical 6,470-frame/manual-match results were reproduced during this
  audit.

The current branch can attempt direct peer introduction through an eight-digit
Dolphin code and preserves Advanced Direct IP when peers can directly reach UDP
2626. It still requires packaged, physical, cross-network, and representative
NAT validation, and it is not a substitute for the authenticated ICE/TURN
production architecture in [netplay-connectivity.md](netplay-connectivity.md).
