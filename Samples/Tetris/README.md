# Tetris (Basalt sample)

A complete Tetris built only with Basalt's automation API and Lua — the way an AI agent builds games
(see `.claude/skills/basalt-make-game`).

- `Assets/Scripts/Tetris.lua` — the whole game (7-bag randomizer, wall kicks, line clears, levels). Cleared
  rows burst into sparks (a world-space particle emitter moved to each row) and shake the camera: trauma that
  decays over time, driven by triangle waves rather than `Math.Random`, so the shake never changes which pieces
  come next, and moves the camera identically on every platform (no `math.sin`), so replays stay portable.
- `build_scene.json` — the batch that created `Assets/Scenes/Tetris.bscene`:
  `build/bin/basalt --project Samples/Tetris batch Samples/Tetris/build_scene.json`
- `test_game.json` — headless gameplay checks (run by ctest as `SampleTetris`).

Play: `build/bin/BasaltRuntime --project Samples/Tetris` — Left/Right move, Up rotate, Down soft drop,
Space hard drop, R restart. Export: `build/bin/basalt --project Samples/Tetris project.export '{"output": "Dist/Tetris"}'`.
