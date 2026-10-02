#!/bin/sh
# Downloads the pinned arcade SDK release (GitHub release arcade-sdk-v$SDK_VERSION of
# get-wrecked/ai-research) and unpacks what we need, for Linux and Windows:
#   ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so    (shipped next to the executable; not committed)
#   ThirdParty/arcade_sdk/windows_64/arcade_sdk.dll    (likewise, with its .pdb)
#   arcade/tools/arcade-sdk, arcade/tools/arcade-sdk.exe  (the CLI: submit, describe, and
#                                                         `arcade-sdk debug` for the debug app; not committed)
# The header, protos, guide and changelog in ThirdParty/arcade_sdk ARE committed; when
# bumping SDK_VERSION, diff those against the archive and update them (and the engine,
# if the InitRequest contract changed) in the same commit.
#
# Usage:  ./fetch_sdk.sh [linux] [windows]     (default: both)
#   SDK_VERSION=x.y.z   a GitHub release (needs the GitHub CLI, gh, signed in to an account that may read the repository)
#   SDK_SHA=<commit>    instead: a per-commit build from the SDK bucket,
#                       gs://gi-prod-games-cluster-app/arcade_sdk[-linux]-<sha>.zip (needs gsutil with read access)
set -eu

SDK_VERSION=${SDK_VERSION:-2.2.1}
SDK_SHA=${SDK_SHA:-fc970a05a52fb4abec49122e42928c574da6e2e0}
SDK_REPO=${SDK_REPO:-get-wrecked/ai-research}
SDK_BUCKET=${SDK_BUCKET:-gs://gi-prod-games-cluster-app}
PLATFORMS=${*:-linux windows}

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$HERE/tools"

for p in $PLATFORMS; do
	if [ -n "$SDK_SHA" ]; then
		case "$p" in linux) zip="arcade_sdk-linux-$SDK_SHA.zip" ;; windows) zip="arcade_sdk-$SDK_SHA.zip" ;; *) echo "unknown platform: $p" >&2; exit 1 ;; esac
		echo "fetching $zip from $SDK_BUCKET"
		gsutil -q cp "$SDK_BUCKET/$zip" "$TMP/$zip"
	else
		zip="arcade_sdk-$p-$SDK_VERSION.zip"
		echo "fetching $zip from $SDK_REPO (arcade-sdk-v$SDK_VERSION)"
		gh release download "arcade-sdk-v$SDK_VERSION" -R "$SDK_REPO" -p "$zip" -D "$TMP" --clobber
	fi
	rm -rf "$TMP/$p" && unzip -q "$TMP/$zip" -d "$TMP/$p"
	case "$p" in
	linux)
		mkdir -p "$REPO/ThirdParty/arcade_sdk/linux_64"
		cp "$TMP/$p/libarcade_sdk.so" "$REPO/ThirdParty/arcade_sdk/linux_64/"
		case "$(uname -s)" in
		MINGW*|MSYS*|CYGWIN*) ;;	# no use on Windows, and MSYS would write it over arcade-sdk.exe
		*) cp "$TMP/$p/arcade-sdk" "$HERE/tools/" && chmod +x "$HERE/tools/arcade-sdk" ;;
		esac
		lib="$REPO/ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so"
		;;
	windows)
		mkdir -p "$REPO/ThirdParty/arcade_sdk/windows_64"
		cp "$TMP/$p/arcade_sdk.dll" "$TMP/$p/arcade_sdk.pdb" "$REPO/ThirdParty/arcade_sdk/windows_64/"
		cp "$TMP/$p/arcade-sdk.exe" "$HERE/tools/"
		lib="$REPO/ThirdParty/arcade_sdk/windows_64/arcade_sdk.dll"
		;;
	*)
		echo "unknown platform: $p (linux or windows)" >&2; exit 1 ;;
	esac

	same() { # same A B: equal but for line endings (the Windows zip has CRLF)
		[ "$(tr -d '\r' < "$1" | cksum)" = "$(tr -d '\r' < "$2" | cksum)" ]
	}
	for f in include/arcade_sdk.h proto/arcade_sdk.proto proto/arcade_common.proto CHANGELOG.md; do
		if ! same "$TMP/$p/$f" "$REPO/ThirdParty/arcade_sdk/$f"; then
			echo "NOTE: $f differs from the committed copy; review and update ThirdParty/arcade_sdk (and vendor_pb.h / the engine if needed)"
		fi
	done
	same "$TMP/$p/examples/host.c" "$REPO/ThirdParty/arcade_sdk/host.c" || echo "NOTE: examples/host.c differs from ThirdParty/arcade_sdk/host.c"
	same "$TMP/$p/README.md" "$REPO/ThirdParty/arcade_sdk/README_SDK.md" || echo "NOTE: README.md differs from ThirdParty/arcade_sdk/README_SDK.md"
	echo "$p SDK library: $(grep -aoE '[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}' "$lib" | head -1)"
done
echo "CLI:         $HERE/tools/"
