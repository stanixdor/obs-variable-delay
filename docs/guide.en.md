# OBS Dynamic Delay

Native OBS Studio plugin that lets you **add, cancel, change, or remove delay without stopping the stream or recording**. It operates on the already-compressed packets that OBS sends to the output, so its steady-state cost is primarily RAM rather than permanent secondary encoding.

Version: `1.1.1`<br>
Reference compatibility: OBS Studio `32.2.x`, Qt 6, macOS 13+, Windows 10/11 x64, and Ubuntu 24.04 x86_64.

Version 1.1.1 fixes single-track Hybrid MP4/MOV and RTMP detection in OBS 32 on **macOS, Windows, and Linux**:
these outputs advertise multivideo capability even when they are not using it. It also fixes the global installation
path on Windows and expands auxiliary encoder diagnostics on all three platforms.

## What it does

- Controls active streaming and recording outputs independently, including when both are running at the same time.
- Lets you select 1 to 300 seconds with a slider and numeric field.
- Activates, cancels, or removes delay without restarting the output.
- Preserves normal OBS operation: scenes can be changed and Studio can be operated while delay is active.
- Shows a selected scene while building the initial buffer.
- Calculates expected RAM usage from the observed real bitrate or the configured bitrate.
- Includes a collapsible audience preview at 320×180 and 2 fps, which is captured only while open.
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

The local artifact is signed *ad hoc*. It is not notarized with an Apple Developer account, so macOS may request authorization when installing it on a different computer.

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

The Debian package is the recommended option for a native installation. It requires OBS Studio 32.2 or later;
if that version is not available from the configured repositories, it can be obtained from the official OBS PPA:

```bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.1-x86_64-linux-gnu.deb
```

As an alternative archive, the `tar.xz` contains the standard `lib/share` layout for `/usr`:

```bash
sudo tar -xJf obs-dynamic-delay-1.1.1-x86_64-ubuntu-gnu.tar.xz -C /usr
```

Restart OBS and enable **Docks → Dynamic Delay**. These two artifacts are built for native OBS on
Ubuntu 24.04 x86_64; they are not presented as packages for Flatpak, Snap, or other distributions.

## Usage and performance

The memory estimate uses `total bitrate × seconds ÷ 8`, with a 20% margin. With CQP, CRF, ICQ, lossless, or a track without a fixed bitrate, the dock indicates that it is measuring and shows the real value after a two-second output window. Opening the audience preview adds its RGB16 history; in the worst case of 300 seconds, this is approximately 67 MiB extra. The main buffer has a 4 GiB safety limit and the auxiliary preroll has a 128 MiB limit.

During **Preparing/Filling**, a second video encoder is created with the same configuration as the output. This is necessary to produce the hold scene with compatible headers. It is destroyed upon entering **Delay active**. For the lowest impact while gaming:

- use a hardware encoder;
- keep the audience preview closed when it is not needed;
- avoid running streaming and recording with different configurations if both outputs are not needed;
- configure a reasonable keyframe interval (for example, 2 seconds), because every entry and exit waits for a safe keyframe.

Simultaneous streaming and recording share a single hold-scene view, clock, and FIFO. The PCM bridge is
fixed and bounded: 16 blocks × 1024 frames × 6 mixes × 8 channels, approximately 3 MiB per activation, without
reserving audio proportional to the delay duration. Its internal lookahead is one OBS quantum, approximately 21.3 ms
at 48 kHz.

## Deliberate limitations of version 1.1

- **Transition:** the production mode is `Cut on keyframe`. The fade option is shown disabled because a real crossfade between already-delayed and live video requires decoding, compositing, and re-encoding the entire signal. That conflicts with the low-resource objective and can degrade image quality and latency.
- **Audio splice:** switching between the main and auxiliary encoder can introduce a very brief gap of
  one or two AAC frames. The several seconds of silence during filling no longer occur.
- **Audio safety:** a shared source or a reserved track that stops being exclusive causes silence
  only in the hold scene. The plugin does not modify global routing in an attempt to correct it automatically.
- **Removing delay:** the return is immediate in control terms, but the actual splice waits for the next keyframe to keep the stream decodable. The usual maximum is the configured GOP interval.
- **Audience preview:** approximately represents the frames sent by OBS. It does not include the server, CDN, or viewer-player buffer. During the hold scene, it shows its status instead of maintaining another auxiliary render.
- Encoded outputs with audio and video are supported. Normal Hybrid MP4/MOV and RTMP outputs are compatible when
  they have exactly one active video track. Raw, audio-only, video-only outputs, OBS native delay, and layouts
  with multiple video tracks such as `Enhanced Broadcasting` are not supported.
- The Windows binary is built and structurally validated against the official OBS 32.2.2 distribution; the full functional test for this release was performed on macOS arm64.

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

The pure core in `src/core/` models per-track queues, generations, limits, and temporal mapping without depending on OBS. The adapter in `src/output-session.cpp` applies the same strategy to `encoder_packet` and manages the particulars of the real libobs lifecycle.

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
cmake --build build_macos --target packet_delay_tests --config RelWithDebInfo --parallel
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
Actions. They generate a ZIP for Windows, a PKG or `tar.xz` for macOS, and a `tar.xz` plus a Debian package for
Linux. The release tag must match the version in `buildspec.json`; prerelease suffixes
such as `-beta2` or `-rc1` are allowed.

## Validation performed

- 26 packet-core cases: exact delay, cancellation, multiple tracks, keyframes,
  generations/rearming, memory limits, non-unit timebases, and A/V continuity.
- 9 audio-bridge scenarios: bounded/concurrent FIFO, positive/negative offsets, discontinuities,
  48 kHz phase, underrun, and absence of drift.
- Strict build with warnings treated as errors.
- Native Linux x86_64 build on Ubuntu 24.04 against OBS Studio 32.2.0 and Qt 6.4.2; both suites pass and
  `ldd -r` finds no unresolved symbols.
- Real Debian package load test in OBS 32.2.0 under Docker/Xvfb/llvmpipe: module 1.1.1 loads and
  OBS completes startup. The harness's forced `SIGINT` shutdown ends in a segfault both with the plugin
  and in the baseline without it, so that environment is not used to certify a clean shutdown.
- ASan + UBSan and TSan on both cores, with no findings.
- Real test in OBS 32.2.2 on macOS arm64: start a recording, cancel during `Preparing/Filling`, activate a
  complete delay, change the Program scene, open/close the preview, and return to live without stopping the output.
- 174.002 s validation recording: H.264 1920×1080 + AAC 48 kHz stereo, complete decoding with no
  errors and 0 DTS regressions across 10,425 video packets and 8,154 audio packets.
- Non-silent audio measured during both hold scenes: −21.5 dB and −21.9 dB mean volume.
- Windows x64 DLL: validated PE32+ format, all eight OBS exports present, and imports resolved against the official distribution.

## License

GPL-2.0-or-later. See `LICENSE`.
