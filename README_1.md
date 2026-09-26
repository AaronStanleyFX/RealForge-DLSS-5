# RealForge DLSS 5

**Realistic neural rendering for your images and videos, powered by NVIDIA DLSS 5 through the RenoDX DLSS5 add-on.**

RealForge runs your pictures and video frames through NVIDIA DLSS the way a game frame would be, so the RenoDX DLSS 5 add-on can apply **DLSS 5 Neural Rendering** to them. Everything is driven from a simple local web interface: drop a file, adjust the settings, compare before / after, export.

> [!IMPORTANT]
> RealForge does **not** ship any NVIDIA, ReShade or RenoDX files. You must provide your own DLSS 5 files (see [Requirements](#requirements)).

---

## Features

- 🖼️ **Images and videos**: PNG, JPG, WebP, TIFF, BMP · MP4, MOV, MKV, WebM, AVI…
- 🎛️ **DLSS 5 settings**: NR Intensity, Local Tone, Local Structure, Local Skin (0 to 2, slider or typed value), character detection, 1 to 4 NR passes
- ⭐ **"First render quality"**: one click restores a known-good reference setup
- 🔍 **Before / after comparison**: draggable slider for images, synchronized players for videos
- 📐 **Output resolution** 1× (DLAA), 1.5×, 2×, 3×, with automatic downscaling of very large images (up to 8K working size)
- 🎬 **Video export**: MP4 H.264 / H.265 / NVENC or **MOV ProRes 4444 XQ**, original audio kept
- ✅ **Render verification**: RealForge reads the add-on log after every render and reports an error if Neural Rendering did not actually apply, instead of silently returning the original image
- 🧰 **Zero-setup launcher**: `start.bat` downloads portable Python and ffmpeg if missing, compiles the engine, and opens the interface in your browser

## How it works

```
your image ──► realforge_engine.exe ──► NVIDIA DLSS (NGX, D3D12)
                     │                        │
                     │            ReShade (dxgi.dll) + RenoDX DLSS5 add-on
                     │                        │  injects DLSS 5 Neural Rendering
                     ◄────────── rendered frame read back ──► PNG / video
```

1. **The engine** (`src/realforge_engine.cpp`) is a minimal Direct3D 12 host. It converts each image to linear HDR (FP16), exactly like a game frame, and evaluates NVIDIA DLSS on it.
2. **ReShade** (loaded as `dxgi.dll`) loads the **RenoDX DLSS5** add-on, which hooks that DLSS evaluation and runs the DLSS 5 neural renderer (`nvngx_dlssnr.dll`).
3. **The server** (`server.py`, Python standard library only) writes the add-on settings to `ReShade.ini`, runs the engine, handles video through ffmpeg, and serves the web interface.

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11 (64-bit) |
| GPU | NVIDIA RTX with a recent driver |
| Compiler | Microsoft C++ Build Tools (one-time install, see below) |
| Python | 3.8+ (downloaded automatically if missing) |
| ffmpeg | for videos (downloaded automatically if missing) |

**DLSS 5 files (not included, provide your own)**, placed in the folder that contains RealForge (the parent folder by default):

| File | Role |
|---|---|
| `dxgi.dll` | ReShade with add-on support, renamed |
| `renodx-dlss5.addon64` | RenoDX DLSS 5 ReShade add-on |
| `nvngx_dlssnr.dll` | DLSS 5 neural renderer |
| `nvngx_dlss.dll` | DLSS Super Resolution (Streamline production build) |

Sub-folders are searched too (for example an unpacked Streamline folder). The folder can be changed in **Diagnostics**.

## Installation

1. Install the Microsoft C++ compiler (once):
   ```bat
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
2. Clone this repository next to your DLSS 5 files:
   ```
   YourFolder\
   ├── dxgi.dll
   ├── renodx-dlss5.addon64
   ├── nvngx_dlssnr.dll
   ├── nvngx_dlss.dll
   └── RealForge-DLSS5\     ← this repository
   ```
3. Double-click **`start.bat`**.

On first launch `start.bat` will:
- download portable Python and ffmpeg if they are not installed,
- offer to turn off NVIDIA's on-screen **DLSS indicator** if it is enabled (it would print text on your renders),
- download the NVIDIA DLSS SDK headers from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) and compile `engine\realforge_engine.exe`,
- open the interface at `http://127.0.0.1:8765`.

Keep the console window open while you use RealForge. Running `start.bat` again replaces an older running version automatically.

## Usage

1. Drop images or videos into **Import**.
2. Adjust **DLSS 5 Settings** (or click **Apply** next to *First render quality*).
3. Choose the **Output** resolution and, for videos, the format.
4. Click **Start DLSS 5 render**.
5. Compare with the before / after slider, then **Download**.

### Settings reference

| Setting | Range | Effect |
|---|---|---|
| NR Intensity | 0 – 2 | Overall strength of the neural pass |
| Local Tone | 0 – 2 | Lighting and tone, region by region |
| Local Structure | 0 – 2 | Fine detail: hair, fabrics, objects, scenery |
| Local Skin | 0 – 2 | Skin texture (0 = reference, higher = more detail) |
| Character detection | on / off | Lets the runtime apply Local Skin to detected characters |
| NR passes | 1 – 4 | Stacked neural passes, each one strengthens the effect |
| Color input (Advanced) | Linear HDR / sRGB | Linear HDR feeds DLSS like a game (recommended) |
| Max working size | 1080p – 8K | Larger images are scaled down before rendering |

**Tips**
- Start from **First render quality**, then change one setting at a time.
- A high **Local Tone** flattens contrast; to push realism while keeping texture, raise **Local Structure** and **Local Skin** first.
- Stacking many passes or rendering at 2× / 3× changes the look and takes longer.

## Project structure

```
RealForge-DLSS5/
├── start.bat              # launcher: Python, ffmpeg, DLSS indicator, build, server
├── build.bat              # downloads the NGX SDK and compiles the engine (MSVC)
├── server.py              # local web server + render pipeline (stdlib only)
├── web/index.html         # web interface
├── src/realforge_engine.cpp   # Direct3D 12 / NGX host
├── engine/                # generated: exe, links to your DLLs, ReShade.ini, logs
└── work/                  # generated: one folder per render
```

## Troubleshooting

| Symptom | What to check |
|---|---|
| Interface does not open | `start.log` and `realforge.log` in the RealForge folder |
| "DLSS 5 not applied" | Click **Log** under the preview; the add-on's reason is shown. Try a smaller *Max working size* |
| Render almost identical to the original | Increase NR Intensity / Local Structure / Local Skin |
| Text printed at the bottom of renders | NVIDIA's DLSS indicator: accept the prompt in `start.bat` or use the banner button |
| Video export fails | The job log shows the ffmpeg command and error |
| Engine won't compile | Install the C++ Build Tools (see Installation) and run `build.bat` |

**Diagnostics** (top right) checks the four DLSS 5 files, initializes NVIDIA NGX and shows the DLSS / NR lines from `ReShade.log`.

## Limitations

- Output is 8-bit SDR (PNG, H.264/H.265, or 12-bit ProRes from 8-bit frames). HDR videos are converted to SDR.
- A still image or video has no depth or motion vectors: the neural renderer works from the picture alone, so fast motion in videos can ghost (enable **Anti-ghosting** in the Video settings).
- The result depends on NVIDIA's model and the RenoDX add-on version you use.

## Credits

- **NVIDIA DLSS / NGX**: [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) SDK (headers and library are downloaded at build time, not redistributed)
- **ReShade** by crosire: <https://reshade.me>
- **RenoDX** DLSS5 add-on
- **ffmpeg**: <https://ffmpeg.org>

## Disclaimer

RealForge is an independent, unofficial project. It is not affiliated with or endorsed by NVIDIA, ReShade or RenoDX. NVIDIA, DLSS and RTX are trademarks of NVIDIA Corporation. You are responsible for obtaining the DLSS, ReShade and RenoDX files and for complying with their respective licenses.
