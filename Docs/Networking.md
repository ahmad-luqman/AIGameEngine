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
connected when the run ends exits with code 8, so a test cannot pass without a session.

The project file's `Network` object (also `project.set {"network": {...}}`) holds the session settings:

| Key | Default | Meaning |
|-----|---------|---------|
| `Port` | 7777 | The UDP port a host listens on, and a client's default. |
| `MaxClients` | 8 | Clients a host admits at once; the next is refused (`ServerFull`). |
| `TickRate` | 30 | Snapshots per second to each client, and updates per second back. A client takes the host's. |
| `MaxBytesPerSecond` | 65536 | Each connection's budget per direction. What does not fit waits. |
| `TimeoutSeconds` | 10 | A connection that hears nothing for this long ends (`Timeout`). |

## Connections

A client connects, then introduces itself with a `Hello`: the protocol version (`NetProtocol::Version`), the hash of
what replicates (`ReplicationSchema`, the component registry's names, replicated fields and their types), and the game
(the project's `Name` and `Version`). The host answers `Welcome` with the client's peer ID, or ends the connection with a
reason. The client learns the reason before the connection closes.

| Reason | When |
|--------|------|
| `ProtocolMismatch` | The peers speak different protocol versions. |
| `SchemaMismatch` | They replicate different components or fields (different engine builds). |
| `GameMismatch` | The client plays another game, or another version of it. |
| `ServerFull` | `MaxClients` clients are connected. |
| `HandshakeTimeout` | The client connected but never sent its `Hello`. |
| `InvalidPacket` | A message did not decode, or a client sent one only hosts send. |
| `Timeout` | Nothing arrived for `TimeoutSeconds`. |
| `ConnectionFailed` | The host never answered. |
| `ClientQuit`, `ServerShutdown` | The client left, or the host stopped. |

Peer IDs start at 1 and are never reused in a session; the host is 0.

## Replication

An entity with a `Networked` component replicates, together with its descendants. A descendant with a `Networked`
component of its own replicates separately, under it.

- **Spawns.** A client receives the entity's whole tree as scene-file records, with the host's UUIDs, so the IDs agree on
  every peer. It creates them through `SceneSerializer`, so every value passes the checks a scene file's does. Asset
  references must stay inside the project (relative, without `..`); a spawn naming any other file is refused. `Prefab`
  tracking is left out: instances created during Play keep none.
- **Updates.** Afterwards only the fields components declare in `ComponentInfo::ReplicatedFields` travel, and only when
  they change: `Transform` (Translation, Rotation, Scale), `Networked` (Owner, Authority), `Animator` (Clip, Speed),
  `Material` (AlbedoColor, EmissiveColor, EmissiveIntensity) and the lights' Color and Intensity. A component with
  replicated fields that is added or removed is added or removed on clients too; components without them, and fields
  not listed, reach a client only inside a spawn. A changed parent replicates.
- **Despawns.** Destroying the entity, or removing its `Networked` component, removes it from clients.

Values are sent as a compact binary form of the registry's JSON (`Network/NetValue`). Changes are found by comparing
those encodings, and each client receives them through `ComponentRegistry`, so a value a component rejects is refused
and logged. Adding a replicated field is a registry change, which moves the schema hash, so mismatched builds refuse
each other.

### Deltas and loss

Snapshots travel on the unreliable channel. Each carries every field whose value differs from the last one that client
acknowledged, or that went out in a snapshot it has not yet acknowledged. The newest snapshot a client applies therefore
completes its state, and it drops any older snapshot that arrives later. Spawns repeat until acknowledged and despawns
likewise. A lost snapshot costs a resend, never a stale value. Clients acknowledge the newest snapshot in every update
they send.

### Bandwidth

Each connection spends at most `MaxBytesPerSecond` per direction, refilled continuously and capped at one second's
worth. Within a snapshot, entities whose changes wait gain their `Networked.Priority` every tick, and the highest totals
go first, so a busy scene shares the budget instead of starving anything. A record larger than the remaining budget
still goes when nothing else has (the budget runs into debt), so a large spawn is never stuck. One no message could carry
(more than 1 MiB) is logged and never sent.

## Authority

`Networked.Authority` decides who sets an entity's replicated fields:

- `Server` (the default): the host.
- `Owner`: the client whose peer ID is `Owner` sends them, and the host checks that the sender owns the entity before
  applying them and passing them on. While that client is not connected, or `Owner` is 0, the host decides.

Ownership, parents and components are the host's alone: clients cannot change them. A peer drives the dynamic bodies of
entities it has no authority over kinematically (`PhysicsWorld::SetDrivenExternally`), so they follow the replicated
transform instead of simulating against it. The component keeps its `Type`.

`Owner` is a peer ID assigned at runtime, so a value saved in a scene means nothing until gameplay sets it (Lua
`SetComponent`, or the host's scripts, which own it).

## What clients do with the scene

A client loads the same scene as the host, keeps everything that is not networked (level geometry, cameras, UI), and
removes its own copies of networked entities when the host accepts it: the host spawns the real ones. Scripts run on
every peer, so a script on a networked entity runs on clients too. A Lua API for telling roles apart comes in
Networking II. A client's runtime entity UUIDs come from a stream of their own (the session seed mixed with a constant),
so what a client spawns locally never takes an ID the host gives its entities.

Current limits, each planned for a later session:

- Scene loads are refused during a session (`Scene.Load` fails with an error); every peer would have to switch together.
- Interpolation and prediction are not done yet: a client shows each snapshot as it arrives.
- A client should not destroy networked entities: one a client destroys stays gone until the host despawns it.
- Clients cannot reconnect; a session that ended stays ended.
- IPv4 only.

## Transport

UDP through [ENet](https://github.com/lsalzman/enet) (`Network/EnetTransport`), pinned in `cmake/Dependencies.cmake` to
a commit after 1.3.18 for its fragment bounds checks. ENet was chosen over yojimbo and GameNetworkingSockets because it
builds on all three platforms from source alone (no libsodium, mbedTLS, protobuf or OpenSSL), and its surface is small
enough to audit. It gives reliable ordered and unreliable sequenced channels, connection handshakes, round-trip times and
timeouts. The host refuses client messages over 64 KiB before decoding them, and ENet itself caps every message, and the
data a peer may leave waiting, at 1 MiB.

**Encryption is not done yet.** Traffic is plain UDP, so a session should not carry secrets. The path to encryption is
ENet's per-datagram compressor hook (`enet_host_compress`), where an AEAD cipher keyed by an authenticated handshake would
seal every datagram without changing the protocol above it.

Every message is untrusted input. `Network/NetBuffer` reads with bounds checks and limits every count by the bytes that
remain before anything is allocated. `Network/NetValue` limits nesting, refuses non-UTF-8 text and duplicate keys, and
`NetProtocol::Decode` checks every component and field index against the schema. The `FuzzNetPacket` target fuzzes the
decoder, its re-encoding, and a live client applying whatever decodes (`Tests/Fuzz`).

## Testing

`SimulatedNetwork` is an in-process transport with latency, jitter, loss and timeouts, driven by `Advance` rather than
the wall clock and seeded, so a fixed-timestep test replays exactly. `Tests/Source/NetworkTests.cpp` runs a host and
several clients in one process over it, covering spawns, updates, despawns, ownership, refusals, timeouts, a lossy
reordering link, the bandwidth budget and kinematic driving. The `NetworkSession` ctest runs two `BrecciaRuntime`
processes over real UDP on a port the system picks.
