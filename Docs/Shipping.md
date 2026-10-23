# Shipping a game

How a Basalt project becomes something players download: the project's identity (name, version, company, icon), the
export, platform packages, signing, and the release pipeline that builds Basalt itself.

## Project identity

Set these in Project Settings > General or with `project.set`:

| Setting | `project.set` key | Used for |
|---------|-------------------|----------|
| Name | `name` | Executable and package names (letters, digits, `-`, `_`; spaces become `_`), window title, save data directory |
| Version | `version` | `MAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH.BUILD`, each 0–65535. Windows file and product version, macOS `CFBundleShortVersionString` (first three numbers) and `CFBundleVersion` (all of them), the AppImage file name |
| Company | `company` | Windows `CompanyName` and copyright, macOS copyright, and the macOS bundle identifier |
| Icon | `icon` | A PNG under `Assets/` (at most 4096 px; square and 512 px or more looks best). The window icon on Windows and Linux X11, and the icon of every package |

```bash
basalt --project MyGame project.set '{"version": "1.2.0", "company": "Acme Games", "icon": "Assets/Textures/Icon.png"}'
basalt --project MyGame project.info     # also shows the derived bundleIdentifier
```

- The icon is an asset reference, like the start scene: the project file stores its UUID and path, so `asset.move`
  and `asset.rename` keep it. An icon that is not square is centered on a transparent square, never stretched.
- The macOS bundle identifier is `com.<company>.<name>` with every run of other characters replaced by `-`
  (`com.basalt.<name>` without a company). Changing the name or company changes it, and macOS then treats the game as a
  new app (its permissions and preferences).
- `project.validate` reports an icon that is missing, not a PNG or too large (errors), and one under 256 px (a
  warning).
- Scripts read the name, version and company with `Game.GetInfo()` (a title screen or credits).

## Export

`project.export` validates the project, copies the runtime (renamed after the game) with the project file, its input
actions and `Assets/`, and optionally runs the result headless (`smokeTestFrames`). The output directory must be outside
the project, and empty or a previous export. Export with a `Dist` build of the tools for distribution.

```bash
basalt --project MyGame project.export '{"output": "Dist/MyGame", "smokeTestFrames": 120}'                   # a folder
basalt --project MyGame project.export '{"output": "Dist/MyGame", "package": true, "smokeTestFrames": 120}'  # a package
```

The result names the `executable` (what the smoke test ran) and the `package` (what to distribute). The editor's
File > Export dialog has the same options.

### Packages (`"package": true`)

Each platform packages the host's runtime, so build each package on its own platform (the release pipeline does).

**macOS: `<Name>.app`.**

```
<Name>.app/Contents/
  Info.plist  PkgInfo
  MacOS/<Name>                         the runtime
  Frameworks/libvulkan.1.dylib         the Vulkan loader and MoltenVK (VULKAN_SDK, then Homebrew)
  Frameworks/libMoltenVK.dylib
  Resources/<Name>.icns                16-1024 px PNG representations, none larger than the source icon
  Resources/Project.bproject  Input.binput  Assets/
  Resources/vulkan/icd.d/MoltenVK_icd.json
```

The runtime finds its project in `Contents/Resources` when none is next to the executable, and the Vulkan loader
finds MoltenVK through the manifest there. Sign the bundle before distributing it (see below).

**Windows: a folder.** The same layout as a folder export, but the executable carries the project's icon (an
`RT_GROUP_ICON` named `GLFW_ICON`, which GLFW also uses for the window class, with PNG images from 16 to 256 px) and a
version resource (Explorer's Details tab). Zip the folder to distribute it.

**Linux: an AppImage.**

```
<Name>.AppDir/
  AppRun -> <Name>   <Name>   <Name>.desktop   <Name>.png   .DirIcon -> <Name>.png
  Project.bproject  Input.binput  Assets/
<Name>-<version>-<arch>.AppImage
```

The AppDir always gets built. The `.AppImage` file needs [appimagetool](https://github.com/AppImage/appimagetool):
`appImageTool` in the command, else `$APPIMAGETOOL`, else `appimagetool` on `PATH`. Without it, the export succeeds
with a warning, and the AppDir is the package. Exports run appimagetool with `APPIMAGE_EXTRACT_AND_RUN=1`, so it
needs no FUSE. Players need a Vulkan driver (their distribution's `libvulkan1` and Mesa or a GPU vendor driver) and a
glibc at least as new as the build machine's (Ubuntu 24.04: 2.39).

### Signing and notarizing on macOS

`scripts/sign-macos.sh` signs a `.app` from the inside out (the bundled libraries, then the bundle) and verifies it:

```bash
scripts/sign-macos.sh Dist/MyGame/MyGame.app                                    # ad hoc: runs on this Mac only
scripts/sign-macos.sh Dist/MyGame/MyGame.app --identity "Developer ID Application: Acme Games (TEAMID)"
scripts/sign-macos.sh Dist/MyGame/MyGame.app --identity "Developer ID Application: Acme Games (TEAMID)" --notarize acme
```

A Developer ID signature uses the hardened runtime and a secure timestamp. `--notarize <profile>` submits the bundle
to Apple's notary service with credentials stored once with
`xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <TEAMID> --password <app-specific password>`,
waits for the result and staples the ticket, so the game also opens offline. Sign again after any change to the
bundle, and zip it with `ditto -c -k --keepParent`, which keeps the signature intact.

Windows executables are not signed. SmartScreen warns about unsigned downloads until they earn a reputation; sign
them with `signtool` and your own certificate when that matters.

## Releasing Basalt

`.github/workflows/release.yml` runs on `v<version>` tags. The tag must match `project(Basalt VERSION ...)` in
`CMakeLists.txt`, or the run fails before building. On Windows, macOS and Linux it builds the `Dist` configuration and
runs `scripts/package_release.py`, which writes:

- `Basalt-<version>-<platform>.zip` (`.tar.gz` on Linux): the editor, runtime and CLI in `bin/`, plus `Docs/`,
  `Samples/`, `README.md` and `LICENSE`. On macOS it also holds the Vulkan loader and MoltenVK next to the tools, so they
  run without the Vulkan SDK.
- `Basalt-<version>-<Sample>-<platform>.*`: every sample exported as a package and smoke-tested headless. These are a
  zipped `.app` (macOS), a zipped folder (Windows) or an `.AppImage` that is also started once (Linux).
- `SHA256SUMS.txt`, built from each platform's `SHA256SUMS-<platform>.txt`.

A final job publishes them as a GitHub release with generated notes. Pull requests that change the release tooling,
and manual runs (`workflow_dispatch`), run everything except publishing. Locally:

```bash
scripts/build.sh Dist
python3 scripts/package_release.py --build build-dist --output release      # --samples Tetris --skip-engine for a quick run
```

**macOS signing in CI.** Without secrets, the samples are signed ad hoc. To sign and notarize them, add these
repository secrets:

| Secret | Value |
|--------|-------|
| `MACOS_CERTIFICATE` | base64 of the "Developer ID Application" certificate exported as `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | the `.p12` password |
| `MACOS_SIGNING_IDENTITY` | e.g. `Developer ID Application: Acme Games (TEAMID)` |
| `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | notarytool credentials (an app-specific password) |

The pipeline pins appimagetool 1.9.1 and the type2 AppImage runtime by SHA-256. To update them, change the URLs and
checksums in the workflow together.
