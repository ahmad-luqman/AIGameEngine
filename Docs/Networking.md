# Networking

Breccia's multiplayer is client-server with an authoritative host. The host owns the shared state: it runs the game,
admits clients, and sends each of them the entities marked `Networked` as snapshots. Clients show that state and send
back only what they have authority over. This page covers the transport, sessions and replication (Networking I). The
Lua `Network` API, client prediction, snapshot interpolation and `net.*` automation commands come next (Networking II,
`ROADMAP.md`).

## Running a session

The runtime hosts or joins a session from its command line:

```bash
# A listen server: the game in a window, hosting on UDP port 7777.
BrecciaRuntime --project MyGame --host 7777
# A dedicated server: no window or GPU, paced to the frame rate.
BrecciaRuntime --project MyGame --headless --host 7777
# A client. The port defaults to the project's Network.Port.
BrecciaRuntime --project MyGame --connect 192.168.1.20:7777
```

`--host 0` lets the system pick a free port; the log says which (`Network: hosting on UDP port N`). Headless runs with a
network session sleep out the rest of each frame instead of spinning a core. With `--frames`, a client that is not
connected when the run ends exits with code 8, so a test cannot pass without a session. A windowed client whose connection
ends shows the reason in a dialog.

The project file's `Network` object (also `project.set {"network": {...}}`) holds the session settings:

| Key | Default | Meaning |
|-----|---------|---------|
| `Port` | 7777 | The port `--connect` uses when the address names none (a host listens on the port `--host` gives). |
| `MaxClients` | 8 | Clients a host admits at once. The next few are refused (`ServerFull`); beyond the transport's four spare slots an attempt goes unanswered (`ConnectionFailed`). |
| `TickRate` | 30 | Snapshots per second to each client, and updates per second back, at most one per frame. A client takes the host's. |
| `MaxBytesPerSecond` | 65536 | Each connection's budget per direction. What does not fit waits. |
| `TimeoutSeconds` | 10 | A connection that hears nothing for this long ends (`Timeout`); a handshake not finished by then ends with `HandshakeTimeout`. |

## Connections

A client connects, then introduces itself with a `Hello`: the protocol version (`NetProtocol::Version`), the hash of the
component registry (`ReplicationSchema`: every component, field and JSON type, the enums' options and the replicated
fields, which is what spawns and updates depend on), and the game (the project's `Name` and `Version`). The host answers
`Welcome` with the client's peer ID, or ends the connection with a reason, which reaches the client with the disconnect
(a lost datagram leaves it a `Timeout`).

| Reason | When |
|--------|------|
| `ProtocolMismatch` | The peers speak different protocol versions. |
| `SchemaMismatch` | They replicate different components or fields (different engine builds). |
| `GameMismatch` | The client plays another game, or another version of it. |
| `ServerFull` | `MaxClients` clients are connected. |
| `HandshakeTimeout` | The handshake did not finish in time: the client connected but never sent its `Hello`, or the host never answered it. |
| `InvalidPacket` | A message did not decode, a peer sent one only the other side sends (a client's over 64 KiB among them, or one that changes a parent, components or ownership), or it acknowledged a message never sent. |
| `Timeout` | Nothing arrived for `TimeoutSeconds`. |
| `ConnectionFailed` | The host never answered. |
| `ClientQuit`, `ServerShutdown` | The client left, or the host stopped. |

Peer IDs start at 1 and are never reused in a session; the host is 0.

## Replication

An entity with a `Networked` component replicates. Its descendants without one of their own are copied once, inside its
spawn, and never updated afterwards; give a descendant its own `Networked` component to keep it in sync (it then
replicates separately, under its parent).

- **Spawns.** A client receives the entity's tree (at most 4,096 records; a larger one is not sent, and logged) as
  scene-file records, with the host's UUIDs, so the IDs agree on every peer. It creates them through `SceneSerializer`,
  so every value passes the checks a scene file's does. Asset references must stay inside the project (relative, without
  `..`, a drive or a backslash); a spawn naming any other file is refused. `Prefab` tracking is left out: instances created
  during Play keep none. A spawn of an entity the client already has is that entity: an authored one the host made
  networked during play, or a tree member that got a `Networked` component of its own. Such an entity takes the spawn's
  replicated fields, parent and components with replicated fields, and keeps the rest of its own (other fields and
  components, its children). Descendants in the spawn that the client lacks (made on the host during play) are created
  under it, checked as a new spawn's are; those it has keep their own values and move where the spawn puts them. A new
  entity's spawn holding entities the client has (an authored one the host moved under it) keeps them the same way.
- **Updates.** Afterwards only the fields components declare in `ComponentInfo::ReplicatedFields` travel, and only when
  they change: `Transform` (Translation, Rotation, Scale), `Networked` (Owner, Authority), `Animator` (Clip, Speed),
  `Material` (AlbedoColor, EmissiveColor, EmissiveIntensity) and the lights' Color and Intensity. A component with
  replicated fields that is added or removed is added or removed on clients too; components without them, and fields
  not listed, reach a client only inside a spawn. A changed parent replicates; a move the client's hierarchy refuses for
  now (a cycle that another move in the same snapshot undoes, or too deep) waits and goes through once it can.
- **Despawns.** Destroying the entity on the host destroys its descendants there too, so they all despawn. Removing its
  `Networked` component removes it, with its non-networked descendants, from clients; descendants with a `Networked`
  component of their own stay, moved to the root until the parent is networked again.

Values are sent as a compact binary form of the registry's JSON (`Network/NetValue`). Changes are found by comparing
those encodings, and each client receives them through `ComponentRegistry`, so a value a component rejects is refused
and logged. Adding a replicated field is a registry change, which moves the schema hash, so mismatched builds refuse
each other.

### Deltas and loss

Snapshots travel on the unreliable channel. Each carries every value that differs from the last one that client
acknowledged, or that an unacknowledged snapshot carried differently. Once a client applies a snapshot it is up to date,
as of that snapshot, on everything any snapshot up to it carried (records that waited for the budget follow in a later
one), and it drops any older snapshot that arrives later. Spawns repeat until acknowledged, and so do despawns, even when
the entity is back meanwhile (its new spawn follows). A lost snapshot costs a resend, never a stale value; once a snapshot
carrying a value is acknowledged, that value is not sent again unless a later unacknowledged snapshot carried something
else. Clients acknowledge the newest snapshot in every update they send. Sequence numbers are 32 bits: at 240 ticks per
second a session lasts some 200 days before they wrap.

### Bandwidth

Each connection spends at most `MaxBytesPerSecond` per direction, refilled continuously and capped at one second's
worth. Within a snapshot, entities whose changes wait gain their `Networked.Priority` every tick, and the highest totals
go first (parents before children when the totals are equal), so a busy scene shares the budget instead of starving
anything. A record larger than the remaining budget still goes when nothing else has (the budget runs into debt), so a
large spawn is never stuck. One no message could carry (1 MiB from a host, 64 KiB from a client) is not sent while it
stays that large (logged once per entity).

## Authority

`Networked.Authority` decides who sets an entity's replicated fields:

- `Server` (the default): the host.
- `Owner`: the client whose peer ID is `Owner` sends them, and the host checks that the sender owns the entity before
  applying them and passing them on. While that client is not connected, or `Owner` is 0, the host decides.

Ownership, parents and components are the host's alone: clients cannot change them (the host refuses and logs it), and an
owner still hears of components the host adds (with their starting values) or removes. A peer drives the dynamic bodies of
entities it has no authority over kinematically (`PhysicsWorld::SetDrivenExternally`), so they follow the replicated
transform instead of simulating against it. The component keeps its `Type`. Character controllers and ragdolls are not
driven this way yet: on a peer without authority they keep simulating between snapshots (Networking II brings prediction
for characters).

`Owner` is a peer ID assigned at runtime: clients get 1, 2, ... in the order they join, so a saved `Owner` hands the
entity to whichever client joins in that place. Usually the host's scripts set it during play.

## What clients do with the scene

A client loads the same scene as the host, keeps everything that is not networked (level geometry, cameras, UI), and
removes its own copies of networked entities when the host accepts it, with their descendants: the host spawns the real
ones, descendants included. Scripts run on
every peer, so a script on a networked entity runs on clients too. A Lua API for telling roles apart comes in
Networking II. A client's runtime entity UUIDs come from a stream of their own (the session seed mixed with a constant):
two independent 64-bit streams practically never meet, and a spawn whose ID a client's own runtime entity happens to hold
takes that entity over.

What a client refuses (a spawn naming a file outside the project, records a scene file would not pass, a value a component
refuses) is logged once per entity and cause (a component, for values), and at most 256 problems per peer; the host counts
a refused spawn as delivered, so that entity stays missing on that client. Statistics (`NetworkSystem::GetStatistics`,
and the summary a `--frames` run logs) count rejected values and records separately.

Current limits, each planned for a later session:

- Scene loads are refused during a session (`Scene.Load` fails with an error); every peer would have to switch together.
- Interpolation and prediction are not done yet: a client shows each snapshot as it arrives.
- A client should not destroy networked entities: one a client destroys stays gone until the host despawns and spawns it
  again (an acknowledged spawn is never resent).
- Clients cannot reconnect; a session that ended stays ended.
- No protection against connection floods: connections that never finish the handshake hold transport slots until
  `TimeoutSeconds`.
- IPv4 only.

## Transport

UDP through [ENet](https://github.com/lsalzman/enet) (`Network/EnetTransport`), pinned in `cmake/Dependencies.cmake` to
a commit after 1.3.18 for its fragment bounds checks. ENet was chosen over yojimbo and GameNetworkingSockets because it
builds on all three platforms from source alone (no libsodium, mbedTLS, protobuf or OpenSSL), and its surface is small
enough to audit. It gives reliable ordered and unreliable sequenced channels, connection handshakes, round-trip times and
timeouts. The host refuses client messages over 64 KiB before decoding them; ENet caps every message at 1 MiB and the
data each peer may leave waiting to be reassembled at 2 MiB.

**Encryption is not done yet.** Traffic is plain UDP, so a session should not carry secrets. The path to encryption is
ENet's per-datagram compressor hook (`enet_host_compress`), where an AEAD cipher keyed by an authenticated handshake would
seal every datagram without changing the protocol above it.

Every message is untrusted input. `Network/NetBuffer` bounds-checks every read. `Network/NetValue` limits its counts by the
bytes that remain before allocating, limits nesting, and refuses non-UTF-8 text and duplicate keys. `NetProtocol::Decode`
caps record, component and field counts (4,096, 256, 256) before allocating and checks every record
(`NetProtocol::CheckUpdate`, `CheckSpawn`; senders check too: `CheckSpawn` always, `CheckUpdate` as an assert). The
`FuzzNetPacket` target fuzzes the decoder, its re-encoding, a live client applying whatever snapshot decodes and a live
host applying whatever client update decodes (`Tests/Fuzz`).

## Testing

`SimulatedNetwork` is an in-process transport with latency, jitter, loss and timeouts, driven by `Advance` rather than
the wall clock and seeded, so a fixed-timestep test replays exactly. `Tests/Source/NetworkTests.cpp` runs a host and
several clients in one process over it, covering spawns, updates, despawns, ownership, refusals, timeouts, a lossy
reordering link, the bandwidth budget and kinematic driving. The `NetworkSession` ctest runs two `BrecciaRuntime`
processes over real UDP on a port the system picks.
