# Shipping The Dark Mod to the arcade

Everything needed to stage, describe and submit an arcade build. The engine-side
integration lives in `game/Arcade/` (see its README for challenges, RPCs, events,
cvars); this directory is the packaging and submission side.

| File | Purpose |
|---|---|
| `arcade.toml` | The build layout the `arcade-sdk` CLI reads: game id, **build id**, notes, client dir and entrypoint. |
| `run_arcade.sh` | The client entrypoint (template; `@BUILD_ID@` is filled in when staging). Opens a plain X11 window sized from `SCREEN_WIDTH`/`SCREEN_HEIGHT`, starts straight into the main menu with the SDK enabled. |
| `LAUNCH.txt` | Human-readable notes shipped with the build. |
| `stage_build.sh` | Assembles `out/client/` from the built engine, the SDK library, game data and missions. |
| `fetch_sdk.sh` | Downloads the pinned SDK release: the shared library into `ThirdParty/arcade_sdk/linux_64/` and the CLI into `tools/`. |

`out/`, `tools/`, `arcade-registration.json` and the SDK `.so` are not committed.

## Release flow

```bash
cd arcade
./fetch_sdk.sh                                   # once per SDK release (needs gsutil auth)
./tools/arcade-sdk login                         # once per machine (vendor account; --no-browser for a device code)

# build the engine as usual (build/ -> thedarkmod.x64, copied to ../darkmod by COPY_EXE)
$EDITOR arcade.toml                              # bump build_id, update notes
./stage_build.sh                                 # -> out/client, with the build id in run_arcade.sh
xvfb-run -a -s "-screen 0 2560x1440x24" ./tools/arcade-sdk describe     # launches it, writes arcade-registration.json
./tools/arcade-sdk submit --dry-run              # package + validate
./tools/arcade-sdk submit                        # upload; build ids are immutable
./tools/arcade-sdk builds show thedarkmod@<build_id>
```

`describe` runs the real build, so read its printout: it is what the operator
will see and play. Everything it lists comes from `CArcadeIntegration::BuildInitRequest`.

## Keeping in sync with upstream

This fork tracks the official mirror. The arcade work lives on the `arcade` branch;
`trunk` stays identical to upstream so rebases and merges are clean:

```bash
git remote add upstream https://github.com/stgatilov/darkmod_src   # once
git fetch upstream
git checkout trunk && git merge --ff-only upstream/trunk && git push origin trunk
git checkout arcade && git merge upstream/trunk                    # resolve, build, describe, submit
```

The integration touches upstream files in only a few places (search for `arcade`):
the `idGame::ArcadeFrame` hook in `game/Game.h`, `game/Game_local.{h,cpp}` and
`framework/Common.cpp`, `idPlayer::ForceReady` in `game/Player.{h,cpp}`, and the
include path and library copy in `CMakeLists.txt`. Everything else is additive.

## Local testing

```bash
cd ../darkmod && ARCADE_SDK_NO_DEBUG_UI=1 ./thedarkmod.x64 +set arcade_enable 1 +set fs_currentfm training_mission +set r_fullscreen 0
../darkmod_src/arcade/tools/arcade-sdk-debug --sdk-addr 127.0.0.1:6006 --port 6060 --no-browser   # UI at http://127.0.0.1:6060
```

Headless: run the game under `xvfb-run` (Mesa llvmpipe renders fine); the in-game
`screenshot` command yields black images there, so capture the X display with
ImageMagick `import -window root` instead.
