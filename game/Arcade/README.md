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
./thedarkmod.x64 +set arcade_enable 1 +set fs_currentfm arcade \
    +set r_fullscreen 0 +set r_customWidth 2560 +set r_customHeight 1440
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

## Missions and challenges

The arcade build ships the two official missions as one fan mission folder
(`fms/arcade`, see `arcade/stage_build.sh`): **A New Job** (map `prologue9`,
mission id `newjob`) and **Tears of St. Lucia** (map `saintlucia`, id `stlucia`).
Because both maps are in the search path, `StartChallenge` can load either by
name; switching `fs_currentfm` at runtime would need an engine restart.

Every attempt reloads the chosen map (a clean world), applies the `difficulty`
variation through the `tdm_difficulty` cvar, seeds `gameLocal.random` from the
request, skips the "press attack to start" overlay and starts the player at the
mission start. Judging uses the game's own systems: the objective states in
`CMissionData`, the mission result, the mission statistics (loot, alerts,
knockouts, kills) and the `info_location` areas.

| Id | Variations | Success | Failure / Timeout |
|---|---|---|---|
| `complete-mission` | `mission`, `difficulty`, `minutes` (10..120) | Mission result COMPLETE (all mandatory objectives). Score `0.5 + 0.5 × stealth`. | Death or mission failed. Timeout scores `0.5 × mandatory objectives done`. |
| `newjob-objective` | `objective` ∈ enter-tavern, find-clue, steal-rubies, meet-contact; `difficulty`, `minutes` (5..60) | The objective is COMPLETE (alternatives such as the easy/normal journal variants count). Score = stealth factor. | Objective FAILED, death, mission failed, timeout. |
| `stlucia-objective` | `objective` ∈ steal-relic, damage-statue, loot-quota, escape; `difficulty`, `minutes` | same | same |
| `newjob-reach`, `stlucia-reach` | `location` (curated `info_location` names), `stealth` ∈ any/unseen/ghost, `difficulty`, `minutes` (2..30) | Player stands in the target area. Score `(0.5 + 0.5 × time left) × stealth`. | Stealth rule broken, death, timeout. |
| `steal-loot` | `mission`, `percent` (10..100 of the map's total loot), `stealth`, `difficulty`, `minutes` | Loot found ≥ target. Score = stealth factor. | Stealth rule broken, death; timeout scores `found / target`. |
| `knockout` | `mission`, `count` (1..4), `difficulty`, `minutes` | Knockouts ≥ count with no kills. | Any kill, death; timeout scores `knockouts / count`. |
| `explore` | `mission`, `difficulty`, `minutes` (3..30) | Always at the time limit; score = distinct named areas visited / areas in the map. | Death. |

Stealth: `unseen` fails the attempt as soon as any AI searches for the player or
spots them; `ghost` also fails on the first suspicious AI. The stealth factor used
in scores is `0.25 + 0.75 / (1 + stealthScore / 10)` where `stealthScore` is TDM's
own weighted alert count (0 for a perfect ghost).

`ChallengeCompleted.final_metrics` carries `time/elapsed_s`,
`player/distance_travelled_m`, `ai/max_alert_index`, `explore/locations_visited`,
`mission/stealth_score`, `mission/times_seen`, `mission/loot_fraction`,
`player/loot`, `ai/knockouts`, `ai/kills`, `mission/objectives_complete`,
`mission/damage_received`.

Resolved instructions give the agent the mission premise, the task with the
variation values spelled out in plain words, and a short controls primer (the
`CONTROLS_HINT` string in `ArcadeIntegration.cpp`).

Mission knowledge (objective indices per mission, curated locations and their
plain-English names, mission summaries) lives in the static tables at the top of
`ArcadeIntegration.cpp`. Objective indices are the mission author's 1-based
numbering from the map's `atdm:target_addobjectives` entity.

The console command `arcade_probe <entity>` (available when the SDK is enabled)
prints the player's objective flag and what an objective volume's clip query sees;
handy when a location objective does not fire where you expect. Note that
`info_tdm_objective_location` volumes are often small boxes at doorways rather
than the whole room.

## Vendor RPCs (`thedarkmod.v1.Game`)

`TeleportPlayer`, `GetPlayerState`, `ListLocations` (with display names),
`ListAi`, `ExecConsoleCommand`, `GetMissionState` (objectives with states and
text, loot, stealth score, knockouts, kills, difficulty, result) — see
`vendor.proto` for messages and coordinate conventions (Z up, 1 unit = 1 inch,
yaw 0 = +X).

## Events and metrics

Events (`thedarkmod.v1.GameEvent`): `player_died`, `loot_picked_up`,
`ai_alert_changed`, `location_changed`, `player_damaged`, `mission_completed`,
`map_loaded`, `objective_changed`, `ai_knocked_out`, `ai_killed`, `mission_ended`.

Metrics: `player/health`, `player/loot`, `player/lightgem`,
`ai/max_alert_index`, `mission/stealth_score`, `mission/loot_fraction`,
`ai/knockouts`, `ai/kills`, `mission/objectives_complete`, and during an attempt
`player/distance_travelled_m`, `explore/locations_visited`.

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
