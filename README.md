<div align="center">

<img src="assets/realforge.png" width="112" alt="RealForge DLSS 5 logo">

# RealForge DLSS 5

**Realistic neural rendering for your images and videos, powered by NVIDIA DLSS 5.**

![Version](https://img.shields.io/badge/version-2.0.0-76b900)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078d4)
![GPU](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)

</div>

RealForge is a Windows desktop app that runs your pictures and video frames through **NVIDIA DLSS** exactly like a game frame, so the **RenoDX DLSS5** add-on can apply **DLSS 5 Neural Rendering** to them. Drop a file, pick a rendering style, compare before / after, export.

> [!IMPORTANT]
> RealForge does **not** include any NVIDIA, ReShade or RenoDX files. You must provide your own DLSS 5 files (see [Requirements](#requirements)).

---

## ✨ Features

- 🖥️ **Real desktop app**: `RealForge DLSS 5.exe` opens in its own window, no console, no browser tabs, with its own icon and a desktop shortcut
- 🎨 **Rendering styles**: Natural, Cinematic, Realism+, Ultra, or Custom
- 🎛️ **Custom settings**: NR Intensity, Local Tone, Local Structure, Local Skin (0 to 2, slider or typed value), character detection, 1 to 4 NR passes
- ⭐ **First render quality**: one click restores a known-good reference setup
- 🔍 **Before / after comparison**: draggable slider for images, synchronized players for videos
- 📐 **Output resolution** 1× (DLAA), 1.5×, 2×, 3×, with automatic downscaling of very large images (up to 8K working size)
- 🎬 **Video export**: MP4 H.264 / H.265 / NVENC or **MOV ProRes 4444 XQ**, original audio kept
- ✅ **Render verification**: after every render RealForge checks that Neural Rendering really applied and tells you if it did not, instead of silently returning the original image
- 🧰 **Zero setup**: the first launch downloads portable Python and ffmpeg if needed and compiles the engine automatically

## 📋 Requirements

| | |
|---|---|
| OS | Windows 10 / 11 (64-bit) |
| GPU | NVIDIA RTX with a recent driver |
| Browser engine | Microsoft Edge (preinstalled on Windows 10 / 11) |
| Compiler | Microsoft C++ Build Tools, one-time install (see below) |
| Python / ffmpeg | downloaded automatically if missing |

**DLSS 5 files (not included, provide your own)**, placed in the folder that **contains** the RealForge folder:

| File | Role |
|---|---|
| `dxgi.dll` | ReShade with add-on support, renamed |
| `renodx-dlss5.addon64` | RenoDX DLSS 5 ReShade add-on |
| `nvngx_dlssnr.dll` | DLSS 5 neural renderer |
| `nvngx_dlss.dll` | DLSS Super Resolution (Streamline production build) |

Sub-folders are searched too (for example an unpacked Streamline folder). The location can be changed later in **Diagnostics**.

## 🚀 Installation

1. **Install the Microsoft C++ compiler** (once):
   ```bat
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
2. **Download** the latest release zip (or clone this repository) and put it next to your DLSS 5 files:
   ```
   YourFolder\
   ├── dxgi.dll
   ├── renodx-dlss5.addon64
   ├── nvngx_dlssnr.dll
   ├── nvngx_dlss.dll
   └── RealForge-DLSS5\
       └── RealForge DLSS 5.exe
   ```
3. **Double-click `RealForge DLSS 5.exe`.**

The first launch shows a setup console that:
- downloads portable Python and ffmpeg if they are not installed,
- offers to turn off NVIDIA's on-screen **DLSS indicator** if it is enabled (it would print text on your renders),
- downloads the NVIDIA DLSS SDK headers from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) and compiles the engine,
- creates a **RealForge DLSS 5** shortcut on your desktop.

After that, RealForge starts instantly. **Closing the window closes RealForge completely.**

> [!NOTE]
> The exe is not code-signed, so Windows SmartScreen may warn you on first launch: click **More info → Run anyway**.

## 🎨 Usage

1. Drop images or videos into **Import**.
2. Pick a **Rendering style**, or **Custom** to set every value yourself. **First render quality → Apply** restores the reference setup.
3. Choose the **Output** resolution and, for videos, the format.
4. Click **Start DLSS 5 render**.
5. Compare with the before / after slider, then **Download**.

### Rendering styles

| Style | NR Intensity | Local Tone | Local Structure | Local Skin | Look |
|---|---|---|---|---|---|
| Natural | 1 | 1 | 1 | 0 | Neutral photo realism, faithful to the original |
| Cinematic | 1 | 1 | 1 | 0 | Film look: contrast and mood |
| Realism+ | 2 | 1.5 | 2 | 1.5 | More detailed skin, hair and fabrics |
| Ultra | 2 | 2 | 2 | 2 | Maximum effect, may exaggerate |
| Custom | your values | | | | Style (Default / Natural / Cinematic) + every value from 0 to 2 |

### Settings reference

| Setting | Range | Effect |
|---|---|---|
| NR Intensity | 0 – 2 | Overall strength of the neural pass |
| Local Tone | 0 – 2 | Lighting and tone, region by region |
| Local Structure | 0 – 2 | Fine detail: hair, fabrics, objects, scenery |
| Local Skin | 0 – 2 | Skin texture (0 = reference, higher = more detail) |
| Character detection | on / off | Applies Local Skin to detected characters |
| NR passes | 1 – 4 | Stacked neural passes, each one strengthens the effect |
| Color input (Advanced) | Linear HDR / sRGB | Linear HDR feeds DLSS like a game (recommended) |
| Max working size | 1080p – 8K | Larger images are scaled down before rendering |

**Tips**
- Start from **Natural** or **First render quality**, then change one setting at a time.
- A high **Local Tone** flattens contrast; to push realism while keeping texture, raise **Local Structure** and **Local Skin** first.
- More passes or 2× / 3× output change the look and take longer.

## ⚙️ How it works

```
RealForge DLSS 5.exe ── starts ──► server.py (hidden) ◄──► app window (Edge app mode)
                                        │
                                        ▼
your image ──► realforge_engine.exe ──► NVIDIA DLSS (NGX, Direct3D 12)
                                        │
                     ReShade (dxgi.dll) + RenoDX DLSS5 add-on inject DLSS 5 Neural Rendering
                                        │
                                        ▼
                         rendered frame ──► PNG / video (ffmpeg)
```

- **`RealForge DLSS 5.exe`** (`src/launcher.cpp`) prepares everything if needed, starts the server hidden, opens the app window and shuts everything down when you close it.
- **The engine** (`src/realforge_engine.cpp`) is a minimal Direct3D 12 host: it converts each image to linear HDR (FP16), like a game frame, and evaluates NVIDIA DLSS on it.
- **ReShade** loads the **RenoDX DLSS5** add-on, which hooks that evaluation and runs the DLSS 5 neural renderer.
- **The server** (`server.py`, Python standard library only) writes the add-on settings, runs the engine, handles video through ffmpeg and serves the interface (`web/index.html`).

## 📁 Project structure

```
RealForge-DLSS5/
├── RealForge DLSS 5.exe       # the application
├── start.bat                  # setup + fallback launcher
├── build.bat                  # downloads the NGX SDK and compiles the engine (MSVC)
├── server.py                  # local server + render pipeline
├── web/index.html             # interface
├── src/realforge_engine.cpp   # Direct3D 12 / NGX engine
├── src/launcher.cpp           # source of RealForge DLSS 5.exe (+ app.rc, app.manifest)
├── assets/                    # icon
├── engine/                    # generated: engine exe, links to your DLLs, ReShade.ini, logs
└── work/                      # generated: one folder per render
```

## 🛠️ Troubleshooting

| Symptom | What to check |
|---|---|
| The app window does not open | `start.log` and `realforge.log` in the RealForge folder; try `start.bat` |
| "DLSS 5 not applied" | Click **Log** under the preview for the add-on's reason; try a smaller *Max working size* |
| Render almost identical to the original | Increase NR Intensity / Local Structure / Local Skin |
| Text printed at the bottom of renders | NVIDIA's DLSS indicator: accept the prompt at setup, or use the button in the app banner |
| Video export fails | The render log shows the ffmpeg command and its error |
| Engine won't compile | Install the C++ Build Tools (see Installation), then run `build.bat` |

**Diagnostics** (top right) checks the four DLSS 5 files, initializes NVIDIA NGX and shows the DLSS / NR lines from `ReShade.log`.

## ⚠️ Limitations

- Output is 8-bit SDR (PNG, H.264 / H.265, or ProRes from 8-bit frames). HDR videos are converted to SDR.
- A still image or a video has no depth or motion vectors: the neural renderer works from the picture alone, so fast motion can ghost (enable **Anti-ghosting** in the Video settings).
- Results depend on NVIDIA's model and on the RenoDX add-on version you use.

## 🙏 Credits

- **NVIDIA DLSS / NGX**: [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) SDK (downloaded at build time, not redistributed)
- **ReShade** by crosire: <https://reshade.me>
- **RenoDX** DLSS5 add-on
- **FFmpeg**: <https://ffmpeg.org>

## ⚖️ Disclaimer

RealForge is an independent, unofficial project. It is not affiliated with or endorsed by NVIDIA, ReShade or RenoDX. NVIDIA, DLSS and RTX are trademarks of NVIDIA Corporation. You are responsible for obtaining the DLSS, ReShade and RenoDX files and for complying with their licenses.
