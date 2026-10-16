# Agent API: command schemas and MCP

Every engine command and editor command declares an object JSON Schema. `CommandRegistry` validates
parameters before invoking the handler. `help` and MCP `tools/list` publish that same schema; there is
no separate tool catalog. The existing CLI, batch files, editor inspector and TCP automation commands
use the same validation path.

## Headless MCP server

Build Basalt, then configure an MCP client to launch the absolute path to `build/bin/basalt`:

```json
{
  "mcpServers": {
    "basalt": {
      "command": "/absolute/path/to/build/bin/basalt",
      "args": ["--project", "/absolute/path/to/game", "mcp"]
    }
  }
}
```

Omit `--project` to create/open a project through tools. A session persists across calls. Scene edits
stay in memory until `scene.save`; with `--scene`, the CLI also saves dirty edits on clean EOF unless
`--no-save` is set. Stopping play discards its scene copy, just as in the editor. EOF stops play and
shuts down the process. A failed tool call does not end the session or prevent later calls.

The stdio transport accepts newline-delimited UTF-8 JSON-RPC messages, with a 64 MiB per-line limit
and the engine's 128-level JSON nesting limit. Logs go to stderr; add `--verbose` to include Info
messages such as Lua `print`. Stdout contains only MCP replies; notifications receive no reply. An
oversized line closes the transport without a reply. Oversized or incomplete final frames and I/O
failures log a reason to stderr and exit nonzero; requested auto-save is skipped with a diagnostic
when edits remain. Syntax errors (including empty LF/CRLF frames) receive `-32700` and the connection
stays usable. Project/scene startup failures exit nonzero and report to stderr without writing a
non-MCP response to stdout.

## Running editor

The editor's existing `127.0.0.1:7420` server also accepts MCP, including all `editor.*` tools. It remains
a **custom TCP transport**, not an HTTP endpoint. Standard stdio clients can use the included bridge:

```json
{
  "mcpServers": {
    "basalt-editor": {
      "command": "python3",
      "args": ["/absolute/path/to/scripts/mcp_editor.py", "--port", "7420"]
    }
  }
}
```

Start the editor first. The bridge only forwards bytes between stdio and loopback TCP. The editor
serves one connection at a time; a second client waits until the first disconnects. Close other clients
or bridges if initialization is waiting. Oversized requests close without a reply and log the size
and limit in the editor. A new TCP connection must initialize again. The first parsed object containing
`jsonrpc` or `method` selects MCP; a malformed first frame containing either quoted key also selects
MCP and receives a parse error. Other parsed values select the original `{command, params}` protocol.
Blank or unidentifiable malformed frames leave detection open. The selected protocol lasts until
disconnect. Command execution and undo-history commits stay on the editor's main thread. The loopback server gives clients the same editing, file-writing and Lua
execution authority as the existing automation API; use it for trusted local clients.

## Protocol

Basalt supports MCP `2025-11-25`, advertising only the `tools` capability. Unsupported client versions
receive this supported version so the client can decide whether to continue. Send `initialize` with
`protocolVersion`, `capabilities`, and `clientInfo` (`name`, `version`), followed by
`notifications/initialized`. Then use:

- `tools/list`: all commands, sorted by name, in one page. No cursor or list-change notifications.
- `tools/call`: `{name: "entity.create", arguments: {name: "Player"}}`. Arguments default to `{}`.
- `ping`: available before and after initialization.

Tool names retain command names, including dots. Successes include JSON text content and, for object
results, the same value in `structuredContent`. Command validation and execution failures return
`isError: true` with actionable text. Unknown tools and malformed call envelopes return JSON-RPC
`-32602`; unknown methods return `-32601`. Notifications never execute commands. JSON-RPC batches,
resources, prompts, MCP subscriptions and notifications (use `events.poll`), cancellation of running synchronous calls and asynchronous tasks
are not implemented. Tools can take time (for example `play.step`); client timeouts should allow for
that. A client disconnect does not roll back an in-progress command.

Protocol references: [lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools), and
[transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).

## Events, snapshots and assertions

Agents can follow a running game and check it without polling or screenshots:

- `events.subscribe {types?, minLevel?}` collects log lines, script errors, play-state changes (`play`, `simulate`,
  `pause`, `resume`, `stop`) and collisions (`collisionBegin`/`collisionEnd`/`triggerEnter`/`triggerExit` with both
  entities, the frame and the contact). On the editor's legacy line protocol the subscription **streams**: event
  frames arrive on the same connection after a response or within about 100 ms when idle, as JSON lines with an
  `event` key and no `ok` key (`{"event":"collision","subscription":1,"seq":7,"type":"collisionBegin",...}`), and the
  subscription ends when the connection closes. A connection that falls behind gets
  `{"event":"dropped","subscription":N,"count":K}` before the remaining events. `seq` counts every event of the
  session, so one subscription sees gaps. A client that reads one line per request must set such frames aside
  (see `Tests/Automation/EditorViewport.py`). The server serves one connection at a time, so subscribe on the
  connection that sends the commands. MCP clients and the CLI get polled subscriptions instead (an MCP client's
  still end with its TCP connection): `events.poll {subscription}` returns the queued events (at most 1024 wait;
  `dropped` counts the rest; with `assertTypes` it fails, leaving them queued, unless every listed type is there).
  `pause` and `resume` come from the editor's Pause button; no command pauses.
- `scene.snapshot {name}` keeps the current state in memory; `scene.diff {from, to?}` compares two snapshots or
  `.bscene` files (omit `to` to compare with the current scene) by entity ID and lists every changed field, and
  whether the entities were reordered.
- `assert.entity_exists` and `assert.component` fail with `assertion failed: ...` unless the scene matches (for
  `assert.component` that includes a missing or ambiguous entity), so a batch file or tool-call sequence reads as a
  specification (`expectError` turns an expected failure into a pass).
- `render.compare {reference}` renders the current scene and compares it with a reference image (editor only);
  `image.compare` compares two files on any host. Both report `similarity` (1 = identical).

## Schemas and help

```sh
basalt help entity.create
basalt help # all commands, including inputSchema
basalt serve # legacy automation protocol, unchanged
```

The legacy JSON request for one command's help is
`{"command":"help","params":{"command":"entity.create"}}`.
[AutomationCommands.md](AutomationCommands.md) embeds the schemas, regenerated by
`python3 scripts/generate_docs.py`.

Schema builders and validation live in `Automation/CommandSchema.*`. The supported vocabulary is
`type`, `description`, `properties`, `required`, boolean `additionalProperties`, `items`, `minItems`,
`maxItems`, `minLength`, `minimum`, `maximum`, `exclusiveMinimum`, `enum` and `anyOf`. Unsupported
keywords fail at registration. Invalid and duplicate registrations log an error and return false.
This is a deliberately bounded validator for engine-owned schemas,
not a general-purpose loader for arbitrary user schemas. Integer command parameters require integer
JSON tokens; values such as `1.0` are not coerced. Numeric validation rejects non-finite values and
checks signed/unsigned integer bounds without wrapping. Vector elements must fit in a float.

Top-level properties are closed: misspelled parameters fail. Optional parameters preserve their
existing null fallback where supported; `anyOf` with `type: null` explicitly marks those properties.
For example, `entity.create.name` accepts null; `entity.duplicate.name`, `play.step.dt`,
`input.mouse.position` and `input.gamepad.connected` do not. Omit an optional property to use its default.
Required strings are non-empty (except text asset content).
Schemas do not inject defaults. Scene settings, physics layers and component payloads remain objects
whose detailed fields are validated by their existing shared readers, avoiding a second component
data contract. Case-insensitive key/button/axis names, relationships between parameters (such as a
trigger's non-negative axis value), project paths, entity existence and play state remain handler
checks. A schema-valid request can therefore still fail with a meaningful tool error.

Add new commands with an explicit `CommandInfo::InputSchema`; use the builders to keep declarations
readable. Add unit tests for validation before mutation and add automation-visible checks to the
feature-test batch. `McpIntegration` exercises the real subprocess transport and project editing;
`FuzzMcp` covers protocol parsing/lifecycle and safe read-only tool calls.

## Viewport authoring tools

The editor exposes authoring tools through both legacy TCP requests and MCP. Their schemas
are available through `help` / `tools/list`; they are editor-only and are not part of the headless CLI.

- `editor.viewport {}` returns transient gizmo/overlay preferences plus the rendered `width` and
  `height`, logical `logicalWidth`/`logicalHeight`, plus active `renderWidth`/`renderHeight` (which can shrink with dynamic resolution). A patch can set `operation` (`None`, `Translate`, `Rotate`, `Scale`), `space` (`Local`,
  `World`), `snap`, `translationStep` (0.0001–10000 units), `rotationStep` (0.0001–360 degrees),
  `scaleStep` (0.0001–100), or `vertexSnap`. Independent overlay switches are `grid`, `colliders`,
  `lightGizmos`, `cameraGizmos`, `audioGizmos`, `particleGizmos`, `uiBounds`, `debugLines`,
  `selectionOutline`, and `selectionBounds`. Selection bounds are off by default; enable them for
  mesh AABB feedback alongside or instead of the visible-surface outline. Collider visibility applies
  to selected entities too; particle emitter shapes have their own switch. Invalid patches are
  atomic. These preferences last for the editor session and do not modify project/scene files.
- `editor.pick {"x":200,"y":150,"select":true}` renders the current scene and picks at an integer output pixel.
  It returns `{"entity":<UUID>}` (0 for background), and selects by default. `select:false` only queries.
  Enabled light, camera and audio helpers use bounds tested against the mesh surface depth.
  Out-of-bounds coordinates and render/readback failures report errors without changing selection. The
  scene view keeps the editor camera in Play (the game runs in the Game view), so picking works there too. On-demand rendering also handles queued edits and camera changes before the next frame,
  including when the window is minimized. It copies one ID/depth texel and waits for the whole GPU.
- `editor.vertex_snap {"source":[x,y],"target":[x,y]}` translates the selected mesh entity to align
  the nearest source and target vertices within 16 viewport pixels. Coordinates use the displayed
  viewport's logical pixels, top-left origin. The target excludes the selected entity and its
  descendants. Vertices use current geometry, including animation poses, and must project inside the
  camera clip volume; this is projected-vertex snapping, so it can find a vertex behind another mesh.
  Equal screen distances prefer the vertex nearer the camera. The move preserves local rotation and
  scale, supports transformed parents, rejects singular parents, and is one undoable edit. It is
  unavailable in Play/Simulate, for UI rects, or with more than one selected entity.

The viewport toolbar exposes the same preferences. Q/W/E/R select the gizmo operation when the
viewport is focused or hovered and text editing/camera flight are inactive. World/local changes translation and
rotation axes; ImGuizmo scaling follows local axes. Ctrl/Cmd temporarily enables step snapping. Hold
V (or enable Vertex snapping) and drag from a highlighted source vertex to a highlighted target;
release to commit. This mode temporarily overrides every gizmo operation, including None; a click
away from a source vertex still selects. V is ignored during text input or active UI edits. Escape,
changing selection, camera commands, viewport resize, or leaving the viewport cancels the gesture.
Releasing V cancels only when sticky Vertex snapping is off. The camera stays fixed during this gesture. The Overlays popup and View menu
share the same switches; Debug Lines controls scripted debug lines independently of grid/helpers.

## Game view and editor windows

The **Game** tab sits next to the scene view. It renders the primary camera at the game resolution (the
project's `windowWidth`/`windowHeight`, the size the exported game opens with), letterboxed into the panel
with the correct aspect ratio, with the game UI on top (also in a scene without a camera, as in the runtime).
Play brings it to the front and runs the game there: the game UI takes the pointer while the image is hovered
and keyboard/gamepad navigation while the tab has focus; Stop returns to the scene view if that was in front.
Outside Play it previews the primary camera, labelled **Edit mode** or **Simulating**. The scene view always
shows the editor camera with its helpers. Each view renders only while visible, so the default tabbed pair
costs one view.

- `editor.game_view {}` returns `visible`, `mode` (`Edit`, `Play`, `Simulate`), `status` (how the last frame
  went: `rendered`, `notRendered` while the tab is hidden, `noCamera`, `rendererUnavailable`, `renderFailed`), the render `width` and
  `height`, the session `override`, `hasCamera`, `imageRect`/`panelRect` (logical units relative to the
  editor window, as last drawn), `windowSize`, and `renderWidth`/`renderHeight` (render scale and dynamic
  resolution apply as in the game). `resolution: [w, h]` (64–16384) overrides the resolution for this
  editor session without changing the project; `null` returns to the project's. `focus: true` brings the tab
  forward. `path` saves what the Game view shows (primary camera at its resolution, game UI, never a debug
  view, without script debug lines) to a PNG; it fails without a primary camera. It renders through the capture
  renderer at the session's render scale (no dynamic resolution) with exposure reset like `render.screenshot`,
  so the live view's exposure and resolution are untouched. A `resolution` in the same call applies first,
  even if the capture then fails.
- `editor.panel {"name":"Project Settings","open":true,"focus":true}` opens, closes or focuses an editor
  window (`Viewport`, `Game`, `Hierarchy`, `Inspector`, `Scene`, `Content Browser`, `Console`,
  `Project Settings`; only Project Settings can be closed) and returns `{name, open}`.

- `editor.thumbnail {"path":"Assets/Models/Ship.glb","output":"ship.png","refresh":false}` makes (or reads from
  the cache) the content-browser preview of an image, `.hdr` environment, `.gltf`/`.glb` model or `.bprefab`
  and returns `kind` (`Image`, `Environment`, `Model`, `Prefab`), `width`/`height` (at most 128), `cached`,
  `cachePath` and `details` (the tooltip's lines), plus `warnings` when the preview was made but could not be
  stored in the cache. `refresh: true` makes it again, first re-reading the model and the files it uses. Other
  files report that they have no preview.
- `editor.material_preview {"entity":"Ball","output":"ball.png"}` renders an entity's Material on a sphere,
  as the inspector shows it (128x128).

The content browser shows these previews in each row (icon size slider in its header), a larger one with
details on hover (a loaded model's materials as spheres), and its context menu clears the cache.

**Edit > Project Settings** edits the project file through `project.set` (name, start scene, game
resolution, fullscreen, random seed, physics step, graphics defaults, physics layers and the collision
matrix) and lists `project.validate` issues; a scene's own physics step is shown and can be added or removed
(`scene.settings`, `FixedTimestep: null` removes it; an undoable scene edit). Project settings are saved at
once and are not undoable.

## Selection and undo transactions

`editor.select` accepts either `entity` (ID/name/null) or `entities` (an array of IDs/names), plus
`mode`: `replace` (default), `add`, or `toggle`. References are all resolved before selection changes;
duplicates are ignored. The last entry is the active entity and supplies local gizmo axes. The result
contains `entities` and the active `selected` UUID (0 when empty). With no references, replace clears
selection. Selection is transient, never marks the scene dirty, and is not an undo entry. Undo/redo
preserves surviving selected UUIDs; scene replacement clears selection and history.

Ctrl/Cmd-click toggles hierarchy/viewport selection. Shift-click selects a visible hierarchy range;
Shift-click in the viewport adds a hit. Drag a rectangle in the viewport to replace selection; Shift
adds, Ctrl/Cmd toggles. Box selection conservatively intersects world bounds with the rectangle's camera frustum and
includes occluded entities. Meshes use their bounds, animated models use current pose bounds, and
other entities use a 0.5-unit helper box. UI rects are selected through the hierarchy. Escape cancels a
rectangle. `editor.box_select {"start":[x,y],"end":[x,y],"mode":"replace"}` exposes the same operation in
logical viewport pixels, top-left origin; either drag direction works. Click/box selection also works
in Simulate and Play, where it operates on the running copy.

The group gizmo uses the mean selected world position as its pivot, with world axes or the active
entity's local orientation. Translate, rotate and scale affect selected root trees; a selected child
of a selected ancestor inherits the change once. A press/hold/drag/release is one named transaction.
Escape, resize, or losing the selection cancels it. External automation first finishes a pending GUI
gizmo/inspector edit. Scaling follows local axes. Transforms that would require shear, non-finite
values or a singular parent fail before any member changes. UI rects use their canvas fields.
`editor.transform {"delta":[16 floats]}` applies the same column-major world delta matrix. For example,
`[1,0,0,0, 0,1,0,0, 0,0,1,0, 2,0,0,1]` translates the group by two world X units.

The multi-selection inspector displays components shared by all selected entities. Mixed fields show
the active value and a mixed label; an edit replaces that entire field on every selected entity.
`editor.component_set {"component":"Material","data":{"Roughness":0.7}}` validates every merged
component before applying any. Continuous field edits are single undo steps. `editor.delete` and
`editor.duplicate` operate on selected root trees as one step; duplicate selects the copied roots.
References between copied trees are remapped to the corresponding copies; references outside the
selected forest keep their original targets. Delete/duplicate also work on the running Play/Simulate
copy, without dirtying the edit scene or adding undo entries. Group transforms and shared component
edits require edit mode.

Menus show names such as **Undo Set Material AlbedoColor**. `editor.history {}` returns `undo`, `redo`,
`canUndo`, `canRedo`, `transaction`, `index`, `entries` (names and retained bytes), and `patchBytes`.
`editor.undo {}` and `editor.redo {}` restore one entry and return the same history description.
A failed restoration leaves the history cursor unchanged and reports its reason in the response,
status bar and Console. A failed gizmo cancellation commits the gesture as one undo step instead;
the status bar explains how to retry Undo.

To group automation edits across requests:

```json
{"command":"editor.transaction","params":{"action":"begin","name":"Arrange Lights"}}
{"command":"component.set","params":{"entity":"Key","component":"Transform","data":{"Translation":[2,3,1]}}}
{"command":"component.set","params":{"entity":"Fill","component":"Transform","data":{"Translation":[-2,2,1]}}}
{"command":"editor.transaction","params":{"action":"commit"}}
```

Use `action:"cancel"` to restore the pre-transaction scene. Transactions are global to the editor
session, do not nest, and must finish before saving, replacing the scene, playing (including replay),
or undo/redo. They survive client disconnects. The status bar displays the active name with Commit
and Cancel controls; the Edit menu offers the same recovery actions. GUI Play/Simulate rejects an
open automation transaction, just like `play.start`; GUI edits cannot join it. Selection and viewport
options remain available.
A failed command leaves prior successful edits in the transaction so the caller can fix the request
or cancel. Empty/cancelled transactions preserve redo; a new committed change discards the redo branch.
A batch without an explicit transaction retains one entry per top-level mutating command. Read-only and no-op commands add no entries. A command that fails after making changes still records
those changes under its own name (or inside the active transaction). Read-only polling does not serialize the scene; authored
edit revisions trigger history capture. Project files and external assets are outside scene undo.

History retains one current scene plus forward/inverse JSON patches, with UUID keys and successor
links preserving hierarchy order. Retention is capped at 128 entries and 32 MiB of serialized patch
and label payloads; oldest entries are evicted. An individual edit exceeding the entire budget clears
retained history and cannot be undone; a Console warning and status message report this. Scene
serialization/diffing and restoration take time proportional to scene size.
