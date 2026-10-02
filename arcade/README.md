# Shipping The Dark Mod to the arcade

Everything needed to stage, describe and submit an arcade build. The engine-side
integration lives in `game/Arcade/` (see its README for challenges, RPCs, events,
cvars); this directory is the packaging and submission side.

There are two builds of the same game, submitted separately (each with its own build id):
**Linux x86-64** (`arcade.toml`) and **Windows x64, run under Proton** on arcade's Linux
machines (`arcade-windows.toml`).

| File | Purpose |
|---|---|
| `arcade.toml` / `arcade-windows.toml` | The build layout the `arcade-sdk` CLI reads: game id, **build id**, notes, client dir, entrypoint, platform (`proton = true` for Windows). Pass the Windows one with `--config arcade-windows.toml`. |
| `run_arcade.sh` / `run_arcade.cmd` | The client entrypoints (templates; `@BUILD_ID@` is filled in when staging). Open a plain window sized from `SCREEN_WIDTH`/`SCREEN_HEIGHT` and start straight into the main menu with the SDK enabled. The `.cmd` is CRLF and runs under Wine's cmd (no PowerShell there). |
| `LAUNCH-linux.txt` / `LAUNCH-windows.txt` | Human-readable notes shipped with the build as `LAUNCH.txt`. |
| `stage_build.sh [linux\|windows]` | Assembles `out/client/` (Linux) or `out/windows/` (Windows, plus `vcruntime140.dll` for the SDK DLL) from the built engine, the SDK library, game data and the two missions (as the combined `fms/arcade` folder). |
| `fm_overrides/` | Loose files for `fms/arcade` that resolve collisions between the two mission pk4s: the merged `tdm_custom_scripts.script` include list, merged sound shaders and subtitles, `darkmod.txt`. Loose files override pk4 contents. |
| `fetch_sdk.sh [linux] [windows]` | Downloads the pinned SDK build (a per-commit zip from the SDK bucket via `gsutil` when `SDK_SHA` is set, which is the default right now; otherwise GitHub release `arcade-sdk-v<version>` of get-wrecked/ai-research, via `gh`): the libraries into `ThirdParty/arcade_sdk/{linux_64,windows_64}/` and the `arcade-sdk` CLI (submit, describe, debug app) into `tools/`. |

`out/`, `tools/`, `arcade-registration*.json` and the SDK libraries are not committed.

## Synchronized media

This adapter depends on [SDK PR 4128](https://github.com/get-wrecked/ai-research/pull/4128),
pinned in `fetch_sdk.sh` to `fc970a05a52fb4abec49122e42928c574da6e2e0`.
ABI 2 is unchanged, but the three timestamp entry points are required; an older
library fails symbol loading with a clear error. Ship the corresponding SDK
library, header, protos, and CLI together.

With `+set arcade_enable 1`, the engine opens an OpenAL Soft loopback device and
registers its actual output: **44,100 Hz, stereo, normalized float PCM**. This
captures OpenAL's final spatialized/effected mix. Arcade mode sends this mix to
the SDK instead of a local speaker device; listen through the SDK debug app.
Normal player mode continues to use the original output device. Sound must be
enabled and `com_asyncSound` must be 1 or 3; unsupported loopback/startup fails
explicitly instead of advertising an audio stream that cannot produce samples.

Audio and video use `arcade_time_ns()`. Video records its observation before
the downscale/readback. The sound interrupt renders the elapsed sample interval
with the previously applied OpenAL state, then applies current game commands for
the next interval. Sample timestamps use cumulative integer sample positions,
so fractional callback lengths cannot accumulate drift. SDK submissions are at
most 10 ms and do not allocate, log, resample, or wait for a consumer. The engine
still performs its existing sound-world updates at approximately 60 Hz; this is
the precision of command onset, not sample-accurate game-event scheduling.

Catch-up is limited to 50 ms, below the streaming sources' queued buffer duration.
A longer stall omits that source interval and marks a discontinuity rather than
performing unbounded rendering. Existing OpenAL source positions resume after
the hole; this is not a seek to where uninterrupted playback would have been.
Rejected SDK submissions also mark the next accepted block discontinuous.
Muted intervals contain silence. Shutdown removes both hooks and waits for any
in-flight callbacks before shutting down/unloading the SDK.

The standalone `arcade/tests/audio_capture_clock.cpp` regression exercises
100,000 variable callbacks, exact fractional accounting, stalls, and restart;
the `Arcade media clock` workflow runs it with address/undefined-behavior
sanitizers. A native engine build and game recordings are still required to
validate OpenAL audibility and actual A/V cue timing.

## Release flow: Linux

```bash
cd arcade
./fetch_sdk.sh                                   # once per SDK release (needs gh auth)
./tools/arcade-sdk login                         # once per machine (vendor account; --no-browser for a device code)

# build the engine as usual (build/ -> thedarkmod.x64, copied to ../darkmod by COPY_EXE)
$EDITOR arcade.toml                              # bump build_id, update notes
./stage_build.sh                                 # -> out/client, with the build id in run_arcade.sh
xvfb-run -a -s "-screen 0 2560x1440x24" ./tools/arcade-sdk describe     # launches it, writes arcade-registration.json
./tools/arcade-sdk submit --dry-run              # package + validate
./tools/arcade-sdk submit                        # upload; build ids are immutable
./tools/arcade-sdk builds status thedarkmod@<build_id> --wait   # arcade runs every build once to verify it
```

## Release flow: Windows (under Proton)

On a Windows desk, in Git Bash, with Visual Studio 2022 (C++ tools) and the
Windows `ThirdParty/artefacts` (`windows_64/` and `tdm_deploy/`, from the TDM SVN):

```bash
# build: MFC is not needed; if it is not installed, point the resource compiler at a
# stub afxres.h that makes doom.rc skip the (MFC-only) editor resources
mkdir -p build/rc_shim && printf '#include <winres.h>\n#define AFX_RESOURCE_DLL\n' > build/rc_shim/afxres.h
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCOPY_EXE=OFF -DWITH_TOOLS=OFF "-DCMAKE_RC_FLAGS=-I$(cygpath -m "$PWD/build/rc_shim")"
cmake --build build --config Release --parallel     # -> build/Release/TheDarkModx64.exe

cd arcade
./fetch_sdk.sh windows
$EDITOR arcade-windows.toml                          # bump build_id (never the Linux one), update notes
GAME_DIR=<TDM install> EXE=../build/Release/TheDarkModx64.exe ./stage_build.sh windows   # -> out/windows
./tools/arcade-sdk.exe --config out/describe-windows.toml describe --out arcade-registration-windows.json
./tools/arcade-sdk.exe --config arcade-windows.toml submit --registration arcade-registration-windows.json --dry-run
./tools/arcade-sdk.exe --config arcade-windows.toml submit --registration arcade-registration-windows.json
./tools/arcade-sdk.exe builds status thedarkmod@<build_id> --wait
```

`describe` goes through `out/describe-windows.toml` (written by `stage_build.sh windows`),
which launches `TheDarkModx64.exe` with `run_arcade.cmd`'s arguments: on Windows the
CLI starts the entrypoint by its `\\?\` path, and cmd.exe cannot run a batch file from
one. Arcade itself launches `run_arcade.cmd` from Linux under Proton.

`describe` runs the real build, so read its printout: it is what the operator
will see and play. Everything it lists comes from `CArcadeIntegration::BuildInitRequest`.
Both builds register the same challenges, so the two registrations should match.

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
include path and library copy in `CMakeLists.txt`, the frame capture hook in the render backend, and the synthetic input in `sys/linux/input.cpp` and `sys/win32/win_input.cpp`. Everything else is additive.

## Local testing

```bash
cd ../darkmod && ARCADE_SDK_NO_DEBUG_UI=1 ./thedarkmod.x64 +set arcade_enable 1 +set fs_currentfm arcade +set r_fullscreen 0
../darkmod_src/arcade/tools/arcade-sdk debug --sdk-addr 127.0.0.1:6006 --port 6060 --no-browser   # UI at http://127.0.0.1:6060
```

`e2e_test.py` drives every challenge type and the input path through the debug
app's HTTP API (all requests carry `"instance": 0`). The debug app ends a play
session after 3 s without input traffic, so scripted drivers must keep sending
(empty) `/api/play/input` requests while waiting; `arcade_debugInput 1` logs every
poll and injected event in the game console.

Headless: run the game under `xvfb-run` (Mesa llvmpipe renders fine); the in-game
`screenshot` command yields black images there, so capture the X display with
ImageMagick `import -window root` instead. The debug app's Play tab shows the
640x360 frames the SDK receives, which is what the agent sees.
