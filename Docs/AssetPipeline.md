# Asset identity and mesh import

Assets acquire a persistent 64-bit UUID when referenced by a saved scene/prefab or project start scene,
or through move/rename or explicit `asset.id {"path":"Assets/Models/Ship.glb"}`. Commit the adjacent `Ship.glb.meta`
with the source file. Metadata contains `{"Version":1,"UUID":"123456789"}`; UUIDs are decimal strings
so JSON clients never lose integer precision. Loading a project scans existing metadata without writing
files. Duplicate UUIDs and corrupt metadata are reported by project validation and excluded from resolution;
unrelated assets remain usable. Neither copy wins an ambiguous identity. Repair the affected sidecars
explicitly rather than silently assigning new identities. Copying an asset as a new independent asset requires a new sidecar.

Scene format 3 and prefab files store declared asset fields as `{"UUID":"123456789","Path":"Assets/Models/Ship.glb"}`.
The UUID is authoritative; the path is a diagnostic hint. Embedded images retain `#image/N` in the hint
and share the model's UUID. A missing or ambiguous component UUID becomes an opaque `asset-uuid://` key in memory, so the scene
or prefab stays editable and renders missing assets with its normal fallback. Saving preserves the exact
UUID and hint; a different file at the hint never substitutes for it. Validation reports unresolved
identities. Reopen after restoring metadata to resolve the identity, or replace the field explicitly.
Legacy string paths remain readable; missing references and paths outside Assets/ remain editable
strings on save. All component readers accept both strings and UUID/path objects.
Components, Lua, automation snapshots and replay hashes continue to expose paths. An unresolved
start-scene UUID opens the project with no start scene and a diagnostic, preserving its reference across
unrelated saves until `project.set startScene` explicitly repairs or clears it. Asset fields are
marked once in ComponentRegistry and shared by persistence and rename. Paths inside arbitrary script
properties, script source, module imports, animation clip names and external glTF resource URIs are
not rewritten or assigned IDs by scene persistence.

Use `asset.resolve {"uuid":"123456789"}` to look up the current path. `asset.move` and `asset.rename`
both take full project-relative `from` and `to` paths, move one file with its sidecar, upgrade legacy
scene/prefab references to that file, and update the open edit scene and project start scene. Stop Play
or Simulate first. Destinations must be unoccupied files under Assets/; folders are not supported.
Case-only renames on case-insensitive filesystems require an intermediate filename. Metadata is hidden in asset listings and the content browser but included in exports. For external
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
