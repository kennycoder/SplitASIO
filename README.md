# SplitASIO (Windows x64)

_**DISCLAIMER/RANT**: I really didn't want to do this project but unfortunately I got utterly upset with all the legacy tooling to force windows work with multiple ASIO devices at the same time. I'm not Windows user myself but I have a couple of pieces of software that are unfortunately windows native and have issues with WINE. It's most likely a unique edge case, but what the hell, it's useful to me, so hopefully will be to someone else too._

---

A lightweight, pure **ASIO-to-ASIO** split input/output driver for 64-bit Windows that pairs two independent ASIO hardware devices into a single ASIO driver for DAWs and standalone amp sims (for example, recording guitar through a **Focusrite Scarlett Solo** and monitoring through a **FiiO K17** DAC) — with zero WDM, WASAPI, MME, or Windows audio mixer involvement.

## Features (if you can call them that..)

- **Pure ASIO-to-ASIO**: Hosts two native 64-bit ASIO COM drivers simultaneously (`Input ASIO` → `DAW / Amp Sim` → `Output ASIO`).
- **Zero Extra Output Latency**: The output ASIO device acts as the master clock driving the DAW, and DAW output buffers alias the output hardware's DMA buffers directly (zero-copy).
- **Automatic Clock-Drift Compensation**: Two independent USB interfaces have independent crystal oscillators that inevitably drift apart. Incoming audio passes through a lock-free SPSC ring buffer with a **[PI-controlled 4-point Catmull-Rom cubic Hermite resampler](https://www.dsprelated.com/showcode/3.php)** that tracks clock drift (typically a few dozen ppm) inaudibly to prevent clicks, pops, or xruns over long sessions.
- **Native Win32 Control Panel**: Open from your DAW's **ASIO Control Panel** button or via the standalone **SplitASIO Settings** Start Menu app to select Input/Output ASIO devices, launch each device's native control panel, tune the input buffer margin, and view live telemetry (sample rate, buffer fill, clock drift in ppm, and xruns).
- **Zero External Dependencies**: Compiled with the static MSVC runtime (`/MT`) — no Visual C++ Redistributable required.

---

## Installation

1. Download and run **`SplitASIO-Setup-1.0.0-x64.exe`** from here (github releases that is).
2. The installer copies the driver to `C:\Program Files\SplitASIO`, automatically registers the 64-bit ASIO COM driver, and adds **SplitASIO Settings** to your Start Menu.
3. At the end of setup, keep **Configure SplitASIO input/output devices now** checked to pick your **Input ASIO Device** and **Output ASIO Device** (you can also use **Device Panel...** to set matching low buffer sizes, e.g., `64` or `128` samples, on both interfaces).
4. Open your 64-bit DAW or standalone amp sim (Reaper, Neural DSP, Ableton Live, Cubase, Studio One, etc.) and choose:
   - **Audio System / Driver Type**: `ASIO`
   - **ASIO Driver / Device**: `SplitASIO`

To uninstall at any time, use **Windows Settings → Apps → Installed apps → SplitASIO** (or the Start Menu shortcut). This automatically unregisters the ASIO driver cleanly.

### Configuration File

Settings are stored per-user in `%APPDATA%\SplitASIO\config.ini`:

```ini
[Devices]
InputDriver=Focusrite USB ASIO
OutputDriver=FiiO ASIO Driver

[Audio]
; 0 = Auto (1.5x max buffer size, lowest safe latency)
RingBufferFrames=0
```

---

## Building from Source (Developers)

Requires **Visual Studio 2022** (MSVC x64) and **CMake 3.20+**:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs in `build\Release\`:
- `split_asio.dll` — 64-bit Windows ASIO COM driver (static `/MT` CRT)
- `split_asio_config.exe` — standalone GUI settings utility
- `asio_probe.exe` — diagnostic CLI to enumerate 64-bit ASIO drivers and test simultaneous streaming
- `bridge_test.exe` — end-to-end COM integration test for `split_asio.dll`

### Manual Registration (Dev Workflow)

To register/unregister `build\Release\split_asio.dll` directly without building the installer (prompts for Administrator UAC):

```powershell
.\scripts\register.ps1
.\scripts\unregister.ps1
```

### Building the Release Installer

Requires **Inno Setup 6** (`winget install JRSoftware.InnoSetup`):

```powershell
.\scripts\build_release.ps1
```

This compiles the Release x64 binaries and packages `dist\SplitASIO-Setup-1.0.0-x64.exe` along with its SHA-256 checksum.

---

## Project Layout

```text
├── CMakeLists.txt
├── installer/
│   └── split_asio.iss          # Inno Setup 6 installer script (64-bit + regserver)
├── scripts/
│   ├── build_release.ps1       # One-command Release + Installer build
│   ├── register.ps1            # Dev helper: regsvr32 register
│   └── unregister.ps1          # Dev helper: regsvr32 /u unregister
└── src/
    ├── asio_abi.h              # Clean-room binary-compatible Steinberg IASIO x64 ABI
    ├── asio_registry.h/.cpp    # HKLM\SOFTWARE\ASIO driver enumeration
    ├── asio_host.h/.cpp        # Child IASIO COM driver wrapper
    ├── bridge_driver.h/.cpp    # Main IASIO COM implementation
    ├── ring_buffer.h           # Lock-free SPSC ring buffer + PI Catmull-Rom resampler
    ├── sample_convert.h        # Real-time ASIO <-> float32 sample converters
    ├── config.h/.cpp           # %APPDATA%\SplitASIO\config.ini persistence
    ├── control_panel.h/.cpp    # Native Win32 Control Panel GUI + live telemetry
    ├── dll_main.cpp            # COM class factory & DllRegisterServer/DllUnregisterServer
    ├── exports.def             # Unmangled COM DLL exports
    ├── version.rc              # Windows file/product version resource
    └── tools/
        ├── config_app.cpp      # Standalone GUI settings launcher
        ├── asio_probe.cpp      # Hardware ASIO probe tool
        └── bridge_test.cpp     # End-to-end COM bridge test
```
