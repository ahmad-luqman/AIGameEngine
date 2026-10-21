# Asset identity and mesh import

Assets acquire a persistent 64-bit UUID when referenced by a saved scene/prefab or project start scene,
or through move/rename or explicit `asset.id {"path":"Assets/Models/Ship.glb"}`. Commit the adjacent `Ship.glb.meta`
with the source file. Metadata contains `{"Version":1,"UUID":"123456789"}`; UUIDs are decimal strings
so JSON clients never lose integer precision. Loading a project scans existing metadata without writing
files. Duplicate UUIDs and corrupt metadata are reported by project validation and excluded from resolution;
unrelated assets remain usable. Neither copy wins an ambiguous identity. Repair the affected sidecars
explicitly rather than silently assigning new identities. Copying an asset as a new independent asset requires a new sidecar.

Scene format 3 and prefab files store declared asset fields as `{"UUID":"123456789","Path":"Assets/Models/Ship.glb"}`.
The UUID is authoritative; the path is a diagnostic hint. Sub-assets (`#image/N` embedded images, `#material/N`
imported materials) retain their fragment in the hint and share the model's UUID. A missing or ambiguous component UUID becomes an opaque `asset-uuid://` key in memory, so the scene
or prefab stays editable and renders missing assets with its normal fallback. Saving preserves the exact
UUID and hint; a different file at the hint never substitutes for it. Validation reports unresolved
identities. Reopen after restoring metadata to resolve the identity, or replace the field explicitly.
Legacy string paths remain readable; missing references and paths outside Assets/ remain editable
strings on save. All component readers accept both strings and UUID/path objects.
Components, Lua, automation snapshots and replay hashes continue to expose paths. An unresolved
start-scene UUID opens the project with no start scene and a diagnostic, preserving its reference across
unrelated saves until `project.set startScene` explicitly repairs or clears it. Asset fields are
marked once in ComponentRegistry and shared by persistence and rename; a list element's asset field (an Animator's
`ClipLibraries[].Mesh`) is declared as `List[].Field` and handled the same way in every element. Paths inside arbitrary script
properties, script source, module imports, animation clip names and external glTF resource URIs are
not rewritten or assigned IDs by scene persistence.

Use `asset.resolve {"uuid":"123456789"}` to look up the current path. `asset.move` and `asset.rename`
both take full project-relative `from` and `to` paths, move one file with its sidecar, upgrade legacy
scene, prefab and material-file references to that file, and update the open edit scene and project start scene. A
scene or prefab that cannot be read stops the move (it might reference the file); a material file that is not valid
JSON is skipped and named in `warnings`. Stop Play or Simulate first. Destinations must be unoccupied files under Assets/; folders are not supported.
Asset registry operations reject symlink files and symlink directory ancestors, so moving or deleting
an alias cannot mutate its target. Case-only renames on case-insensitive filesystems require an intermediate filename. Metadata is hidden in asset listings and the content browser but included in exports. For external
moves, move both files and reopen the scene/project. Existing saved UUID references then resolve to
the new path. Move any external glTF buffers/images together and preserve their relative URIs.

Mesh import uses pinned meshoptimizer v1.0.1: vertex-cache order, overdraw order (1.05 cache degradation
threshold), then vertex fetch. Complete interleaved vertices move together, including skin attributes.
Blended primitives keep their authored triangle order and only optimize vertex fetch. Triangle reordering
can alter exact coplanar depth ties. Built-in primitives keep their existing topology.

Static opaque/masked primitives generate up to three LOD index ranges (targets 50%, 25%, 12.5%, maximum
relative geometric error 1%, locked borders). Material boundaries and vertex seams are retained. A level
that cannot reduce triangles within the error bound is omitted. Skinned and blended primitives retain
full-detail topology. LODs share the original vertices; physics always uses the full-detail range.
`Mesh.LODDistance` opts static rendering into LOD selection: 0 disables it, positive values select the
first reduced level at that world-space distance from the camera to the submesh bounds center, then
subsequent levels at twice the preceding threshold. Camera, picking, depth and shadow draws use the
same selected range. This initial policy has no crossfade/hysteresis or screen-space error adjustment.

`asset.delete` and content-browser file deletion remove the source and its sidecar together. Recreating
that path then receives a fresh identity. When deleting manually, remove both files. Orphan sidecars are
reported as validator warnings; restoring a source together with its old sidecar intentionally restores
its ID. `asset.write` warns when creating a source next to an existing orphan sidecar.
Cooperating first-time importers use a sibling `.meta.importing` lock directory and atomic file writes;
manual filesystem writers must not modify the sidecar during import.
If an importer crashes, remove its stale lock only after confirming the importer has stopped.

Project loading and explicit refresh scan metadata once; save operations scan once per document.
Prefab spawning uses the current index without a directory walk. The host's file watcher updates the
index on changes. Scene reads also refresh it, so external paired moves resolve when reopening a scene.

Legacy version-1 scene migrations retain the original fixed-timestep default semantics before upgrading
the document to version 3. Move preparation may create a source sidecar; failed moves preserve that ID
while restoring any scene, prefab and project documents they changed. Diagnostic path hints may name
an earlier location until the next save; UUID resolution remains authoritative.


## Morph targets

The importer keeps each primitive's morph target POSITION and NORMAL deltas (dense or sparse accessors, float or
normalized integers), target names from `extras.targetNames` (unnamed: `Target<n>`, duplicates get `_1` suffixes),
default weights from `mesh.weights` and per-node overrides from `node.weights`. A target without NORMAL deltas gets the
change in generated normals between the base and fully morphed surface; TANGENT deltas are ignored. Deltas follow their
vertices through the vertex-fetch optimization. Every primitive of a mesh must have the same number of targets; a mesh
holds at most 256 and a model 4,194,304 deltas (targets times vertices). glTF `weights` animation channels import as
clip tracks. Rendering is described in [Rendering.md](Rendering.md#morph-targets).

## Compressed textures and extended glTF materials

`asset.compress_texture {"path":"Assets/Textures/Wall.png","output":"Assets/Textures/Wall.ktx2","srgb":true}`
authors a separate KTX2 asset containing desktop BC7 blocks and a complete mip chain from an 8-bit
PNG/JPEG/TGA/BMP source. The source stays unchanged; point material/image references at the output to use
it. Use `srgb:false` for normal and data maps: the setting picks sRGB or linear mip filtering, encoder
weights and the KTX2 format tag, while sampling still follows the material slot (a mismatch warns). HDR and
already compressed sources are rejected. Re-run compression after editing a source; compression is explicit
rather than an implicit mutation during loading.

Cache entries in `Cache/Textures/` are keyed by source contents, encoder cache version (bumped when the
encoder pin, parameters or mip filter change) and color-space setting, so changed sources or settings
recompress. The cache only saves re-encoding: an unreadable or invalid entry is rebuilt, a failed cache write
still produces the output, and a symlinked cache folder is skipped. Each of these is listed in the result's
`warnings`, as is an output that adopts an orphaned `.meta` (keeping its UUID). The result also reports
`cacheHit`, `overwrote` and the assets `reloaded` from the output. Output write failures report the OS
reason. Outputs live under `Assets/` and export normally; caches are excluded. Source and output paths and
their ancestors must not be symlinks.

The headless loader accepts 2D KTX2 with native BC7, Basis ETC1S, or LDR UASTC (including Zstandard).
Basis textures transcode to BC7 once in the texture load job; the renderer uploads supplied block mip levels
without decoding to RGBA. GPUs without sampled BC7 support decode every level to an RGBA8 mip chain
(warned once per renderer). CPU thumbnails decode level zero. Arrays, cubemaps, 3D textures, HDR Basis,
supercompressed native BC7, non-default orientation/swizzle and other formats are rejected, each with its
own `KTX2:` message. Dimensions are limited to 16384 per side and KTX2 files and BC7 mip chains to 256 MiB.
KTX2 texture color/data interpretation follows its material slot, like other textures.

The glTF importer supports `KHR_texture_basisu`, `KHR_mesh_quantization`, `KHR_texture_transform`,
`KHR_materials_emissive_strength`, `KHR_materials_clearcoat` and `KHR_materials_transmission`. Unknown
required extensions fail with the extension name. Optional unknown extensions retain core fallbacks.
Basis image sources work as external files, data URIs or GLB buffer views. The `KHR_texture_basisu` image
source is authoritative when present: a malformed or unsupported KTX2 reports an asset-load error, and
Basalt does not silently substitute the optional core PNG/JPEG source. Quantized accessors unpack through
the same vertex path as floats. Texture transforms apply scale, rotation, then offset, independently for
every map; UV0 and UV1 are supported (other coordinate sets fail import). Transformed alpha sampling is
shared by lighting, depth, picking and masked shadows. `normalTexture.scale` and `occlusionTexture.strength`
are honored. Invalid factors fail import with the material, field and value.

Clearcoat has independent factor (R), roughness (G) and normal maps, over direct and image-based lighting.
Transmission uses its factor times the transmission map's R channel, with metallic surfaces retaining
reflection. It routes through stable back-to-front transparency, preserves reflected specular, and
reduces diffuse coverage to reveal the destination. This is a thin-surface approximation: no refraction,
volume absorption or roughness-dependent background blur. Any positive transmission factor selects the
blended pass: such draws keep their full-detail, unreordered topology, cast no shadows and write no
prepass normals (they do not contribute to SSAO but still sample the occlusion of what lies behind them).
Coverage follows the alpha mode: OPAQUE and surviving MASK surfaces stay fully covered.

Authored `Material` overrides expose `Clearcoat`, `ClearcoatRoughness`, `Transmission` in [0,1], the four
extension map paths, and shared `UVOffset`, `UVScale`, `UVRotation` (radians). An override replaces the whole
imported material: its one UV transform applies to every map on UV0, and normal scale and occlusion strength
are 1 (a material asset, below, has a UV transform and set per map and its own normal and occlusion strengths). It blends when `AlbedoColor.a < 1` (alpha is then coverage) or `Transmission > 0` (full coverage). All
fields use the component registry for Lua, automation, disk persistence, inspector editing and undo.

The isolated `AssetPipelineFeature` ctest exercises compression and UUID saves on a temporary copy of
the feature project, so `FeatureTest.batch.json` never writes generated textures into checked-in assets.

## Material assets

A material file (`.bmaterial`) holds the full imported material model, so an authored material keeps everything a
glTF material can say. A `Material` component uses one through its `Material` field, which also accepts a model's
imported material directly (`Assets/Models/Ship.glb#material/0`, counted from 0 in glTF order; the importer's
default material for primitives without one is not addressable). While `Material` is set, the asset replaces the
component's inline values for every submesh; they apply only while it has not loaded (a model's material while the
model streams in) or when it cannot load (the failure is logged once, and the project validator reports a broken file).

```json
{
	"Version": 1,
	"AlbedoColor": [1, 0.85, 0.7, 1], "Metallic": 0, "Roughness": 0.6,
	"EmissiveColor": [0, 0, 0], "EmissiveIntensity": 1,
	"AlphaMode": "Mask", "AlphaCutoff": 0.3, "DoubleSided": true,
	"NormalScale": 0.5, "OcclusionStrength": 1,
	"Clearcoat": 0, "ClearcoatRoughness": 0, "ClearcoatNormalScale": 1, "Transmission": 0,
	"AlbedoMap": { "UUID": "9688068201958151311", "Path": "Assets/Textures/Checker.png" },
	"NormalMap": "Assets/Textures/FlatNormal.png",
	"TextureTransforms": { "Albedo": { "Offset": [0.25, 0], "Scale": [3, 3], "Rotation": 0.5, "TexCoord": 1 } }
}
```

- **Fields.** Only `Version` (1) is required; other keys keep the engine's material defaults (white albedo, Metallic 0,
  Roughness 0.5, Opaque, AlphaCutoff 0.5, single-sided, strengths 1, identity UV0 transforms), which is what
  `material.create` without an entity writes. These are not glTF's metallic/roughness defaults of 1. The maps have the
  component's names (`AlbedoMap`, `NormalMap`, `MetallicRoughnessMap`, `OcclusionMap`, `EmissiveMap`, `ClearcoatMap`,
  `ClearcoatRoughnessMap`, `ClearcoatNormalMap`, `TransmissionMap`). `TextureTransforms` is keyed by slot (`Albedo`,
  `Normal`, `MetallicRoughness`, `Occlusion`, `Emissive`, `Clearcoat`, `ClearcoatRoughness`, `ClearcoatNormal`,
  `Transmission`); rotation is in radians and `TexCoord` picks UV0 or UV1 (built-in meshes use one layout for both; a model without `TEXCOORD_1` samples UV1 as
  (0, 0)).
- **Strict reading.** Unknown keys, wrong types, non-finite numbers and the importer's range rules (clearcoat,
  clearcoat roughness, transmission and occlusion strength in [0, 1], nonnegative emissive intensity) fail with the
  field's location. The importer applies the same check, so every model material can be written as a file.
  `Tests/Fuzz/FuzzMaterialAsset` fuzzes the reader and checks that whatever it accepts writes and reads back.
- **References.** Textures are path strings or `{UUID, Path}` objects, like component asset fields; a model's
  embedded images are `Model.glb#image/N`. Files written by the engine (`material.create`, `material.extract`, the
  editor's Material Editor) use UUIDs. `asset.move`/`asset.rename` migrate material files with scenes and prefabs, so a
  moved texture keeps resolving; a UUID whose file is gone renders without that texture and is reported.
- **Commands.** `asset.write` validates a `.bmaterial` before replacing it and warns about its textures the way
  `project.validate` reports them: missing files, wrong letter case, unresolved UUIDs, keys that are not textures,
  paths outside `Assets/`, and embedded images the model lacks (or that are external files). `material.create {path,
  entity?, overwrite?}` writes the defaults, or what an entity's Material draws. `material.extract {model, folder?,
  overwrite?}` writes one file per glTF material (named after it, made safe on every platform and unique) into
  `<model folder>/Materials`, checking every output before writing any. `asset.preload` reads a material and preloads
  its textures.
- **Loading.** `AssetManager::GetMaterial`/`RequestMaterial` parse a file on the main thread and cache it (material
  payloads are tiny and are not charged to `memory.stats`). An imported material shares its model's lifetime:
  `GetMaterial` waits for the model, while `RequestMaterial` (the renderer's) returns nothing until the model is
  published, and the inline values draw meanwhile. A changed file (or texture, or sidecar) is dropped when the file
  watcher or `asset.refresh` reports it and is read again on its next use.
- **Editor.** The content browser previews material files on a sphere (`editor.thumbnail` kind `Material`) and a
  model's materials losslessly in its tooltip. Its context menu creates materials ("New Material...") and extracts a
  model's ("Extract Materials", which reports texture warnings). Double-clicking a material opens the Material Editor
  (`editor.material_editor` for agents): every field, textures dropped from the Content Browser onto map fields, a live
  preview of unsaved edits, Save through `asset.write` (its texture warnings shown), Revert, and Apply to Selection. It
  follows external writes and moves (`asset.move`/`asset.rename` of the material or its textures) and asks before
  discarding edits. Dropping a material on the viewport gives it to the selected entity. The inspector's Material
  section has Edit Material (for a `.bmaterial`) and Save as Material Asset (writes what the entity draws and uses it).
- **Validation.** `project.validate` parses every material file and checks its maps: textures (an image file or
  `Model.glb#image/N`) that exist with the exact letter case. A `Model.glb#material/N` or `#image/N` reference must
  exist in its glTF (checked from the glTF's JSON when it parses).
