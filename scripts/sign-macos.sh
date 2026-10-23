#!/usr/bin/env bash
# Signs a macOS .app bundle made by `project.export {"package": true}` and optionally notarizes it.
#
# Usage: scripts/sign-macos.sh <Game.app> [--identity "Developer ID Application: Name (TEAMID)"] [--notarize <profile>]
#
# Without --identity the bundle is signed ad hoc: it runs here, but Gatekeeper blocks it once downloaded. A Developer
# ID signs it with the hardened runtime and a secure timestamp, which notarization requires. --notarize submits it to
# Apple's notary service with credentials stored once in the keychain:
#   xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <TEAMID> --password <app-specific password>
# then staples the ticket to the bundle, so it also opens offline.
set -euo pipefail

usage() {
	sed -n '2,10s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

[[ $# -ge 1 ]] || usage
app="${1%/}"
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

if [[ ! -f "$app/Contents/Info.plist" || ! -d "$app/Contents/MacOS" ]]; then
	echo "'$app' is not an app bundle (export with \"package\": true)" >&2
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

# Inside out: nested code first, the bundle (which seals everything under Contents) last.
shopt -s nullglob
for library in "$app"/Contents/Frameworks/*.dylib; do
	codesign "${options[@]}" "$library"
done
codesign "${options[@]}" "$app"
codesign --verify --deep --strict --verbose=2 "$app"

if [[ -n "$profile" ]]; then
	workspace="$(mktemp -d)"
	trap 'rm -rf "$workspace"' EXIT
	archive="$workspace/$(basename "$app" .app).zip"
	ditto -c -k --keepParent "$app" "$archive"
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
	xcrun stapler staple "$app"
	spctl --assess --type execute --verbose=2 "$app"
fi
echo "signed '$app' ($([[ "$identity" == "-" ]] && echo "ad hoc" || echo "$identity")$([[ -n "$profile" ]] && echo ", notarized"))"
