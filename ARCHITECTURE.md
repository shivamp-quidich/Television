# Televison architecture

## Purpose

Televison receives live UYVY 4:2:2 video from a Blackmagic DeckLink input,
uploads it into a GPU-owned `FrameData::uyvy_frame`, previews it through
CUDA–OpenGL interop, and sends UYVY frames to a DeckLink output. FreeD tracking
packets can be received over UDP and are shown in the status panel. Still images
from `data/` can be selected and placed on the video with the mouse; multiple
placements are supported and are optionally burned into the SDI output. A
**Recording** source mode plays paired video + `*_stype.csv` files from a
stype_handover recordings folder and draws the world-origin XYZ gizmo.

## Whole-application flow chart

```mermaid
flowchart TB
    subgraph startup["Startup (main)"]
        cfg["Load config/televison.ini (or --config / TELEVISON_CONFIG)"]
        args["optional argv device overrides"]
        glfw["GLFW window from [window] section"]
        imgui["ImGui context and GLFW/OpenGL3 backends"]
        create["CreateIngest -> DeckLinkIngest, initialize, startCapture"]
        cfg --> args --> create
        cfg --> glfw --> imgui
    end

    subgraph capture["DeckLink capture thread"]
        sdi_in["SDI input signal"]
        cb["DeckLinkCaptureCallback::VideoInputFrameArrived"]
        modes["Format-change callback: display mode, width, height, pitch"]
        host["latestFrameBuffer: host UYVY bytes + frameAvailable flag"]
        sdi_in --> cb --> host
        sdi_in --> modes --> cb
    end

    subgraph mainloop["Main loop (UI thread)"]
        get["IIngest::getFrame"]
        alloc["UYVYFrame::allocate + cudaMalloc"]
        h2d["cudaMemcpy H2D + cudaStreamSynchronize"]
        fd["shared_ptr&lt;FrameData&gt; with GPU uyvy_frame"]
        meta["Assign id, timestamp, sync_timestamp_ms"]
        info["IIngest::getVideoInfo -> VideoInputState::Config"]
        gate["Stability gate: same dimensions for 2+ consecutive frames"]
        get --> alloc --> h2d --> fd --> meta
        meta --> info
        meta --> gate
    end

    subgraph previewpath["Preview path (stays on GPU)"]
        d2d["cudaMemcpy2D D2D into CUDA-mapped PBO (drops pitch padding)"]
        tex_uyvy["glTexSubImage2D -> UYVY texture, width/2 as RGBA8"]
        draw["FBO pass: full-screen triangle + UYVY-to-RGB shader (needs bound VAO)"]
        tex_rgb["RGB texture"]
        image["ImGui::Image in preview window"]
        d2d --> tex_uyvy --> draw --> tex_rgb --> image
    end

    subgraph outputpath["Output path"]
        d2h["copyFrameToHost: cudaMemcpy2D D2H into staging vector"]
        burn["optional: burn placed graphic into UYVY via BGR round-trip"]
        send["IIngest::sendFrameToOutput"]
        sched["DeckLink scheduled playback frame pool"]
        sdi_out["SDI output signal"]
        d2h --> burn --> send --> sched --> sdi_out
    end

    create -.-> capture
    host --> get
    meta --> d2d
    gate --> initout["IIngest::initializeOutput with fallback device search"]
    initout --> sched
    gate -->|"mode dimensions match"| d2h
    tex_rgb -->|"preview update succeeded"| d2h
    info --> statuspanel["Status panel text"]
    image --> swap["ImGui render + glfwSwapBuffers"]
    statuspanel --> swap
    fd -.->|"shared_ptr released at end of iteration"| free["UYVYFrame destructor frees the CUDA buffer"]

    subgraph udp["UDP tracking loopback"]
        ui_port["ImGui bind IP + port from [udp]"]
        bind["Bind configured IP:port"]
        recv["UdpReceiver background thread"]
        freed["Unwrap optional 16-byte STQ1 header, parse 29-byte FreeD D1"]
        pose["Latest CameraData + packet counters"]
        plots["PlotLines from received history"]
        send["UdpSender: aligned CSV while recording plays"]
        ui_port --> bind --> recv --> freed --> pose --> plots
        send -->|"localhost FreeD"| bind
    end
    pose --> statuspanel

    subgraph recording["Recording playback"]
        pairs["List video + *_stype.csv in recordings_dir"]
        vcap["OpenCV VideoCapture"]
        csvload["stype::loadCsv"]
        gizmo["drawOverlay: world-origin XYZ gizmo"]
        rectex["Upload BGR to GL texture -> ImGui"]
        recsdi["BGR→UYVY (+ optional graphic burn) → DeckLink SDI"]
        pairs --> vcap --> gizmo --> rectex
        pairs --> csvload --> gizmo
        gizmo --> recsdi
        gizmo -->|"while playing"| send
    end
    rectex --> image
    recsdi --> sdi_out

    subgraph graphic["Graphic overlay"]
        pick["Pick image from data/"]
        gltex["Load RGBA OpenGL texture"]
        hover["Yellow polygon follows mouse"]
        click["Click adds a placed graphic"]
        list["Accumulate multiple placements"]
        preview_draw["ImGui AddImage for each placed + pending"]
        pick --> gltex --> hover --> click --> list --> preview_draw
    end
    preview_draw --> image
    list -->|"burn_into_sdi"| burn
```

Video pixels stay in CUDA device memory from the ingest upload through the
preview render. The only host copy after ingest happens in the output path,
because the DeckLink output API requires CPU-addressable frame bytes.

The output path runs after the preview path for the same frame and is gated on
it: a frame is staged and scheduled for SDI only when `GpuUyvyPreview::update`
reported success for that frame, so a broken preview stops SDI output instead of
letting the two paths diverge. The output forwards the ingested UYVY buffer, not
the preview's RGB result. When any graphics are placed and **Burn into SDI** is
on, the staged host buffer is converted to BGR, each placed PNG/JPEG is
alpha-composited at its position, and the result is converted back to pitched
UYVY.

Two details are easy to get wrong when modifying the preview:

- The preview draw call needs a bound vertex array object. The context is an
  OpenGL 3.3 core profile, so `glDrawArrays` without one is invalid and the RGB
  texture stays empty even when capture and SDI output work normally.
- The preview device-to-device copy narrows each row from `pitch` to
  `width × 2`, so the UYVY texture never contains DeckLink row padding, while
  the output path forwards the original padded pitch unchanged.
- `IngestVideoInfo::signal_detected` must not be used to decide whether the
  frame in hand is valid. `DeckLinkCapture::getFrameBuffer` clears the
  frame-available flag when it hands a frame over, so `getVideoInfo` reports
  `signal_detected == false` immediately after a successful `getFrame`. Gating
  output initialization on it makes startup depend on a race with the capture
  callback, which delays SDI output by an arbitrary amount of time.

## Frame contract

`IIngest::getFrame()` returns a `std::shared_ptr<FrameData>`. Each work package
contains:

| Field | Type | Purpose |
| --- | --- | --- |
| `id` | `int` | Application-assigned frame identifier. |
| `uyvy_frame` | `std::unique_ptr<UYVYFrame>` | Exclusive owner of the captured GPU video buffer. |
| `timestamp` | `std::chrono::steady_clock::time_point` | Monotonic local capture timestamp. |
| `sync_timestamp_ms` | `uint64_t` | Wall-clock metadata synchronization timestamp. |
| `module_data` | `std::map<std::type_index, std::any>` | Optional, type-keyed data produced by processing modules. |
| `data_mutex` | `std::mutex` | Protects changes to frame-owned metadata and buffer release. |
| `modules_to_process` | `std::atomic<int>` | Remaining processing-stage count. |

`UYVYFrame` represents packed UYVY 4:2:2 video and is move-only so exactly one
owner releases its CUDA allocation:

| Field | Type | Purpose |
| --- | --- | --- |
| `d_data` | `void*` | CUDA device pointer to packed UYVY pixels. |
| `size` | `size_t` | Allocated/used device-buffer size in bytes. |
| `width`, `height` | `int` | Active image dimensions in pixels. |
| `pitch` | `int` | Source row stride in bytes, including any DeckLink padding. |
| `actual_width_bytes` | `int` | Active UYVY bytes per row (`width × 2`), excluding padding. |

## Components

| Component | Responsibility |
| --- | --- |
| `common/videos/ingestion/src/decklink_capture.cpp` | DeckLink device discovery, capture callbacks, format detection, scheduled SDI playback, genlock status |
| `common/videos/ingestion/src/ingest.cpp` | `IIngest` GPU-frame adapter; retained for application integrations outside the UI executable |
| `common/videos/ingestion/src/main.cpp` | IIngest/FrameData lifecycle, config load/save, GPU preview, and output staging |
| `common/videos/ingestion/include/app_config.hpp` | INI-backed `AppConfig` for DeckLink, UDP, window, graphic, recording |
| `common/videos/ingestion/src/app_config.cpp` | Load/save and path resolution for `config/televison.ini` |
| `config/televison.ini` | Default operator configuration checked into the repo |
| `common/videos/ingestion/include/gpu_uyvy_preview.hpp` | CUDA–OpenGL UYVY preview resource interface |
| `common/videos/ingestion/src/gpu_uyvy_preview.cpp` | CUDA PBO mapping, device-to-device UYVY transfer, shader decode, and RGB preview texture |
| `common/videos/ingestion/include/udp_receiver.hpp` | Local UDP FreeD D1 receiver interface |
| `common/videos/ingestion/src/udp_receiver.cpp` | Background bind/recv thread, raw packet recording, and FreeD D1 decode of both bare and `STQ1`-wrapped datagrams |
| `common/videos/ingestion/include/udp_sender.hpp` | Internal FreeD D1 (+ STQ1) UDP sender for recording CSV replay |
| `common/videos/ingestion/src/udp_sender.cpp` | Encode aligned CSV pose and send to the receiver bind address |
| `udpOut/udp_raw.txt` | Session-local readable UDP recording: one decoded FreeD pose per line |
| `common/videos/ingestion/include/stype_csv_overlay.hpp` | Stype CSV record + world-origin gizmo projection (stype_player maths) |
| `common/videos/ingestion/src/stype_csv_overlay.cpp` | CSV load and HF/pinhole world→pixel overlay draw |
| `common/videos/ingestion/include/recording_playback.hpp` | Paired recording browser + OpenCV video/CSV player |
| `common/videos/ingestion/src/recording_playback.cpp` | List pairs, play/seek, sync CSV row, refresh gizmo |
| `common/videos/ingestion/include/graphic_overlay.hpp` | OpenGL graphic overlay: data/ picker, multi-placement, SDI burn-in |
| `common/videos/ingestion/src/graphic_overlay.cpp` | Load images to GL textures, draw/place multiple, alpha-composite for SDI |
| `data/` | Still images (png/jpg/…) selectable as graphics |
| `third_party/Decklink-SDK` | DeckLink headers and dynamic API dispatch source |
| `third_party/imgui-package` | Project-local ImGui 1.90.1 package used by the GLFW/OpenGL GUI |

## Operator controls

Startup settings come from `config/televison.ini` (or a path given with
`--config`, `TELEVISON_CONFIG`, or a positional `.ini` argument). The file
covers:

| Section | Keys |
| --- | --- |
| `[decklink]` | `input_device`, `output_device` |
| `[udp]` | `bind_ip`, `port` (receive), `send_ip`, `send_port` (internal sender), `recv_delay_ms`, `raw_output_path` |
| `[window]` | `width`, `height`, `title` |
| `[graphic]` | `data_dir` |
| `[recording]` | `recordings_dir` (video + `*_stype.csv` pairs) |

Integer CLI arguments still override the DeckLink devices for a one-off run:

```bash
decklink_passthrough                  # use config/televison.ini
decklink_passthrough 0 2              # override input/output only
decklink_passthrough --config ./my.ini
decklink_passthrough --config ./my.ini 1 3
```

The right-hand ImGui panel shows the loaded config path, the active DeckLink
indices, and a **Save config** button that writes the current UDP values back to
the file.

UDP receive:

- bind IP / recv port and send IP / send port default from `[udp]` and can be
  edited in the UI; **Apply UDP** rebinds the receiver and reconfigures the
  internal sender; **Save config** persists `send_port` / `send_ip`;
- while a recording is **Play**ing and **UDP send CSV while playing** is on, each
  advanced frame encodes the aligned CSV pose as FreeD D1 (STQ1-wrapped) and
  sends it to the receiver bind address; paused recording stops sending;
- the panel shows listening state, recv/send packet counts, bind errors, and the
  latest decoded FreeD D1 pose when a valid packet arrives;
- **Recv delay (ms)** (`recv_delay_ms`, slider 0–2000) holds each FreeD pose
  until that many milliseconds have elapsed since arrival, so plots and the
  delayed pose readout can be lagged to match video;
- **UDP received plots** chart pan/tilt/roll and X/Y/Z from the *delayed*
  received stream (not the CSV file directly);
- two datagram shapes are accepted: the bare 29-byte FreeD D1 payload a real
  tracking source sends, and the 45-byte form the Stype simulator / internal
  sender produce, which prefixes the same payload with a 16-byte sync header of
  `"STQ1"`, a little-endian `uint32` frame id, and a little-endian `uint64`
  timestamp;
- every valid FreeD datagram is decoded into a readable line in the configured
  `raw_output_path` (default `udpOut/udp_raw.txt`). Invalid datagrams are
  retained as decimal byte values.

Pose telemetry is reported in the UDP section of the panel: camera id, zoom and
focus, pan/tilt/roll, X / Y / Z(depth) in metres, and the ground distance to the
world origin.

Graphic overlay:

- **Refresh data/** rescans the `data/` folder for png/jpg/jpeg/bmp/webp/tga;
- selecting a file loads it as the pending OpenGL RGBA texture (follow-mouse);
- while pending, a yellow polygon the size of the scaled image follows the mouse;
- each left-click on the video (Live or Recording) adds one placed graphic at
  that point (normalized coordinates so preview and SDI agree), then clears
  pending — select the same or another image from the combo to place again;
- **Width fraction** sets the size of the *next* placement as a fraction of
  frame width (aspect ratio preserved; each placed item keeps the size used
  when it was clicked);
- **Burn into SDI** composites every placed graphic into the outgoing UYVY;
- the status panel lists placements with **Remove**; **Cancel pending** drops
  the follow-mouse image; **Clear all graphics** removes every placement.

Recording playback (stype_player-style):

- layout: **Apply Alignment** (left) | **Video Preview** (center) | **Application Status** (right);
- **Apply Alignment** dials pan/tilt/roll and X/Y/Z for the world-origin gizmo:
  each axis has a **+** button that multiplies that sign by -1 (+1 ↔ -1);
- **Source** radio switches between **Live DeckLink** and **Recording**;
- **Refresh recordings** scans `[recording] recordings_dir` (editable in the
  panel) for `.mp4` / `.mkv` / `.mov` / `.avi` files that have a matching
  `{stem}_stype.csv` sidecar (default `/home/quidich/stype_handover/recordings`);
  the **Video** combo lists filenames including the extension;
- selecting a pair loads the OpenCV video and CSV, switches to Recording mode,
  and draws the world-origin XYZ gizmo via the same projection maths as
  `stype_handover/scripts/stype_player.py`;
- **Play / Pause / Prev / Next / Restart**, frame scrub, **CSV offset**,
  **Show world origin**, gizmo length, and k1/k2 distortion toggle;
- live DeckLink capture is paused while Recording is selected; recording frames
  are converted to UYVY and sent on the DeckLink SDI output (scaled to the
  enabled output mode when needed). **SDI out** toggles that path;
- graphic overlay placement uses the same click / pending / multi-place rules
  on the recording preview as on live; **Burn into SDI** composites placed
  graphics into the recording output as well.

DeckLink device indices default from `[decklink]` and may be overridden on the
command line as shown above.

```bash
decklink_passthrough [input-device] [output-device]
```

## Runtime requirements

- Blackmagic Desktop Video driver and accessible DeckLink devices.
- A display-capable OpenGL 3.3 environment for GLFW.
- CUDA/OpenGL interop on the same GPU for the preview PBO.
- The output connector must support the detected input mode. An unlocked genlock
  reference does not prevent output, but output is then not reference-synchronized.
- If the requested output device is unavailable, `DeckLinkCapture` tries the
  remaining output-capable devices and the UI reports the device actually selected.

## Device behavior

`decklink_passthrough [input-device=0] [output-device=1]` selects the preferred
DeckLink input and output indices. The input callback detects the active display
mode and updates frame dimensions and pitch. Output starts only after the input
format is stable, then schedules a small bounded frame pool to limit SDI latency.
If the preferred output cannot be used, the application attempts another
output-capable device and reports the selected device. Genlock is used when it
is supported and locked; otherwise, scheduled output continues without reference
synchronization.

## Change rule

`.cursor/rules/architecture-documentation.mdc` requires this document to be
updated whenever application architecture, data flow, controls, dependencies, or
public interfaces change.
