# OBS Dynamic Delay

Native OBS Studio plugin that lets you **add, cancel, change, or remove delay without stopping the stream or recording**. It operates on the already-compressed packets that OBS sends to the output, so its steady-state cost is primarily RAM rather than permanent secondary encoding.

Version: `1.1.2`<br>
Reference compatibility: OBS Studio `32.2.x`, Qt 6, macOS 13+, Windows 10/11 x64, and Ubuntu 24.04 x86_64.

Version 1.1.2 fixes cancel/rearm deadlocks, progress during recording pauses, reconnect timeline resets, and
persistent A/V skew after encoder gaps on **macOS, Windows, and Linux**. It also reduces GPU readback and compatible
auxiliary-encoder work without adding permanent program re-encoding. Download the
[official 1.1.2 assets and checksums](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2).

## What it does

- Controls active streaming and recording outputs independently, including when both are running at the same time.
- Lets you select 1 to 300 seconds with a slider and numeric field.
- Activates, cancels, or removes delay without restarting the output.
- Preserves normal OBS operation: scenes can be changed and Studio can be operated while delay is active.
- Shows a selected scene while building the initial buffer.
- Calculates expected RAM usage from the observed real bitrate or the configured bitrate.
- Includes a collapsible audience preview, downscaled on the GPU to 320×180 before readback at up to 2 fps.
- Plays audio during the hold scene through a private mixer; it includes scene, dedicated source, reserved track,
  and explicit silence modes.
- Preserves the output track layout and performs video splices only on keyframes.
- Rearms each streaming output separately after a reconnection and returns to a safe state if it detects an unsupported format or a RAM limit.

## Exact workflow

1. The user selects a duration and hold scene, then clicks **Add delay**.
2. The program remains live while compatible auxiliary encoders are prepared, audio is validated, and a safe
   keyframe is awaited.
3. On that keyframe, the output switches to the hold scene with the selected audio mode, and normal content
   starts being stored as compressed packets.
4. Once the configured duration has been filled, the output starts sending that buffer with the selected delay. The auxiliary encoder shuts down.
5. When **Remove delay / cancel** is pressed, the plugin continues providing valid output until the next live keyframe and then returns to live, without stopping streaming or recording.

If it is canceled during preparation or filling, the plugin returns to the live signal in the same way. Changing the duration or scene while delay is active automatically rearms with the new configuration; the output remains active.

Recording pauses stop media-clock filling progress and pause the private auxiliary output. Resuming continues the
same buffer instead of counting the paused wall-clock interval as new content. A streaming reconnect clears the
previous packet queues and timestamp bridge before the new epoch's first audio or video packet, then rearms.

If an encoder loses frames or an audio track develops a gap, the plugin can realign all active audio tracks and
video at a complete buffered GOP. This recovery may shorten the effective delay; it does not decode/re-encode the
program, splice into dependent P/B frames, or show another hold scene. The configured slider and the actual emitted
frame are distinct: the preview follows the emitted capture timestamp, not an unapplied slider value.

## Hold scene audio

The default mode is **Scene mix (recommended)**. Libobs renders the hold scene in a private view;
the plugin copies its already-mixed PCM into a bounded SPSC FIFO and feeds it to a private audio clock. It does not assign,
mute, or modify OBS global sources, tracks, or buses.

- **Scene mix:** preserves the source track routing from the scene. This is the normal option.
- **Dedicated source:** uses a single audio source exclusively for the hold. It preserves that source's track assignment.
- **Reserved OBS track (advanced):** takes the mix from a reserved OBS track 1-6 and replicates it across the
  audio tracks of the hold output. That track cannot be encoded by any active output.
- **Silence:** intentional silence, useful as an explicit policy or for diagnosing configurations.

Before every activation, a conservative *preflight* traverses scenes, groups, hidden items,
canvases, sources, and active outputs. If `Scene mix` shares an audio source with another scene or with Program,
the configured dedicated source is used when it is exclusive; if that is not safe either, only the hold audio falls
back to silence. Video and delay keep working. If the topology changes during filling, the degradation to
silence remains fixed until the next activation to avoid duplicating audio in Program.

To configure **Reserved OBS track**:

1. In **Advanced Audio Properties**, assign only the hold-scene sources to the selected track.
2. Remove that track from every other source.
3. Do not select that track for streaming, recording, Replay Buffer, Virtual Camera, or another active output/plugin.
4. Select the same track in the dock. The diagnostic must confirm that it is exclusive before activating delay.

## Installation

### macOS

1. Close OBS.
2. Copy `obs-dynamic-delay.plugin` to:

   ```text
   ~/Library/Application Support/obs-studio/plugins/
   ```

3. Open OBS and enable **Docks → Dynamic Delay**.

Use the universal ZIP as the recommended download. Its plugin bundle is signed *ad hoc*. The alternative universal
PKG has no Developer ID signature and is not Apple-notarized; macOS may require explicit authorization. Verify the
download with [SHA256SUMS.txt](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/SHA256SUMS.txt).

### Windows x64

1. Close OBS.
2. Open this path in the File Explorer address bar (the `ProgramData` folder is normally hidden).
   If `obs-studio\plugins` does not exist, create it; Windows may request administrator permissions:

   ```text
   %PROGRAMDATA%\obs-studio\plugins\
   ```

3. Extract **the complete `obs-dynamic-delay` folder** from the ZIP there. Do not copy loose `bin` and `data`
   folders into the OBS installation directory, and do not use `%APPDATA%`.
4. Verify that the DLL ends up at exactly:

   ```text
   %PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll
   ```

   You can also verify it from PowerShell:

   ```powershell
   Test-Path "$env:ProgramData\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll"
   ```

   The result must be `True`.
5. Open OBS and enable **Docks → Dynamic Delay**.

These paths apply to a normal, non-portable OBS installation. If the dock does not appear, open
**Help → Log Files → View Current Log** and verify that `obs-dynamic-delay.dll` appears among the loaded
modules.

If the dock displays **ERROR / LIVE FALLBACK**, first read the small text below the status: it contains
the exact cause. The same reason appears in the log with the `[obs-dynamic-delay]` prefix. When requesting help,
attach the log generated after reproducing the failure; logs are stored in `%APPDATA%\obs-studio\logs\`.

The included DLL is not Authenticode-signed. Public distribution signing requires a publisher code-signing certificate.

### Ubuntu 24.04 x86_64

The Debian package is the recommended option for a native installation. The release targets the OBS Studio
32.2.x module ABI on Ubuntu 24.04 x86_64, not an arbitrary newer OBS ABI. Install a compatible OBS 32.2.x build;
the official OBS PPA is one source, but check its offered version before installation:

```bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.2-x86_64-linux-gnu.deb
```

As an alternative archive, the `tar.xz` contains the standard `lib/share` layout for `/usr`:

```bash
sudo tar -xJf obs-dynamic-delay-1.1.2-x86_64-ubuntu-gnu.tar.xz -C /usr
```

Restart OBS and enable **Docks → Dynamic Delay**. These two artifacts are built for native OBS on
Ubuntu 24.04 x86_64; they are not presented as packages for Flatpak, Snap, or other distributions.

## Usage and performance

The memory estimate uses `total bitrate × seconds ÷ 8`, with a 20% margin. With CQP, CRF, ICQ, lossless, or a track
without a fixed bitrate, the dock measures the real value after a two-second output window. Opening the audience
preview adds RGB16 history: a normal 300-second history costs approximately 67 MiB. The history also retains the
currently effective delay and required pre-pause frames, with a bounded frame count. The main packet buffer has a
4 GiB safety limit and the auxiliary preroll has a 128 MiB limit.

During **Preparing/Filling**, a second video encoder is created with the same configuration as the output. This is necessary to produce the hold scene with compatible headers. It is destroyed upon entering **Delay active**. For the lowest impact while gaming:

- use a hardware encoder;
- keep the audience preview closed when it is not needed;
- avoid running streaming and recording with different configurations if both outputs are not needed;
- configure a reasonable keyframe interval (for example, 2 seconds), because every entry and exit waits for a safe keyframe.

Simultaneous outputs can share the hold-scene view, clock, and PCM bridge. Compatible outputs also
reuse one complete auxiliary encoding bundle only when the original video encoder, every audio encoder and track
position, and hold-media hub are identical. Matching the video alone is insufficient: partial matches retain
private auxiliary encoders. Fully matching outputs already share the same original encoders; auxiliary pause
follows their shared encoder state, including when recording is pausable. Stopping one subscriber does not destroy
the bundle while another still needs it.

The PCM bridge is fixed and bounded: 16 blocks × 1024 frames × 6 mixes × 8 channels, approximately 3 MiB per
activation, independent of delay duration. Explicit **Silence** does not allocate this FIFO. Redundant PCM clearing
and copying have been removed while retaining the same routing and silence behavior. The internal lookahead is
one OBS quantum, approximately 21.3 ms at 48 kHz.

The preview reuses OBS's rendered Program texture: it downsizes to 320×180 on the GPU and reads back at most twice
per second, mapping the transfer on the following video frame. It does not register a raw video consumer, force
full-resolution per-frame readback, or keep `obs_video_active` true by itself. Folding the preview frees capture,
GPU resources, and history. Hiding the entire dock retains low-rate history for reopening but stops repainting.
During a paused recording, capture is suspended and required history is retained for resuming. When streaming and
recording coexist, the preview follows streaming; otherwise it follows the recording. Server/CDN/player latency is
not included.

## Deliberate limitations of version 1.1.2

- **Transition:** the production mode is `Cut on keyframe`. The fade option is shown disabled because a real crossfade between already-delayed and live video requires decoding, compositing, and re-encoding the entire signal. That conflicts with the low-resource objective and can degrade image quality and latency.
- **Audio splice:** switching between the main and auxiliary encoder can introduce a very brief gap of
  one or two AAC frames. The several seconds of silence during filling no longer occur.
- **Codec alignment:** B-frame reordering and AAC packet boundaries can add a small A/V offset at a splice.
  Splices are not sample-exact; Program is not decoded/re-encoded to remove the offset.
- **Native OBS pause issue:** OBS 32.2.2 can stall after recording pause with x264 and FPS divisor 2, also without
  this plugin. Use divisor 1 when recording pause is needed; the plugin does not patch OBS internals.
- **Audio safety:** a shared source or a reserved track that stops being exclusive causes silence
  only in the hold scene. The plugin does not modify global routing in an attempt to correct it automatically.
- **Removing delay:** the return is immediate in control terms, but the actual splice waits for the next keyframe to keep the stream decodable. The usual maximum is the configured GOP interval.
- **Audience preview:** approximately represents the frames sent by OBS. It does not include the server, CDN, or viewer-player buffer. During the hold scene, it shows its status instead of maintaining another auxiliary render.
- Encoded outputs with audio and video are supported. Normal Hybrid MP4/MOV and RTMP outputs are compatible when
  they have exactly one active video track. Raw, audio-only, video-only outputs, OBS native delay, and layouts
  with multiple video tracks such as `Enhanced Broadcasting` are not supported.
- Safe recovery from encoder gaps may shorten the effective delay to an aligned GOP rather than preserve an
  incorrect A/V offset. Exact delay cannot be guaranteed across missing source packets.
- Codec/driver runtime compatibility requires real OBS testing; passing the SDK-free suites alone does not certify
  a complete Windows, macOS, or Linux recording workflow.

## Architecture

Each output's synchronous packet callback acts as a pacing carrier. The plugin preserves its wall timestamps, track, encoder, and timebase, and replaces only the payload with the corresponding packet from the buffer. Each source switch creates a new temporal epoch and applies a common A/V bridge when needed, preventing non-monotonic PTS with encoders that use B-frames.

The auxiliary audio does not use the Program bus. A private `COMPOSITE` source participates in the private view's audio
render tree without being registered as a global audio source. The libobs producer writes fixed PCM blocks
to an SPSC FIFO; the consumer, running on the private clock, applies a single phase/sync correction and
then advances by frames to prevent drift. If the backend stops being safe, an atomic kill switch clears the
audio enumeration without dismantling the video view. Observers use RAII connections to the `signal_handler`,
so they do not keep scenes or sources created by other plugins alive.

The state per output is:

```text
LIVE → PREPARING → FILLING → DELAY ACTIVE → RETURNING LIVE → LIVE
                    ↘ cancellation ────────────────↗
```

The production state machine is `src/output-session.cpp`; tests compile that same file and exercise its registered
packet callback with controlled libobs and auxiliary-encoder boundaries. The separate packet-delay model in
`src/core/packet_delay.cpp` is not linked into the plugin and its tests do not establish production behavior.
`src/core/preview-timing.hpp` and the PCM FIFO, in contrast, are helpers used by production code and tested directly.

OBS APIs used as a contract:

- [Output packet callbacks](https://docs.obsproject.com/reference-outputs#c.obs_output_add_packet_callback)
- [Outputs and native delay](https://docs.obsproject.com/reference-outputs#c.obs_output_set_delay)
- [Encoders](https://docs.obsproject.com/reference-encoders)
- [Frontend API](https://docs.obsproject.com/reference-frontend-api)
- [Displays and views](https://docs.obsproject.com/frontends#displays)

## Building

The project is based on the official `obs-plugintemplate` system; it downloads and pins by hash the OBS 32.2.2 sources, `obs-deps`, and Qt 6.11.1 defined in `buildspec.json`.

### Local macOS arm64

Requires OBS 32.2.2 installed at `/Applications/OBS.app`, Xcode Command Line Tools, CMake, and Ninja:

```bash
cmake --preset macos-local-arm64 -DENABLE_TESTS=ON
cmake --build --preset macos-local-arm64 --parallel
ctest --test-dir build_macos_arm64 --output-on-failure
```

### Universal macOS

Requires the full Xcode 16 application—not just Command Line Tools—and CMake 3.28 or later:

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo --parallel
ctest --test-dir build_macos --build-config RelWithDebInfo --output-on-failure
```

### Windows x64

From a Visual Studio 2022 Developer PowerShell with CMake 3.28+:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
ctest --test-dir build_x64 --build-config RelWithDebInfo --output-on-failure
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

### Ubuntu 24.04 x86_64

Requires CMake 3.28+, Ninja, GCC, Qt 6, and a native OBS Studio 32.2.x installation with its development files:

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --parallel
ctest --test-dir build_x86_64 --output-on-failure
cmake --install build_x86_64 --prefix release/RelWithDebInfo
```

The included workflows build and test universal macOS, Windows x64, and Ubuntu 24.04 x86_64 on GitHub
Actions. Releases include the universal macOS ZIP and PKG, Windows ZIP, Linux archive and Debian package, debug
symbols, a source archive, and `SHA256SUMS.txt`. The release tag must match the version in `buildspec.json`; prerelease suffixes
such as `-beta2` or `-rc1` are allowed.

## Validation for 1.1.2

Five SDK-free suites pass on macOS with warnings as errors, ASan+UBSan, and TSan:

- **Production OutputSession cases:** real packet callbacks, reference ownership, cancel/rearm teardown,
  paused media-clock filling, audio-first/video-first reconnects, B-frame composition timestamps, video and
  individual audio-track gaps during Filling and Delayed, multi-track alignment, RAM limits, and safe fallback.
- **Production HoldPipeline cases:** full-layout sharing, isolated encoders, late subscribers, coordinated pause,
  concurrent unsubscribe, and failed-start cleanup against a controlled libobs boundary.
- **5 production preview timing cases** and **9 production audio FIFO scenarios**.
- **26 separate packet-model cases**, retained as model coverage rather than evidence about the real session.

Run them without an OBS SDK:

```bash
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

Opt-in integration targets `preview_gpu_integration` and `output_session_integration` use real libobs and a graphics
backend. Enable them with `-DENABLE_OBS_INTEGRATION_TESTS=ON` in an OBS SDK build; they are separate from the five
SDK-free suites and are not automatically run by those CTest commands. Their native runtime results must be
reported separately; see the [current release validation](releases/1.1.2.md#validation).

The 174.002-second recording and Linux Docker/Xvfb/llvmpipe startup checks were performed for **1.1.1**, not 1.1.2.
Their historical results and limitations remain in the [1.1.1 release notes](releases/1.1.1.md#validation).

## License

GPL-2.0-or-later. See `LICENSE`.
