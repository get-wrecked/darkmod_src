# Changelog

What changed in each release of the arcade game SDK: the library
(`arcade_sdk.dll` / `libarcade_sdk.so`), its header and protos, and the
`arcade-sdk` command line. It ships in the SDK zip beside the integration
guide (`README.md`).

## Versions

The SDK follows [semantic versioning](https://semver.org). One version covers
everything in the zip; `arcade_version()` and `arcade-sdk --version` both
report it, followed by the commit it was built from (`1.2.0+0123456789ab`).

- **Major** (`2.0.0`): you have to change your integration or rebuild your
  game. A new C ABI (`ARCADE_SDK_ABI_VERSION`) is always a major release, and
  builds made against the previous ABI are no longer accepted. Breaking
  changes to the protos, `arcade.toml` or the command line are major too.
- **Minor** (`1.1.0`): something new you can use — an optional field, a new
  command or flag, a debug app feature. A game built against `1.0.0` keeps
  working unchanged.
- **Patch** (`1.0.1`): fixes, nothing to change on your side.

Each release names its ABI. Within a major version it never changes, so a
newer `1.x` library works with a game compiled against an older `1.x`
header; the reverse is not promised, since a newer header may declare
functions an older library lacks. Ship the library, header and protos from
one zip, and use the `arcade-sdk` from that zip too.

## Unreleased

- **Timestamped sound and video.** Declare optional `InitRequest.audio`
  (8000..192000 Hz, mono/stereo), submit F32/S16 PCM through
  `arcade_submit_audio_at(instance, &audio, first_sample_ns)`, and submit
  frames with their original capture timestamp through `arcade_submit_frame_at`.
  Map the engine's DSP/render clocks to the shared `arcade_time_ns()` clock;
  preserve times across mixer buffering, GPU readback and worker queues.
  `ARCADE_AUDIO_DISCONTINUITY` marks resets/loss, and omitted silence remains
  a real gap. Older calls remain approximate: untimed audio counts samples
  after its first arrival. Send zeros through silence or set the discontinuity
  flag on resumed PCM, and keep that flag set after any non-OK status until
  the next accepted submission. The structs, wire messages and ABI version
  are unchanged.
- **Bounded audio ingress.** Audio submission copies into a preallocated SPSC ring
  without allocating, logging, network I/O or waiting on a lock. Full/busy
  ingress rejects the whole buffer with `QUEUE_FULL`; keep advancing source
  time and do not retry inside the callback. Calls are limited to both
  `ARCADE_MAX_AUDIO_FRAMES` and 100 ms (`ARCADE_MAX_AUDIO_MS`), with larger
  buffers split by the adapter. Draining never holds a lock shared with
  the producer, preventing drain-induced empty-queue callback loss. SDK
  lifecycle lookup can still reject on its nonwaiting try-lock. Stop/join
  callbacks before shutdown.
- **Policy conversion and recordings.** Filtering now scales with the
  input/output ratio, retaining stopband rejection for 96/192 kHz input.
  A bounded cache of normalized rational-phase taps reduces per-sample work
  at common mixer rates while preserving the interpolation fallback.
  The documented passband extends to 80% of the lower Nyquist rate; equal
  rates bypass filtering. Policy packets are at most 10 ms, stale backlog
  expires after 200 ms even during silence, and discontinuities retain source
  timestamps. A 50 ms source-time lateness deadline delivers held short cues
  on open streams; invalid or far-future packets cannot poison the timeline.
  Recordings align PCM to video source intervals with one shared playout
  delay, preserving silence and discarding stale packets after stalls or
  episode cuts. `describe` and the debug app's Sound button also expose the
  submitted audio. Without `InitRequest.audio`, the agent remains silent.

- **Rank a benchmark for your build.** A new optional field,
  `InitRequest.benchmark`, lists runs of your challenges — a challenge and
  values for some of its variations — ranked by how much each tells about the
  agent, the most informative first. We play the top of that list: the first
  10 cases by default, a handful for a quick check, 50 or 100 when a
  benchmark matters. So order it such that every prefix is a good benchmark on its
  own; "Rank a benchmark" in the guide shows how. `arcade_init` checks every
  case (a declared challenge, declared variations, values in range, no two
  cases alike); `arcade-sdk describe` prints the ranking and the debug app
  lists it with a Start per case. Build verification plays all of it — every
  case, and each challenge it leaves out once at its defaults — and every case
  must start and report an outcome. Without a benchmark nothing changes: runs
  and verification play each challenge once at its defaults.

- **32-bit Windows games are supported.** A new zip,
  `arcade_sdk-windows-x86-<version>.zip`, carries a 32-bit `arcade_sdk.dll`,
  `arcade_sdk.lib` and `arcade_sdk.pdb`; use it for an x86 build of your game.
  The header, protos and `arcade-sdk.exe` (still 64-bit) are the same as in the
  x64 zip. Calling from C# in a 32-bit build, add `CallingConvention =
  CallingConvention.Cdecl` to every `DllImport`; the guide's C# example now
  declares it on each one.

## 2.2.1 — 2026-10-01 (ABI 2)

- **Keep the agent in the game.** The guide now spells out that the agent's
  input is gameplay input only: nothing it sends may open the pause or main
  menu, settings or the console, or quit. Ignore inputs that only do that,
  and for one that does both — `Escape` cancels a spell, and otherwise
  pauses — map it by its gameplay meaning and apply only that. See "Keep the
  agent in the game" in the guide. Nothing changes in the library; check
  your build against it.

- **Windows builds run under Proton.** Under Wine, `arcade_init` failed with
  `bind the SDK's QUIC endpoint on 127.0.0.1:6006: OS Error 10045`, so a
  Windows build submitted with `proton = true` never became ready on arcade.
  The library now falls back to a plain UDP socket when the network stack
  refuses the socket options it normally sets. Nothing changes natively.
  Rebuild with this library and resubmit your build.

## 2.2.0 — 2026-09-30 (ABI 2)

- **Build verification.** Every build you submit is now run once on arcade to
  check that it launches, declares the challenges it registered, and reports an
  outcome for each. `arcade-sdk builds status <game_id>@<build_id>` prints the
  verdict (`passed`, `failed` or `inconclusive`, with why) and exits non-zero
  when the build failed; `--wait` checks every minute until it is in.
  `builds show` notes it too, `submit` says how to follow it, and the portal's
  build list has a Verification column. See "Step 4: wait for the verdict" in
  the guide.

## 2.1.0 — 2026-09-30 (ABI 2)

- **Windows builds, under Proton.** A Windows build can be submitted: set
  `platform = "windows-x86_64"` and `proton = true` in its `[client]` (and
  `[server]`) in `arcade.toml`, and we run it under Proton on our Linux
  machines. `arcade-sdk init` writes both for a `.exe` entrypoint; `submit`
  refuses a Windows build without `proton = true`. See "Windows builds, under
  Proton" in the guide.
- **Action map.** Say what each input does in your game with
  `InitRequest.action_map` (optional): `{"KeyW": "Move forward", "Space":
  "Jump", "MouseLeft": "Fire", "MouseMove": "Look around"}`, keys by their
  W3C `KeyboardEvent.code`, mouse buttons as `MouseLeft` … `MouseForward`,
  and `MouseMove` / `MouseWheel`. It is how the agent learns your controls.
  `arcade_init` refuses an unknown input or an empty action;
  `arcade-sdk describe` prints the map, `arcade-sdk submit` warns when there
  is none, and the debug app's Play page lists it as your game's controls.

## 2.0.0 — 2026-09-30 (ABI 2)

- **ABI 2 — rebuild your game.** The agent now sees the frames you hand the
  SDK and plays through the input the SDK hands you, instead of a screen
  capture and OS input. Builds made against ABI 1 are no longer accepted:
  rebuild against this SDK, run `arcade-sdk describe` and submit again.
  - `InitRequest.video` (required): the size of the frames you submit.
  - `arcade_submit_frame(instance, &ArcadeFrame)` every frame: RGBA8, BGRA8
    or RGB8 at any stride, optionally bottom-up (`ARCADE_FRAME_FLIP_Y`),
    with a strictly increasing frame index.
  - `arcade_poll_input(instance, frame_index, …)` every frame, after
    submitting it: an `Input` with the keys and buttons held, the mouse and
    wheel motion, and the ordered edges since the last poll. It never blocks.
  - Every per-world call names an instance: `arcade_report` and
    `arcade_push_f32_metric` take one first, and `RpcRequest.instance` says
    which instance a call is for.
- **Several instances per process.** Declare how many copies of your world
  one process can host (`InitRequest.max_instances`, default 1, at most 64);
  we launch it with `ARCADE_SDK_INSTANCES` set to how many to host (1 when
  unset), and `arcade_instance_count()` says how many that is. Each instance
  is its own world with its own agent: challenges, reports, metrics, frames
  and input are all per instance.
- `arcade-sdk describe` checks that the game submits frames and writes the
  first one to `arcade-frame.png` beside `arcade.toml`; `--instances N` runs
  the build with N instances. `arcade-sdk submit` refuses a registration
  without a frame size.
- The debug app has a Play page: your frames live, and your keyboard and
  mouse sent to the game (click the picture to capture them). With several
  instances, pick one at the top of the page; Challenges can start on all.
- The Windows zip is named `arcade_sdk-windows-<version>.zip`, like the
  Linux one (`arcade_sdk-linux-<version>.zip`). 1.0.0's was
  `arcade_sdk-1.0.0.zip`.

## 1.0.0 — 2026-09-30 (ABI 1)

The first versioned release. Earlier zips were named by commit only and
reported `0.1.0` as their version.

- The C ABI: `arcade_init` with an `InitRequest` declaring your game,
  challenges, metrics, the RPC services you serve and your world's
  `coordinate_system` (required); `arcade_poll_request`,
  `arcade_respond` and `arcade_fail` to serve RPCs; `arcade_report`,
  `arcade_metric_handle`, `arcade_push_f32_metric` and `arcade_log` for
  outcomes, events, metrics and log lines; `arcade_shutdown`;
  `arcade_abi_version`, `arcade_version` and `arcade_last_error`.
- `proto/arcade_sdk.proto` (the messages and the `ArcadeChallenges` service)
  and `proto/arcade_common.proto` (vectors, quaternions, transforms, colours).
- `arcade-sdk debug`: the debug app, launched by the library when
  `arcade-sdk` sits beside it — challenges, typed RPC forms from your
  `.proto`, live metrics and events.
- Build submission: `arcade-sdk login` with your vendor account, `init` to
  write `arcade.toml` (client, and an optional dedicated server), `describe`
  to record what the build declares, `submit` to upload it, `builds show` and
  `whoami`.
