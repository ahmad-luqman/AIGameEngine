# Shipping a game

How a Breccia project becomes something players download: the project's identity (name, version, company, icon), the
export, platform packages, the asset archive, signing, crash reports, and the release pipeline that builds Breccia itself.

## Project identity

Set these in Project Settings > General or with `project.set`:

| Setting | `project.set` key | Used for |
|---------|-------------------|----------|
| Name | `name` | At most 256 bytes. Executable and package names (letters, digits, `-`, `_`; spaces become `_`, other characters are dropped, `Game` if nothing is left), window title, save data directory |
| Version | `version` | `MAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH.BUILD`, each 0–65535 without leading zeros. Windows file and product version (all four numbers), macOS `CFBundleShortVersionString` and `CFBundleVersion` (the first three: Apple documents both as at most three), the AppImage file name |
| Company | `company` | At most 256 bytes. Windows `CompanyName` and copyright, macOS copyright, and the macOS bundle identifier |
| Icon | `icon` | A PNG under `Assets/` (at most 4096 px; square and 512 px or more looks best). The window icon on Windows and Linux X11, and the icon of every package |

```bash
breccia --project MyGame project.set '{"version": "1.2.0", "company": "Acme Games", "icon": "Assets/Textures/Icon.png"}'
breccia --project MyGame project.info     # also shows the derived bundleIdentifier
```

- The icon is an asset reference, like the start scene: the project file stores its UUID and path, so `asset.move`
  and `asset.rename` keep it, and `asset.delete` warns when it deletes it. An icon that is not square is centered on a
  transparent square, never stretched.
- The macOS bundle identifier is `com.<company>.<name>`. Each part keeps its ASCII letters and digits (letter case
  included) with one `-` for each inner run of other characters; a company without any becomes `breccia`, a name
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
breccia --project MyGame project.export '{"output": "Dist/MyGame", "smokeTestFrames": 120}'                   # a folder
breccia --project MyGame project.export '{"output": "Dist/MyGame", "package": true, "smokeTestFrames": 120}'  # a package
breccia --project MyGame project.export '{"output": "Dist/MyGame", "package": true, "archive": true}'         # Assets in one file
```

The result names the `executable` (what the smoke test ran), the `package` (what to distribute) and any `warnings`
(a package without a project icon, an AppDir without appimagetool, the originals an archive left out).
The editor's File > Export Game... dialog offers the smoke test, the package, the archive and, with it, keeping originals.

### Packages (`"package": true`)

Each platform packages the host's runtime, so build each package on its own platform (the release pipeline does).

**macOS: `<Name>.app`.**

```
<Name>.app/Contents/
  Info.plist  PkgInfo
  MacOS/<Name>                         the runtime
  Frameworks/libvulkan.1.dylib         the Vulkan loader and MoltenVK (next to the runtime, then
                                       $VULKAN_SDK/lib, then Homebrew)
  Frameworks/libMoltenVK.dylib
  Resources/<Name>.icns                with a project icon: 16-1024 px PNG representations, none larger than the
                                       source icon (but at least 16 px)
  Resources/Project.bproject  Assets/  Input.binput (when the project has one)
  Resources/vulkan/icd.d/MoltenVK_icd.json
```

The runtime finds its project in `Contents/Resources` when none is next to the executable, and the Vulkan loader
finds MoltenVK through the manifest there. `Info.plist` states the oldest macOS the runtime is built for
(`LSMinimumSystemVersion`: 13.3 by default, `CMAKE_OSX_DEPLOYMENT_TARGET`). Every export replaces the whole bundle.

The bundle holds the architectures of the runtime and of the libraries it was given: a release's tools are universal
(arm64 and x86-64) and carry universal libraries next to the runtime, which an export from them takes first (a build
tree has none there, so `VULKAN_SDK`'s are taken: the LunarG SDK's are universal, while Homebrew's hold one
architecture). An export warns when a library is Mach-O code that lacks an architecture the runtime has, since the game
would not start on such Macs; `lipo -archs` on the files says which they hold.

Every macOS export, folder or bundle, is signed ad hoc as its last step (`/usr/bin/codesign`, part of macOS): the
libraries, then the executable or the bundle, which seals everything under `Contents`. A release's runtime, once signed
with Breccia's Developer ID, has the hardened runtime, whose library validation would refuse Vulkan libraries signed by
anyone else, and would name Breccia's publisher as the game's; the ad hoc signature names no one. Sign the export with
your own identity before distributing it (see below).

**Windows: a folder.** The same layout as a folder export, but the executable carries a version resource (Explorer's
Details tab) and, with a project icon, the icon: an `RT_GROUP_ICON` named `GLFW_ICON` (which GLFW's Win32 backend also
loads as the window class icon) with PNG images from 16 to 256 px. Zip the folder to distribute it.

Every Windows export, folder or package, removes the Authenticode signature of the runtime it copies (a release's
runtime, once signed, names Breccia's publisher): the game is yours to sign (see below), and a package's resource
rewrite would break the signature anyway. An export refuses a runtime whose certificate table is not where signing tools
put it (past the headers and every section, at the end of the file).

**Linux: an AppImage.**

```
<Name>.AppDir/
  AppRun -> <Name>   <Name>   <Name>.desktop   <Name>.png   .DirIcon -> <Name>.png
  Project.bproject  Input.binput  Assets/
<Name>-<version>-<arch>.AppImage
```

`<Name>.png` is the project icon (at most 512 px), or Breccia's own icon at 256 px when the project has none (an
AppImage must have one). The AppDir is replaced on every export, and an export that builds an AppImage removes the
game's earlier ones (other games' AppImages in the folder stay). The AppDir always gets built. The `.AppImage` file
needs [appimagetool](https://github.com/AppImage/appimagetool): `appImageTool` in the command, else `$APPIMAGETOOL`,
else `appimagetool` on `PATH`. Without it, the export succeeds with a warning, and the AppDir is the package. Exports
run appimagetool with `APPIMAGE_EXTRACT_AND_RUN=1`, so it needs no FUSE. Players need a Vulkan driver (their
distribution's `libvulkan1` and Mesa or a GPU vendor driver) and a glibc at least as new as the build machine's (Ubuntu
24.04: 2.39).

### Asset archive (`"archive": true`)

With `archive`, the export packs `Assets/` into one `Assets.bpak` next to the project file instead of copying the
folder. The project file and `Input.binput` stay loose, so every layout above (folder, `.app`, AppDir) is unchanged
apart from that one file, and the runtime finds the project as before. Switching between the two forms removes the
other one from the output, so a game never carries both. `archive.inspect` lists an archive (and with `verify: true`
reads every entry and checks its hash); the result of `project.export` reports `archive: {files, compressed (files kept
compressed), size (uncompressed bytes), archiveSize (bytes on disk), omitted (original images left out)}`.

```bash
breccia archive.inspect '{"path": "Dist/MyGame/Assets.bpak", "verify": true}'
```

- **Contents.** Every file under `Assets/` except the `.meta` sidecars and the two groups below (compressed texture originals, names no key can hold). Each entry carries its asset's UUID in the
  index instead, so references stored as UUIDs (scenes, prefabs, materials, the start scene, the icon) resolve exactly
  as they do in the project. An asset whose sidecar is broken goes in without its UUID, and the export warns
  (`project.validate`, which runs first unless `validate: false`, reports it as an error). A linked folder or a broken
  link inside `Assets/` (unless its own name is one no key can hold, below), or a path an archive cannot hold (a `:` or `\` in a name, or over 1024 bytes), stops the
  export. An archive holds at most 2^20 files of at most 4 GiB each.
- **Compressed texture originals.** A PNG, JPEG, TGA or BMP stays out when a `.ktx2` (in lower case) of the same name
  sits beside it (`Wall.ktx2` next to `Wall.png`) and neither the project file nor any asset other than images, `.hdr` and
  `.ktx2` textures, audio, fonts and glTF `.bin` buffers mentions it. A mention is its UUID, or its file name compared
  by letters, digits and non-ASCII bytes only, in the text as written and with JSON and Lua string escapes and then URI
  percent escapes undone. So a script's string (even `"Wall" .. ".png"`), a script property, a material or a glTF URI
  keeps a file, and a false match only keeps it too. A `.ktx2` elsewhere is not recognized, so that image stays, and a
  file that cannot be searched keeps every original, with a warning. `project.export` lists what it left out in
  `archive.omitted`, and `warnings` names up to five of them. A script that builds such a name from separate pieces at run
  time would no longer find the file: pass `"keepOriginals": true` with `archive` (a checkbox in the editor's export
  prompt) to keep every original. Folder exports copy every file.
- **Names no key can hold.** A file or folder whose name has no UTF-8 form stays out, with a warning for each
  (`Docs/AssetPipeline.md`): no scene, prefab or material can name it. Off Windows (in practice Linux, as APFS refuses such
  names) a Lua string with raw bytes or a percent-escaped glTF URI can, which works in a folder export and finds nothing in
  an archive, so read these warnings.
  Folder exports copy such files byte for byte (a broken link stops one, whatever its name) and add no warnings of their
  own; `validation`, when it runs, reports them for both forms.
- **Compression.** Each file except MP3 and FLAC audio is compressed with zstd (level 9) on the job system, up to 256 files
  or 256 MiB of them at a time (a larger file goes alone), and kept compressed only when that makes it smaller. Files are
  written in path order, so the same files and Breccia build give the same bytes however many cores the machine has.
  PNG/JPEG images usually end up stored as they are. MP3 and FLAC files always are, without trying zstd, which saves 0.1 to
  5% of them and would stop a long clip being read in part (the engine plays WAV, FLAC and MP3). `.ktx2` textures without
  supercompression (BC7 from `asset.compress_texture`, plain UASTC) usually shrink further, while ETC1S and
  Zstandard-supercompressed UASTC do not.
- **Loading.** The runtime mounts the archive over the project folder before it loads the project. A file the archive
  holds (scenes, prefabs, scripts and `require`d modules, models and their buffers, textures, materials, animation
  graphs, fonts, audio clips) is read only from it; anything else, such as loose files placed beside the archive, comes
  from disk (their sidecars are ignored: UUIDs come from the index). Paths match exactly, letter case included, as on
  Linux (`project.validate` reports references whose case differs from the file). An archive that fails its checks when
  it opens (header, layout, index hash) stops the game at start with exit code 1 and the reason; a damaged file fails
  only its own load (a long stored audio clip may not fail at all, see below), never falling back to a disk copy. Scene, prefab, script, material, texture, animation graph, font
  and model loads and `require` put the reason in their error; audio clips log it. An audio clip of at least 1 MiB
  stored uncompressed is read from the archive as it decodes, with or without `AudioSource.Stream`, so a long streamed
  clip stays out of memory. Every such clip skips the hash check, since a part of a file cannot be checked: reads stay
  inside its entry, damage plays as noise or fails to decode, and `archive.inspect` with `verify: true` finds it. Any other
  clip, such as a short one or a WAV that zstd shrinks, is read into memory whole when it opens and hash checked; with
  `Stream` it then decodes page by page from that copy.
- **Format** (`Core/PackArchive`, little-endian): a 40-byte header (`BPAK`, format version u32 = 1, flags u32 = 0, entry
  count u32, index offset u64, index size u64, index hash u64), the files' bytes, then the index, which must end the
  file exactly. Each index entry is the path's length (u32) and UTF-8 bytes (normalized, `/`-separated, sorted
  byte-wise and unique), the asset UUID (u64, 0 without one), the data's offset, stored size and size (u64 each), the
  method (u8: 0 stored, 1 zstd) and the file's hash (u64). Both hashes are `HashBytes64` (`Core/Hash.h`) of the
  uncompressed file and of the index bytes. The same files always give the same archive with the same Breccia build (no
  timestamps), so rebuilds can be compared byte for byte. Archives are read as untrusted input: every count, offset and
  size is checked before anything is allocated, a compressed entry may not claim more than its stored bytes can hold
  (128 KiB per 3 bytes), and a zstd frame must declare exactly the indexed size (`Tests/Fuzz/FuzzPackArchive`).

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

Given a folder instead (a folder export, or the tools' `bin/`), the script signs the `.dylib` files and the Mach-O
executables directly in it, libraries first, and verifies each; links and other files are left alone, and Mach-O code
that is neither, or a file it cannot read, stops it. Notarizing a folder submits those files; since only bundles, disk
images and installers can hold a ticket, macOS checks each one online the first time it runs. Each submission is named in
the log before the script waits for it, at most 30 minutes.

### Signing on Windows

`scripts/sign-windows.py` signs executables with `signtool` (from the Windows SDK; `--signtool` or `SIGNTOOL` name
another), using SHA-256 and an RFC 3161 timestamp, and then verifies them:

```powershell
python scripts\sign-windows.py Dist\MyGame --thumbprint 0123456789ABCDEF0123456789ABCDEF01234567 --description "My Game"
```

A folder stands for the `.exe` and `.dll` files directly in it; files may be named too. Sign after every export, which
never keeps a signature (see Packages). The certificate is taken from the current user's certificate store by its SHA-1
thumbprint (`Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert` lists them), or with `--machine-store` from the
machine's. Publicly trusted certificates issued since June 2023 keep their key on a hardware token or in a cloud HSM,
whose key storage provider puts them in a store. A `.pfx` file gets there with:

```powershell
Import-PfxCertificate -FilePath cert.pfx -CertStoreLocation Cert:\CurrentUser\My -Password (Read-Host -AsSecureString)
```

A signing service with its own signtool library, such as Azure Trusted Signing, is used with `--dlib
<Azure.CodeSigning.Dlib.dll> --dlib-metadata <metadata.json> --timestamp-url http://timestamp.acs.microsoft.com`.

The timestamp (default `http://timestamp.digicert.com`, any RFC 3161 server works with any certificate) keeps the
signature valid after the certificate expires; a signing that fails because of the server (signtool names it, or
reports a network error) is tried again after 5 and 15 seconds, and `--no-timestamp` leaves the timestamp out. Each
file is then verified with `signtool verify /pa /v`, which requires a certificate that chains to a root this machine
trusts, and with `--thumbprint` the script also requires that certificate in each file's chain (the verbose output lists
each certificate's SHA-1 hash), so a second code signing certificate in the store cannot stand in for it.
`--allow-untrusted` verifies each file on its own and accepts a failure only when it is an untrusted root, for test
certificates. The script exits 2 for invalid options and 1 when a file, a folder's code or
signtool is missing or signing or verifying fails. A signature names the publisher in Windows' prompts and file
properties; SmartScreen may still warn about a new program until downloads of it build a reputation, whatever the
certificate.

## Crash reports

The runtime, the editor and the CLI write a report when they crash: a fatal signal on macOS and Linux (`SIGSEGV`,
`SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`, `SIGTRAP`), an unhandled exception on Windows, plus `abort()`, invalid C runtime
parameters and pure virtual calls there. A report holds:

- the application (the executable's name: an exported game's is the project name with spaces as `_` and characters
  other than letters, digits, `-` and `_` dropped; the CLI is `breccia`),
  the project's name and version (runtime reports), the platform, the process ID and the time;
- the reason (`SIGSEGV (invalid memory access) at 0x…`, `EXCEPTION_ACCESS_VIOLATION …`), and the message of an
  uncaught C++ exception that ended the process;
- a stack trace (`backtrace_symbols_fd` on macOS and Linux, where Linux frames are module and offset; dbghelp on
  Windows, where a watchdog thread also writes a minidump next to the report);
- the last 64 log lines;
- on Linux with `/usr/bin/addr2line`, the executable's frames with their function names (and files and lines when the
  binary has debug information), last, so that a slow `addr2line` costs only this section: it is stopped after 15
  seconds, with a note saying so. A player's machine rarely has it; a report from one still gives each frame's offset,
  for `addr2line -e` on the same build.

Reports go to `<user data>/<application>/Crashes/crash-<unix time>-<process>.txt` (the per-user data directory that
save data uses, `BRECCIA_USER_DATA_DIR` included), so a signed `.app` or a read-only install is normally left alone. Only
when there is no user data directory, or the report cannot be created there, does the reporter try `CrashReports/` next
to the executable. The folders are created only when something crashes (up to eight missing levels; a deeper chain, or
a report folder path over 1024 bytes, falls back to `CrashReports/`).

The process then ends: by the same signal on macOS and Linux, with the exception code on Windows, or with exit code 3
(the C runtime's abort code) after `abort()`, a pure virtual call or an invalid C runtime parameter on Windows, where
the runtime would otherwise fail fast with 0xC0000409. `project.smoke_test` reports the first two as `crashed`; Windows
exit code 3 shows up as `exited` with exit code 3 ("the game aborted"). If another thread crashes while a report is
being written, it waits for the report. A report stuck for 30 seconds (a lock the crash left held) ends the process
anyway: by SIGALRM on macOS and Linux (which `addr2line`'s section restarts, so there the limit can reach almost a
minute), and on Windows from a thread that does nothing else. On Windows everything but function names is written before
anything that can stall.

The windowed runtime also shows a message box with the report's path, offering to open the report: a task-modal Windows
message box (the process ends when it is closed; the offer needs the watchdog thread); osascript on macOS, or zenity,
kdialog or xmessage from `/usr/bin` on Linux when a display is set, started as a separate process (the crashed process
ends at once; on Linux the offer needs `/usr/bin/xdg-open`). Reports are not sent anywhere: the engine has no network client
and no destination for them. A failed start that is no crash (an archive that does not open, a project or start scene
that does not load, no window or graphics device) shows its reason the same way before the exit code 1, cut at 4 KiB. The CLI, the editor, and headless, offscreen and
`--frames` runs never show one. AddressSanitizer builds leave crashes to AddressSanitizer.

On macOS and Linux the handler formats into fixed buffers, allocates nothing and never forks: the dialog and `addr2line`
start through `posix_spawn` (which runs no atfork handlers), prepared when the reporter installs, and backtrace's unwinder
is loaded at install. It runs on an alternate stack, so stack overflows are reported on the main thread and on every
thread holding a `CrashReporter::ThreadStack`: the job workers, the file watcher and the automation server. On Windows it
uses static buffers, and those threads keep a 128 KiB stack guarantee (taken from their 1 MiB stacks). It writes the text,
then asks for the minidump, and names functions last, because dbghelp may load symbol files for that. A watchdog thread
waiting from install on writes the minidump, with its own stack, and opens the report. A dump not finished after 20
seconds, or by what the 30-second limit leaves, is abandoned and the watchdog ended: the report says so, no partial dump
stays, and no message box shows, since it could hang on a lock the watchdog held. Should the dump writer suspend the
threads waiting for it, these limits cannot fire. Opening the report gets 5 seconds. Threads the engine does not start
(miniaudio's, a graphics driver's) have neither stack, so their stack overflows end the process without a report.

## Releasing Breccia

`.github/workflows/release.yml` runs on `v<version>` tags. The tag must match `project(Breccia VERSION ...)` in
`CMakeLists.txt`, or the run fails before building. On Windows, macOS and Linux it builds the `Dist` configuration and
runs `scripts/package_release.py`, which writes:

- `Breccia-<version>-<platform>.zip` (`.tar.gz` on Linux): the editor, runtime and CLI in `bin/`, plus `Docs/`,
  `Samples/`, `README.md` and `LICENSE`. On macOS it also holds the Vulkan loader and MoltenVK next to the tools, so they
  run without the Vulkan SDK.
- `Breccia-<version>-<Sample>-<platform>.*`: every sample exported as a package with its assets in `Assets.bpak`, and
  smoke-tested headless. These are a
  zipped `.app` (macOS; smoke-tested again after signing), a zipped folder (Windows; smoke-tested again after signing,
  when signing is set up) or an `.AppImage` that is also
  started once (Linux; run locally without appimagetool, the script archives the AppDir instead, unless
  `--require-appimage`).
- `SHA256SUMS.txt`, built from each platform's `SHA256SUMS-<platform>.txt`.

A final job publishes them as a GitHub release with generated notes. Windows and Linux are x86-64 (`windows-x64`,
`linux-x64`). macOS is universal (`macos-universal`): built on Apple Silicon (`macos-15`) for both arm64 and x86-64
(`CMAKE_OSX_ARCHITECTURES`) and the engine's minimum macOS (see Packages), with a Vulkan loader built the same way at the Vulkan-Headers pin
and MoltenVK's universal release (`scripts/ci-universal-vulkan.sh`, which pins its version and checksum).
Every Mach-O file shipped (the tools, their libraries, each sample's executable and libraries) must hold exactly the
CLI's architectures. On a universal build, the x86-64 half of the packaged tools and of each sample is started through
Rosetta 2 (the GitHub runners have it); a Mac without it only warns, unless `--require-universal` is given (as the
release does), which also fails unless the CLI holds both arm64 and x86-64. Jolt's x86-64 half is built for SSE4.2 (`cmake/Dependencies.cmake`), which every Mac of that minimum has. A local
run without `CMAKE_OSX_ARCHITECTURES` packages the host's architecture (`macos-arm64` on Apple Silicon).

Unless `--skip-engine`, the packaged CLI (`project.info` on a sample) and runtime (headless) are started from the tools
archive's folder once it is archived, after any signing (the editor needs a display and is not). Pull requests that change the main inputs of the packages
(the workflow, the packaging and signing scripts, the top-level and engine `CMakeLists.txt`, `cmake/Dependencies.cmake`,
the Project module, the runtime, the Process, Window, VulkanLoader, PackArchive and CrashReporter sources, the samples),
and manual runs (`workflow_dispatch`), run everything except publishing. Locally:

```bash
scripts/build.sh Dist
python3 scripts/package_release.py --build build-dist --output release      # --samples Tetris --skip-engine for a quick run
```

On Linux, `--gzip-level 1` gzips the `.tar.gz` archives fastest (the default is 9; other hosts write zip archives and
warn that it has no effect). The `ReleasePackaging` test uses it, because a Debug build's binaries hold hundreds of
megabytes of debug information.

**macOS signing in CI.** Without secrets, the tools and samples are signed ad hoc, and a tag release shows a warning.
To sign and notarize them, add all of these repository secrets. Once `MACOS_CERTIFICATE` is set, the job fails and names
any other one that is missing; without it, the rest are ignored, and a tag release also says in its notes that the macOS
tools and samples are signed ad hoc. With them, the tools (the editor, the runtime, the CLI and their Vulkan libraries)
are signed with the hardened runtime and notarized as a folder, and each sample's bundle is notarized and stapled.

| Secret | Value |
|--------|-------|
| `MACOS_CERTIFICATE` | base64 of the "Developer ID Application" certificate exported as `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | the `.p12` password |
| `MACOS_SIGNING_IDENTITY` | e.g. `Developer ID Application: Acme Games (TEAMID)` |
| `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | notarytool credentials (an app-specific password) |

**Windows signing in CI.** Without secrets, the Windows tools and samples are not signed, a tag release shows a warning
and says so in its notes. With these two secrets, the `.pfx` goes into the runner's certificate store, its one
certificate that has its private key, is valid now and allows code signing is picked, and `package_release.py
--windows-certificate <thumbprint>` signs the tools' `bin/` and each sample's folder (timestamped by DigiCert's server);
each sample is then smoke-tested again. Without `WINDOWS_CERTIFICATE` the password is ignored.

A `.pfx` secret fits only a certificate whose key may leave its hardware. No publicly trusted code signing certificate
valid today does: those issued since June 2023 may not, and older ones (at most 39 months) have expired, while a private
one fails verification on the runner, which does not trust its root. So in practice Windows signing in CI goes through a
signing service such as Azure Trusted Signing: replace the import step with the service's own setup, which installs its
signtool library and writes its metadata file, and have it set `WINDOWS_SIGNING_DLIB` and `WINDOWS_SIGNING_METADATA` (the
two paths) in `GITHUB_ENV`, and `WINDOWS_SIGNING_TIMESTAMP_URL` for the service's own timestamp server
(`http://timestamp.acs.microsoft.com` for Azure). The Package step then passes them as `--windows-dlib`,
`--windows-dlib-metadata` and `--windows-timestamp-url`, and the release notes no longer call the packages unsigned. Pull requests from branches of this repository get the secrets too,
so they sign with the real certificates, which tests the signing path before a tag.

| Secret | Value |
|--------|-------|
| `WINDOWS_CERTIFICATE` | base64 of the code signing certificate exported as `.pfx` with its private key |
| `WINDOWS_CERTIFICATE_PASSWORD` | the `.pfx` password (the job fails when only the certificate is set) |

The pipeline pins appimagetool 1.9.1 and the type2 AppImage runtime by SHA-256. To update them, change the URLs and
checksums in the workflow together.
