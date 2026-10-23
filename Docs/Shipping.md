# Shipping a game

How a Basalt project becomes something players download: the project's identity (name, version, company, icon), the
export, platform packages, signing, and the release pipeline that builds Basalt itself.

## Project identity

Set these in Project Settings > General or with `project.set`:

| Setting | `project.set` key | Used for |
|---------|-------------------|----------|
| Name | `name` | At most 256 bytes. Executable and package names (letters, digits, `-`, `_`; spaces become `_`, other characters are dropped, `Game` if nothing is left), window title, save data directory |
| Version | `version` | `MAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH.BUILD`, each 0–65535 without leading zeros. Windows file and product version (all four numbers), macOS `CFBundleShortVersionString` and `CFBundleVersion` (the first three: Apple documents both as at most three), the AppImage file name |
| Company | `company` | At most 256 bytes. Windows `CompanyName` and copyright, macOS copyright, and the macOS bundle identifier |
| Icon | `icon` | A PNG under `Assets/` (at most 4096 px; square and 512 px or more looks best). The window icon on Windows and Linux X11, and the icon of every package |

```bash
basalt --project MyGame project.set '{"version": "1.2.0", "company": "Acme Games", "icon": "Assets/Textures/Icon.png"}'
basalt --project MyGame project.info     # also shows the derived bundleIdentifier
```

- The icon is an asset reference, like the start scene: the project file stores its UUID and path, so `asset.move`
  and `asset.rename` keep it, and `asset.delete` warns when it deletes it. An icon that is not square is centered on a
  transparent square, never stretched.
- The macOS bundle identifier is `com.<company>.<name>`. Each part keeps its ASCII letters and digits (letter case
  included) with one `-` for each inner run of other characters; a company without any becomes `basalt`, a name
  without any `game`. Changing the name or company changes it, and macOS then treats the game as a new app (its
  permissions and preferences).
- `project.validate` reports an icon that is missing, not a PNG or too large (errors), and one under 256 px (a
  warning).
- Scripts read the name, version and company with `Game.GetInfo()` (a title screen or credits).

## Export

`project.export` validates the project, copies the runtime (renamed after the game) with the project file, its input
actions and `Assets/`, and optionally runs the result headless (`smokeTestFrames`). The output directory must be outside
the project, and empty or a previous export of the same game (its project file and executable, or its `<Name>.app` or
`<Name>.AppDir`).
Export with a `Dist` build of the tools for distribution.

```bash
basalt --project MyGame project.export '{"output": "Dist/MyGame", "smokeTestFrames": 120}'                   # a folder
basalt --project MyGame project.export '{"output": "Dist/MyGame", "package": true, "smokeTestFrames": 120}'  # a package
```

The result names the `executable` (what the smoke test ran), the `package` (what to distribute) and any `warnings`
(a package without a project icon, an AppDir without appimagetool).
The editor's File > Export Game... dialog offers the smoke test and the package option.

### Packages (`"package": true`)

Each platform packages the host's runtime, so build each package on its own platform (the release pipeline does).

**macOS: `<Name>.app`.**

```
<Name>.app/Contents/
  Info.plist  PkgInfo
  MacOS/<Name>                         the runtime
  Frameworks/libvulkan.1.dylib         the Vulkan loader and MoltenVK (VULKAN_SDK, then Homebrew)
  Frameworks/libMoltenVK.dylib
  Resources/<Name>.icns                with a project icon: 16-1024 px PNG representations, none larger than the
                                       source icon (but at least 16 px)
  Resources/Project.bproject  Assets/  Input.binput (when the project has one)
  Resources/vulkan/icd.d/MoltenVK_icd.json
```

The runtime finds its project in `Contents/Resources` when none is next to the executable, and the Vulkan loader
finds MoltenVK through the manifest there. Every export replaces the whole bundle. Sign the bundle before
distributing it (see below).

**Windows: a folder.** The same layout as a folder export, but the executable carries a version resource (Explorer's
Details tab) and, with a project icon, the icon: an `RT_GROUP_ICON` named `GLFW_ICON` (which GLFW's Win32 backend also
loads as the window class icon) with PNG images from 16 to 256 px. Zip the folder to distribute it.

**Linux: an AppImage.**

```
<Name>.AppDir/
  AppRun -> <Name>   <Name>   <Name>.desktop   <Name>.png   .DirIcon -> <Name>.png
  Project.bproject  Input.binput  Assets/
<Name>-<version>-<arch>.AppImage
```

`<Name>.png` is the project icon (at most 512 px), or Basalt's own icon at 256 px when the project has none (an AppImage
must have one). The AppDir is replaced on every export, and an export that builds an AppImage removes the game's
earlier ones (other games' AppImages in the folder stay). The AppDir always gets built. The `.AppImage` file needs [appimagetool](https://github.com/AppImage/appimagetool):
`appImageTool` in the command, else `$APPIMAGETOOL`, else `appimagetool` on `PATH`. Without it, the export succeeds
with a warning, and the AppDir is the package. Exports run appimagetool with `APPIMAGE_EXTRACT_AND_RUN=1`, so it
needs no FUSE. Players need a Vulkan driver (their distribution's `libvulkan1` and Mesa or a GPU vendor driver) and a
glibc at least as new as the build machine's (Ubuntu 24.04: 2.39).

### Signing and notarizing on macOS

`scripts/sign-macos.sh` signs a `.app` from the inside out (the bundled libraries, then the bundle) and verifies it:

```bash
scripts/sign-macos.sh Dist/MyGame/MyGame.app                                    # ad hoc: Gatekeeper blocks it once downloaded
scripts/sign-macos.sh Dist/MyGame/MyGame.app --identity "Developer ID Application: Acme Games (TEAMID)"
scripts/sign-macos.sh Dist/MyGame/MyGame.app --identity "Developer ID Application: Acme Games (TEAMID)" --notarize acme
```

A Developer ID signature uses the hardened runtime and a secure timestamp. `--notarize <profile>` submits the bundle
to Apple's notary service with credentials stored once with
`xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <TEAMID> --password <app-specific password>`,
waits for the result (printing Apple's log when it is rejected) and staples the ticket, so the game also opens offline. Sign again after any change to the
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
  zipped `.app` (macOS; smoke-tested again after signing), a zipped folder (Windows) or an `.AppImage` that is also
  started once (Linux; run locally without appimagetool, the script archives the AppDir instead, unless
  `--require-appimage`).
- `SHA256SUMS.txt`, built from each platform's `SHA256SUMS-<platform>.txt`.

A final job publishes them as a GitHub release with generated notes. Each platform has one architecture: Apple
Silicon on macOS (`macos-15`) and x86-64 on Windows and Linux. Pull requests that change the main inputs of the packages
(the workflow, the packaging scripts, the Project module, the runtime, the Process, Window and VulkanLoader sources, the
samples), and manual runs
(`workflow_dispatch`), run everything except publishing. Locally:

```bash
scripts/build.sh Dist
python3 scripts/package_release.py --build build-dist --output release      # --samples Tetris --skip-engine for a quick run
```

**macOS signing in CI.** Without secrets, the samples are signed ad hoc, and a tag release shows a warning. To sign and
notarize them, add all of these repository secrets. Once `MACOS_CERTIFICATE` is set, the job fails and names any other
one that is missing; without it, the rest are ignored, and a tag release also says in its notes that the macOS samples
are signed ad hoc. Only the samples are signed: the tools archive is not, so macOS quarantines downloaded tools until you allow
them.

| Secret | Value |
|--------|-------|
| `MACOS_CERTIFICATE` | base64 of the "Developer ID Application" certificate exported as `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | the `.p12` password |
| `MACOS_SIGNING_IDENTITY` | e.g. `Developer ID Application: Acme Games (TEAMID)` |
| `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | notarytool credentials (an app-specific password) |

The pipeline pins appimagetool 1.9.1 and the type2 AppImage runtime by SHA-256. To update them, change the URLs and
checksums in the workflow together.
