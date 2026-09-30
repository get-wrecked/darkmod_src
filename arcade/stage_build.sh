#!/bin/sh
# Assembles an arcade client folder: the engine executable, the arcade SDK library,
# base game data, the bundled missions, vendor.proto and the launch script, with the
# build id from the platform's arcade toml written into the launch script.
#
# Usage:  ./stage_build.sh [linux|windows]      (default: linux; windows runs in Git Bash)
#   linux:    arcade.toml          -> out/client   (thedarkmod.x64,     libarcade_sdk.so, run_arcade.sh)
#   windows:  arcade-windows.toml  -> out/windows  (TheDarkModx64.exe,  arcade_sdk.dll,   run_arcade.cmd; runs under Proton)
#   GAME_DIR   where the TDM assets live                    (default: ../../darkmod, i.e. CMake's GAME_DIR)
#   EXE        engine executable to ship                   (default: $GAME_DIR/thedarkmod.x64 or $GAME_DIR/TheDarkModx64.exe)
#   SDK_LIB    SDK library to ship                         (default: ../ThirdParty/arcade_sdk/{linux_64,windows_64}/..., see fetch_sdk.sh)
#   VCRUNTIME  windows only: vcruntime140.dll to ship      (default: the newest Visual Studio 2022 redist)
#
# The two official missions ship as ONE fan mission folder, fms/arcade: both pk4s
# plus the override files in fm_overrides/ (a merged custom-scripts include, sound
# shaders and subtitles, since the pk4s each define those). Loose files in the FM
# folder take precedence over the pk4s, so either map can be loaded by name and the
# game never needs to switch fs_currentfm (which would require a restart).
#
# Files are hard-linked where possible, so staging costs almost no disk space.
set -eu

PLATFORM=${1:-linux}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
GAME_DIR=${GAME_DIR:-"$REPO/../darkmod"}
case "$PLATFORM" in
linux)
	TOML="$HERE/arcade.toml"
	OUT="$HERE/out/client"
	EXE=${EXE:-"$GAME_DIR/thedarkmod.x64"}
	EXE_NAME=thedarkmod.x64
	SDK_LIB=${SDK_LIB:-"$REPO/ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so"}
	SDK_LIB_NAME=libarcade_sdk.so
	LAUNCH=run_arcade.sh
	;;
windows)
	TOML="$HERE/arcade-windows.toml"
	OUT="$HERE/out/windows"
	EXE=${EXE:-"$GAME_DIR/TheDarkModx64.exe"}
	EXE_NAME=TheDarkModx64.exe
	SDK_LIB=${SDK_LIB:-"$REPO/ThirdParty/arcade_sdk/windows_64/arcade_sdk.dll"}
	SDK_LIB_NAME=arcade_sdk.dll
	LAUNCH=run_arcade.cmd
	# arcade_sdk.dll links the Visual C++ runtime; a fresh Proton prefix has none installed
	if [ -z "${VCRUNTIME:-}" ]; then
		VCRUNTIME=$(ls -d "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT/vcruntime140.dll 2>/dev/null | tail -1 || true)
	fi
	[ -f "${VCRUNTIME:-}" ] || { echo "vcruntime140.dll not found (install the VS 2022 C++ tools, or set VCRUNTIME)" >&2; exit 1; }
	;;
*)
	echo "usage: $0 [linux|windows]" >&2; exit 1 ;;
esac
MISSIONS="newjob stlucia"

BUILD_ID=$(sed -n 's/^build_id *= *"\([^"]*\)".*/\1/p' "$TOML" | head -1)
[ -n "$BUILD_ID" ] || { echo "no build_id in $TOML" >&2; exit 1; }
[ -f "$EXE" ] || { echo "executable not found: $EXE (build TDM first, or set EXE)" >&2; exit 1; }
[ -f "$SDK_LIB" ] || { echo "SDK library not found: $SDK_LIB (run ./fetch_sdk.sh, or set SDK_LIB)" >&2; exit 1; }
[ -f "$GAME_DIR/tdm_base01.pk4" ] || { echo "game data not found in $GAME_DIR (set GAME_DIR)" >&2; exit 1; }

link() { # link SRC DST: hard link, fall back to copy across filesystems
	ln -f "$1" "$2" 2>/dev/null || cp -f "$1" "$2"
}

rm -rf "$OUT"
mkdir -p "$OUT/fms"

link "$EXE" "$OUT/$EXE_NAME"
link "$SDK_LIB" "$OUT/$SDK_LIB_NAME"
[ "$PLATFORM" = windows ] && cp -f "$VCRUNTIME" "$OUT/vcruntime140.dll"
for f in "$GAME_DIR"/*.pk4 "$GAME_DIR"/tdm_shared_stuff.zip; do
	[ -f "$f" ] && link "$f" "$OUT/$(basename "$f")"
done
for f in config.spec darkmod.ini alsoft.ini alsoft-hrtf-default-44100.mhr alsoft-hrtf-default-48000.mhr \
         ca-bundle.crt description.txt AUTHORS.txt LICENSE.txt TDM_icon.ico darkmod.ico; do
	[ -f "$GAME_DIR/$f" ] && link "$GAME_DIR/$f" "$OUT/$f"
done
mkdir -p "$OUT/fms/arcade"
for m in $MISSIONS; do
	[ -d "$GAME_DIR/fms/$m" ] || { echo "mission not installed: $GAME_DIR/fms/$m (install it with the TDM mission downloader)" >&2; exit 1; }
	for f in "$GAME_DIR/fms/$m"/*.pk4; do link "$f" "$OUT/fms/arcade/$(basename "$f")"; done
done
(cd "$HERE/fm_overrides" && find . -type f) | while read -r f; do
	mkdir -p "$OUT/fms/arcade/$(dirname "$f")"
	cp "$HERE/fm_overrides/$f" "$OUT/fms/arcade/$f"
done

cp "$REPO/game/Arcade/vendor.proto" "$OUT/vendor.proto"
cp "$REPO/game/Arcade/README.md" "$OUT/ARCADE_README.md"
SDK_VERSION=$(grep -aoE '[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}' "$SDK_LIB" | head -1 || true)
sed "s|@BUILD_ID@|$BUILD_ID|g" "$HERE/$LAUNCH" > "$OUT/$LAUNCH"
if [ "$PLATFORM" = windows ]; then	# cmd wants CRLF; MSYS sed drops the template's
	sed -i 's/\r*$/\r/' "$OUT/$LAUNCH"
fi
sed "s|@BUILD_ID@|$BUILD_ID|g; s|@SDK_VERSION@|${SDK_VERSION:-unknown}|g" "$HERE/LAUNCH-$PLATFORM.txt" > "$OUT/LAUNCH.txt"
chmod +x "$OUT/$LAUNCH" "$OUT/$EXE_NAME"

if [ "$PLATFORM" = windows ]; then
	# `arcade-sdk describe` on a Windows desk starts the entrypoint by its \\?\ path, and cmd.exe
	# cannot run a batch file from one. For describe only, launch the exe with run_arcade.cmd's
	# arguments; the registration comes from the game's InitRequest, so it is the same.
	# (Arcade launches run_arcade.cmd itself, from Linux under Proton.)
	cat > "$HERE/out/describe-windows.toml" <<EOF
# Generated by stage_build.sh windows: arcade-windows.toml with the exe as entrypoint, for describe only.
game_id = "thedarkmod"
build_id = "$BUILD_ID"
[client]
dir = "windows"
entrypoint = "TheDarkModx64.exe"
args = ["+set", "arcade_enable", "1", "+set", "arcade_buildId", "$BUILD_ID",
        "+set", "fs_currentfm", "arcade", "+set", "r_fullscreen", "0", "+set", "r_customWidth", "1280", "+set", "r_customHeight", "720",
        "+set", "tdm_player_wait_until_ready", "0", "+set", "com_showFPS", "0"]
vendor_proto = "vendor.proto"
platform = "windows-x86_64"
proton = true
EOF
fi

echo "staged thedarkmod@$BUILD_ID ($PLATFORM) in $OUT ($(find "$OUT" -type f | wc -l) files, SDK ${SDK_VERSION:-unknown})"
if [ "$PLATFORM" = windows ]; then
	echo "next:  cd $HERE && tools/arcade-sdk.exe --config out/describe-windows.toml describe --out arcade-registration-windows.json"
	echo "       tools/arcade-sdk.exe --config arcade-windows.toml submit --registration arcade-registration-windows.json --dry-run"
else
	echo "next:  cd $HERE && xvfb-run -a -s '-screen 0 2560x1440x24' tools/arcade-sdk describe && tools/arcade-sdk submit --dry-run"
fi
