# Arena (Breccia sample)

A small multiplayer arena, seen from above, for up to four clients plus a host that plays. Each player walks with WASD
(or the left stick) and tags with Space (or the pad's A or right bumper): a beam along where it faces. A tag on another
player or on one of the two bots that walk lanes across the middle scores a point and sends the target back to a spawn
point. The first to five wins the round, and three seconds later a new round starts. Like the other samples, the scene,
the input actions and the player prefab are built only through the automation API (`build_scene.json`).

It puts the networking of sessions 26 and 27 together (`Docs/Networking.md`):

- **Sessions.** `BrecciaRuntime --project Samples/Arena --host 7777` hosts and plays (a listen server);
  `--headless --host 7777` is a dedicated server, which has no player of its own (`Game.IsHeadless`); `--connect
  ADDRESS[:PORT]` joins. Without either, the game is practice against the bots. `Match.lua` sits on an entity that is not
  networked, so a client keeps it when the host's entities replace its own, and hears `Connected` there.
- **Spawning and ownership.** The host spawns `Assets/Prefabs/Player.bprefab` for every peer that joins (and its own
  player unless headless), sets `Networked.Owner` to that peer before the next tick, and removes the player when the peer
  leaves. A new player starts at the spawn point farthest from everyone else.
- **Prediction.** Players are character controllers with `Predicted` authority: the owner moves at once from its own
  input, and the host moves the character from the inputs it sends, no faster than `Networked.MaxInputSpeed` (6 m/s, a
  little above the walking speed of 5 m/s, so the owner's own clamp never cuts it). A respawn on the host corrects the
  owner.
- **Lag compensation.** A tag is an RPC to the host (`Player.Rpcs = { Tag = "Owner" }`) carrying the direction and the
  owner's `Network.GetViewTime()`. The host checks what a client sent (a direction, a number, the cooldown), casts from
  where it has the player, and asks `Network.Raycast` about where the bodies stood at that time, so a tag hits what the
  client saw, though the bots are already further along on the host.
- **Interpolation.** The bots are networked characters with `Server` authority: the host walks them, and clients show
  them `InterpolationDelay` behind, moving smoothly between snapshots.
- **Scores by RPC.** The host keeps the scores (`Match.lua`) and sends the whole table to every client through the
  networked `Scoreboard` entity's `ShowScores` RPC, which only the host may call. Sending it whole means a client that
  joins late gets the current table. Every peer draws its own HUD from it, and the tag beams are drawn on every peer
  from the host's `ShowTag` RPC.

The scripts: `Match.lua` (session, players, scores, rounds), `Player.lua` (movement, tags), `Bot.lua` (the lanes),
`Scoreboard.lua` (the HUD's table) and `Fade.lua` (a beam's short life).

Play: `build/bin/BrecciaRuntime --project Samples/Arena --host 7777`, then
`build/bin/BrecciaRuntime --project Samples/Arena --connect 127.0.0.1:7777` for each other player.
Rebuild the scene and the prefab: `build/bin/breccia --project Samples/Arena batch Samples/Arena/build_scene.json`.

## The tests

`test_game.json` (ctest `SampleArena`) plays a whole session in one process with the `net.*` commands. The network
advances one frame at a time, so it runs the same way every time. It checks:

- The edit scene: the scoreboard and the bots are networked, `Match` is not, and no player is placed in the scene.
- Practice: the player spawns, walks at 5 m/s and turns to face where it walks, the cooldown holds on both ends (a second
  press sends nothing, and the host refuses a second tag that soon), and a tag on a passing bot scores, sends the bot
  back to the start of its lane and shows a beam that fades.
- Hosting under the CLI, which is headless: the host's player goes (a dedicated server). Two clients join and every peer
  shows both players and the same scores. Then `HostPlays = "Yes"` brings the host's player in.
- Prediction over a 100 ms link: client 1's player moves on client 1 on the first frame, before the host has the input,
  and every peer agrees where it stops.
- Lag compensation: a bot walks through client 1's line of fire; client 1 sees it 0.2 s (0.6 m) behind the host, more
  than the bot's radius, and tags it where it saw it. A cast at the host's present would miss, and the test checks that it
  would.
- A round: with `WinScore` 1 that tag wins, every peer shows "Player 1 wins!", nobody can move during the break, and the
  next round puts everyone back at spawn points (correcting client 1's player).
- A player tags a player: client 2 tags client 1, which is corrected to its new spawn point.
- A client that joins late sees the whole table. A client that leaves loses its player and its row on every peer. When
  the host stops hosting, the clients say why and the host plays on alone.

Every step fails on script errors, the session logs no warnings until the host stops (each client then warns that the
host left), and the edit scene is unchanged after play.

`test_network.py` (ctest `SampleArenaNetwork`) plays the arena over real UDP: a headless `BrecciaRuntime --host 0` and two
`--connect` clients, in a copy of the sample with `Match.Autoplay` on and `WinScore` 3. Autoplay plays the clients'
players: player 1 walks to a spot on the near side and tags along it, and player 2 stands at the far end and walks back
after every tag. Player 1 wins whatever the timing. The host then waits a second and a half for everyone to come to rest
and tells the clients to report. Every peer logs the scores and where it sees each character, and quits. The test
requires the same scores (1:3, 2:0) and the same positions (within 2 cm) on all three, and that nothing was refused or
malformed. Nothing in it waits a fixed time.
