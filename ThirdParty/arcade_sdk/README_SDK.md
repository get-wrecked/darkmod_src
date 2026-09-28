# Arcade Game SDK — integration guide

This is the guide for game developers integrating the arcade game SDK. It ships
in the SDK zip as `README.md`, next to:

| File | What it is |
|---|---|
| `arcade_sdk.dll`, `arcade_sdk.lib` (Windows) / `libarcade_sdk.so` (Linux) | The SDK. Link the import library or `LoadLibrary` the DLL; link or `dlopen` the shared object. |
| `include/arcade_sdk.h` | The C ABI: twelve functions, one status enum. |
| `proto/arcade_sdk.proto` | The messages you exchange with the SDK, and the one service you implement. |
| `proto/arcade_common.proto` | Vectors, quaternions, transforms, colours — use these in your own `.proto`. |
| `arcade-sdk-debug.exe` / `arcade-sdk-debug` | The debug app: a local web page to drive your integration while you build it. |
| `examples/host.c` | A complete minimal integration in C. |

There is one zip per platform, `arcade_sdk-<sha>.zip` for Windows and
`arcade_sdk-linux-<sha>.zip` for Linux, built from the same commit. The API,
protos and debug app are identical; only the library's file name and how you
load it differ. Below, "the DLL" means whichever you are shipping.

## 1. What you are building

A build of your game that an external agent can play as a research
environment. The game runs fullscreen on a Windows or Linux machine we
operate; our agent sees the screen and plays through keyboard and mouse, exactly like a
person. You do not change rendering or input. What you add, through the SDK, is
structure:

- **Challenges**: tasks the agent is asked to do — an id, an instruction
  sentence, and *variations* (parameters such as difficulty, time limit,
  which item). Your game starts a challenge on request, in seconds, from a
  clean state.
- **RPCs**: functions we call into your game — ours (`StartChallenge`,
  `StopChallenge`, `Ping`) and any you choose to expose (teleport the player,
  set the time of day, read the player's state). You describe them in a
  `.proto` file; the SDK carries them.
- **Reports**: what your game tells us — a challenge started or completed
  (success/failure, score), typed game events (an item was picked up, the
  player died), log lines, and numeric metrics sampled during play.

The SDK is one DLL. Inside your process it does two things: it gives you a
small C API, and it serves a local network endpoint that our tooling (and the
debug app) connects to. It never calls into your code: you poll it.

## 2. Checklist

What we need from you, in the end:

1. `vendor.proto`: your RPC service and your event union (section 4).
2. A build of your game with the SDK integrated (sections 5 and 6).
3. The list of challenges, designed together with us (section 3).
4. A way to launch straight into a state where a challenge can start: an
   executable plus arguments, no launcher, no login, no menu to click through,
   no modal dialogs.
5. A fixed screen resolution (we run at 2560×1440 by default) and no exclusive
   fullscreen requirement that fights window focus.

## 3. Design your challenges together with us

A good challenge:

- has **one clear goal** the game itself can judge (reach a place, collect N
  things, survive T seconds, defeat X);
- **restarts in seconds** from a known state — `StartChallenge` resets the
  world and places the player;
- has **variations that change difficulty or content, not the goal**: a
  time limit, an enemy count, a map, a target colour;
- **ends unambiguously**: your game reports `ChallengeCompleted` once, with
  `SUCCESS`, `FAILURE`, `TIMEOUT` or `ABORTED`, and optionally a score.

The **instruction** is what the agent is told. It is a template with `{name}`
placeholders for the variations, e.g. `Reach the {color} beacon within
{seconds} seconds`; your game returns the resolved sentence from
`StartChallenge`. Write it the way you would tell a new player.

Declare each challenge in `InitRequest.challenges`, with its variations:

| Kind | Fields | Example |
|---|---|---|
| `EnumVariation` | `values`, `default` | colour: red, blue, green |
| `IntVariation` | `min`, `max`, `default`, `step` | time limit 10..120 s |
| `FloatVariation` | `min`, `max`, `default` | gravity scale 0.5..2.0 |
| `BoolVariation` | `default` | night time |

`StartChallenge` always carries **every** declared variation (we fill in
defaults for any the caller omitted), so your code never has to guess.

## 4. Write `vendor.proto`

Two building blocks, both plain protobuf:

- a **service** with the RPCs your game serves (one service is the norm);
- one **event union** message: a `oneof` over your event messages.

```proto
syntax = "proto3";
package acme.roguelike.v1;

import "arcade_common.proto";   // Vec3, Quat, Transform, Color, ...

// RPCs your game serves. Called by fully-qualified name:
// ("acme.roguelike.v1.Game", "TeleportPlayer").
service Game {
  rpc TeleportPlayer(TeleportPlayerRequest) returns (TeleportPlayerResponse);
  rpc SetTimeOfDay(SetTimeOfDayRequest) returns (SetTimeOfDayResponse);
  rpc GetPlayerState(GetPlayerStateRequest) returns (PlayerState);
  rpc GiveItem(GiveItemRequest) returns (GiveItemResponse);
}

message TeleportPlayerRequest { arcade.common.v1.Vec3 position = 1; float yaw_deg = 2; }
message TeleportPlayerResponse {}
message SetTimeOfDayRequest { float hours = 1; }          // 0..24
message SetTimeOfDayResponse {}
message GetPlayerStateRequest {}
message PlayerState { arcade.common.v1.Transform transform = 1; float health = 2; repeated string inventory = 3; }
message GiveItemRequest { string item_id = 1; uint32 count = 2; }
message GiveItemResponse { uint32 count_in_inventory = 1; }

// The event union: what you put in Report.event. Registered as
// InitRequest.event_type = "acme.roguelike.v1.GameEvent". The oneof member
// name is how the event is labelled everywhere downstream.
message GameEvent {
  oneof event {
    ItemPickedUp item_picked_up = 1;
    PlayerDied   player_died    = 2;
    EnemyKilled  enemy_killed   = 3;
  }
}
message ItemPickedUp { string item_id = 1; arcade.common.v1.Vec3 position = 2; }
message PlayerDied { string cause = 1; double survived_s = 2; }
message EnemyKilled { string enemy_type = 1; float distance_m = 2; }
```

Rules:

- Use the types in `arcade_common.proto` instead of your own `Vec3`. They are
  read in the frame you declare in `InitRequest.coordinate_system` (section 5);
  document which way is forward, and the sign of each Euler angle, in a comment.
- **Append-only, forever.** Never renumber, reuse or delete a field, a `oneof`
  member or a method once a build has shipped. Add new ones at the end.
- Build the descriptor set the SDK needs, with the SDK's `proto/` directory on
  the include path:

  ```bash
  protoc -I. -I<sdk>/proto --include_imports --descriptor_set_out=vendor.pb vendor.proto
  ```

  The bytes of `vendor.pb` go in `InitRequest.vendor_descriptor_set`. Generate
  message code for your language from `vendor.proto`, `arcade_sdk.proto` and
  `arcade_common.proto` the same way (`--cpp_out`, `--csharp_out`, …). Unreal:
  the protobuf third-party module or a static protobuf-lite. Unity: the
  `Google.Protobuf` NuGet package.

## 5. Integrate the DLL

Everything is serialized protobuf in and out; you own every buffer. The
sequence in one game session:

```c
// Startup — once, after your engine is up.
uint8_t* init = ...; size_t init_len = ...;          // an encoded InitRequest
if (arcade_abi_version() != ARCADE_SDK_ABI_VERSION) { /* wrong header/DLL pair */ }
if (arcade_init(init, init_len) != ARCADE_STATUS_OK) {
    log("arcade_init: %s", arcade_last_error());   // what is wrong with the InitRequest
}
uint32_t distance_metric = arcade_metric_handle("distance/travelled_m");

// Every frame, on the game thread.
uint8_t buf[ARCADE_MAX_REQUEST_BYTES]; size_t n;
while (arcade_poll_request(buf, sizeof buf, &n) == ARCADE_STATUS_OK && n > 0) {
    RpcRequest req = decode_RpcRequest(buf, n);
    if (eq(req.service, "arcade.sdk.v1.ArcadeChallenges") && eq(req.method, "StartChallenge")) {
        StartChallengeRequest r = decode(req.request);
        start_challenge(r.challenge_id, r.variations);                  // your game logic
        StartChallengeResponse resp = { .instruction = resolve_instruction(r), .game_time_s = now() };
        arcade_respond(req.request_id, encode(resp));
    } else if (eq(req.service, "acme.roguelike.v1.Game") && eq(req.method, "TeleportPlayer")) {
        TeleportPlayerRequest r = decode(req.request);
        if (!teleport(r.position, r.yaw_deg)) arcade_fail(req.request_id, "no such location");
        else arcade_respond(req.request_id, encode((TeleportPlayerResponse){}));
    } else {
        arcade_fail(req.request_id, "unhandled method");
    }
}
arcade_push_f32_metric(distance_metric, player.distance, now());

// Whenever something happens — any thread.
arcade_report(encode(Report{ .game_time_s = now(), .event = encode(GameEvent{ .item_picked_up = {...} }) }));
arcade_report(encode(Report{ .game_time_s = now(), .challenge_completed = { .challenge_id = id, .outcome = OUTCOME_SUCCESS, .score = 1.0 } }));

// Shutdown — before your process exits or unloads the DLL.
arcade_shutdown();
```

`examples/host.c` is the complete version of this loop.

### The contract

| Rule | Why |
|---|---|
| Call `arcade_poll_request` **once per frame on the game thread**, draining until it returns 0 bytes. | The poll is the SDK's liveness signal; a frame without one reads as a hang. Your RPC handlers then run on your thread, so game state is safe to touch. |
| Answer **every** request with `arcade_respond` (the method's response message) or `arcade_fail` (a message). | An unanswered request times out for the caller after `timeout_ms` (30 s by default) and blocks nothing else, but reads as a bug. |
| The response must be **that method's** response type. | The SDK checks it decodes; a wrong type fails the request for its caller with `ARCADE_STATUS_VALIDATION_ERROR`. |
| `arcade_report`, `arcade_push_f32_metric`, `arcade_log` are safe from **any thread**. | They are a channel send. |
| Resolve metric handles **once** at startup, not per frame. | `arcade_metric_handle` takes a lock; `arcade_push_f32_metric` does not. |
| Every buffer is yours; the SDK never allocates for you and never keeps your pointer. | No `arcade_free`, no lifetime questions. |
| `arcade_init` and `arcade_shutdown` must not overlap other SDK calls. | Everything else may run concurrently. |
| Call `arcade_shutdown` from ordinary code before exit or `FreeLibrary` — **never** from `DllMain` or an `atexit` handler. | Windows holds the loader lock there, and the SDK's threads need it to exit: joining them from `DllMain` deadlocks. |
| On `ARCADE_STATUS_SHUTDOWN_INCOMPLETE`, do **not** unload the DLL; retry later or just exit the process. | A thread still running inside an unmapped DLL crashes the process. |

### Status codes

| Status | Meaning |
|---|---|
| `OK` | Done. |
| `NOT_INITIALIZED` | `arcade_init` has not succeeded, or `arcade_shutdown` ran. |
| `ALREADY_INITIALIZED` | `arcade_init` called twice. |
| `INVALID_ARGUMENT` | Null pointer, zero length where data is required, or bad UTF-8. |
| `DECODE_ERROR` | The bytes are not the expected protobuf message. |
| `VALIDATION_ERROR` | Decoded but not acceptable: unknown challenge, value out of range, response of the wrong type, event of the wrong type, bad metric name. |
| `BUFFER_TOO_SMALL` | `arcade_poll_request`: `*out_len` holds the size needed; the request stays queued. `ARCADE_MAX_REQUEST_BYTES` never hits this. |
| `UNKNOWN_REQUEST` | No such pending request (already answered, timed out, or never issued). |
| `UNKNOWN_METHOD` | A caller asked for a service/method your game does not serve. |
| `QUEUE_FULL` | 256 requests are waiting to be polled: the game stopped polling. |
| `SHUTDOWN_INCOMPLETE` | See above. |
| `PANIC` | A bug in the SDK. Send us `arcade_sdk.log`. |
| `INTERNAL` | Anything else; `arcade_last_error()` says what (typically the port is in use). |

`arcade_last_error()` returns the details of the last non-OK status **on the
calling thread**, valid until that thread's next SDK call.

### C#

Every parameter is blittable; x64 has one calling convention.

```csharp
using System.Runtime.InteropServices;

static class Arcade {
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern uint arcade_abi_version();
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern IntPtr arcade_last_error();
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_init(byte[] init, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_shutdown();
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_poll_request(byte[] buf, nuint cap, out nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_respond(ulong requestId, byte[] response, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_fail(ulong requestId, [MarshalAs(UnmanagedType.LPUTF8Str)] string error);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_report(byte[] report, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern uint arcade_metric_handle([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport("arcade_sdk", ExactSpelling = true)] public static extern int arcade_push_f32_metric(uint handle, float value, double gameTimeS);
    public static string LastError() => Marshal.PtrToStringUTF8(arcade_last_error()) ?? "";
}
```

### The `InitRequest`

Fill it once, at startup:

| Field | What to put |
|---|---|
| `game_id` | A stable lowercase identifier for your game, `[a-z0-9_-]+`. |
| `build_id` | Your build/version string; recorded with every run. |
| `abi_version` | `ARCADE_SDK_ABI_VERSION` from the header you compiled against. |
| `challenges` | Section 3. |
| `metrics` | The metrics you will push: `name` (`/`-separated, no `.` or `:`), `kind` (SCALAR, COUNTER, BOOL), `unit`, `description`. Names you push without declaring are accepted as SCALAR. |
| `vendor_descriptor_set` | The bytes of `vendor.pb`. |
| `services` | `["arcade.sdk.v1.ArcadeChallenges", "acme.roguelike.v1.Game"]` — every service you serve, by fully-qualified name. |
| `event_type` | `"acme.roguelike.v1.GameEvent"`, or empty if you report no events. |
| `coordinate_system` | **Required.** Your world frame: `up` (`AXIS_POS_Y`, `AXIS_POS_Z`, …), `handedness` (`HANDEDNESS_LEFT` or `HANDEDNESS_RIGHT`) and `euler_order`, the order your Euler angles apply in (`EULER_ORDER_YAW_PITCH_ROLL`, …). See below. |

`coordinate_system` is how we read every `Vec3`, `Quat` and `Euler` you send.
All three fields must be set; `arcade_init` refuses `UNSPECIFIED`.

- **Handedness:** point your thumb along +X and your index finger along +Y; the
  hand whose middle finger then points along +Z is yours. A positive rotation
  turns the way that hand's fingers curl around its thumb.
- **Euler order:** the order of *intrinsic* rotations — each about the object's
  own axes as the rotations before it left them. Yaw is about up, pitch about
  the object's right axis, roll about its forward axis. If your engine states
  its order about the fixed world axes (extrinsic), reverse it: Unity's "Z, X,
  then Y about the world axes" is `EULER_ORDER_YAW_PITCH_ROLL`.

| Engine | `up` | `handedness` | `euler_order` |
|---|---|---|---|
| Unity | `AXIS_POS_Y` | `HANDEDNESS_LEFT` | `EULER_ORDER_YAW_PITCH_ROLL` |
| Unreal | `AXIS_POS_Z` | `HANDEDNESS_LEFT` | `EULER_ORDER_YAW_PITCH_ROLL` |
| Godot | `AXIS_POS_Y` | `HANDEDNESS_RIGHT` | `EULER_ORDER_YAW_PITCH_ROLL` |
| Source, idTech | `AXIS_POS_Z` | `HANDEDNESS_RIGHT` | `EULER_ORDER_YAW_PITCH_ROLL` |

These are the engines' defaults; a game that converts coordinates on its way
out declares the frame it sends.

`arcade_init` rejects anything inconsistent with a message that names the
field, and has no side effects when it does.

## 6. Implement `ArcadeChallenges`

| Method | What your game must do |
|---|---|
| `StartChallenge(StartChallengeRequest)` | Reset the world and put the player at the start of `challenge_id`, applying every variation in `variations`. Seed randomness from `seed` so equal seeds reproduce. Return `StartChallengeResponse { instruction, game_time_s }` — the instruction template with the values substituted. The player must be able to act within a few seconds of the response. Also send a `ChallengeStarted` report with `run_id` echoed. |
| `StopChallenge(StopChallengeRequest)` | Abort the running challenge (if any) and return to an idle, ready state. |
| `Ping(PingRequest)` | Return `PingResponse { game_time_s }` from the game loop. |

When the game has judged the attempt, send **one** `ChallengeCompleted` report,
immediately, with `challenge_id`, the `run_id` from the start request, the
`outcome`, an optional `score`, a human-readable `detail`, and any
per-attempt `final_metrics`.

## 7. Test with the debug app

1. Put `arcade_sdk.dll` and `arcade-sdk-debug.exe` (Linux: `libarcade_sdk.so`
   and `arcade-sdk-debug`) next to your executable.
2. Run the game. When `arcade_init` succeeds the SDK launches the debug app,
   which opens a browser page. (Or run
   `arcade-sdk-debug --sdk-addr 127.0.0.1:6006` yourself.)
3. **Challenges**: start each challenge with chosen variation values and watch
   the game do it; the resolved instruction comes back on the card.
4. **Services**: call any of your RPCs from a form generated from your `.proto`,
   and see the typed response.
5. **Live**: the SDK's liveness tick, every metric with a sparkline, and the
   report stream — challenge events, your typed events, log lines.

The SDK writes `arcade_sdk.log` next to the DLL. Environment variables:

| Variable | Effect |
|---|---|
| `ARCADE_SDK_PORT` | The SDK's loopback port (default 6006). |
| `ARCADE_SDK_NO_DEBUG_UI=1` | Do not launch the debug app. |
| `ARCADE_SDK_LOG` | Log filter, e.g. `debug` (default `info`). |
| `ARCADE_SDK_LOG_DIR` | Where to write `arcade_sdk.log`. |
| `SCREEN_WIDTH`, `SCREEN_HEIGHT` | Set by our machines: the display size your window should fill. |

## 8. Submit it

Builds are submitted with the `arcade-sdk` command line from the SDK archive
(`arcade-sdk` on Linux, `arcade-sdk.exe` on Windows). A submission is one
*build*: `<game_id>@<build_id>`, where the build id is your version string —
ideally the same one you register as `InitRequest.build_id`. A build is the
client, and the dedicated server too if your game needs one.

```bash
arcade-sdk login      # once per computer
arcade-sdk init       # once: writes arcade.toml
arcade-sdk describe   # every build: runs it, writes down what it declares
arcade-sdk submit     # every build: uploads it
```

### What we run

Today: **Linux x86-64 clients**, and their Linux dedicated servers. The build
runs in a container with an X11 display (`$DISPLAY`), an NVIDIA GPU (Vulkan and
OpenGL), PulseAudio, and no network beyond loopback, as an unprivileged user.
So:

- The build as a folder: your executable, its data, and `libarcade_sdk.so`
  beside it. No installer, no auto-updater, no online login; it must run
  offline. Follow-symlinks are fine; we package what they point at.
- It opens a window on `$DISPLAY` at the resolution in `SCREEN_WIDTH` /
  `SCREEN_HEIGHT` (2560×1440 by default), not exclusive fullscreen, and needs
  no `sudo`, no `/dev/input`, no Wayland.
- The command line you submit takes it straight into a state where
  `StartChallenge` can be served — no launcher, login or menu.
- `vendor.proto` (the source, not only `vendor.pb`), inside the folder: it is
  what benchmarks against your services are written from.
- A **dedicated server**, if the client cannot play without one: a Linux
  folder of its own (or the same folder). We start it on the client's machine,
  before the client, and the client connects to it on `$ARCADE_SERVER_HOST`
  (`127.0.0.1` today — read the variable rather than hard-coding loopback, so
  your build keeps working if the server moves to a machine of its own). The
  SDK library stays in the client: the client serves the challenges and
  reports their outcomes. If either process exits, both are restarted.

Windows clients are a shape the layout already has room for; ask us before
building one.

### Who may submit

You, with a General Intuition vendor account: your contact sends you an invite
link, you pick your email and password on it, and the games you may submit are
listed when you sign in at <https://vendors.generalintuition.ai>. Sign the tool
in once per computer with `arcade-sdk login` (it opens your browser; on a
machine without one, `--no-browser` prints a link and asks for the code the page
shows). `arcade-sdk whoami` shows who you are signed in as.

### Step 1: `arcade.toml`

`arcade-sdk init` asks for your game id, the first build id, the client's
folder and entrypoint (an executable, or a script: `.sh` with a `#!` line,
`.ps1`, `.bat`) and whether there is a server, and writes `arcade.toml` —
every key commented. Every flag it asks about can also be passed
(`--game-id`, `--client-dir`, `--client-entrypoint`, `--client-arg`,
`--server-dir`, `--server-entrypoint`, `--server-arg`, `--ready-tcp-port`).
Keep the file beside your build scripts and commit it:

```toml
game_id = "acme_shooter"
build_id = "1.4.0"          # bump for every submit
notes = "beacon challenge now spawns in the east wing"

[client]                    # the game the agent plays; ships libarcade_sdk.so
dir = "out/linux"           # relative to arcade.toml
entrypoint = "bin/acme"     # relative to dir
args = ["--arcade", "--no-intro"]
vendor_proto = "vendor.proto"
platform = "linux-x86_64"

[server]                    # only if the game needs a dedicated server
dir = "out/linux-server"
entrypoint = "run_server.sh"
args = ["--port", "7777"]
ready_tcp_port = 7777       # the client starts once this accepts connections
platform = "linux-x86_64"
```

The `args` are the ones we launch with: together they must take the game
straight into a state where a challenge can start. Without `ready_tcp_port`
the client is started right after the server, so it must retry its connect.

### Step 2: describe the build

What your build declares — its challenges, variations, metrics, services and
events — lives in its `InitRequest`, which exists only inside the running
game. So the second step runs the build once, where it runs, and writes that
declaration down:

```bash
arcade-sdk describe
```

It starts the server (and waits for its `ready_tcp_port`) if there is one,
then the client with the SDK on a free loopback port and without the debug
app, waits for `arcade_init` (up to `--timeout-s`, 300 s by default), reads
the registration, checks the game has started polling, and stops both. It
prints what it found — every challenge with its variations and defaults,
every metric, your services and event kinds — and writes
`arcade-registration.json` beside `arcade.toml`. Read the printout: it is
exactly what we will see and play. A wrong default, a missing challenge or a
metric you forgot to declare shows up here, not after a run. It also refuses
a build that registers a different `game_id` than `arcade.toml` says.

### Step 3: submit

```bash
arcade-sdk submit
```

It checks the folders (the entrypoints, the SDK library in the client, your
proto) and the registration (the same `game_id`; a different registered
`build_id` is only a warning), packages each folder preserving file modes, and
uploads the server first and the client last, each as an archive and then a
manifest describing it — registration included, so we know what the build can
do before anything runs it. Then it prints the commands that run it.
`--dry-run` does everything but upload — run that yourself before sending us a
build; `--out DIR` keeps the archives and manifests for inspection.
`--build-id` and `--notes` override `arcade.toml` for one submission (CI:
`arcade-sdk submit --build-id git-$(git rev-parse --short HEAD)`), and
`--config` points every command at another `arcade.toml`.

If you cannot launch the build where you submit from (a Linux build from a
Windows desk, say), run `describe` where it runs and bring the file along
(`--registration path`); it is plain JSON. `--no-registration` submits without
one, and then nothing about the build's challenges is known until a machine
runs it — avoid it.

**Build ids are never reused.** A submitted build is immutable — the same
build id a second time is refused — so every iteration gets a new id
(`1.4.1`, `git-3fa9c2`, `2026-09-21-2`…). `arcade-sdk builds show
acme_shooter@1.4.0` (or `@latest`) prints what a submission recorded,
registration included; the portal lists your builds too.

The same SDK library you tested against runs in our environment; we replace
nothing. What comes back: recordings of every attempt, and per challenge and
build the success rate, your metrics and events, and how the agent's inputs
looked.

## 9. FAQ and gotchas

- **`BUFFER_TOO_SMALL`**: only with a buffer smaller than
  `ARCADE_MAX_REQUEST_BYTES`. Grow to `*out_len` and poll again; the request is
  still there.
- **Changing the proto later**: append fields, `oneof` members and methods;
  never renumber or remove. Our side decodes with your descriptor set from the
  build it is running, so additions are safe and removals are not.
- **`SHUTDOWN_INCOMPLETE` in practice**: a thread was mid-request. Wait a
  second and call `arcade_shutdown` again, or let the process exit; only
  `FreeLibrary` is dangerous.
- **Exclusive fullscreen vs borderless**: borderless fullscreen at the
  configured resolution is the safest; exclusive fullscreen loses focus to
  nothing but must not minimize on focus loss.
- **Pausing when unfocused**: don't. The agent's inputs arrive as normal
  keyboard/mouse events; a game that pauses on focus loss stalls the attempt.
- **Modal dialogs**: any dialog that needs a click to dismiss (crash reporter,
  "press any key", EULA) stops everything. Disable them in the build you ship.
- **Two games on one machine**: set `ARCADE_SDK_PORT` differently; the SDK
  refuses to start if its port is taken rather than silently picking another.
