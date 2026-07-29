# Televison — DeckLink ingestion

SDI capture and playback, extracted from the StiQy 2.0 `ingestion` module. Frames
are captured as 8-bit UYVY 4:2:2, uploaded to the GPU, and handed out as
`FrameData`; playback goes back out over SDI through DeckLink scheduled playback.

## Layout

| Path | Contents |
| --- | --- |
| `common/videos/ingestion` | The module: `IIngest` interface, `CreateIngest()` factory, `DeckLinkCapture` |
| `common/logger` | spdlog wrapper used by the module (`getModuleLogger`) |
| `include` | Frame and state contracts: `shared_data.h` (`UYVYFrame`, `FrameData`), `shared_state.h` |
| `third_party/Decklink-SDK` | Vendored Blackmagic DeckLink SDK headers and `DeckLinkAPIDispatch.cpp` |
| `cmake/FindDeckLinkAPI.cmake` | Locates the SDK and the runtime driver library |

## Dependencies

- CMake 3.18+, a C++17 compiler
- CUDA Toolkit
- OpenCV **built with CUDA** (`cudaimgproc`)
- OpenGL, GLEW, spdlog
- Blackmagic Desktop Video driver, for anything beyond compiling

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

If more than one OpenCV is installed, point CMake at the CUDA-enabled one, since
a distro package without the `cuda*` modules will fail to configure:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR=/usr/local/lib/cmake/opencv4
```

## DeckLink discovery

`libDeckLinkAPI.so` is **not** a link input. `DeckLinkAPIDispatch.cpp` `dlopen()`s
it by soname at run time, so the build links only `libdl` and the loader resolves
the driver library — no absolute path such as `/usr/lib/libDeckLinkAPI.so` is
baked into the build, and none is needed for `libc++` either.

`FindDeckLinkAPI` still resolves the library so a missing driver is reported while
configuring rather than at first capture. Both locations can be overridden by
cache variable or environment variable:

| Variable | Purpose | Default |
| --- | --- | --- |
| `DECKLINK_SDK_DIR` | SDK root holding `include/DeckLinkAPI.h` | `third_party/Decklink-SDK` |
| `DECKLINK_LIBRARY_DIR` | Directory holding `libDeckLinkAPI.so` | loader's default search path |

A driver installed outside the loader's default directories is reached by adding
that directory to the consumer's RPATH:

```bash
cmake -S . -B build -DDECKLINK_LIBRARY_DIR=/opt/blackmagic/lib
```

## Usage

```cpp
#include "ingest.hpp"

IngestConfig config;          // backend "decklink", input_device, output_device
auto ingest = CreateIngest(config);
ingest->setSharedState(&shared_state);
ingest->initialize(config);
ingest->initializeOutput(config.output_device);
ingest->startCapture();

std::shared_ptr<FrameData> frame;
if (ingest->getFrame(frame)) {
    // frame->uyvy_frame->d_data is device-side UYVY, pitch = rowBytes
}

ingest->sendFrameToOutput(host_uyvy, size, width, height, pitch);
```

`initializeOutput(device, slot)` and `sendFrameToOutput(..., slot)` drive a second
SDI output. The Deltacast backend ships alongside but is off unless configured
with `-DSTIQY_ENABLE_DELTACAST=ON`, which additionally needs the VideoMasterHD SDK.
