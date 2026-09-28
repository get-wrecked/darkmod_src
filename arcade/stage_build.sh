#!/bin/sh
# Assembles the arcade client folder (out/client) that arcade.toml points at:
# the engine executable, the arcade SDK library, base game data, the bundled
# missions, vendor.proto and the launch script, with the build id from
# arcade.toml written into run_arcade.sh.
#
# Usage:  ./stage_build.sh
#   GAME_DIR   where the TDM assets + thedarkmod.x64 live   (default: ../../darkmod, i.e. CMake's GAME_DIR)
#   EXE        engine executable to ship                   (default: $GAME_DIR/thedarkmod.x64)
#   SDK_LIB    libarcade_sdk.so to ship                    (default: ../ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so, see fetch_sdk.sh)
#
# The two official missions ship as ONE fan mission folder, fms/arcade: both pk4s
# plus the override files in fm_overrides/ (a merged custom-scripts include, sound
# shaders and subtitles, since the pk4s each define those). Loose files in the FM
# folder take precedence over the pk4s, so either map can be loaded by name and the
# game never needs to switch fs_currentfm (which would require a restart).
#
# Files are hard-linked where possible, so staging costs almost no disk space.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
GAME_DIR=${GAME_DIR:-"$REPO/../darkmod"}
EXE=${EXE:-"$GAME_DIR/thedarkmod.x64"}
SDK_LIB=${SDK_LIB:-"$REPO/ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so"}
MISSIONS="newjob stlucia"
OUT="$HERE/out/client"

BUILD_ID=$(sed -n 's/^build_id *= *"\([^"]*\)".*/\1/p' "$HERE/arcade.toml" | head -1)
[ -n "$BUILD_ID" ] || { echo "no build_id in arcade.toml" >&2; exit 1; }
[ -x "$EXE" ] || { echo "executable not found: $EXE (build TDM first, or set EXE)" >&2; exit 1; }
[ -f "$SDK_LIB" ] || { echo "SDK library not found: $SDK_LIB (run ./fetch_sdk.sh, or set SDK_LIB)" >&2; exit 1; }
[ -f "$GAME_DIR/tdm_base01.pk4" ] || { echo "game data not found in $GAME_DIR (set GAME_DIR)" >&2; exit 1; }

link() { # link SRC DST: hard link, fall back to copy across filesystems
	ln -f "$1" "$2" 2>/dev/null || cp -f "$1" "$2"
}

rm -rf "$OUT"
mkdir -p "$OUT/fms"

link "$EXE" "$OUT/thedarkmod.x64"
link "$SDK_LIB" "$OUT/libarcade_sdk.so"
for f in "$GAME_DIR"/*.pk4 "$GAME_DIR"/tdm_shared_stuff.zip; do
	link "$f" "$OUT/$(basename "$f")"
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
SDK_VERSION=$(strings -n 8 "$SDK_LIB" | grep -oE '0\.[0-9]+\.[0-9]+\+[0-9a-f]{12}' | head -1 || true)
sed "s|@BUILD_ID@|$BUILD_ID|g" "$HERE/run_arcade.sh" > "$OUT/run_arcade.sh"
sed "s|@BUILD_ID@|$BUILD_ID|g; s|@SDK_VERSION@|${SDK_VERSION:-unknown}|g" "$HERE/LAUNCH.txt" > "$OUT/LAUNCH.txt"
chmod +x "$OUT/run_arcade.sh" "$OUT/thedarkmod.x64"

echo "staged thedarkmod@$BUILD_ID in $OUT ($(find "$OUT" -type f | wc -l) files, SDK ${SDK_VERSION:-unknown})"
echo "next:  cd $HERE && xvfb-run -a -s '-screen 0 2560x1440x24' arcade-sdk describe && arcade-sdk submit --dry-run"
