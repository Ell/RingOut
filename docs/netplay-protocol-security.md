# Netplay protocol, security, and reliability audit

Audit target: `ff0ad952980f5083afd21c3d3758208a7a093d72`
Audit date: 2026-08-25
Method: static source review of RingOut and its vendored Dolphin/SFML/ENet paths. No
claim in this document depends on a later commit. Exploitability findings are based
on reachable parser and filesystem behavior; no weaponized peer was run.

## Scope and claim boundaries

- This is a source audit of the direct RingOut frontend path at the exact commit
  above. Vendored traversal, index, UPnP, Qt, and unrelated Dolphin modes are not
  claimed to be active merely because their code exists. RingOut constructs a
  direct traversal configuration and disables the index at
  `ModernGekko/tools/netplay_session.cpp:821-824` and
  `ModernGekko/tools/netplay_session.cpp:872-884`.
- “Remote” parser findings require a peer that has established an ENet connection.
  Server-side findings assume the joiner supplies the public matching source
  revision; client-side findings assume the user connects to a hostile server.
  This audit does not claim that an arbitrary off-path UDP sender can inject an
  established ENet stream.
- The GCI and LZO findings are code-path reachability findings, not a claim that a
  cooperative RingOut host selects every save subtype in ordinary play. A server
  can dispatch `SyncSaveData` subtypes to any non-host client without first proving
  that the subtype matches the configured game/save mode
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:448-449`,
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientSaves.cpp:23-58`).
- “Potential code execution” in NP-03 is the conservative consequence class for an
  attacker-influenced out-of-bounds write. No exploitability chain or code execution
  was demonstrated. Path traversal impact is likewise bounded by the client's OS
  permissions, filesystem semantics, and reachable path from the data directory.
- The absence of authentication/encryption refers to the reviewed application
  handshake and ENet packet path. Passive disclosure follows from plaintext;
  successful active on-path injection was not tested.
- Performance and network-quality conclusions follow the queue/transport design.
  They are not field measurements across representative Internet links. Existing
  determinism soak results are useful engineering evidence but are not a substitute
  for the hostile-input, cross-platform, and impairment tests specified below.

### Reproducing the source audit

Prerequisites are Git, ripgrep, GNU `nl`, and `sed`; no game image, compiler, or
network access is required once the audited checkout is present. Run from the
repository root:

```sh
test "$(git rev-parse HEAD)" = "ff0ad952980f5083afd21c3d3758208a7a093d72"

rg -n "fixeddelay|SetHostInputAuthority|m_target_buffer_size|SendAsync" \
  ModernGekko/tools/netplay_session.cpp \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay

rg -n "while \(!packet\.endOfPacket\(\)\)|endOfPacket|checkSize" \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay \
  ModernGekko/vendor/dolphin/Externals/SFML/SFML/src/SFML/Network/Packet.cpp

rg -n "OnSyncSaveDataGCI|DecompressPacketIntoBuffer|lzo1x_decompress" \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay

rg -n "GetScmRevGitStr|CompatibilityFingerprint|IsHost|StopGame|PowerButton|TimeBase" \
  ModernGekko/tools ModernGekko/tests \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay

nl -ba ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp | sed -n '551,558p'
```

The commands locate the evidence but do not constitute dynamic exploit tests. The
verification plan at the end of this document defines the missing executable proof.

## Later rollback-branch protocol update (2026-08-25)

Everything below this section remains the historical `ff0ad952` security audit.
The current `codex/rollback-netplay` worktree has closed two compatibility/mode
gaps for RingOut peers, but it has not changed the trusted-network threat model.

- RingOut adds a strict version-2 connect extension containing explicit
  rollback-capable/requested bits and a bounded compatibility fingerprint. A
  RingOut server requires the extension in both modes, compares the complete
  fingerprint and requested mode before allocating a player ID, and rejects a
  rollback capability/mode mismatch
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayConnectProtocol.h:17-95`;
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:463-503`).
  The fingerprint is computed from the inspected game/runtime/module identity
  at `ModernGekko/tools/netplay_session.cpp:1040-1051`. It is compatibility
  evidence only: plaintext, self-asserted data is not peer authentication.
- Rollback input is a separately bounded/versioned SI-batch format on its own
  unreliable-sequenced ENet channel. The server rejects invalid size,
  generation, session, or pad ownership before relay
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1010-1045`).
  Lobby, save, code, and other controls remain on the existing reliable path.
- Rollback protocol v2 requires a compact confirmed-state report every 60
  logical frames. A client reports only after the frame's final SI dependency
  is covered by its authoritative frontier and no correction/replay is pending.
  The server groups by session generation, frame, and expected player; rejects
  malformed, duplicate, stale, future, wrong-session, and unexpected-player
  reports; and retains at most eight incomplete frames. A mismatch stops every
  peer through the existing desync route
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/RollbackStateDigestProtocol.h:20-157`;
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:198-317`;
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1057-1110,1812-1850`).
  The values cover real MEM1 CRC32, locked-L1 CRC32, and emulated timebase. They
  are an error detector, not authentication, confidentiality, or a full-state
  serialization.
- A requested rollback session refuses a silent fixed-delay downgrade in both
  interactive and headless start paths
  (`ModernGekko/tools/netplay_session.cpp:668-683,1296-1306,1348-1357`). Fixed
  delay remains available only by having all players select it and reconnect.
- Rollback start forces persistent save writes and writable SD off and requires
  a GameCube-only, safe EXI/GBA production policy
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1127-1146,1644-1697`;
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:143-188`).
  This prevents rollback speculation from being copied to the user's save; it
  does not retroactively prove hostile save-transfer parsing safe.

No room secret, authenticated identity, AEAD, relay, or NAT-traversal service
has been added. Use both fixed-delay and rollback only with mutually trusted
peers on a LAN or private VPN. The historical parser/save-transfer findings
below remain blockers for public Internet rooms unless separately closed and
retested.

## Executive summary

RingOut's shipping netplay is fixed-delay deterministic lockstep, not rollback.
Each client samples its local controller, pads its local input queue to the
host-selected target, sends the samples to the host, and blocks emulation whenever
the next mapped input is unavailable. The host validates controller ownership and
relays the input to the other clients. RingOut explicitly selects `fixeddelay` and
disables host input authority (`ModernGekko/tools/netplay_session.cpp:821-824`,
`ModernGekko/tools/netplay_session.cpp:912-923`; input path at
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:50-77`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:285-308`;
relay at
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:682-729`).

The protocol is suitable only for mutually trusted peers until the critical parser
and save-transfer issues below are fixed. A malicious matching-version client can
wedge the server's network thread with a truncated pad packet. A malicious host can
provide a path-traversing GCI filename and can drive decompression paths that trust
an attacker-controlled allocation size and do not bound LZO output. The protocol
also has no peer authentication or encryption, derives host identity from whichever
connection receives player ID 1, accepts disruptive control messages from ordinary
clients, and permits more connections than there are playable controller slots.

Reliability is adequate for cooperative peers on a stable LAN, but loss causes
retransmission and head-of-line waiting rather than prediction or rollback. Native
desync detection compares only the emulated timebase once every 60 frames, not RAM
or complete deterministic state. Compatibility checks cover the source revision
and `main.dol`, but the stronger recompiler-module fingerprint is test-only and is
not part of the connection handshake.

## Threat model

### Assets to protect

- Availability of the host, clients, UI, emulator threads, and running match.
- Integrity and containment of files under the user's RingOut/Dolphin data roots.
- Integrity of controller ownership, lobby authority, synchronized settings,
  synchronized saves/codes, and deterministic game state.
- Confidentiality of input, chat, lobby identity, and transferred save data on an
  untrusted network.
- Accurate failure reporting: a malformed, incompatible, or disconnected session
  must stop explicitly rather than hang, silently continue, or report a clean run.

### Actors and trust boundaries

- **Malicious joiner:** can reach the exposed host port and can speak enough of the
  protocol to pass the source-revision check.
- **Malicious host:** controls the server and all server-to-client messages,
  including settings, save data, and codes. This is the strongest remote attacker
  because RingOut enables save loading/writing and code synchronization at
  `ModernGekko/tools/netplay_session.cpp:813-823`.
- **On-path observer/attacker:** can observe plaintext application traffic; there
  is no application-layer authentication, encryption, or replay protection in the
  reviewed handshake.
- **LAN discovery spoofer:** can advertise a fake room to a client. Discovery
  accepts the public `RINGOUT1` beacon and address data without a cryptographic
  proof (`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:945-972`).
- **Accidental faults:** packet truncation, resolver failure, loss, jitter,
  disconnects, incompatible local modules, and concurrent lobby updates.

The current exact source-revision comparison is a compatibility check, not an
authentication boundary. It is public information and provides no secret proof of
room membership or host identity (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:241-250`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:443-464`).

## Current packet path and transport behavior

1. RingOut constructs a direct server bound to `ENET_HOST_ANY`, then constructs the
   host's own loopback client. A joining client also uses direct mode; RingOut does
   not enable traversal/index service in this path
   (`ModernGekko/tools/netplay_session.cpp:872-884`,
   `ModernGekko/tools/netplay_session.cpp:924-933`; server bind at
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:153-173`).
2. The client sends source revision, Dolphin-version label, and nickname. The
   server compares only the source revision before assigning the first available
   numeric player ID (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:241-250`,
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:443-480`,
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:2145-2157`).
3. A client serializes `PadData` on the default ENet channel. In fixed-delay mode,
   it repeats the current sample until its queue is above the target buffer
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:50-77`,
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:298-305`;
   the default-channel declaration is
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.h:110-117`).
4. The server validates that each pad index is mapped to the sender, then relays the
   packet to every other client (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:682-729`).
   This ownership check is a real positive control and has a localhost regression
   test (`ModernGekko/tests/netplay_protocol_test.cpp:246-298`).
5. Each client enqueues received samples. The emulation thread waits indefinitely
   for a mapped queue to become nonempty, unless stopping wakes it
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:674-694`,
   `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:105-115`).

There are two ENet channels: `DEFAULT_CHANNEL` and `CHUNKED_DATA_CHANNEL`
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayProto.h:226-235`).
Large save/code transfers use the chunked channel
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServerChunked.cpp:167-177`),
but inputs and ordinary control messages share the default channel. Every packet on
both channels is created with `ENET_PACKET_FLAG_RELIABLE`
(`ModernGekko/vendor/dolphin/Source/Core/Common/ENet.cpp:40-63`). Therefore:

- lost input is retransmitted in order;
- a missing earlier input/control packet blocks later reliable packets on the same
  channel;
- empty input queues stall emulation rather than predict a value; and
- the buffer absorbs bounded jitter but cannot mask sustained loss or a peer that
  stops sending. ENet's configured no-acknowledgement timeout is 30 seconds
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.h:18-21`).

The server defaults to a target of five input samples/queue slots
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:167-173`).
The lobby exposes 1-20 but incorrectly labels those slots as “frames” and converts
them to milliseconds at 60 Hz (`ModernGekko/tools/netplay_session.cpp:703-716`).
The actual samples are produced at the SI input-poll rate, which is variable and
typically 120 Hz (`ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp:551-558`).
Therefore the displayed latency is not a valid general conversion. `--buffer auto`
is also not adaptive: it simply leaves the five-slot server default unchanged
(`ModernGekko/tools/netplay_session.cpp:912-923`).

## Findings

| ID | Severity | Finding | Primary impact |
| --- | --- | --- | --- |
| NP-01 | Critical | Failed packet reads can leave repeated-record parsers in an infinite loop | Remote denial of service of host or client network thread |
| NP-02 | Critical | Host-controlled GCI filenames are appended without containment validation | Client-side path traversal and file overwrite within process permissions |
| NP-03 | Critical | Save decompression trusts declared allocation sizes and uses unbounded LZO output | Memory exhaustion, out-of-bounds write, crash, potential code execution |
| NP-04 | High | No authentication/encryption; player ID 1 is race-assigned as host | Room hijack, impersonation, traffic disclosure, amplified trust in wrong peer |
| NP-05 | High | Client authority and room capacity are not constrained to the gameplay model | Any peer can stop a match; excess peers consume slots/resources |
| NP-06 | High | Desync reports cover only timebase every 60 frames and are not uniqueness/window checked | Silent state divergence, false desync, and unbounded report-state growth |
| NP-07 | High | Strong ModernGekko/module compatibility fingerprint is not in the handshake | Same-revision peers can boot incompatible recomp modules and desync |
| NP-08 | High | Lobby exposes raw player pointers after releasing their lock | Join/leave race, use-after-free, crash or corrupted lobby display |
| NP-09 | Medium | Resolver, handshake, send, disconnect, and reconnect failures are not fail-closed consistently | Undefined behavior, stale connection state, long stalls, poor diagnosis |
| NP-10 | Medium | Reliable ordered input shares the control channel; buffer units are mislabeled and not adaptive | Head-of-line stalls, misleading latency display, and avoidable delay under jitter/loss |

Severity assumes an Internet-reachable host or joining an untrusted host. For a
strictly private, mutually trusted LAN, NP-02 through NP-05 are less likely to be
intentional attacks, but malformed-data and accidental-fault consequences remain.

### NP-01: invalid reads can wedge packet parsers

Both server and client pad parsers use `while (!packet.endOfPacket())` and extract
an entire record without checking packet validity after any field
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:691-714`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:674-694`).
The same pattern appears in related pad/remote/chunk payload handlers at
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:741-760`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:775-790`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:591-597`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:699-723`, and
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:728-744`.

SFML's `Packet::checkSize` marks the packet invalid on an undersized read but does
not move `m_readPos`; `endOfPacket` checks only whether `m_readPos` reached the data
size (`ModernGekko/vendor/dolphin/Externals/SFML/SFML/src/SFML/Network/Packet.cpp:86-95`,
`ModernGekko/vendor/dolphin/Externals/SFML/SFML/src/SFML/Network/Packet.cpp:572-578`).
Once invalid, later extraction checks remain false. A concrete trigger
is a valid owned pad index followed by only one byte of the two-byte button field:
the button read fails while one unread byte remains, the read position never
advances, and the loop condition remains true forever. A malicious joiner can wedge
the host; a malicious host can wedge each client.

The outer dispatchers are also not fail-closed: they extract an uninitialized
`MessageID` and switch on it without testing the packet
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:629-642`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:337-345`).
The connection reply similarly extracts into an uninitialized `ConnectionError`
and branches without checking `rpac` (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:252-302`).

**Required fix:** parse through a bounded reader that returns an error for every
failed field; require exact record boundaries and a maximum record count; reject
trailing partial bytes; and disconnect the offending side on the first invalid
read. Never continue parsing a false `sf::Packet`. Initialize all enums before
reading, but treat initialization as defense in depth rather than a substitute for
validity checks.

### NP-02: GCI save filename traversal

RingOut enables host save sync for every netplay session
(`ModernGekko/tools/netplay_session.cpp:813-816`). During GCI sync, the client reads
`file_name` from the host and writes to `path + DIR_SEP + file_name` without
rejecting absolute paths, `..`, either path separator, drive/UNC syntax, or Windows
alternate-stream syntax
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientSaves.cpp:111-143`).
The adjacent folder decoder demonstrates that name validation was intended for
other transfer types—it rejects separators and all-dot components—but that logic is
not used by GCI sync
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.cpp:228-257`).

Because `DecompressPacketIntoFile` opens the resulting path with `"wb"`, a hostile
host can create or truncate a file reachable from the data root within the client's
OS permissions (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.cpp:174-185`).

**Required fix:** accept only one normalized filename component; reject empty,
absolute, rooted, `.`/`..`, slash, backslash, colon, drive/UNC, and reserved Windows
device names on every platform. Resolve the candidate beneath a canonical staging
directory and prove containment before opening it. Write to a new temporary file,
validate the complete transfer, then atomically rename. Never follow symlinks out
of the staging root.

### NP-03: unsafe allocation and LZO output accounting

`DecompressPacketIntoBuffer` reads an attacker-controlled `u64` and immediately
allocates that many bytes. It then calls `lzo1x_decompress` with `&out_buffer[i]`
without supplying remaining capacity, checking `new_len` against the remaining
declared size, checking addition overflow, or proving the final output exactly
matches the declaration
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.cpp:269-310`).
This permits memory-exhaustion attempts and makes malformed compressed chunks an
out-of-bounds-write risk. Host-controlled Mii and Wii save payloads reach this path
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientSaves.cpp:175-193`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientSaves.cpp:243-264`).

The file decoder has a fixed 64 KiB output buffer but likewise calls the unsafe
decoder and never verifies per-chunk output or total bytes against the declared
file size (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.cpp:174-225`;
buffer constants at
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.cpp:18-19`). The
compressed input length is capped, but compressed
input length is not an output bound.

These handlers are not restricted to the save type an honest host would select:
the server-supplied subtype directly selects raw, GCI, Wii, or GBA handling for any
client whose PID is not treated as host
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientSaves.cpp:23-58`).

**Required fix:** impose protocol-level maximums before allocation; use
`lzo1x_decompress_safe` (or an equivalently bounded decoder) with the exact remaining
output capacity; reject overflow, zero-progress, premature terminator, trailing
data, and total output not equal to the declared size. Bound per-file, file-count,
per-session, folder-depth, and total transfer sizes. Abort the whole sync and remove
staging output after any violation.

### NP-04: unauthenticated peers, plaintext traffic, and host-ID race

The handshake proves only that the peer sent the expected public source revision.
There is no room secret, password proof, nonce, authenticated transcript, host key,
or application-layer encryption
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:241-250`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:443-480`).
ENet reliability does not provide cryptographic identity or confidentiality.

Both client and server define the host as player ID 1
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.h:98-108`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.h:80-96`). The
server begins accepting on all interfaces and starts its network thread in its
constructor, while RingOut constructs the loopback host client only after that
constructor returns (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:153-173`,
`ModernGekko/tools/netplay_session.cpp:880-928`). IDs are allocated starting at 1
to the first connection (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:2145-2157`).
Consequently, a remote connection in that window can claim the identity that other
code treats as host.

**Required fix:** reserve a server-local host identity before exposing the socket;
do not infer authority from connection order or a gameplay PID. Add a random
high-entropy room token and a nonce-bound authenticated handshake before allocating
player state or transferring data. Authenticate the protocol version,
compatibility fingerprint, requested role, and transcript. Add an audited AEAD
transport/session layer if confidentiality and on-path integrity are goals; document
clearly if the product remains trusted-friend/plaintext only.

### NP-05: ordinary peers have disruptive authority; capacity exceeds play slots

Any connected client can send `StopGame`; the server stops and broadcasts it
without a host/role check. Any client can also broadcast `PowerButton`
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:882-901`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:928-933`). Pad
ownership is correctly enforced, but control-plane authorization is not.

The direct ENet host accepts 10 peers, the protocol's server-full guard is 255, and
the pad mapping contains only four entries
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:153-173`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:443-464`;
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayProto.h:226-240`).
RingOut assigns at most one of those four ports to each lowest PID
(`ModernGekko/tools/netplay_session.cpp:248-295`). Extra peers can consume transport
and synchronization resources without being playable.

**Required fix:** define a message-by-role authorization matrix and enforce it in
the server dispatcher. Only the server-local host should start/stop the match,
power off, change mapping/buffer/game, kick, or initiate save/code sync. Clients
should be limited to their owned input, bounded status/capability replies, and
sync acknowledgements. Reject joins above the configured room size before creating
player or transfer state; rate-limit connection attempts, chat, status, ping, and
desync reports.

### NP-06: weak and abuseable desync detection

The client sends only `GetFakeTimeBase()` once every 60 emulated frames
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1623-1640`).
The server groups reports by caller-provided frame and compares only those timebase
values (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:936-977`).
Two peers can therefore diverge in RAM while their timebase remains equal. The
manual harness calls this out and optionally hashes all guest RAM every frame
(`.github/scripts/netplay-local.sh:89-104`).

The server does not enforce one report per PID per frame, a moving frame window, or
a cap on outstanding frame keys. Duplicate reports can satisfy the player-count
threshold prematurely; arbitrary never-completed frame numbers can grow
`m_timebase_by_frame`; and a malicious peer can force misleading results.

**Required fix:** accept exactly one report per authenticated player for a bounded
recent frame window and evict stale/future entries. Compare a versioned deterministic
state digest—not only timebase—at a measured cadence, with frame number and session
ID in the authenticated message. On mismatch, stop consistently, preserve both
diagnostic digests, and identify missing/duplicate reporters without guessing.

### NP-07: runtime compatibility fingerprint is not negotiated

RingOut has a useful `CompatibilityFingerprint` containing the source revision,
disc ID, DOL SHA-256, module/CPU ABI versions, `CPUState` size, and module descriptor
hash (`ModernGekko/tools/netplay_compatibility.cpp:67-96`). At the audited commit it
is exercised only as pure computation in the protocol test
(`ModernGekko/tests/netplay_protocol_test.cpp:178-198`). The test's own preamble
documents that the vendored public protocol lacks `SetCompatibilityFingerprint`
and a compatibility-mismatch connection error
(`ModernGekko/tests/netplay_protocol_test.cpp:1-24`).

The live handshake compares the source revision. Game selection separately hashes
the configured `sys/main.dol`: RingOut constructs a `GameFile` from that path
(`ModernGekko/tools/netplay_session.cpp:855-870`), and Dolphin hashes ELF/DOL file
contents for its sync identifier
(`ModernGekko/vendor/dolphin/Source/Core/UICommon/GameFile.cpp:677-724`,
`ModernGekko/vendor/dolphin/Source/Core/UICommon/GameFile.cpp:727-784`).
That is useful, but it does not prove that peers loaded the same recompiled DLL,
descriptor chunks, CPU ABI/layout, or other deterministic runtime artifact.

**Required fix:** negotiate and compare the strong fingerprint during the
authenticated handshake, before lobby admission and before any save/code transfer.
Reject mismatches with a specific error showing which non-secret component differs.
Hash the artifact actually loaded, not merely the requested path, and bind the
fingerprint to the session transcript.

### NP-08: lobby player snapshots can become dangling pointers

`GetPlayers()` locks the player map, constructs a vector of raw pointers into map
values, releases the lock, and returns those pointers
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1159-1169`).
The lobby iterates and dereferences them after the lock has been released
(`ModernGekko/tools/netplay_session.cpp:625-699`). The network thread can erase a
player concurrently on `PlayerLeave`
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:501-518`).
That is a use-after-free race.

Mapping arrays and the target buffer also cross threads through unguarded references
or assignments (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1652-1670`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:298-305`).

**Required fix:** return player and mapping snapshots by value, or run a callback
while holding a lock with a documented no-reentry rule. Make scalar cross-thread
state atomic where appropriate; otherwise update and read the entire synchronized
configuration under one mutex. Run ThreadSanitizer coverage over join/leave,
mapping, buffer changes, start, and disconnect.

### NP-09: connection and disconnect paths do not fail closed consistently

- Direct connection ignores the return value of `enet_address_set_host` after
  declaring an uninitialized `ENetAddress`; a resolver failure can proceed with an
  invalid address (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:125-155`).
- The initial response parser does not validate a truncated packet before reading
  `ConnectionError` and PID (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:241-318`).
- `Common::ENet::SendPacket` reports allocation/send failures, but client and server
  wrappers discard its boolean result, so the session receives no failure state
  (`ModernGekko/vendor/dolphin/Source/Core/Common/ENet.cpp:40-63`,
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1008-1011`,
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:2107-2110`).
- A client disconnect event calls the UI and stops a running game but does not clear
  `m_is_connected` or leave the service loop; the RingOut UI separately tracks a
  connection-lost flag as compensation
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1092-1155`,
  `ModernGekko/tools/netplay_session.cpp:167-175`,
  `ModernGekko/tools/netplay_session.cpp:621-629`).
- A mapped non-host disconnect disables the entire running match; there is no
  reconnection or deterministic resumption path
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:523-565`).
- If the interactive lobby window cannot open, RingOut logs “falling back to
  auto-start” and returns success, but the caller then rejects the unarmed session
  (`ModernGekko/tools/netplay_session.cpp:600-605`,
  `ModernGekko/tools/netplay_session.cpp:953-973`).

**Required fix:** zero-initialize and validate addresses; reject resolver errors
immediately; validate the complete handshake and expected EOF; propagate send
failures into a single connection state machine; clear connected/running state on
disconnect; wake every blocked input/transfer waiter; and exit each network loop
exactly once. During a match, use a deterministic all-peer stop with an explicit
reason. Rejoin/resume should remain disabled until a tested state-resynchronization
protocol exists. Make the lobby fallback either a real headless start or an honest
failure, not a success return.

### NP-10: loss and jitter behavior is avoidably expensive

Reliable input prevents silent sample loss, but it also forces retransmission of an
obsolete sample before a newer sample on the same channel can arrive. Input and
ordinary control share the default channel, while only bulk chunks are isolated.
The emulator blocks at an empty queue, and “auto” does not measure RTT or jitter.
This produces latency spikes and, at default settings, five input-sample queue slots
of baseline delay even on a clean link. The actual time depends on the variable SI
poll cadence; the UI's fixed 60 Hz conversion is not valid
(`ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp:551-558`,
`ModernGekko/tools/netplay_session.cpp:703-716`).

**Required fix:** first isolate gameplay input from lobby/control traffic on a
dedicated channel and add queue-depth, RTT, jitter, retransmit, stall-duration, and
late-input telemetry. Implement bounded automatic delay selection from observed
RTT/jitter. A later protocol revision may use frame-numbered, sequenced input with
redundant recent samples and acknowledgements; do not simply mark current input
unreliable, because fixed-delay lockstep cannot skip a sample safely.

## Fail-closed remediation plan

### Phase 0: block hostile data paths before broader release

1. Introduce a shared checked packet reader. Convert handshake, dispatch, pad,
   wiimote, chunk, save, code, status, and timebase messages. Each decoder must
   return either a fully validated value or a typed protocol error; partial values
   must never mutate session state.
2. Add explicit protocol limits: packet bytes, records per packet, string bytes and
   Unicode code points, players, chunks, concurrent transfers, files, directories,
   recursion depth, declared/decompressed bytes, and outstanding desync frames.
3. Fix GCI containment and all LZO decoders as specified in NP-02/NP-03. Stage and
   atomically commit save data only after the entire manifest and byte count pass.
4. On a malformed client packet, log the message type, peer ID, and bounded reason,
   then disconnect only that client. On a malformed server packet, stop netplay,
   wake emulation, discard staged transfers, and show a protocol-error message.
5. Add the runtime/module compatibility fingerprint to the connection exchange.

### Phase 1: establish room identity and authority

1. Reserve the local host principal before bind/listen; separate principal/role from
   pad PID.
2. Add nonce-based room-token authentication and bind protocol version,
   compatibility fingerprint, role, and transcript. Decide and document whether
   encryption is required; if so, use a reviewed authenticated-encryption protocol,
   not custom cryptography.
3. Implement a server-side allowlist for every message and a configured maximum of
   playable/observer peers. Default to four total players and no observers.
4. Add rate and memory limits before allocating player/transfer/desync state.

### Phase 2: make deterministic failures observable and bounded

1. Replace raw-pointer lobby views and synchronize mappings/buffer state.
2. Implement one connection state machine for resolve, connect, authenticate,
   lobby, syncing, running, stopping, disconnected, and failed. Propagate all ENet
   send/service failures and wake all waiters on terminal states.
3. Add authenticated, bounded deterministic-state digests and strict per-player,
   per-frame report accounting.
4. Separate input/control channels, add telemetry, and implement measured automatic
   delay. Keep fixed-delay as the explicit fallback.

## Verification plan and release gates

### Parser and save-security tests

- For every message decoder, feed every byte-prefix truncation plus trailing bytes;
  assert completion in bounded time, no state mutation, and the intended peer
  disconnect. The pad regression must include the exact “valid map + one button
  byte” case that currently wedges the loop.
- Fuzz all message dispatchers and save manifests with libFuzzer/AFL++ under ASan,
  UBSan, and a watchdog. Seed with valid host/client captures and cross-message
  mutations. Treat timeout, excessive allocation, alert/modal display, and leaked
  staged files as failures.
- Test GCI names including `../`, `..\\`, absolute POSIX, drive-rooted, UNC,
  alternate-stream, separator-mixed, Unicode-normalization, reserved-device, empty,
  dot, and symlink-escape cases on Linux and Windows.
- Test declared sizes of zero, cap, cap+1, `UINT64_MAX`, wrong final size, truncated
  chunks, oversized compressed chunks, maximum-expansion chunks, missing/endless
  terminators, trailing bytes, addition overflow, and multiple files exceeding the
  session cap. Verify no output exists after rejection.

### Protocol/authority integration tests

- Race remote connections continuously while constructing a host; assert the local
  principal is always host and no unauthenticated peer enters the roster.
- Verify missing/wrong/replayed room proofs fail before PID assignment, game/status
  state, save sync, or code sync. Verify transcript/fingerprint changes invalidate
  authentication.
- Table-test every message against host/player/observer roles. Explicitly assert an
  ordinary client cannot stop, power off, remap, change buffer/game, kick, or start
  a transfer; retain the existing pad-ownership regression at
  `ModernGekko/tests/netplay_protocol_test.cpp:246-298`.
- Connect at, below, and above room capacity and verify excess peers consume no
  persistent player/transfer state.
- Deliberately vary the loaded recompiled module descriptor, chunk hash, DOL, ABI,
  and CPU-state size; each mismatch must fail before the lobby becomes ready.

### Reliability and determinism tests

- Make `.github/scripts/netplay-local.sh` and
  `.github/scripts/netplay-match.sh` exit nonzero when either peer fails to arm,
  reaches too few frames, logs a disconnect/desync, leaves a process alive, or has
  a RAM-hash mismatch. They currently print outcomes without failing at
  `.github/scripts/netplay-local.sh:160-193` and
  `.github/scripts/netplay-match.sh:155-175`.
- Run two-peer lifecycle/input tests under controlled 0/1/3/5% loss, 20-200 ms RTT,
  jitter, duplication, and reorder. Gate on bounded startup/stop time, identical
  deterministic hashes, no unbounded queue/map growth, and useful terminal errors.
- Inject a one-byte deterministic-state difference while timebase stays equal;
  require the new digest protocol to stop on the expected frame. Send duplicate,
  missing, old, future, and many unique timebase/digest reports; memory must remain
  bounded and attribution accurate.
- Stress join/leave/remap/buffer/start/disconnect under ThreadSanitizer and require
  zero races. Abruptly kill each role during lobby, save sync, game boot, input wait,
  and active match; every survivor must terminate or return to lobby by policy
  without hanging.
- Exercise Linux-to-Linux, Windows-to-Windows, and Linux-to-Windows peers with the
  exact release artifacts. Linux CI currently runs CTest
  (`.github/workflows/linux-appimage.yml:128-164`), while Windows explicitly sets
  `BUILD_TESTING=OFF` (`.github/workflows/windows-cross.yml:118-142`) and its Wine
  smoke only reaches `moderngekko-run --help`
  (`.github/scripts/smoke-windows-package.sh:95-105`). Windows protocol tests and a
  real two-peer Windows/cross-OS run are release gates, not optional smoke.

### Minimum acceptance criteria

- No unauthenticated peer reaches lobby state or controls PID/role assignment.
- Every malformed packet terminates parsing in bounded time and produces a typed,
  visible error; sanitizers report no memory error.
- No save-transfer input can create, truncate, or follow a path outside its staging
  root; allocations and decompressed output remain within documented caps.
- Only the server-local host can perform host-authority actions; room capacity and
  report/transfer state remain bounded under abuse.
- Peers with different loaded deterministic artifacts cannot start.
- Loss/disconnect tests never deadlock an emulator or networking thread, and a
  deterministic mismatch cannot be reported as a clean session.

Until Phase 0 and the hostile-host portions of Phase 1 pass these gates, release
notes should describe netplay as direct, fixed-delay, and intended only for trusted
peers.
