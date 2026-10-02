# Arcade Game SDK — integration guide

This is the guide for game developers integrating the arcade game SDK. It ships
in the SDK zip as `README.md`, next to:

| File | What it is |
|---|---|
| `arcade_sdk.dll`, `arcade_sdk.lib` (Windows) / `libarcade_sdk.so` (Linux) | The SDK. Link the import library or `LoadLibrary` the DLL; link or `dlopen` the shared object. |
| `include/arcade_sdk.h` | The C ABI: fifteen functions, one status enum, the `ArcadeFrame` struct. |
| `proto/arcade_sdk.proto` | The messages you exchange with the SDK, and the one service you implement. |
| `proto/arcade_common.proto` | Vectors, quaternions, transforms, colours — use these in your own `.proto`. |
| `arcade-sdk.exe` / `arcade-sdk` | The command line: `arcade-sdk debug` is the debug app, a local web page to drive your integration while you build it; the other commands submit your builds (§8). |
| `examples/host.c` | A complete minimal integration in C: one instance, frames, input, challenges. |
| `CHANGELOG.md` | What changed in each version, and what a version number promises. |

There is one zip per platform and version, built from the same commit:

| Zip | For |
|---|---|
| `arcade_sdk-windows-<version>.zip` | 64-bit Windows games (x64) |
| `arcade_sdk-windows-x86-<version>.zip` | 32-bit Windows games (x86) |
| `arcade_sdk-linux-<version>.zip` | Linux games (x86-64) |

The API, protos and `arcade-sdk` are identical; only the library's file
name and how you load it differ. Below, "the DLL" means whichever you are
shipping.

**A 32-bit game takes the x86 zip**: a process can only load a DLL of its own
bitness. That zip's `arcade_sdk.dll`, `arcade_sdk.lib` and `arcade_sdk.pdb` are
32-bit; its `arcade-sdk.exe` is the same 64-bit program as in the x64 zip — it
runs as its own process, so it does not have to match the game. The header is
the same, and its functions use the C calling convention (`__cdecl`) on both;
see [C#](#c) if you call them from managed code.

Versions are [semantic](https://semver.org): a new major version (`2.0.0`)
means you have to change your integration or rebuild, a minor or patch one
does not. `arcade_version()` and `arcade-sdk --version` both report the version
you have; `CHANGELOG.md` says what each one changed. Keep the library, header,
protos and `arcade-sdk` from the same zip.

## 1. What you are building

A build of your game that an external agent can play as a research
environment, on a Windows or Linux machine we operate. The agent plays like a
person — it sees frames and presses keys and moves the mouse — but both go
through the SDK, not the screen and the OS:

- **Frames**: every frame, your game hands the SDK the image the agent should
  see (`arcade_submit_frame`) — typically a small render target of its own,
  640×360 is plenty — with a frame index.
- **Input**: every frame, your game asks the SDK what the agent did
  (`arcade_poll_input`): which keys and mouse buttons are held, how far the
  mouse and wheel moved, and the ordered key/button/mouse events that got
  there. Feed it to your input code the way you feed the OS's.
- **Instances**: one process of your game can host several independent
  copies of the world (`arcade_instance_count()`, one by default), each with
  its own frames, input and challenges — so one machine runs several agents.

What you add on top of that is structure:

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
debug app) connects to. It never calls into your code: you poll it. Everything
about one world — its frames, its input, its challenges, its reports and
metrics — names the instance it is about, `0..arcade_instance_count()`.

## 2. Checklist

What we need from you, in the end:

1. `vendor.proto`: your RPC service and your event union (section 4).
2. A build of your game with the SDK integrated (sections 5 and 6): frames
   submitted and input taken every frame, for every instance.
3. The list of challenges, designed together with us (section 3).
4. A way to launch straight into a state where a challenge can start: an
   executable plus arguments, no launcher, no login, no menu to click through,
   no modal dialogs.
5. Your frame size, and how many instances one process can host (`InitRequest.max_instances`; 1 is fine to
   start with).
6. Your action map: what each key and mouse input does in your game
   (`InitRequest.action_map`, section 5).
7. No input from the agent takes it out of the game: no pause or main menu,
   no settings, no quit ("Keep the agent in the game", section 5).

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

### Rank a benchmark

Variations multiply: three challenges with a few variations each is already
hundreds of distinct runs, and we cannot play them all on every build. Tell
us which matter with `InitRequest.benchmark`: a list of cases — a challenge
and values for some of its variations, the rest at their defaults — ranked by
how much each one tells about the agent playing, **the most informative
first**.

We play a prefix of it: the top 10 by default, the top 3 or 5 for a quick
check, the top 50 or 100 when a benchmark matters enough to spend the time.
So rank it such that **every prefix is a good benchmark by itself**:

1. **Each challenge once first**, at settings that separate a capable agent
   from a weak one — usually your defaults, or a notch harder.
2. **Then what changes the outcome most**: each variation that makes a
   challenge much harder or different (night, more enemies, a tighter time
   limit, another map), at its telling extremes.
3. **Then combinations** of those.
4. **Last, the fine sweeps** — the steps between the extremes, more maps,
   more colours — which add detail but rarely change the picture.

```textproto
# 1. each challenge once
benchmark { challenge_id: "reach-the-beacon" }
benchmark { challenge_id: "collect-coins" }
# 2. what makes them hard
benchmark {
  challenge_id: "reach-the-beacon"
  variations { name: "night" bool_value: true }
  variations { name: "seconds" int_value: 30 }
}
benchmark {
  challenge_id: "collect-coins"
  variations { name: "count" int_value: 15 }
}
# 4. the sweep
benchmark {
  challenge_id: "reach-the-beacon"
  variations { name: "color" enum_value: "green" }
}
```

`arcade_init` refuses a case that names an unknown challenge or variation, a
value out of range, or two cases that resolve to the same values (each would
measure the same thing twice), and more than 1024 cases. `arcade-sdk
describe` prints the ranking, and the debug app's Challenges page lists it
with a Start per case. The benchmark is optional: without one we play each
challenge once at its defaults, in the order you declared them.

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

Everything is serialized protobuf in and out — except a frame's pixels, which
you hand over as they are — and you own every buffer. The sequence in one
game session:

```c
// Startup — once, after your engine is up.
uint8_t* init = ...; size_t init_len = ...;          // an encoded InitRequest
if (arcade_abi_version() != ARCADE_SDK_ABI_VERSION) { /* wrong header/DLL pair */ }
if (arcade_init(init, init_len) != ARCADE_STATUS_OK) {
    log("arcade_init: %s", arcade_last_error());   // what is wrong with the InitRequest
}
uint32_t n_worlds = arcade_instance_count();       // create this many worlds
uint32_t distance_metric = arcade_metric_handle("distance/travelled_m");

// Every frame, on the game thread.
uint8_t buf[ARCADE_MAX_REQUEST_BYTES]; size_t n;
while (arcade_poll_request(buf, sizeof buf, &n) == ARCADE_STATUS_OK && n > 0) {
    RpcRequest req = decode_RpcRequest(buf, n);
    World* w = &worlds[req.instance];                                   // every RPC is for one instance
    if (eq(req.service, "arcade.sdk.v1.ArcadeChallenges") && eq(req.method, "StartChallenge")) {
        StartChallengeRequest r = decode(req.request);
        start_challenge(w, r.challenge_id, r.variations);                // your game logic
        StartChallengeResponse resp = { .instruction = resolve_instruction(r), .game_time_s = now() };
        arcade_respond(req.request_id, encode(resp));
    } else if (eq(req.service, "acme.roguelike.v1.Game") && eq(req.method, "TeleportPlayer")) {
        TeleportPlayerRequest r = decode(req.request);
        if (!teleport(w, r.position, r.yaw_deg)) arcade_fail(req.request_id, "no such location");
        else arcade_respond(req.request_id, encode((TeleportPlayerResponse){}));
    } else {
        arcade_fail(req.request_id, "unhandled method");
    }
}
for (uint32_t i = 0; i < n_worlds; i++) {
    // What the agent sees: this frame's image of world i.
    ArcadeFrame f = { .frame_index = frame, .game_time_s = now(), .pixels = worlds[i].rgba,
                      .width = 640, .height = 360, .format = ARCADE_PIXEL_FORMAT_RGBA8 };
    arcade_submit_frame(i, &f);
}
for (uint32_t i = 0; i < n_worlds; i++) {
    // What the agent did: its input for world i since the last frame. Returns at once.
    uint8_t in[ARCADE_MAX_INPUT_BYTES]; size_t in_len;
    arcade_poll_input(i, frame, in, sizeof in, &in_len);
    Input input = decode_Input(in, in_len);
    apply_input(&worlds[i], &input);             // keys_down, mouse_dx/dy, ... or input.events
    arcade_push_f32_metric(i, distance_metric, worlds[i].distance, now());
}
simulate_one_frame(worlds, n_worlds);
frame++;

// Whenever something happens — any thread.
arcade_report(i, encode(Report{ .game_time_s = now(), .event = encode(GameEvent{ .item_picked_up = {...} }) }));
arcade_report(i, encode(Report{ .game_time_s = now(), .challenge_completed = { .challenge_id = id, .outcome = OUTCOME_SUCCESS, .score = 1.0 } }));

// Shutdown — before your process exits or unloads the DLL.
arcade_shutdown();
```

`examples/host.c` is the complete version of this loop for one instance.

### The contract

| Rule | Why |
|---|---|
| Call `arcade_poll_request` **once per frame on the game thread**, draining until it returns 0 bytes. | The poll (and `arcade_poll_input`) is the SDK's liveness signal; a frame without one reads as a hang. Your RPC handlers then run on your thread, so game state is safe to touch. `RpcRequest.instance` says which world a request is for. |
| Every frame, for **every instance**: `arcade_submit_frame(i, N)`, then `arcade_poll_input(i, N, …)`, then simulate. | The frame is what the agent sees at frame N; the input is what it did since, applied before the next frame. An instance that stops submitting frames reads as stalled. Neither call blocks. |
| Submit frames at **the size you declared** (`InitRequest.video`), in `RGBA8`, `BGRA8` or `RGB8`, any row stride, either way up. | The SDK converts them; anything else is `ARCADE_STATUS_VALIDATION_ERROR`. It copies the pixels before returning (about 0.3 ms at 640×360) and never blocks. |
| Answer **every** request with `arcade_respond` (the method's response message) or `arcade_fail` (a message). | An unanswered request times out for the caller after `timeout_ms` (30 s by default) and blocks nothing else, but reads as a bug. |
| The response must be **that method's** response type. | The SDK checks it decodes; a wrong type fails the request for its caller with `ARCADE_STATUS_VALIDATION_ERROR`. |
| `arcade_report`, `arcade_push_f32_metric`, `arcade_log` are safe from **any thread**. Reports and metrics name their instance; `arcade_log` (and `arcade_report` with `ARCADE_NO_INSTANCE`) is about the whole process. | They are a channel send. |
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
| `BUFFER_TOO_SMALL` | `arcade_poll_request` / `arcade_poll_input`: `*out_len` holds the size needed; nothing was taken. `ARCADE_MAX_REQUEST_BYTES` / `ARCADE_MAX_INPUT_BYTES` never hit this. |
| `UNKNOWN_REQUEST` | No such pending request (already answered, timed out, or never issued). |
| `UNKNOWN_METHOD` | A caller asked for a service/method your game does not serve. |
| `QUEUE_FULL` | 256 requests are waiting to be polled: the game stopped polling. |
| `SHUTDOWN_INCOMPLETE` | See above. |
| `PANIC` | A bug in the SDK. Send us `arcade_sdk.log`. |
| `INTERNAL` | Anything else; `arcade_last_error()` says what (typically the port is in use). |
| `UNKNOWN_INSTANCE` | The instance is not below `arcade_instance_count()`. |

`arcade_last_error()` returns the details of the last non-OK status **on the
calling thread**, valid until that thread's next SDK call.

### C#

Every parameter is blittable. The SDK's functions are `__cdecl`, which is the
only calling convention on x64 but not .NET's default on 32-bit Windows
(`StdCall`), so every `DllImport` below says `CallingConvention =
CallingConvention.Cdecl`. Keep it when you copy them: a 32-bit game that leaves
it out calls the SDK with the wrong convention and corrupts its stack. In a
64-bit build it changes nothing.

```csharp
using System.Runtime.InteropServices;

static class Arcade {
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern uint arcade_abi_version();
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr arcade_last_error();
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_init(byte[] init, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_shutdown();
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_poll_request(byte[] buf, nuint cap, out nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_respond(ulong requestId, byte[] response, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_fail(ulong requestId, [MarshalAs(UnmanagedType.LPUTF8Str)] string error);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern uint arcade_instance_count();
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_report(uint instance, byte[] report, nuint len);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern uint arcade_metric_handle([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_push_f32_metric(uint instance, uint handle, float value, double gameTimeS);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_submit_frame(uint instance, in ArcadeFrame frame);
    [DllImport("arcade_sdk", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)] public static extern int arcade_poll_input(uint instance, ulong frameIndex, byte[] buf, nuint cap, out nuint len);
    public static string LastError() => Marshal.PtrToStringUTF8(arcade_last_error()) ?? "";
}

// 48 bytes (in a 32-bit build `Pixels` is 4 bytes and the last 4 are padding;
// LayoutKind.Sequential matches the C layout either way). Pin the pixel array
// (or use a native buffer) for the call.
[StructLayout(LayoutKind.Sequential)]
struct ArcadeFrame {
    public ulong FrameIndex;
    public double GameTimeS;
    public IntPtr Pixels;
    public uint Width, Height, Stride, Format, Flags, Reserved;
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
| `video` | **Required.** The width and height of every frame you submit: even, 64..3840 × 64..2160. 640×360 is plenty for today's agents. |
| `max_instances` | The most instances one process can host (0 = 1, at most 64). |
| `action_map` | What each input does in your game: `{"KeyW": "Move forward", "Space": "Jump", "MouseLeft": "Fire", "MouseMove": "Look around"}`. See [Input](#input). |
| `benchmark` | Optional. Runs of your challenges, the most informative first; we play the top of the list. See [Rank a benchmark](#rank-a-benchmark). |

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

### Frames

A frame is what the agent sees of one instance at one moment: an `ArcadeFrame`
naming your pixels, its `frame_index` and your game clock.

- **Size**: exactly `InitRequest.video`. Render the agent's view into a render
  target of that size (or downscale into one); your window, its size and
  whether it is focused do not matter to the agent.
- **Layout**: `ARCADE_PIXEL_FORMAT_RGBA8` (`GL_RGBA`,
  `DXGI_FORMAT_R8G8B8A8_UNORM`), `BGRA8` (`DXGI_FORMAT_B8G8R8A8_UNORM`,
  Unreal's `FColor`) or `RGB8`; `stride` is the bytes from one row to the
  next (0 = tightly packed). Alpha is ignored.
- **Upside down?** `glReadPixels` (and a GL render texture read back in
  Unity) is bottom-up: set `flags = ARCADE_FRAME_FLIP_Y`.
- **Frame index**: any strictly increasing number per instance — your frame
  counter will do.
- Read back without stalling your GPU: a ring of two or three staging
  buffers, submitting the one that finished.

`arcade-sdk describe` writes the first frame it sees as `arcade-frame.png`:
look at it.

### Input

`arcade_poll_input(i, N, …)` gives you an encoded `Input` for instance `i`
after frame `N` — everything that arrived since the previous poll, to apply
before simulating the next frame. It returns at once:

| Field | What it is |
|---|---|
| `keys_down` | Every key held, by position: `KEY_CODE_KEY_W` is where US layouts have W, whatever the player's layout prints on it (the W3C `KeyboardEvent.code` names). |
| `buttons_down` | Mouse buttons held. |
| `mouse_dx`, `mouse_dy` | Relative motion since the last poll, in raw counts like `WM_INPUT`/`XI_RawMotion`: +x right, +y down. Apply your look sensitivity as you do to the OS's. |
| `wheel_dx`, `wheel_dy` | Wheel notches; +y is away from the user (scroll up). |
| `events` | The key/button/move/wheel events that got there, in order — for input code that consumes events rather than state. |
| `driven` | Whether an agent drives the instance. Undriven, nothing is held. |

The agent only sends what a keyboard and mouse can do; there is no text
input and no absolute cursor. When the agent stops driving an instance (its
attempt ended, it disconnected), you get the releases for everything it held.
Do not read the OS's keyboard or mouse for an instance under the agent: its
input is this.

Tell us what the inputs do with `InitRequest.action_map`, one entry per input
your game responds to: this is how the agent learns your controls.

| Key | Names |
|---|---|
| A key | Its W3C `KeyboardEvent.code`, the `KeyCode` name without its prefix in CamelCase: `KeyW`, `Digit1`, `Space`, `ShiftLeft`, `ControlLeft`, `ArrowUp`, `F5`. |
| A mouse button | `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, `MouseForward`. |
| The mouse | `MouseMove` for its motion, `MouseWheel` for the wheel. |

Each value says what the input does in a few words (`"Crouch"`, `"Open
inventory"`): one line, at most 128 bytes. Several inputs may do the same
thing: map both `KeyW` and `ArrowUp` to `"Move forward"`. An input you leave
out is one the agent is told does nothing, so list them all.
`arcade_init` refuses a key it does not know (`"W"` is not one; `"KeyW"` is)
and names it in the error. The debug app's Play page shows the map as your
game's controls.

#### Keep the agent in the game

The agent's input is **gameplay input only**. Nothing it sends may take an
instance out of play: no pause menu, main menu, options or key-binding
screens, no quit or "return to title", no save/load screens, no console, no
fullscreen or window toggles, no screenshots. Out of play the agent sees a
menu instead of the world, the challenge stalls (or ends without reporting),
and the attempt is lost. Leaving the game is for us to do, through
`StopChallenge`, never for the agent.

Menus that are part of play are fine: an inventory, a map, a crafting or
dialogue screen, as long as the agent can get back into play from them with
inputs in your action map, and closing them is listed too.

This is your game's job, not the SDK's: the SDK hands you every key the
agent presses, and only your game knows what a key does at that moment.
Filter in your input code, for the input you take from `arcade_poll_input`:

- **An input that only reaches a system screen** (`F10` opens options,
  `Backquote` opens the console): leave it out of the action map and ignore
  it.
- **An input that does both** — `Escape` cancels the spell being cast, and
  otherwise opens the pause menu: list it in the action map by what it does
  in play (`"Escape": "Cancel spell"`), and apply only that. While a spell is
  being cast, `Escape` cancels it; otherwise it does nothing. It must never
  open the pause menu for an instance under the agent.

The usual way is one switch at the point where your game turns a key into an
action: for input that came from the SDK, drop the system actions, keep the
gameplay ones. Your own keyboard (and the OS's) is not affected, so you can
still reach your menus while you develop.

### Instances

`arcade_instance_count()` (after `arcade_init`) says how many worlds to run:
1 unless we launch you with more (`ARCADE_SDK_INSTANCES`, never more than your
`max_instances`). Each instance is an independent copy of the game — its own
world, player, challenge, frames and input — in the same process: share what
can be shared (assets, the renderer), not state. Every RPC carries its
`instance`, and every report, metric, frame and input poll names one.

## 6. Implement `ArcadeChallenges`

| Method | What your game must do |
|---|---|
| `StartChallenge(StartChallengeRequest)` | Reset the instance's world (`RpcRequest.instance`) and put its player at the start of `challenge_id`, applying every variation in `variations`. Seed randomness from `seed` so equal seeds reproduce. Return `StartChallengeResponse { instruction, game_time_s }` — the instruction template with the values substituted. The player must be able to act within a few seconds of the response. Also send a `ChallengeStarted` report with `run_id` echoed. |
| `StopChallenge(StopChallengeRequest)` | Abort the instance's running challenge (if any) and return it to an idle, ready state. |
| `Ping(PingRequest)` | Return `PingResponse { game_time_s }` from the game loop. |

When the game has judged the attempt, send **one** `ChallengeCompleted` report
for that instance, immediately, with `challenge_id`, the `run_id` from the start request, the
`outcome`, an optional `score`, a human-readable `detail`, and any
per-attempt `final_metrics`.

## 7. Test with the debug app

1. Put `arcade_sdk.dll` and `arcade-sdk.exe` (Linux: `libarcade_sdk.so`
   and `arcade-sdk`) next to your executable.
2. Run the game. When `arcade_init` succeeds the SDK launches the debug app
   (`arcade-sdk debug`), which opens a browser page. (Or run
   `arcade-sdk debug` yourself; `--sdk-addr 127.0.0.1:<port>` if you changed
   `ARCADE_SDK_PORT`.)
3. **Play**: see each instance's frames and play it with your own keyboard and
   mouse — click the picture to capture the mouse, Esc to let go. This is
   exactly what the agent sees and sends: if you can play it here, the agent
   can too. Press every key, in and out of the situations where it does
   something, and check none takes you out of the game — Esc included, with
   the **Send Esc** button (the Esc key itself only frees the mouse).
4. **Challenges**: start each challenge with chosen variation values and watch
   the game do it; the resolved instruction comes back on the card.
5. **Services**: call any of your RPCs from a form generated from your `.proto`,
   and see the typed response.
6. **Live**: the SDK's liveness tick, each instance's frames and driver, every
   metric with a sparkline, and the report stream — challenge events, your
   typed events, log lines.

With more than one instance, pick the instance at the top of the page.

The SDK writes `arcade_sdk.log` next to the DLL. Environment variables:

| Variable | Effect |
|---|---|
| `ARCADE_SDK_PORT` | The SDK's loopback port (default 6006). |
| `ARCADE_SDK_NO_DEBUG_UI=1` | Do not launch the debug app. |
| `ARCADE_SDK_LOG` | Log filter, e.g. `debug` (default `info`). |
| `ARCADE_SDK_LOG_DIR` | Where to write `arcade_sdk.log`. |
| `ARCADE_SDK_INSTANCES` | How many instances to host (default 1; set by our machines). |
| `SCREEN_WIDTH`, `SCREEN_HEIGHT` | Set by our machines: the display size, if you open a window at all. |

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

**Linux x86-64 builds**, and **Windows x86-64 builds under Proton** (below),
clients and their dedicated servers alike. The build runs in a container with an X11 display (`$DISPLAY`), an NVIDIA GPU (Vulkan and
OpenGL), PulseAudio, and no network beyond loopback, as an unprivileged user.
So:

- The build as a folder: your executable, its data, and `libarcade_sdk.so`
  beside it. No installer, no auto-updater, no online login; it must run
  offline. Follow-symlinks are fine; we package what they point at.
- It may open a window on `$DISPLAY` (at most `SCREEN_WIDTH` × `SCREEN_HEIGHT`)
  or render offscreen; the agent sees only the frames you submit. It needs no
  `sudo`, no `/dev/input`, no Wayland.
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

#### Windows builds, under Proton

Our machines are all Linux, so a Windows build runs on them under
[Proton](https://github.com/GloriousEggroll/proton-ge-custom) (GE-Proton 11:
Wine, with DXVK and VKD3D-Proton translating Direct3D 9–12 to Vulkan). Say
so in `arcade.toml`: `platform = "windows-x86_64"` and `proton = true` in
`[client]`, and the same in `[server]` for a Windows server. `arcade-sdk
init` writes both for a `.exe` entrypoint; `submit` refuses a Windows build
without `proton = true`, since nothing else would run it.

Set it once the build runs under Proton: on a Linux machine or a Steam Deck,
add the `.exe` to Steam as a non-Steam game and force a Proton version in its
properties, or start it with [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher)
(`GAMEID=0 umu-run Acme.exe --arcade`). Everything above holds, and:

- The entrypoint is a `.exe`, `.bat` or `.cmd` (Wine has no PowerShell), and
  `arcade_sdk.dll` ships in the folder instead of `libarcade_sdk.so`.
- The first launch creates a fresh Wine prefix — a clean Windows with no
  installer ever run in it — kept for later launches of the same build.
  Ship whatever runtime DLLs the game needs beside it (the Visual C++ runtime,
  say); do not rely on a redistributable being installed.
- The environment variables in the table above reach the game as usual, and
  the working directory is as `arcade.toml` gives it. The machine's
  filesystem is drive `Z:`; stick to paths relative to your folder.
- No anti-cheat and no DRM: they rarely work under Proton, and nothing here
  signs in anyway.
- `arcade-sdk describe` launches the build where you run it, so run it on
  your Windows desk (natively — `proton` only matters on our machines).

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
every key commented, the platform taken from the entrypoint (a `.exe` is a
Windows build, set to run under Proton). Every flag it asks about can also be passed
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

A Windows build instead (a Windows server likewise):

```toml
[client]                    # ships arcade_sdk.dll
dir = "out/win64"
entrypoint = "Acme.exe"
args = ["-arcade", "-nointro"]
platform = "windows-x86_64"
proton = true               # we run it under Proton, on Linux
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
the registration, checks the game has started polling and submitting frames
(writing the first one as `arcade-frame.png` beside `arcade.toml`), and
stops both.
`--instances N` runs it with N instances. It prints what it found — every
challenge with its variations and defaults, your benchmark's ranking, every
metric, your action map,
your frame size and instances, your services and event kinds — and writes
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

### Step 4: wait for the verdict

Every build you submit is run once on arcade, on its own, to check that it
works there: that it installs and launches, declares the challenges it
registered at submit, and that every one of them starts and reports an outcome
(any outcome: a `FAILURE` or `TIMEOUT` still shows the challenge works; what
fails the check is a challenge that never reports). It usually takes a few
hours.

```bash
arcade-sdk builds status acme_shooter@1.4.0 --wait
```

prints the verdict, and with `--wait` checks every minute until it is in:

- **passed**: it works on arcade.
- **failed**, with the first thing that went wrong: the build never became
  ready (it crashed or hung at start-up; relaunching it on your side with the
  same `exe` and `args` is the first thing to try), it declared other
  challenges than its registration (run `describe` again and submit under a new
  build id), or a challenge never reported how it ended before its time limit.
  Fix it and submit a new build id.
- **inconclusive**: our infrastructure failed every attempt; nothing about
  your build. We look into these.

`builds status` exits non-zero when the build failed, so CI can gate on it.
The portal's build list shows the same verdict.

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
- **The frame is upside down**: set `ARCADE_FRAME_FLIP_Y` (GL readbacks are
  bottom-up). **Red and blue swapped**: it is `BGRA8`, not `RGBA8`.
- **Pausing when unfocused**: don't. The agent's input comes through the SDK,
  not the window; a game that pauses on focus loss stalls the attempt.
- **An ABI 1 build**: builds from before frames and input went through the SDK
  are refused. Rebuild against this SDK, `describe` and submit a new build id.
- **Modal dialogs**: any dialog that needs a click to dismiss (crash reporter,
  "press any key", EULA) stops everything. Disable them in the build you ship.
- **Two games on one machine**: set `ARCADE_SDK_PORT` differently; the SDK
  refuses to start if its port is taken rather than silently picking another.
