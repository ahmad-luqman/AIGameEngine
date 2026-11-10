# Arena (Breccia sample)

A small multiplayer arena, seen from above, for up to four clients plus a host that plays. Each player walks with WASD
(or the left stick) and tags with Space (or the pad's A or right bumper): a beam along where it faces. A tag on another
player or on one of the two bots that walk lanes across the arena scores a point. A tagged player goes back to a spawn
point, a bot to the start of its lane. The first to five wins the round, and three seconds later a new round starts.
Like the other samples, the scene, the input actions and the player prefab are built only through the automation API
(`build_scene.json`).

It puts the networking of sessions 26 and 27 together (`Docs/Networking.md`):

- **Sessions.** `BrecciaRuntime --project Samples/Arena --host 7777` hosts and plays (a listen server);
  `--headless --host 7777` is a dedicated server, which has no player of its own (`Game.IsHeadless`); `--connect
  ADDRESS[:PORT]` joins. Without either, the game is practice against the bots. `Match.lua` sits on an entity that is not
  networked, so a client keeps it when the host's entities replace its own and hears `Connected` there. When its host
  goes away, a client says why and stays in its ended session, so the runtime still reports it (a dialog, or exit code 8
  for a `--frames` run), until its player presses Enter (or the pad's Start) to practise.
- **Spawning and ownership.** The host spawns `Assets/Prefabs/Player.bprefab` for every peer that joins (and its own
  player unless headless), sets `Networked.Owner` to that peer before the next tick, and removes the player when the peer
  leaves. A new player starts at the spawn point farthest from the other players.
- **Prediction.** Players are character controllers with `Predicted` authority: the owner moves at once from its own
  input, and the host moves the character from the inputs it sends, no faster than `Networked.MaxInputSpeed` (6 m/s, a
  little above the walking speed of 5 m/s, so the owner's own clamp never cuts it). A respawn on the host corrects the
  owner.
- **Lag compensation.** A tag is an RPC to the host (`Player.Rpcs = { Tag = "Owner" }`) carrying the direction and the
  owner's `Network.GetViewTime()`. The host refuses what is not a direction and a finite time (an error in an RPC would
  stop the player's script on the host), allows one tag per cooldown, and casts from where it has the player. It asks
  `Network.Raycast` about where the bodies stood at that time, so a tag hits what the client saw though the bots are
  already further along on the host, but no further back than the client's round trip plus 0.25 s (its 0.1 s
  InterpolationDelay, a snapshot interval and jitter; `Arena.MaxViewLag`, to raise with the project's delay): a modified
  client cannot reach a second into the past. It does still choose the direction. Scoring runs in Match, reached
  through an event, so an error there stops Match's script, not the shooter's.
- **Interpolation.** The bots are networked characters with `Server` authority: the host walks them, and clients show
  them `InterpolationDelay` behind, moving smoothly between snapshots.
- **Scores by RPC.** The host keeps the scores (`Match.lua`) and sends the whole table, with the round's winner, to every
  client through the networked `Scoreboard` entity's `ShowScores` RPC, which only the host may call. It sends it whenever
  something changes and to every client that joins (RPCs are not replayed to clients that join later); a client whose
  send fails (its message rate or bandwidth spent) gets it the next frame, and the others are not sent it again. Every peer draws its own HUD from it, and the tag beams are drawn on every peer from
  the host's `ShowTag` RPC.
- **The break between rounds.** Players and bots stand still. The players' scripts stop them once they hear of it, and
  the host holds each player too, since it moves a predicted character from whatever its owner sends: after the
  player's round trip and view lag (its last honest steps are still arriving), wherever it stands then, a player that
  appears during the break included.
- **Settings.** `Match`'s `WinScore`, `RoundBreak` and `HostPlays` are checked when the game starts (an error), and a
  script that later sets one to something it cannot be gets a warning while the last good value stays.

The scripts: `Match.lua` (session, players, scores, rounds), `Player.lua` (movement, tags), `Bot.lua` (the lanes),
`Scoreboard.lua` (the HUD's table), `Fade.lua` (a beam's short life), `Lib/Arena.lua` (what they share: names, reading
what a client sent, finding each other) and `Autoplay.lua` (the computer playing, for the network test; it is not in the
scene).

Play: `build/bin/BrecciaRuntime --project Samples/Arena --host 7777`, then
`build/bin/BrecciaRuntime --project Samples/Arena --connect 127.0.0.1:7777` for each other player.
Rebuild the scene and the prefab: `build/bin/breccia --project Samples/Arena batch Samples/Arena/build_scene.json`.

## The tests

`test_game.json` (ctest `SampleArena`) plays a whole session in one process with the `net.*` commands. The network
advances one frame at a time, so it runs the same way every time. Its `lua.exec` steps share helpers from
`TestHelpers.lua` (outside `Assets/`, so exports leave it out). It checks:

- The edit scene: the scoreboard and both bots are networked, `Match` is not, and no player or Autoplay is in the scene.
- Practice: the player spawns, walks at 5 m/s and turns to face where it walks. The cooldown holds on both ends (a second
  press within it asks for nothing, `Player:Tag`, the host's check, ignores a second tag that soon, and a tag after it
  counts). A tag on a passing bot scores, sends the bot back to the start of its lane and shows a beam that fades, and a bot turns
  round at the end of its lane.
- Hosting under the CLI, which is headless: the host's player goes (a dedicated server). Two clients join and every peer
  shows both players and the same scores, and client 1 their colors. `HostPlays`: "Auto" plays when the host renders,
  "No" never, "Yes" always; a wrong `HostPlays` or `WinScore` set in play is warned about and the last good one kept.
- Prediction over a link with 100 ms of latency each way: client 1's player moves on client 1 on the first frame, before
  the host has the input, and every peer agrees where it stops.
- Lag compensation: a bot walks through client 1's line of fire; client 1 sees it 0.2 s (the latency plus the 0.1 s
  InterpolationDelay), 0.6 m, behind the host, more than the bot's radius. Client 1 sends the time it saw, and tags the
  bot where it saw it while the host already has it more than its radius off the line. Every peer draws one beam, which
  ends at the bot.
- A round: with `WinScore` 1 that tag wins, and every peer shows "Player 1 wins!". During the break client 1 cannot move,
  BotA stands still, the host ignores tags (and counts them), and a modified client that walks on is held in place by the
  host. The next round puts everyone back at spawn points (correcting client 1's player).
- A modified client that sends a time almost a second back, when the bot crossed its line as the host saw it, misses: the
  host clamps the time. A tag sent 0.1 s late, about 0.43 s behind the host, still hits what the client saw.
- A round that ends while client 1 walks does not pull it back, and a player that appears during that break (one
  respawned after something destroyed it) is held where it stands.
- A player tags a player: client 2 tags client 1, which is corrected to its new spawn point.
- A client that joins late sees the whole table, and a destroyed player comes back with its row. A client that leaves
  loses its player and its row on the host and the other clients.
- What a modified client may not do: tag as another client's player, or call the host-only `ShowScores` and `ShowTag`;
  tag twice in a frame, or 0.33 s apart (a tag 0.45 s later counts: still within the 0.5 s cooldown, but past the 80% of
  it, 0.4 s, that the host enforces); or send malformed tags (short, empty or named vectors, a bad second or third component, a
  string, a number, a boolean, infinite and NaN values, a straight-up direction, huge ones). The host refuses them all, warning once, and the player can still be tagged
  afterwards; a tag time past a million seconds (a session weeks old) is a time like any other. A client's own copy of a
  player never tags, and a beam or scoreboard table that is not one (a malformed, holed or repeated row, a winner that is
  no peer) is kept out, warning once.
- A score send that fails is retried to the client that missed it, with one warning, until it gets through.
- When the host stops hosting, each client says why and stays in its ended session; client 1 presses Enter, removes the
  host's players and practises, while client 3 still waits. A session that starts again is a new match: the host's player may tag at once (its cooldown does
  not follow the session clock), and a client that joins it forgets why it left the last one.

Every `play.step` fails on script errors, the session logs no warnings until the forbidden and malformed calls (which
warn, as they should, and whose warnings the steps count), and the edit scene is unchanged after play.

`test_network.py` (ctest `SampleArenaNetwork`) plays the arena over real UDP: a headless `BrecciaRuntime --host 0` and two
`--connect` clients, in a copy of the sample with an `Autoplay` entity added and Match's `WinScore` set to 3. Autoplay plays the clients'
players: player 1 walks to a spot on the north side and tags east along it; player 2 stands at the east end of that side
and walks back after every tag sends it to the south-east corner. Player 1 wins whatever the timing. The host then waits
a second and a half (90 of its fixed frames) for everyone to come to rest and tells the clients to report. Every peer logs
the scores and where it sees each character, says it is quitting, and quits (a peer that ran out of frames instead
fails the test). The test requires one result line per peer with the same scores (1:3, 2:0) and the same positions
(within 2 cm) on all three, no rejected values or records in any peer's network statistics, and none of the listed
problems in any peer's log (refusals, failed sends, the engine's once-only reports and the sample's own warnings).
Nothing in it depends on how fast the machine is.
