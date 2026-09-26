# RealForge DLSS 5

RealForge DLSS 5 is a Windows-based local application for processing
images and videos through NVIDIA DLSS and DLSS 5 Neural Rendering.

It combines a native Direct3D 12 / NVIDIA NGX engine with ReShade and
the RenoDX DLSS 5 add-on, while providing a simple local web interface
for configuring and reviewing renders.

> **Important:** RealForge is an experimental desktop tool built around
> NVIDIA DLSS/NGX, ReShade, and RenoDX components. Compatibility depends
> on your NVIDIA GPU, driver, DLSS runtime, and the third-party binaries
> used by the project.

## Features

-   Process individual images with NVIDIA DLSS.
-   Process videos while preserving temporal history between frames.
-   DLAA / output scaling support.
-   DLSS 5 Neural Rendering through the RenoDX DLSS 5 add-on.
-   Local browser-based user interface.
-   Automatic verification of Neural Rendering through `ReShade.log`.
-   Multiple Neural Rendering looks:
    -   **Natural** --- neutral photorealistic rendering.
    -   **Cinematic** --- film-oriented look.
    -   **Realism+** --- stronger skin, hair, fabric, and structural
        detail.
    -   **Ultra** --- maximum effect.
    -   **Custom** --- manual control over NR intensity, local tone,
        local structure, and skin structure.
-   Adjustable effect blending with the original image.
-   Automatic FFmpeg setup for video processing.
-   Automatic portable Python setup when a suitable system Python
    installation is unavailable.
-   Built-in diagnostics for NVIDIA NGX, DLSS, ReShade, and the RenoDX
    add-on.
-   Automatic downscaling when the requested working resolution exceeds
    supported rendering limits.

## How It Works

The native engine in `src/realforge_engine.cpp` creates a minimal
Direct3D 12 rendering environment and sends each image through NVIDIA
NGX/DLSS similarly to a game frame.

For the recommended **Linear HDR** input mode, source images are
converted to linear FP16 data before DLSS evaluation. ReShade loads the
RenoDX DLSS 5 add-on, which hooks into the DLSS pipeline to apply Neural
Rendering. The final image is then converted back for output.

RealForge does not simply assume that Neural Rendering worked. After a
render, the application inspects `ReShade.log` and checks whether the
RenoDX Neural Rendering path actually engaged. If it did not, the job is
reported as an error instead of presenting an untreated image as a
successful Neural Rendering result.

For videos, FFmpeg extracts and encodes frames while the engine can
preserve DLSS temporal history across the sequence.

## Requirements

### Operating System

-   Windows 10 or Windows 11, 64-bit.

### GPU and Drivers

-   An NVIDIA GPU and driver configuration on which NVIDIA NGX reports
    DLSS as available.
-   Drivers compatible with the DLSS runtime used by the project.

Use the **Diagnostic** section in the RealForge interface to check the
actual capabilities reported by NGX on your machine.

### Microsoft C++ Build Tools

The native engine is compiled with Microsoft Visual C++ and requires the
Visual Studio 2022 C++ Build Tools.

You can install them from a terminal with:

``` powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

This is the only dependency that normally requires manual installation.

### Python

Python 3.8 or newer can be used.

If a compatible Python installation is not found, `start.bat`
automatically downloads a portable Python 3.12 runtime into the project
directory.

The Python server uses the standard library and does not require a
separate `pip install` step.

### FFmpeg

FFmpeg is required for video processing.

If FFmpeg and FFprobe are not available locally or on `PATH`,
`start.bat` attempts to download them automatically. Image processing
remains available if FFmpeg setup fails.

## Required DLSS / ReShade Files

The engine expects these files:

``` text
dxgi.dll
renodx-dlss5.addon64
nvngx_dlssnr.dll
nvngx_dlss.dll
```

Their roles are:

  File                     Purpose
  ------------------------ -------------------------------
  `dxgi.dll`               ReShade loader
  `renodx-dlss5.addon64`   RenoDX DLSS 5 add-on
  `nvngx_dlssnr.dll`       DLSS Neural Rendering runtime
  `nvngx_dlss.dll`         DLSS Super Resolution runtime

By default, RealForge searches for these files from the configured DLL
directory. The current default is the parent directory of the RealForge
project.

When possible, the application creates hard links to the files inside
`engine/`; otherwise, it copies them. The DLL source directory can be
changed from the **Diagnostic** panel in the web interface.

Make sure you have the right to use and redistribute any NVIDIA,
ReShade, RenoDX, or other third-party binaries you place in the project.
Their licensing terms are separate from the RealForge source code.

## Quick Start

Clone or download the repository, then install the Microsoft C++ Build
Tools described above.

Launch:

``` bat
start.bat
```

The launcher will:

1.  Find a compatible Python installation or download portable Python.
2.  Find FFmpeg or download it for video support.
3.  Check whether NVIDIA's DLSS indicator is enabled and offer to
    disable it.
4.  Compile the native C++ engine when necessary.
5.  Start the local RealForge server.
6.  Open the web interface in your default browser.

By default, the server starts on:

``` text
http://127.0.0.1:8765/
```

If that port is unavailable, RealForge tries the next available port
within a small range.

Keep the `start.bat` terminal window open while using RealForge.

## Usage

### 1. Start RealForge

Double-click `start.bat` or run it from a terminal.

### 2. Check Diagnostics

Open the Diagnostic panel and verify that the required DLSS/ReShade
files are detected and NVIDIA NGX initializes correctly.

If the DLLs are stored somewhere else, change the configured DLSS file
directory from this panel.

### 3. Add Media

Upload an image or video through the local web interface.

Supported image extensions include:

``` text
.png .jpg .jpeg .bmp .tif .tiff .webp .jxr .heic
```

Supported video/container extensions include:

``` text
.mp4 .mkv .mov .avi .webm .m4v .wmv .flv .ts .mts .gif
```

Actual codec/container support for video also depends on the installed
FFmpeg build.

### 4. Configure the Render

For a first render, the recommended starting point is:

-   **Neural Rendering:** enabled
-   **Style:** Natural
-   **Color input:** Linear HDR
-   **Scale:** 1x / DLAA
-   **NR passes:** 1
-   **Effect mix:** 100%

Multiple stacked Neural Rendering passes can over-process an image, so
one pass is the recommended default.

### 5. Render

Start the job from the web interface.

RealForge stores job data and rendered media under `work/`, using a
separate directory for each job.

## Render Styles

### Natural

Neutral photorealistic rendering and the recommended starting point.

### Cinematic

A more film-oriented Neural Rendering profile.

### Realism+

Uses stronger Neural Rendering parameters for additional local structure
and details such as skin, hair, and fabrics.

### Ultra

Pushes the available Neural Rendering controls to their strongest preset
values. It can intentionally produce a more exaggerated result.

### Custom

Allows manual adjustment of the Neural Rendering parameters exposed by
the RenoDX add-on.

The **effect mix** control can blend the processed output with the
original when the full Neural Rendering result is too strong.

## Image Processing Recommendations

For typical image processing:

-   Start with **Natural**.
-   Use **1 Neural Rendering pass**.
-   Start at **1x / DLAA**.
-   Keep **Linear HDR** input enabled.
-   Reduce the effect mix if the result looks over-processed.
-   Use higher scaling factors only when actual upscaling is required.

The engine also limits oversized working resolutions when required by
the DLSS pipeline.

## Video Processing

Video jobs use FFmpeg for decoding/extraction and final encoding.

The native engine supports a `sequence` mode designed for video frames.
Unlike independent image processing, this mode can retain temporal
history between frames.

Available server-side video settings include options such as:

-   Start time.
-   Maximum duration.
-   Output codec.
-   CRF.
-   Temporal-history reset.
-   Frame passes.
-   Keeping intermediate frames.

## Building the Engine Manually

Run:

``` bat
build.bat
```

The build script:

1.  Checks for the NVIDIA DLSS SDK headers and NGX import library.
2.  Downloads the required SDK files from NVIDIA's DLSS GitHub
    repository when they are missing.
3.  Locates Microsoft Visual C++ Build Tools.
4.  Compiles `src\realforge_engine.cpp` as C++17.
5.  Writes the executable to:

``` text
engine\realforge_engine.exe
```

The native engine links against NVIDIA NGX and Windows Direct3D/DXGI
components.

## Native Engine CLI

The engine can also be invoked directly.

Basic syntax:

``` text
realforge_engine.exe --in <file|directory> --out <file|directory> [options]
```

Notable options include:

``` text
--mode image|sequence
--color linear|sdr
--scale <factor>
--warmup <count>
--frame-passes <count>
--reset-each
--depth <value>
--preset <A..O|0>
--max-pixels <count>
--mix <factor>
--orig <path>
--hidden
--diagnose
--verbose
```

Example diagnostic run:

``` bat
cd engine
realforge_engine.exe --diagnose
```

The CLI writes machine-readable progress information such as `INFO`,
`PROGRESS`, `DONE`, and `ERROR` messages to standard output.

## Configuration

The Python server reads optional configuration from:

``` text
config.json
```

The local server port can be overridden with the `REALFORGE_PORT`
environment variable:

``` bat
set REALFORGE_PORT=9000
start.bat
```

The server binds to `127.0.0.1`, so the web interface is local to the
machine by default.

## Project Structure

``` text
RealForge-DLSS5/
├── build.bat
├── start.bat
├── server.py
├── src/
│   └── realforge_engine.cpp
├── web/
│   └── index.html
├── engine/
│   ├── realforge_engine.exe
│   ├── ReShade.ini
│   └── ... runtime DLLs and logs
├── third_party/
│   └── DLSS/
│       ├── include/
│       └── lib/
├── work/
│   └── ... render jobs
└── LISEZMOI.txt
```

### Main Components

**`server.py`**\
Local HTTP server, job manager, configuration layer, diagnostics,
image/video processing orchestration, and web API.

**`web/index.html`**\
Self-contained local browser interface.

**`src/realforge_engine.cpp`**\
Native Direct3D 12 / NVIDIA NGX rendering engine.

**`build.bat`**\
Downloads missing NVIDIA DLSS SDK development files and compiles the C++
engine.

**`start.bat`**\
Bootstrap launcher for Python, FFmpeg, engine compilation, and the web
server.

**`engine/`**\
Runtime directory containing the native executable, ReShade
configuration, required runtime components, and diagnostic logs.

**`work/`**\
Generated jobs, source copies/frames, outputs, and job metadata.

## Troubleshooting

### C++ compiler not found

Install Visual Studio 2022 Build Tools with the Desktop C++ workload:

``` powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Then restart `start.bat`.

### Required DLSS files are missing

Open **Diagnostic** and configure the directory containing:

``` text
dxgi.dll
renodx-dlss5.addon64
nvngx_dlssnr.dll
nvngx_dlss.dll
```

### Neural Rendering does not engage

Check the Diagnostic panel and:

``` text
engine\ReShade.log
```

RealForge uses this log to distinguish a genuine Neural Rendering render
from a failed or inactive hook.

Possible causes include incompatible runtime files, ReShade not loading,
the RenoDX add-on not loading, driver/GPU incompatibility, or an
unsupported working resolution.

### NVIDIA DLSS indicator appears in renders

`start.bat` checks the NVIDIA NGX `ShowDlssIndicator` setting. If it is
enabled, the launcher offers to disable it with administrator
permission.

### Video processing is unavailable

Verify that both `ffmpeg.exe` and `ffprobe.exe` are available either in
the local `ffmpeg` directory or on your system `PATH`.

### Port 8765 is already in use

RealForge automatically tries subsequent ports. You can also choose a
starting port manually:

``` bat
set REALFORGE_PORT=9000
start.bat
```

## Logs

Useful diagnostic files include:

``` text
start.log
realforge.log
engine\ReShade.log
engine\nvsdk_ngx.log
engine\nvngx.log
engine\nvngx_dlss_*.log
```

These files can help diagnose startup, NGX, DLSS, ReShade, and Neural
Rendering issues.

## Security

RealForge's HTTP server binds to the loopback address (`127.0.0.1`)
rather than exposing the interface directly to the local network.

The application can download external dependencies during setup. Review
`start.bat` and `build.bat` if you want to audit the download sources
and bootstrap process before running them.

## Third-Party Components

RealForge integrates or relies on third-party technologies including:

-   NVIDIA DLSS / NVIDIA NGX
-   ReShade
-   RenoDX DLSS 5 add-on
-   FFmpeg
-   Python
-   Microsoft Visual C++ Build Tools

All trademarks belong to their respective owners. Third-party components
remain subject to their own licenses and distribution terms.

## License

No RealForge project license file is currently included in this
repository.

If you intend to publish the repository publicly, add a `LICENSE` file
that clearly defines how the RealForge source code may be used,
modified, and redistributed. This does not override the separate
licenses or redistribution restrictions of third-party SDKs, DLLs,
add-ons, or other bundled components.

## Disclaimer

RealForge DLSS 5 is an independent project and is not presented as an
official NVIDIA, ReShade, RenoDX, FFmpeg, Python, or Microsoft product.

DLSS and NVIDIA NGX behavior depends on compatible hardware, drivers,
runtime files, and third-party integrations. Neural Rendering
availability should be verified through RealForge's diagnostics on the
target system.
