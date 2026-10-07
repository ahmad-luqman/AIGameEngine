# Tetris (Basalt sample)

A complete Tetris built only with Basalt's automation API and Lua — the way an AI agent builds games
(see `.claude/skills/basalt-make-game`).

- `Assets/Scripts/Tetris.lua` — the whole game (7-bag randomizer, wall kicks, line clears, levels).
- `build_scene.json` — the batch that created `Assets/Scenes/Tetris.bscene`:
  `build/bin/basalt --project Samples/Tetris batch Samples/Tetris/build_scene.json`
- `test_game.json` — headless gameplay checks (run by ctest as `SampleTetris`).

Play: `build/bin/BasaltRuntime --project Samples/Tetris` — Left/Right move, Up rotate, Down soft drop,
Space hard drop, R restart. Export: `build/bin/basalt --project Samples/Tetris project.export '{"output": "Dist/Tetris"}'`.
