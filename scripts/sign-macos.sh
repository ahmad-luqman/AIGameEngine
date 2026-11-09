#!/usr/bin/env bash
# Signs a macOS .app bundle made by `project.export {"package": true}`, or a folder of loose code, and optionally notarizes it.
#
# Usage: scripts/sign-macos.sh <Game.app | folder> [--identity "Developer ID Application: Name (TEAMID)"] [--notarize <profile>]
#
# Without --identity the code is signed ad hoc: it runs here, but Gatekeeper blocks it once downloaded. A Developer
# ID signs it with the hardened runtime and a secure timestamp, which notarization requires. --notarize submits it to
# Apple's notary service with credentials stored once in the keychain:
#   xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <TEAMID> --password <app-specific password>
# then staples the ticket to a bundle, so it also opens offline.
# A folder is a folder export (the game, the Vulkan loader and MoltenVK) or the tools' bin/: its .dylib files and
# executables (Mach-O files) directly in it are signed one by one. Only bundles, disk images and installers can hold a
# ticket, so a notarized folder's tools are checked online the first time each one runs.
set -euo pipefail

usage() {
	sed -n '2,13s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

[[ $# -ge 1 ]] || usage
target="${1%/}"
shift
identity="-"
profile=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--identity) [[ $# -ge 2 && -n "$2" ]] || usage; identity="$2"; shift 2 ;;
		--notarize) [[ $# -ge 2 && -n "$2" ]] || usage; profile="$2"; shift 2 ;;
		*) echo "unknown argument '$1'" >&2; usage ;;
	esac
done

bundle=0
if [[ "$target" == *.app ]]; then
	if [[ ! -f "$target/Contents/Info.plist" || ! -d "$target/Contents/MacOS" ]]; then
		echo "'$target' is not an app bundle (export with \"package\": true)" >&2
		exit 1
	fi
	bundle=1
elif [[ ! -d "$target" ]]; then
	echo "'$target' is neither an app bundle nor a folder" >&2
	exit 1
fi
if [[ -n "$profile" && "$identity" == "-" ]]; then
	echo "--notarize needs --identity: Apple notarizes only Developer ID signatures" >&2
	exit 2
fi

options=(--force --sign "$identity")
# The hardened runtime and a timestamp come with a real identity (notarization requires both); an ad hoc signature has
# no team, so library validation would only get in the way of the bundled Vulkan libraries.
[[ "$identity" != "-" ]] && options+=(--options runtime --timestamp)

# Mach-O files start with a thin 64-bit (cf fa ed fe) or a universal (ca fe ba be, ca fe ba bf) header.
is_mach_o() {
	local magic
	magic="$(od -An -tx1 -N4 "$1" | tr -d ' \n')"
	[[ "$magic" == cffaedfe || "$magic" == cafebabe || "$magic" == cafebabf ]]
}

# Inside out: nested code first, then what holds it (a bundle seals everything under Contents).
shopt -s nullglob
signed=()
if ((bundle)); then
	for library in "$target"/Contents/Frameworks/*.dylib; do
		codesign "${options[@]}" "$library"
	done
	codesign "${options[@]}" "$target"
	codesign --verify --deep --strict --verbose=2 "$target"
else
	# Links are left alone (signing one would sign its target, which a folder may not hold).
	for library in "$target"/*.dylib; do
		if [[ -f "$library" && ! -L "$library" ]]; then
			signed+=("$library")
		fi
	done
	for file in "$target"/*; do
		if [[ -f "$file" && ! -L "$file" && -x "$file" && "$file" != *.dylib ]] && is_mach_o "$file"; then
			signed+=("$file")
		fi
	done
	if [[ ${#signed[@]} -eq 0 ]]; then
		echo "'$target' holds no libraries or executables to sign" >&2
		exit 1
	fi
	for file in "${signed[@]}"; do
		codesign "${options[@]}" "$file"
	done
	for file in "${signed[@]}"; do
		codesign --verify --strict --verbose=2 "$file"
	done
fi

if [[ -n "$profile" ]]; then
	workspace="$(mktemp -d)"
	trap 'rm -rf "$workspace"' EXIT
	archive="$workspace/$(basename "$target" .app).zip"
	if ((bundle)); then
		ditto -c -k --keepParent "$target" "$archive"
	else
		# Only the code: the notary service records each file's signature, whichever archive later ships it.
		mkdir "$workspace/$(basename "$target")"
		for file in "${signed[@]}"; do
			ditto "$file" "$workspace/$(basename "$target")/$(basename "$file")"
		done
		ditto -c -k --keepParent "$workspace/$(basename "$target")" "$archive"
	fi
	# The verdict is read from the result rather than the exit status (which does not reflect a rejection on every Xcode),
	# and a rejection prints Apple's log, which says what to fix.
	submitted=0
	result="$(xcrun notarytool submit "$archive" --keychain-profile "$profile" --wait --output-format plist)" || submitted=$?
	status="$(plutil -extract status raw -o - - <<< "$result" 2>/dev/null || true)"
	if [[ "$status" != "Accepted" ]]; then
		echo "notarization ${status:-failed (notarytool exited with $submitted)}" >&2
		echo "$result" >&2
		if submission="$(plutil -extract id raw -o - - <<< "$result" 2>/dev/null)"; then
			xcrun notarytool log "$submission" --keychain-profile "$profile" >&2 || true
		fi
		exit 1
	fi
	if ((bundle)); then
		xcrun stapler staple "$target"
		spctl --assess --type execute --verbose=2 "$target"
	fi
fi
what="'$target'"
((bundle)) || what="${#signed[@]} files in '$target'"
echo "signed $what ($([[ "$identity" == "-" ]] && echo "ad hoc" || echo "$identity")$([[ -n "$profile" ]] && echo ", notarized"))"
