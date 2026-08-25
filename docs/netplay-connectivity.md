# Netplay connectivity, room codes, and relay architecture

Status: implemented Online Room beta plus source-led production architecture,
recorded 2026-08-25.

RingOut baseline: branch `codex/rollback-netplay`; the Online Room implementation
landed after checkpoint `05798a513f34a02ea5983daa65521cd36532cc0f`, and the
launcher-only player flow plus diagnostic logging is committed at
`6c89d6d4c801003fbe8cad34b1d8939998921750` on 2026-08-25. Source paths and line
numbers in the current-behavior sections refer to that latest checkpoint. Upstream
source revisions inspected on 2026-08-25 are listed under
[Primary sources](#primary-sources).

This document is deliberately separate from the live rollback implementation.
Rollback changes how peers schedule, predict, restore, and replay simulation; it
does not by itself discover peers, traverse NAT, authenticate a room, encrypt
traffic, or hide an address. RingOut now has a deliberately limited Dolphin
hosted-rendezvous beta; everything under [Proposed production
architecture](#proposed-production-architecture) remains future design work,
not current or shipped production infrastructure.

## Executive decision

RingOut now exposes **Online Room (beta)** as the ordinary launcher flow and
retains Direct IP as an advanced option. The beta removes manual IP exchange
for many permissive NATs by using Dolphin's hosted rendezvous service, but it is
still unauthenticated, unencrypted, direct peer-to-peer gameplay with no relay
or IP hiding. It is appropriate only for trusted friends and is not the final
production connectivity architecture.

The production Internet flow should eventually replace that beta boundary with:

1. an open, versioned HTTPS/WebSocket room and signaling service;
2. short-lived, signed session tickets bound to the exact RingOut compatibility
   fingerprint, requested netplay mode, role, controller mapping, and session
   generation;
3. standards-based ICE candidate gathering and connectivity checks;
4. STUN for public endpoint discovery;
5. authenticated TURN relay allocation as both a connectivity fallback and an
   explicit peer-IP-privacy mode; and
6. an audited authenticated-encryption layer for the gameplay session.

Dolphin's traversal server and Slippi's public matchmaking client are valuable
references, but neither public design is a complete production answer for
RingOut. Dolphin provides an unauthenticated rendezvous and UDP hole punch with
no relay. Slippi adds account/connect-code matchmaking and automated endpoint
exchange, then also connects peers directly. Slippi's matchmaking backend is
private and cannot be audited or reused.

## Current RingOut behavior

### Online Room beta

The integrated desktop launcher now defaults to an eight-hex-digit Online Room
flow. It labels the feature as a trusted-friends beta, fixes the session mode to
rollback, asks only a joiner for a room code, and keeps **Advanced: use Direct
IP** available (`ModernGekko/tools/moderngekko_launcher.cpp:1199-1273`). Host
and Join pass `--netplay-traversal`; room-code input is normalized to exactly
eight hexadecimal characters before launch
(`ModernGekko/tools/moderngekko_launcher.cpp:1293-1324,1474-1505` and
`ModernGekko/tools/frontend_config.cpp:45-61`).

The runtime maps that flag to `NetplayConnection::OnlineRoom` and defaults to:

```text
host: stun.dolphin-emu.org
main UDP port: 6262
alternate UDP port: 6226
room code: exactly eight hexadecimal characters
```

The option model and typed failures are at
`ModernGekko/tools/netplay_session.hpp:16-64`; CLI parsing and override wiring
are at `ModernGekko/tools/moderngekko_run.cpp:31-47,112-124,157-213,354-379`.
The host constructs Dolphin's traversal-enabled server but deliberately keeps
its own loopback `NetPlayClient` direct. A remote joiner uses the traversal
configuration and room code. This avoids replacing the host's shared ENet
socket while preserving one normal RingOut protocol path after rendezvous
(`ModernGekko/tools/netplay_session.cpp:1149-1168,1255-1273`).

Once Dolphin assigns a code, the lobby shows it and offers **Copy code**. The
same window states that gameplay is direct P2P, unencrypted, exposes both peer
IPs, and can fail through strict NATs
(`ModernGekko/tools/netplay_session.cpp:235-251,757-796`). Service resolution,
expiry, send timeout, missing room, and traversal failure are mapped to explicit
logs/exit codes and launcher messages
(`ModernGekko/tools/netplay_session.cpp:212-233,1276-1304` and
`ModernGekko/tools/moderngekko_launcher.cpp:1596-1613`).

This beta does **not** use Dolphin's public room index. The eight-character code
is exchanged out of band between trusted players; after rendezvous, normal ENet
gameplay and RingOut's exact mode/fingerprint handshake run directly between
the peers.

### Advanced Direct IP

Advanced Direct IP preserves the previous path for either fixed delay or
rollback:

- The host binds the selected UDP port, normally 2626, on all local interfaces.
- The host creates its own ordinary NetPlay client over loopback.
- A guest enters a hostname or IP address and connects directly with ENet.
- `NetTraversalConfig` leaves traversal disabled, the Dolphin public index is
  disabled, and host construction does not request UPnP port forwarding
  (`ModernGekko/tools/netplay_session.cpp:1149-1168`).
- Address validation accepts hostnames and IPv4-like input but currently rejects
  IPv6 literals because `:` is rejected
  (`ModernGekko/tools/frontend_config.cpp:80-85`).

The in-game LAN scanner is a separate plaintext UDP broadcast protocol. An
interactive host sends `RINGOUT1 <port> <nickname>` to UDP 2627 once per second;
the joiner listens for 2.5 seconds and adopts the packet's source address
(`ModernGekko/tools/netplay_session.cpp:608-668` and
`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/RecompMenu.cpp:881-977`).
It does not cross ordinary routers and is not an authenticated room mechanism.

Consequences:

- Same-LAN sessions usually work without router configuration.
- Private routed VPNs work when both endpoints can address each other.
- A direct Internet host generally needs manual UDP forwarding.
- There is no room code on this advanced path, and neither path has a relay,
  public browser, account matchmaking, or peer-IP privacy.
- The remote peer and any on-path observer can see plaintext traffic. The exact
  mode/fingerprint extension is a compatibility check, not peer identity or
  encryption
  (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayConnectProtocol.h:17-95`).

The live rollback branch's Ready gate, compatibility negotiation, state restore,
and output quarantine do not authenticate or encrypt the transport. Online Room
is therefore limited to mutually trusted friends; Direct IP remains appropriate
for a trusted LAN or private VPN. Rollback-session persistence is separately
quarantined; see [the rollback implementation
handoff](rollback-netplay-implementation.md).

### Using the beta

Ordinary launcher flow:

1. Both players use the same RingOut build, extracted game, recomp module, and
   controller setup.
2. The host opens **Netplay**, leaves **Advanced: use Direct IP** off, enters a
   nickname, and selects **Host online room**.
3. The lobby displays an eight-character room code. The host selects **Copy
   code** and sends it privately to the other player.
4. The guest opens **Netplay**, leaves Direct IP off, enters the code, and
   selects **Join online room**.
5. Both verify the expected peer, exact game/module compatibility, controller
   assignment, and rollback mode; both select **Ready**. The host selects
   **Start game** only after the exact roster is Ready.
6. If the service or traversal fails, retry once, then use Advanced Direct IP
   over a trusted LAN/private VPN. There is no relay fallback and no silent
   rollback-to-fixed-delay downgrade.

The equivalent runner commands, with package-relative paths shown, are:

```bash
# Host. Read the assigned code from the lobby or RingOut.log.
./bin/moderngekko-run --game ./game --module ./bin/gGRSEAF_recomp.so \
  --user-dir /absolute/path/to/host-user --controller "Standard Controller" \
  --netplay-host --netplay-traversal --netplay-mode rollback \
  --nickname Host --buffer auto --netplay-diagnostics

# Guest. Replace 0123abcd with the host's code.
./bin/moderngekko-run --game ./game --module ./bin/gGRSEAF_recomp.so \
  --user-dir /absolute/path/to/guest-user --controller "Standard Controller" \
  --netplay-join 0123abcd --netplay-traversal --netplay-mode rollback \
  --nickname Guest --buffer auto --netplay-diagnostics
```

The launcher always captures basic session output in `Logs/RingOut.log` and
preserves one `RingOut.previous.log`. Its persisted diagnostics checkbox passes
`--netplay-diagnostics`, which adds Dolphin NETPLAY transport and handshake
events to the captured console stream and native Dolphin log. The launcher can
copy the support-log path and warns that these files may contain room codes, IP
addresses, nicknames, controller names, and local paths.

`--traversal-server`, `--traversal-port`, and `--traversal-alt-port` override
the defaults for a controlled compatible service. They are advanced diagnostic
flags, not a way to turn an RFC STUN/TURN server into a Dolphin traversal
server (`ModernGekko/tools/moderngekko_run.cpp:31-47,182-201`).

The two-peer real-game harness can exercise the same room-code branch:

```bash
PKG=/absolute/path/to/RingOut-package \
RINGOUT_NETPLAY_TRAVERSAL=1 \
RINGOUT_ROLLBACK_PRODUCTION=1 \
bash .github/scripts/netplay-match.sh /tmp/ringout-online-room 60 2640
```

The harness starts the host, waits up to 30 seconds for the logged room code,
passes that code to the guest, and still requires both peers to arm before the
scripted two-input match proceeds. A compatible private service can be selected
with `RINGOUT_TRAVERSAL_SERVER`, `RINGOUT_TRAVERSAL_PORT`, and
`RINGOUT_TRAVERSAL_ALT_PORT`
(`.github/scripts/netplay-match.sh:181-234`). Because the default route depends
on external Dolphin infrastructure and real NAT behavior, it belongs in an
opt-in/network integration tier rather than hermetic per-change CI.

## Dolphin traversal findings

Dolphin currently defaults its traversal configuration to
`stun.dolphin-emu.org` on UDP 6262, with alternate port 6226. Its optional index
is `https://lobby.dolphin-emu.org`; direct connection remains the configured
default and index use defaults off
([`Source/Core/Core/Config/NetplaySettings.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/Config/NetplaySettings.cpp)).

The traversal protocol is a small packed UDP protocol, version 0, with an
eight-character host ID and these operations: Ack, Ping, Hello, ConnectPlease,
PleaseSendPacket, ConnectReady/Failed, and TestPlease
([`Source/Core/Common/TraversalProto.h`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalProto.h)).

The connection flow is:

1. A host says hello to the central service.
2. The service records the host's observed UDP endpoint and returns a random
   eight-hex-digit host ID.
3. A joiner submits that ID.
4. The service sends the joiner's observed endpoint to the host and asks the
   host to send a UDP packet to it.
5. After the host acknowledges the punch request, the service returns the host's
   endpoint to the joiner.
6. Normal NetPlay connects peer-to-peer over ENet. The traversal service does
   not carry gameplay data.

The client implementation, including retries and the endpoint-directed punch,
is in
[`Source/Core/Common/TraversalClient.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalClient.cpp).
The CC0 central service creates a 32-bit random host code, expires endpoint
records after 30 seconds, refreshes them with pings, and forwards connection
instructions
([`Source/Core/Common/TraversalServer.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalServer.cpp)).

The optional lobby index lists a traversal host code instead of a host IP, but
the subsequent traversal still gives the peers each other's endpoints
([`Source/Core/Core/NetPlayServer.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/NetPlayServer.cpp#L187-L220)
and
[`Source/Core/Core/NetPlayClient.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/NetPlayClient.cpp#L118-L221)).

### Live hosted-service compatibility check

At `2026-08-25T16:20:32-05:00`, a non-invasive check resolved
`stun.dolphin-emu.org` to IPv4 `144.76.17.114`. One correctly sized 37-byte
protocol-version-0 `HelloFromClient` sent to UDP 6262 received both a successful
acknowledgement and `HelloFromServer`. The response contained an eight-byte
lowercase-hex room code, a valid observed IPv4 address family, and a nonzero
observed port. The server reply was acknowledged so it would not retry.

One registered `Ping` sent to UDP 6226 then received a successful acknowledgement
from the alternate port. A later automated run of
`.github/scripts/test-dolphin-traversal-live.py` registered two independent UDP
sockets with the same hosted service and completed `ConnectPlease`,
`PleaseSendPacket`, its positive acknowledgement, and matching `ConnectReady`.
The command exited zero:

```bash
.github/scripts/test-dolphin-traversal-live.py
```

This proves that DNS, both UDP endpoints, packet size, protocol version,
room-code lookup, and the complete hosted rendezvous exchange matched the
vendored client on that date. The two sockets shared one public NAT endpoint;
the endpoint-directed punch did not hairpin back to the guest. Therefore this
still does **not** prove two-peer hole punching through representative NATs, an
entire RingOut lobby, or gameplay.

The ISO-backed two-process production rollback harness was also run with
`RINGOUT_NETPLAY_TRAVERSAL=1`. The final diagnostics-enabled run assigned host
code `f2a7304d` after one second. Both real game processes registered the same
public IPv4 endpoint with distinct UDP ports; the host logged the requested
punch, and the guest logged `ConnectReady`, the supplied ENet endpoint, and then
`ENet did not connect after traversal rendezvous`. Evidence is retained at
`/tmp/ringout-two-instance-hosted-diagnostics-final-20260825`. This result shows
that same-host reachability was absent, without assigning that absence to a
specific NAT or firewall behavior. The corresponding diagnostics-enabled
Direct localhost regression reached a two-controller VS Battle, activated
production rollback on both peers, accepted scripted input from both owners,
and completed 690 retained physical-frame rows; its evidence is at
`/tmp/ringout-two-instance-direct-diagnostics-20260825b`. The correct conclusion
is that rollback/session integration remains healthy and the hosted control
plane works, while a second independently routed machine is still required for
the final hosted gameplay acceptance test.

The exact official Dolphin master at the time was
`26d5cd38bd068f878b6e64ee8b705787b3a16164`. RingOut's vendored
`NetplaySettings.cpp`, `TraversalProto.h`, and `TraversalServer.cpp` were
byte-identical to that revision. `TraversalClient.cpp` differed only in the
include path needed by RingOut's vendor layout before the branch's explicit
alternate-port fix. The branch also corrects the upstream client's apparent
host-versus-port typo in its alternate-port validation
(`ModernGekko/vendor/dolphin/Source/Core/Common/TraversalClient.cpp:344-399`).

### Dolphin boundary

Dolphin's code solves manual address exchange for many NATs. It does not solve:

- peer-IP privacy: both peers receive and use the other's endpoint;
- relay fallback when UDP hole punching fails;
- authenticated room membership or identity;
- encryption or transcript integrity;
- a strong capability token: the displayed ID has only 32 bits of entropy; or
- public-service rate limits and abuse controls in the inspected server.

The server is a useful, easily self-hosted prototype and its traversal files are
CC0. The current beta deliberately exposes it only as a trusted-friends
convenience; RingOut should not promote this version-0 protocol to its public
production trust boundary. The service is operated by Dolphin rather than
RingOut and provides RingOut no documented SLA. Permission and capacity for a
broad RingOut beta have not been established, so a larger rollout should first
coordinate with Dolphin or self-host the compatible service.

## Slippi matchmaking findings

Project Slippi's public C++ client supports ranked, unranked, direct, teams, and
party modes. It connects to `mm.slippi.gg:43113`
([`SlippiMatchmaking.h`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiMatchmaking.h)).

The client binds matchmaking to a random local UDP port in 41000-50999 and
deliberately reuses that port for the later gameplay connection so the
matchmaking exchange creates a useful NAT mapping. It sends a reliable JSON
`create-ticket` request containing:

- user ID, play key, display name, and the player's connect code;
- matchmaking mode and an optional target connect code;
- application version; and
- a LAN address candidate.

The assignment contains match ID, player identity/rank data, local/remote
external endpoints, LAN endpoints, roles, and rules. When peers share an
external address, the client selects the LAN endpoint; otherwise it selects the
external endpoint. It then disconnects from matchmaking and creates direct ENet
connections to the returned peer endpoints
([`SlippiMatchmaking.cpp`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiMatchmaking.cpp#L261-L613)).

Both peers attempt direct connections using the assigned endpoints and the same
bound local port
([`SlippiNetplay.cpp`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiNetplay.cpp#L63-L144)).
If the public client cannot establish them, team mode reports a timeout and
other modes return to matchmaking; the inspected path has no TURN or other
relay candidate
([`SlippiMatchmaking.cpp`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiMatchmaking.cpp#L766-L852)).

Slippi's current public Rust user component manages authentication state and
loads the play key/connect code from the user record
([`slippi-rust-extensions/user`](https://github.com/project-slippi/slippi-rust-extensions/tree/2d29e794de8497582675fb70877851f2cdd2f256/user)).
The public C++ client shows JSON sent over ENet but does not establish a publicly
auditable end-to-end confidentiality layer. That is a deliberately bounded
source finding, not a packet-capture claim about every deployed service layer.

Most importantly, Slippi states that the matchmaking server lives in a private
repository and is not planned for publication
([`slippi-wiki/GETTING_STARTED.md`](https://github.com/project-slippi/slippi-wiki/blob/71c6a395f841ff67f75ab4c0084fd1d6ee22c2db/GETTING_STARTED.md)).
Its server-side authentication, ticket validation, selection, abuse controls,
retention, and deployment cannot be independently audited or reused.

### Slippi boundary

Slippi provides the player experience RingOut should learn from: persistent
connect codes, automatic matchmaking, compatibility/version rejection, same-port
hole punching, same-NAT LAN selection, roles, and a clear transition from queue
to match. It still exchanges endpoints for direct gameplay. It avoids asking a
person to paste an IP; it does not hide that IP from the service or opponent.

RingOut should reuse those concepts and state transitions, not depend on or try
to reproduce an unavailable private backend contract.

## Proposed production architecture

### Control plane

Use HTTPS for create/join requests and one authenticated WebSocket per active
launcher for room events and ICE signaling. Define the service contract in this
repository and version every request and event.

A room should have:

- a random 128-bit internal room ID;
- a short-lived human code with at least about 60 random bits, for example 12
  normalized Crockford Base32 characters plus a typo-detection checksum;
- creation and unstarted-room expiration, initially about 15 minutes;
- an exact maximum player count and no implicit spectators;
- creator, member, requested role, controller slot, Ready, and acceptance state;
- exact RingOut protocol, game/DOL/module fingerprint, fixed-delay/rollback mode,
  rollback state-format version, and session-generation requirements;
- privacy preference and selected network path; and
- a monotonically increasing room revision for idempotent state changes.

The human code is a lookup capability, not a durable account credential. Apply
per-IP/device/account attempt limits, constant-shape failure responses, expiry,
and one-room membership constraints. Do not disclose peer candidates until both
players accept and compatibility has passed.

Each client creates an ephemeral session key. The service issues a short-lived
signed ticket binding:

```text
protocol version
room ID and room revision
session generation
peer/device principal and ephemeral public key
host/player role and controller slot
exact compatibility fingerprint and requested netplay mode
ICE credentials and relay authorization scope
issued-at, expiry, and one-time nonce
```

The existing RingOut host remains the NetPlay coordinator; the room service is
not an emulator-state authority. It authorizes membership and roles and relays
signaling metadata only.

### NAT traversal data plane

[ICE, RFC 8445](https://www.rfc-editor.org/rfc/rfc8445.html) gathers multiple
candidate address pairs and performs peer-to-peer connectivity checks rather
than assuming one NAT topology.
[STUN, RFC 8489](https://www.rfc-editor.org/rfc/rfc8489.html) discovers a NAT's
mapped endpoint and keeps mappings alive.
[TURN, RFC 8656](https://www.rfc-editor.org/rfc/rfc8656.html) supplies a relay
when a direct pair cannot be found. TURN's specification also explicitly covers
peer IP-location privacy by disclosing only relay candidates.

Gather and race, in priority order:

1. same-LAN host candidates;
2. server-reflexive public candidates learned through STUN; and
3. preallocated regional TURN relay candidates.

Use one stable local UDP socket per session, preserving the useful Slippi
same-port behavior. Preallocate relay candidates rather than waiting for a long
direct timeout. Nominate the lowest-latency successful direct pair in ordinary
Auto mode; nominate only relay candidates in Hide my IP mode.

[`libjuice`](https://github.com/paullouisageneau/libjuice/tree/f09cf92522de0b619a63d55d007b5466c7c14ad1)
is a promising implementation candidate because it provides dependency-light,
cross-platform C/CMake UDP ICE, STUN, TURN, consent freshness, IPv4/IPv6, and
application datagram callbacks. It is MPL-2.0 and requires a project license
review before adoption. It is not a drop-in ENet socket: RingOut must either add
a tested datagram transport adapter around its vendored ENet or carry the
required reliability/session protocol over the ICE datagram API.

[`coturn`](https://github.com/coturn/coturn/tree/9600cbd603e08e09841070a83ab78fa6423a67ac)
is a deployable open-source STUN/TURN server with container packaging,
authentication, TLS/DTLS options, metrics integration, and external credential
stores. RingOut must issue short-lived per-room credentials; an unauthenticated
public relay is not acceptable.

### Gameplay-session security

ICE and TURN establish a route; they do not make RingOut's ENet payload an
authenticated private protocol. Add an audited secure-session layer before
public rooms. It should:

- authenticate both ephemeral keys against the signed room tickets;
- bind room ID/revision, generation, compatibility fingerprint, selected mode,
  roles, and controller mapping into the handshake transcript;
- derive independent send and receive keys;
- protect every gameplay/control packet with authenticated encryption;
- use direction-specific sequence numbers and bounded replay windows; and
- make reconnect create a new ticket, generation, key, and sequence space.

Use a reviewed protocol/library such as DTLS or a reviewed Noise construction.
Do not design a custom cipher or treat ENet reliability, the public fingerprint,
or a room code as cryptographic authentication.

## Future production player experience

The implemented beta flow is documented under [Using the beta](#using-the-beta).
After authenticated rooms, ICE, and TURN exist, the production launcher flow
should become:

1. Select **Host online room**.
2. Choose **Auto** or **Hide my IP**, expected player count, and fixed-delay or
   rollback mode.
3. Copy a human room code or deep link.
4. The joiner selects **Join room**, pastes the code, and reviews the host name,
   requested mode, and compatibility result.
5. Both accept, ICE establishes a path, and the lobby displays `Direct`, `Relay`,
   or `Relay - IP hidden` plus measured ping.
6. Each player verifies controller mapping and selects Ready. Existing
   authoritative Ready/start behavior remains the final gate.

Connection choices:

| Choice | Behavior | Privacy statement |
| --- | --- | --- |
| **Auto** | Try LAN/direct candidates and fall back to regional relay. | A direct opponent can see the peer IP. The service sees connection metadata. |
| **Hide my IP** | Exchange only relay candidates; never signal a peer-reflexive or server-reflexive peer endpoint. | The opponent sees the relay, not the peer IP. The room/relay service still sees source IPs. |
| **Direct IP (advanced)** | Preserve the implemented hostname/IP and UDP-port flow with no service dependency. | Trusted LAN/private VPN only; the opponent sees the IP and current traffic is not secure. |

Never silently change a privacy or simulation mode. If Hide my IP cannot obtain
a relay, fail with a specific error. If rollback compatibility fails, offer a
new fixed-delay session; do not relabel an established rollback room.

Useful connection states are `Creating room`, `Waiting for player`,
`Authenticating`, `Checking compatibility`, `Finding direct route`,
`Connecting through relay`, `Connected`, and a typed terminal error. Provide a
copyable support code that contains non-secret room revision, region, candidate
type, and failure stage, not tokens or endpoints.

## Trust and privacy boundaries

| Party | What it must know | What it should not retain or receive |
| --- | --- | --- |
| Opponent in Auto/direct | Selected direct endpoint, public identity/profile data, compatibility result | Account credentials, room-service token, private logs |
| Opponent in Hide my IP | Relay endpoint and allowed lobby identity/profile data | Peer public/LAN endpoint candidates |
| Room service | Source connection metadata, room membership, compatibility/mode, signed-ticket fields, ICE signaling | Gameplay payload, save data, long-lived plaintext credentials |
| TURN relay | Source endpoints, allocation/permission metadata, opaque encrypted datagrams | Gameplay plaintext or reusable account credentials |
| RingOut host coordinator | Authenticated room roles, mappings, synchronized game data required by NetPlay | Authority derived merely from connection order |

Publish a retention policy before public matchmaking. Prefer ephemeral room and
candidate state, coarse region metrics, bounded security logs, endpoint
redaction, and explicit deletion deadlines. Relay-only protects an address from
the opponent, not from RingOut's service operator or the player's ISP.

Accounts are not required for the first friend-room release. A local device
principal plus a high-entropy room capability can authorize a private room.
Public queues, ratings, moderation, blocks, and abuse reports require persistent
accounts and a separate policy review.

## Service and operational requirements

### Room/signaling service

- Horizontally scalable HTTPS/WebSocket API with one documented protocol schema.
- Ephemeral room store with atomic revisions, expiry, and idempotent operations.
- Signed-ticket key rotation and a short overlap window.
- Per-principal and per-network rate limits before room/player allocation.
- Strict message-size, string, candidate-count, player-count, and lifetime caps.
- Compatibility and acceptance gates before candidate disclosure.
- Regional service health and typed client errors; no silent fallback to an
  insecure direct mode.

### STUN/TURN fleet

- Public IPv4 and IPv6, UDP first, with TURN-over-TCP/TLS as a later restricted-
  network fallback if measurement justifies it.
- At least two regions before broad testing, with latency-based selection and
  explicit regional outage behavior.
- Short-lived scoped credentials, allocation and peer permissions, bandwidth and
  duration quotas, abuse monitoring, and egress alerts.
- Datagram MTU limits and no IP fragmentation dependency for gameplay packets.
- Capacity planning from concurrent relayed matches, not room count. Relay
  bandwidth is the dominant recurring cost.

### Availability policy

- Direct-IP advanced sessions remain usable when the room service is down.
- An already established direct session should not require the signaling service
  to stay online.
- An established relay session necessarily depends on its relay. Relay failure
  must stop or deliberately re-negotiate at a safe session boundary; it must not
  continue speculative rollback through an unbounded outage.
- Service degradation must not disable the fixed-delay compatibility path.

## Staged implementation

The Dolphin Online Room beta precedes these production stages. It is useful for
collecting opt-in connectivity data, but it does not complete Stage 1 because
it has no RingOut room service, tickets, authentication, or ephemeral keys, and
does not complete Stage 2 because it is not standards-based ICE.

### Stage 0: preserve and harden direct play

- Keep Direct IP advanced and LAN scan behavior available.
- Finish the fail-closed parser, save-transfer, authority, and public-room
  security prerequisites in
  [the protocol audit](netplay-protocol-security.md).
- Add accurate resolver/firewall errors and explicit trusted-network wording.
- Keep all new connectivity behind a negotiated version; do not alter legacy
  fixed-delay wire behavior accidentally.

### Stage 1: open friend-room control plane

- Specify and test create/join/accept/leave/ready/start schemas.
- Implement room IDs/codes, expiry, rate limits, signed tickets, compatibility
  binding, and ephemeral client keys.
- Add a local fake service for deterministic launcher and protocol tests.
- Initially retain Direct IP as the only production-approved transport while the
  signaling/authentication layer is exercised without private game assets.

### Stage 2: direct ICE

- Integrate an ICE client on Linux and Windows with a stable UDP port.
- Exchange bounded candidates only after room acceptance and compatibility.
- Prefer LAN, then direct public candidates. Record selected candidate type and
  setup time without retaining raw endpoints in ordinary telemetry.
- Run the NAT matrix below before making Auto the default. Until TURN lands,
  report `Direct route unavailable` rather than implying universal traversal.

### Stage 3: relay fallback and Hide my IP

- Deploy authenticated coturn in test regions and issue short-lived credentials.
- Pre-gather relay candidates and add automatic fallback.
- Implement relay-only signaling for Hide my IP and verify it with packet capture
  and service-trace assertions.
- Put the authenticated gameplay envelope on both direct and relay paths. A relay
  must see only ciphertext.

### Stage 4: public matchmaking

- Add account authentication, blocks, moderation/reporting, queue cancellation,
  match acceptance, and regional/rating selection.
- Match exact protocol/fingerprint/mode first; only then consider region, latency,
  and rating.
- Issue a fresh private room and the same signed ticket/ICE flow after a match.
  Matchmaking must not create a second gameplay transport protocol.

### Stage 5: release gate and operations

- Pass the automated NAT/security matrix, cross-OS artifact tests, relay load and
  outage tests, and privacy assertions.
- Publish endpoint visibility, retention, abuse, and outage policies.
- Exercise packaged Windows and AppImage launchers against staging and production
  service configurations without embedding long-lived service secrets.
- Preserve Direct IP and fixed delay as diagnosable fallbacks.

## Automated NAT-matrix test plan

### Current Dolphin-beta tier

The implemented `RINGOUT_NETPLAY_TRAVERSAL=1` harness route is the first
automation seam. Before calling Online Room broadly usable, run it between two
real networks and in a namespace matrix covering permissive, port-restricted,
strict stateful, double-NAT, and blocked-UDP cases. Successful cases must reach
the existing two-input match and rollback confirmed-state checks; expected
failures must return the typed service, missing-room, or traversal error rather
than hanging. Preserve Advanced Direct IP as the control case.

This beta tier should additionally verify that:

- the host receives exactly one valid eight-hex-digit code and keeps it alive;
- an invalid, expired, and unknown code fails without roster admission;
- service loss before connection is distinguishable from peer traversal failure;
- the selected peer endpoint is IPv4 and gameplay bypasses the rendezvous server;
- neither UI nor logs claim relay or IP privacy; and
- fixed-delay Direct IP still works even when the hosted service is unavailable.

### Linux namespace harness

Create a privileged CI/nightly harness with isolated namespaces:

```text
client-a -- nat-a -- simulated WAN -- nat-b -- client-b
                         |
                 room / STUN / TURN
```

Use veth pairs and an isolated WAN bridge. Configure forwarding, SNAT/DNAT,
hairpin behavior, and filtering with nftables. Apply deterministic delay, jitter,
loss, duplication, reordering, and outages with `tc netem`. Capture each link with
`tcpdump` and archive the topology, nftables rules, netem seed/profile, selected
candidate pair, service trace, both process logs, and artifact fingerprints.

Required connectivity cases:

| Case | Expected Auto result | Expected Hide my IP result |
| --- | --- | --- |
| Same LAN | Host/LAN direct | Relay only |
| Same public NAT with hairpin | LAN preferred; reflexive as fallback | Relay only |
| Endpoint-independent NAT on both sides | Direct server-reflexive | Relay allowed |
| Port-restricted NAT | ICE direct when checks open mappings | Relay allowed |
| Address/port-dependent mapping on one side | Direct if a valid pair wins; otherwise relay | Relay allowed |
| Symmetric-style mapping on both sides | Relay fallback | Relay |
| Double NAT / CGNAT simulation | Direct if checks succeed; otherwise relay | Relay |
| Native IPv6 and mixed IPv4/IPv6 | Best valid family/candidate | Relay in an allowed family |
| Direct UDP blocked, TURN UDP allowed | Relay fallback | Relay |
| UDP blocked, TURN TCP/TLS enabled | Later fallback or typed unsupported error | Same |
| Room service unavailable before join | Typed failure; no insecure fallback | Typed failure |
| Signaling lost after direct establishment | Match continues by policy | Match continues while relay lives |
| Relay killed during match | Explicit bounded stop/re-negotiation | Explicit bounded stop/re-negotiation |

For every successful case, run both fixed delay and rollback negotiation. The
connectivity harness must require the exact mode/fingerprint, Ready gate, real
remote input influence, minimum game progress, and a clean typed outcome. For
rollback, include a bounded late-authoritative correction and compare confirmed
logical-state evidence; connectivity success alone is not rollback correctness.

### Security and privacy assertions

- Missing, expired, wrong-room, replayed, wrong-role, wrong-generation, and
  wrong-fingerprint tickets fail before roster allocation or candidate disclosure.
- Candidate messages exceeding count/size/type limits fail closed.
- A room-code enumeration test remains bounded by expiry and rate limits and
  produces no room-existence oracle beyond the documented response.
- A peer cannot reuse TURN credentials for another room, peer, or lifetime.
- Direct and relayed gameplay reject forged, replayed, truncated, or cross-session
  encrypted datagrams.
- In Hide my IP mode, the signaling audit contains no peer host or
  server-reflexive candidate, and client-A packet captures contain no packets to
  client-B's public endpoint. The only gameplay destination is the selected relay.
- Logs and crash bundles redact room capabilities, ticket signatures, ICE
  passwords, TURN passwords, ephemeral private keys, and raw peer endpoints.

### CI tiers

- **Per change, unprivileged:** schema/codec tests, ticket/expiry/replay tests,
  fake signaling service, ICE state-machine unit tests, malformed candidates,
  launcher state/error mapping, and legacy Direct IP/fixed-delay regression.
- **Linux privileged nightly:** the namespace/NAT matrix, netem profiles, packet
  capture privacy checks, direct-to-relay fallback, and relay outage tests.
- **Artifact nightly:** packaged AppImage against a staging room/TURN service and
  Windows-to-Windows plus Windows-to-Linux automation on dedicated runners.
- **Release/manual:** two physical consumer networks, CGNAT/mobile hotspot,
  restrictive campus/corporate network where authorized, Steam Deck, real
  controllers, regional relay failover, and a sustained relay load/thermal match.

Minimum production acceptance is 100% typed completion for the supported matrix,
no credential or endpoint privacy failure, no unbounded allocation/state growth,
no downgrade from Hide my IP, no cross-room packet acceptance, and no regression
in Direct IP or fixed-delay sessions.

## Reproducing the source review

From the RingOut rollback worktree, verify the recorded committed baseline and
locate both connection choices in the current worktree:

```bash
git show -s --format=fuller 05798a513f34a02ea5983daa65521cd36532cc0f
git status --short
rg -n "OnlineRoom|netplay-traversal|traversal_server|NetTraversalConfig" \
  ModernGekko/tools/moderngekko_launcher.cpp \
  ModernGekko/tools/moderngekko_run.cpp \
  ModernGekko/tools/netplay_session.cpp \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay
rg -n "RINGOUT_NETPLAY_TRAVERSAL|RINGOUT_TRAVERSAL" \
  .github/scripts/netplay-match.sh
rg -n "RINGOUT1|2627|NetPlayConnectProtocol|CompatibilityFingerprint" \
  ModernGekko/tools ModernGekko/vendor/dolphin/Source/Core/VideoCommon \
  ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay
```

Verify the pinned upstream branch heads used by this review:

```bash
git ls-remote https://github.com/dolphin-emu/dolphin.git refs/heads/master
git ls-remote https://github.com/project-slippi/Ishiiruka.git refs/heads/slippi
git ls-remote https://github.com/project-slippi/slippi-wiki.git refs/heads/master
git ls-remote https://github.com/project-slippi/slippi-rust-extensions.git refs/heads/main
git ls-remote https://github.com/paullouisageneau/libjuice.git refs/heads/master
git ls-remote https://github.com/coturn/coturn.git refs/heads/master
```

The resulting hashes should match [Primary sources](#primary-sources). If an
upstream branch has moved, inspect the pinned URL first, then treat newer source
as a separate update rather than silently changing this record.

## Primary sources

All repository URLs below are pinned to the revisions inspected on 2026-08-25.

### Dolphin

- Revision `26d5cd38bd068f878b6e64ee8b705787b3a16164`.
- Traversal protocol:
  [`Source/Core/Common/TraversalProto.h`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalProto.h).
- Traversal client:
  [`Source/Core/Common/TraversalClient.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalClient.cpp).
- Traversal server:
  [`Source/Core/Common/TraversalServer.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Common/TraversalServer.cpp).
- NetPlay defaults:
  [`Source/Core/Core/Config/NetplaySettings.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/Config/NetplaySettings.cpp).
- NetPlay client/server integration:
  [`NetPlayClient.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/NetPlayClient.cpp) and
  [`NetPlayServer.cpp`](https://github.com/dolphin-emu/dolphin/blob/26d5cd38bd068f878b6e64ee8b705787b3a16164/Source/Core/Core/NetPlayServer.cpp).

### Project Slippi

- Ishiiruka revision `e7711b104b339a99385f2bb12b472d46140a7bc7`:
  [`SlippiMatchmaking.cpp`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiMatchmaking.cpp),
  [`SlippiMatchmaking.h`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiMatchmaking.h), and
  [`SlippiNetplay.cpp`](https://github.com/project-slippi/Ishiiruka/blob/e7711b104b339a99385f2bb12b472d46140a7bc7/Source/Core/Core/Slippi/SlippiNetplay.cpp).
- Public architecture and private-server boundary, wiki revision
  `71c6a395f841ff67f75ab4c0084fd1d6ee22c2db`:
  [`GETTING_STARTED.md`](https://github.com/project-slippi/slippi-wiki/blob/71c6a395f841ff67f75ab4c0084fd1d6ee22c2db/GETTING_STARTED.md).
- Public user/auth component, Rust extensions revision
  `2d29e794de8497582675fb70877851f2cdd2f256`:
  [`user/`](https://github.com/project-slippi/slippi-rust-extensions/tree/2d29e794de8497582675fb70877851f2cdd2f256/user).

### Standards and implementation candidates

- [RFC 8445: Interactive Connectivity Establishment](https://www.rfc-editor.org/rfc/rfc8445.html).
- [RFC 8489: Session Traversal Utilities for NAT](https://www.rfc-editor.org/rfc/rfc8489.html).
- [RFC 8656: Traversal Using Relays around NAT](https://www.rfc-editor.org/rfc/rfc8656.html).
- libjuice revision `f09cf92522de0b619a63d55d007b5466c7c14ad1`:
  [repository](https://github.com/paullouisageneau/libjuice/tree/f09cf92522de0b619a63d55d007b5466c7c14ad1) and
  [`include/juice/juice.h`](https://github.com/paullouisageneau/libjuice/blob/f09cf92522de0b619a63d55d007b5466c7c14ad1/include/juice/juice.h).
- coturn revision `9600cbd603e08e09841070a83ab78fa6423a67ac`:
  [repository](https://github.com/coturn/coturn/tree/9600cbd603e08e09841070a83ab78fa6423a67ac) and
  [`README.turnserver`](https://github.com/coturn/coturn/blob/9600cbd603e08e09841070a83ab78fa6423a67ac/README.turnserver).

## Claim boundaries

- The current RingOut section describes the branch snapshot integrated on
  2026-08-25. It does not claim a released artifact, public matchmaking, a
  RingOut-operated service, or packaged Windows/AppImage validation.
- The live hosted-service check proves main/alternate UDP protocol compatibility,
  not a complete two-peer traversal or RingOut match.
- The Dolphin and Slippi findings describe the pinned public source. They do not
  infer undisclosed deployed controls.
- “No Slippi relay” means no relay exists in the inspected public client path; it
  is not a claim about private source or future service behavior.
- Avoiding manual IP entry is not the same as peer-IP privacy. Direct P2P reveals
  endpoint addresses by design.
- libjuice and coturn are candidates, not selected dependencies. Their license,
  security, Windows packaging, performance, and ENet integration must be reviewed
  and tested before adoption.
- The current eight-hex-digit code and 30-second refreshed mapping are Dolphin
  beta behavior. The longer room-code design, production expiry, service
  topology, regional thresholds, and CI cadence are proposed starting points,
  not measured production settings.
