# Networking

Breccia's multiplayer is client-server with an authoritative host. The host owns the shared state: it runs the game,
admits clients, and sends each of them the entities marked `Networked` as snapshots. Clients show that state and send
back only what they have authority over, the inputs of the characters they predict, and script messages. This page covers
the transport, sessions and replication, interpolation, prediction and lag compensation, and the `net.*` automation
commands. The Lua `Network` API is in `Docs/ScriptingAPI.md` (Networking). `Samples/Arena` is a complete multiplayer game
built on it (sessions, spawning and ownership, prediction, interpolation, lag compensation, RPCs and the `net.*`
commands), with tests that play it in one process and over UDP.

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

Scripts start sessions too (`Network.Host`, `Network.Connect`, `Network.Disconnect`), and the editor's Play can host or
join over UDP (the toolbar's Network button), so the editor and runtimes play together. `Game.IsHeadless()` says the
game runs with no window (`--headless`, or the `breccia` CLI); with `Network.IsHost()` it tells a dedicated server,
where a game usually spawns no player for the host. `--host 0` lets the system pick a
free port; the log says which (`Network: hosting on UDP port N`). Headless runs with a
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
| `InterpolationDelay` | 0.1 | Seconds (0 to 1) a client shows the entities it does not decide behind its estimate of the host's clock (`Network.GetTime`; the moment it shows is `Network.GetViewTime`), moving them smoothly between snapshots; 0 shows each snapshot as it arrives. A client uses its own project's. |
| `MaxMessagesPerSecond` | 120 | Script messages and RPCs each connection may send per second and direction; a host hands a client's to scripts at this rate and disconnects a client that keeps sending faster. |

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

Peer IDs start at 1 and are never reused in a session; the host is 0. Scripts hear of connections through `Network.On`:
`PeerConnected` and `PeerDisconnected` (with the reason) on the host, `Connected` and `Disconnected` on a client. A client
whose session ended may connect again (it joins as a new peer, so entities its old peer ID owned are not its own).

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

A client shows the transforms of entities it does not decide `InterpolationDelay` behind the host (see Interpolation),
and every other value as it arrives.

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
- `Predicted`: the host decides, from the inputs of the client named by `Owner`, which predicts the entity's character
  controller (see Prediction). On an entity without a character controller (or whose character the host could not build),
  or while that client is not connected, this is `Server` on the host (a client whose character exists still predicts it,
  and the host ignores its inputs, logged).

Ownership, parents and components are the host's alone: clients cannot change them (the host refuses and logs it), and an
owner still hears of components the host adds (with their starting values) or removes. A peer drives the dynamic bodies of
entities it has no authority over kinematically (`PhysicsWorld::SetDrivenExternally`), so they follow the replicated
transform instead of simulating against it. The component keeps its `Type`. Character controllers it neither decides nor
predicts are driven too: they follow the replicated transform without gravity or `Move`, and other characters still meet
them. Ragdolls are not driven this way: on a peer without authority they keep simulating between snapshots.

`Owner` is a peer ID assigned at runtime: clients get 1, 2, ... in the order they join, so a saved `Owner` hands the
entity to whichever client joins in that place. Usually the host's scripts set it during play.

## What clients do with the scene

A client loads the same scene as the host, keeps everything that is not networked (level geometry, cameras, UI), and
removes its own copies of networked entities when the host accepts it, with their descendants: the host spawns the real
ones, descendants included. Scripts run on
every peer, so a script on a networked entity runs on clients too: `Network.IsHost`, `Network.IsOwner` and
`Network.HasAuthority` tell it which peer it is on and what it decides. Its `Network.On` handlers end with the local copy
the client removes, so a script that must hear `Connected` belongs on an entity that is not networked. A client's runtime entity UUIDs come from a stream of their own (the session seed mixed with a constant):
two independent 64-bit streams practically never meet, and a spawn whose ID a client's own runtime entity happens to hold
takes that entity over.

What a client refuses (a spawn naming a file outside the project, records a scene file would not pass, a value a component
refuses) is logged once per entity and cause (a component, for values), and at most 256 problems per peer; the host counts
a refused spawn as delivered, so that entity stays missing on that client. Statistics (`NetworkSystem::GetStatistics`,
and the summary a `--frames` run logs) count rejected values and records separately.

Current limits, each planned for a later session:

- Scene loads are refused during a session (`Scene.Load` fails with an error); every peer would have to switch together.
- A client should not destroy networked entities: one a client destroys stays gone until the host despawns and spawns it
  again (an acknowledged spawn is never resent).
- A client that reconnects is a new peer: there is no way yet to take back the old peer ID and what it owned.
- No protection against connection floods: connections that never finish the handshake hold transport slots until
  `TimeoutSeconds`.
- IPv4 only.

## Scripts: messages and RPCs

Scripts send named messages (`Network.Send`) and call methods of other peers' scripts (`Network.Rpc`), reliably or not
(`Docs/ScriptingAPI.md`). They travel as their own `Script` messages, apart from snapshots: a client's go to the host only,
which knows the sender from the connection, never from the message. An RPC names a method the entity's script lists in
its `Rpcs` table with who may call it (`Host`, `Owner` or `Any`); anything else is dropped. A message may not be named like a
connection event (`Connected`, `Disconnected`, `PeerConnected`, `PeerDisconnected`): the decoder refuses one, so a peer
cannot fake them. Limits: a name of 1 to 64 bytes, 32 arguments, 32 KiB in all. Each connection sends at most
`MaxMessagesPerSecond` a second, and they share its `MaxBytesPerSecond` with snapshots (one message may run the budget into
debt, as a record may). A host keeps a client's messages in a queue, as they arrived, and decodes and hands them to scripts at
that rate, so a burst waits and a client cannot make the host's scripts run more often than that; more than 1,024 waiting,
or 1 MiB, disconnect it (`InvalidPacket`), and when a client leaves, what it sent first reaches the scripts before
`PeerDisconnected` (up to a message that does not decode: it and the rest are dropped, counted and logged). Messages wait
in the queue, too, while the host's event queue is full. An RPC that reaches a client before the entity's spawn waits for it for two seconds, and the reliable script
messages after it wait behind it, so they arrive in order (unreliable ones promise no order and do not wait); an RPC for an
entity the client saw despawned, or whose spawn it refused, in the last two seconds is dropped at once, as the host drops an
RPC for an entity it does not have. Messages and
connection events reach scripts before `OnUpdate`, never in the middle of the network session's own work.

## Interpolation

Snapshots arrive a few times a second, and some are lost. A client shows the entities it does not decide (or predict) as
they were `InterpolationDelay` seconds before the newest snapshot, moving each smoothly between the two snapshots around that
moment (positions and scale linearly, rotations by slerp). Snapshots carry the host's clock; the client follows it,
smoothing the jitter of their arrival, and `Network.GetViewTime()` is the moment it shows. Delta compression leaves out what
did not change, so an entity at rest starts moving from where it rested as of the snapshot before its change. A new parent
makes it jump, as does a client's own change to the transform, which stops the interpolation until the host's next update.
`InterpolationDelay` 0 turns interpolation off. Physics steps are not interpolated yet (fixed-step interpolation, P2 in
`ROADMAP.md`), so a predicted character moves at the fixed step rate.

## Prediction

A character controller with `Predicted` authority moves on the client that owns it as soon as its scripts call `Move`
(only the owner should: `Network.IsOwner`). Every fixed step the client records what the character walked with (its `Move`
velocity, mixed with root motion, and its rotation) and numbers it; each update to the host carries the newest steps the
host has not acknowledged (up to 32, so a few lost updates lose nothing), and a client that predicts sends an update at
least every 16 fixed steps, whatever the tick rate, so every input goes out. The host moves the character by exactly those
steps (`PhysicsWorld::StepCharacter`, the same code a physics step runs), never by its own physics steps or `Move` calls.
The host trusts what an input asks for only so far:

- No faster than its own fixed steps: it keeps half a second of room for inputs, half of it ahead of its steps, so a client
  sending inputs faster than real time waits and moves no faster.
- No faster than `Networked.MaxInputSpeed`, the length of the whole velocity, vertical included: set it to the fastest
  velocity a step asks for (`sqrt(run^2 + jump^2)` when the character can jump while running), and a modified client runs
  and jumps no faster. The owner sends and replays its inputs with the same clamp (only its own step of a faster `Move`
  is corrected), from the value its spawn carried: the field is not replicated afterwards, so set it before the entity is
  networked. A velocity above 1,000 m/s is invalid on the wire.
- An owner that falls silent for a quarter of a second (half the input room) has its character stepped without inputs
  (gravity still pulls), at most 8 steps a frame, so it does not hang in mid-jump; its inputs, when they come again, follow,
  and the client is corrected. The quarter second counts from the owner's first input, whatever its trip to the host took.
- A `Predicted` entity the host cannot move (no character controller built there) is still predicted by its owner; the
  host ignores its inputs and logs it once, and the owner is never corrected.

Each snapshot to the owner carries the character's state after the newest input the host applied; the client puts the
character there and replays the inputs since, which leaves it where it already was when nothing disagreed. Where something
did (a door the host closed, a respawn), the character snaps to the corrected place; `NetworkStatistics::Corrections` counts
those of more than a centimetre. The host's other clients see the character like any remote entity.

## Lag compensation

A client acts on what it sees, which is `InterpolationDelay` and half a round trip old by the time the host hears of it. A
host records where every networked body that moves (a dynamic or kinematic rigid body, or a character) was at each tick, for
the last second, whether or not a client is connected. `Network.Raycast(origin, direction, maxDistance, time)` (`NetworkSystem::RaycastAt`) casts against those
bodies where they were at `time`, interpolated between ticks, with their current shapes; bodies the host had not recorded then
(created later) are not hit, and everything else (level geometry, bodies that are not networked, the parts of an active
ragdoll) is tested where it is. The time comes from the
client (its `Network.GetViewTime()` in the RPC that fires), clamped to the last second, so a client can at most aim a second
into the past.

## Automation and testing

The `net.*` commands play a multiplayer game headless, in one process (`Docs/AutomationCommands.md`):

```json
{ "command": "play.start" }
{ "command": "net.host" }
{ "command": "net.connect" }
{ "command": "net.link", "params": { "latency": 0.05, "jitter": 0.01, "loss": 0.05 } }
{ "command": "play.step", "params": { "frames": 120 } }
{ "command": "net.status" }
{ "command": "scene.hash", "params": { "client": 1 } }
```

`net.host` makes the playing scene the host, on the session's in-process network (`SimulatedNetwork`) or with `udp` on a
real port. `net.connect` without an address adds an in-process client: another copy of the edit scene playing as a client,
stepped with the host by `play.step` (the network advances one frame at a time, so a run replays exactly; a client's script
errors fail `assertNoErrors` too, listed as `clientScriptErrors`); with an address the playing scene joins a UDP host.
`net.link` sets latency, jitter and loss, `net.focus` picks which peer injected input reaches (the others' games read no keys,
buttons, mouse motion, scroll, text or gamepads; the pointer position stays visible), `net.status` reports connections and
statistics, `net.command` runs a read command, `component.set` or `lua.exec` on a client's scene, and `net.disconnect` ends a
client or the session. `scene.hash` with `client` hashes a
client's scene: peers agree once the networked entities are at rest, as a client shows moving ones behind the host. Under
the CLI, scripts' `Network.Host` and `Network.Connect` use the same in-process network, so `net.connect` can join a game
that hosts itself.

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
(`NetProtocol::CheckUpdate`, `CheckSpawn`, `CheckScript`; senders check too: `CheckSpawn` and `CheckScript` always,
`CheckUpdate` as an assert). Snapshot times, predicted states and inputs must be finite (inputs at most 1,000 m/s;
predicted velocities at most 1,000 m/s and positions within 10,000 km of the origin on each axis; rotations of roughly unit length, numbered without
wrapping), and each character appears at most once. The `FuzzNetPacket` target fuzzes the
decoder, its re-encoding, a live client applying whatever snapshot or script message decodes and a live host applying
whatever client update or script message decodes, inputs to a predicted character included (`Tests/Fuzz`). The protocol is
version 2 (`NetProtocol::Version`): Networking I's peers are refused with `ProtocolMismatch`.

## Testing

`SimulatedNetwork` is an in-process transport with latency, jitter, loss and timeouts, driven by `Advance` rather than
the wall clock and seeded, so a fixed-timestep test replays exactly. `Tests/Source/NetworkTests.cpp` runs a host and
several clients in one process over it, covering spawns, updates, despawns, ownership, refusals, timeouts, a lossy
reordering link, the bandwidth budget and kinematic driving; `NetworkGameplayTests.cpp` covers script messages and RPCs,
connection events, interpolation, prediction, lag compensation, the Lua API and the `net.*` commands. The `NetworkSession` ctest runs two `BrecciaRuntime`
processes over real UDP on a port the system picks.
