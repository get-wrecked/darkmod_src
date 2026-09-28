# Arcade game SDK integration

This directory wires The Dark Mod into the *arcade game SDK*
(`ThirdParty/arcade_sdk`, guide in `ThirdParty/arcade_sdk/README_SDK.md`): an
external agent plays the game through the normal screen, keyboard and mouse,
while the SDK gives the operator structure — **challenges** to start,
**RPCs** into the game, and **reports** (events, metrics, outcomes) back out.

Nothing here changes rendering or input. A normal player build is unaffected:
the integration is off unless `arcade_enable` is set, and the SDK library is
loaded at runtime only when asked for.

## Files

| File | Purpose |
|---|---|
| `ArcadeIntegration.{h,cpp}` | The integration: SDK lifecycle, request dispatch, challenge state machine, judging, event/metric observers. |
| `ArcadeSdkLoader.{h,cpp}` | `dlopen`/`LoadLibrary` binding of `libarcade_sdk.so` / `arcade_sdk.dll` from the executable directory, ABI check. |
| `ArcadeProto.{h,cpp}` | Minimal proto3 wire-format writer/reader (the engine has no protobuf dependency). Includes doctest cases. |
| `vendor.proto` | Our schema: the `thedarkmod.v1.Game` RPC service and the `GameEvent` union. Append-only. |
| `vendor_pb.h` | Generated: serialized `FileDescriptorSet` of `vendor.proto` embedded in the binary. |
| `gen_vendor_pb.py` | Regenerates `vendor_pb.h` (needs `protoc`). Run after editing `vendor.proto`. |

Engine hooks: `idGame::ArcadeFrame()` is called from `idCommonLocal::Frame()`
(and from `GUIFrame()` during map loads, poll-only); `idGameLocal` calls
`Init/Shutdown/OnMapStarted/OnMapShutdown` at the matching points.

## Running

Put `libarcade_sdk.so` next to `thedarkmod.x64` (the CMake `COPY_EXE` step
copies it from `ThirdParty/arcade_sdk/linux_64`), install a fan mission, and
launch straight into the main menu:

```bash
./thedarkmod.x64 +set arcade_enable 1 +set fs_currentfm training_mission \
    +set r_fullscreen 2 +set r_customWidth 2560 +set r_customHeight 1440
```

No menu needs clicking: `StartChallenge` loads the mission's starting map
itself, and the "press attack to start" overlay is skipped (`arcade_autoReady`).
The SDK listens on `127.0.0.1:6006` (`ARCADE_SDK_PORT`), writes
`arcade_sdk.log` next to the executable, and launches the debug app if it is
present (`ARCADE_SDK_NO_DEBUG_UI=1` to suppress).

Cvars:

| Cvar | Default | Meaning |
|---|---|---|
| `arcade_enable` | 0 | Load the SDK at startup (command line only). |
| `arcade_autoReady` | 1 | Skip the wait-until-ready overlay after a map loads. |
| `arcade_metricsIntervalMs` | 100 | Metric sample period. |
| `arcade_mapLoadTimeoutSec` | 60 | Fail `StartChallenge` if the map does not come up in time. |
| `arcade_buildId` | "" | Build id registered with the SDK; set it to the `--build-id` the build is submitted under (command line only). |

## Submitting a build

Packaging and submission live in the top-level `arcade/` directory (`arcade.toml`,
`stage_build.sh`, `fetch_sdk.sh`, `run_arcade.sh`); see `arcade/README.md` for the
full flow. In short, `arcade-sdk submit` packages the staged client folder and
uploads it to the game-builds bucket as `thedarkmod@<build-id>`; build ids are
never reused:

```bash
arcade-sdk login                       # once per machine: vendor account (device-code flow with --no-browser)
# 1. describe the build layout once; writes arcade.toml next to the staging folder
arcade-sdk init --game-id thedarkmod --build-id <id> \
    --client-dir thedarkmod-linux-<id> --client-entrypoint run_arcade.sh
# 2. launch the build once and record what it registers (challenges, metrics, services, events,
#    coordinate system); writes arcade-registration.json beside arcade.toml
xvfb-run -a -s "-screen 0 2560x1440x24" arcade-sdk describe
# 3. package + upload, registration included
arcade-sdk submit --dry-run --out /tmp/submit-check
```

Drop `--dry-run` to upload. For every new submission bump `build_id` in `arcade.toml`
(or pass `submit --build-id`) and set `+set arcade_buildId <id>` in `run_arcade.sh` to
the same id, so the registration and the artifact agree. The `InitRequest` declares
idTech4's frame (Z up, right-handed, yaw-pitch-roll); the SDK rejects an init without it. The launch script must read `SCREEN_WIDTH` /
`SCREEN_HEIGHT` and open a plain window (`r_fullscreen 0`), not exclusive
fullscreen; `arcade-sdk whoami --game-id thedarkmod` shows whether the
current account may submit.

## Challenges

All challenges run on the starting map of the installed fan mission
(`fs_currentfm`); `StartChallenge` reloads it for a clean world and seeds
`gameLocal.random` from the request. Every attempt fails on player death.

| Id | Variations | Success | Failure / Timeout |
|---|---|---|---|
| `reach-location` | `location` (enum of the map's `info_location` entity names), `seconds` | Player stands in the target location area. Score `1 - elapsed/limit`. | Timeout. |
| `collect-loot` | `loot` (25..2000), `seconds` | Carried loot value ≥ target. | Timeout, score `loot/target`. |
| `stay-hidden` | `seconds`, `max_alert` (`suspicious`, `searching`, `agitated_searching`, `combat`) | Time limit reached. | Any living AI reaches the alert index. |
| `free-roam` | `seconds` | Time limit reached; score = distance travelled (m). | — |

`ChallengeCompleted.final_metrics` carries `time/elapsed_s`,
`player/distance_travelled_m`, `ai/max_alert_index`, `player/loot`.

## Vendor RPCs (`thedarkmod.v1.Game`)

`TeleportPlayer`, `GetPlayerState`, `ListLocations`, `ListAi`,
`ExecConsoleCommand` — see `vendor.proto` for messages and coordinate
conventions (Z up, 1 unit = 1 inch, yaw 0 = +X).

## Events and metrics

Events (`thedarkmod.v1.GameEvent`): `player_died`, `loot_picked_up`,
`ai_alert_changed`, `location_changed`, `player_damaged`, `mission_completed`,
`map_loaded`.

Metrics: `player/health`, `player/loot`, `player/lightgem`,
`ai/max_alert_index`, `player/distance_travelled_m` (during an attempt).

## Testing

- Unit tests for the codec: `runTests -tc=ArcadeProto*` in the game console.
- End to end with the debug app: run the game as above, then
  `arcade-sdk-debug --sdk-addr 127.0.0.1:6006 --port 6060 --no-browser` and
  open http://127.0.0.1:6060/. Its HTTP API (all `POST`, JSON):

  ```bash
  curl -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:6060/api/state
  curl -X POST -H 'Content-Type: application/json' \
    -d '{"input":{"challenge_id":"free-roam","values":{"seconds":45},"run_id":"r1","seed":42,"timeout_ms":60000}}' \
    http://127.0.0.1:6060/api/start_challenge
  curl -X POST -H 'Content-Type: application/json' \
    -d '{"service":"thedarkmod.v1.Game","method":"GetPlayerState","request":{},"timeout_ms":5000}' \
    http://127.0.0.1:6060/api/call
  curl -X POST -H 'Content-Type: application/json' -d '{"since_seq":0}' http://127.0.0.1:6060/api/reports
  curl -X POST -H 'Content-Type: application/json' -d '{"since_id":0}'  http://127.0.0.1:6060/api/metrics
  ```

- Headless: run under `Xvfb` with Mesa llvmpipe (`xvfb-run -a -s "-screen 0 1280x720x24"`);
  the in-game `screenshot` command yields black images there, capture the X
  display with ImageMagick `import -window root` instead.
