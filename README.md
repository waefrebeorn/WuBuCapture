# WuBuCapture

Ultra-low latency capture card viewer for Windows 11. DirectShow/Media Foundation → D3D11 with GPU-accelerated display.

## Features

- **Sub-frame latency**: DXGI tearing present (no vsync wait), lock-free triple-buffered SPSC queue
- **GPU-accelerated**: D3D11 fullscreen quad with bilinear scaling, swap chain resizing for fullscreen
- **Device selection**: Hot-switch between capture devices with number keys (0-9)
- **Borderless fullscreen**: Press `F` for seamless fullscreen on any monitor
- **YUY2 native**: Zero-copy from capture card to GPU texture

## Tearing vs Latency

This app uses `DXGI_PRESENT_ALLOW_TEARING` for zero-latency display. On a fixed-refresh monitor without adaptive sync, you may see minor tearing during fast motion.

**To eliminate tearing without adding lag:**

1. **Enable FreeSync/G-Sync on your monitor** (via OSD menu)
2. **Windows Settings** → System → Display → Graphics → "Variable refresh rate" → On
3. **GPU control panel** → Enable G-Sync Compatible for windowed mode

With adaptive sync active, tearing disappears completely while maintaining zero-lag presentation — the display refreshes exactly when each new frame arrives.

## Controls

| Key | Action |
|-----|--------|
| `F` | Toggle borderless fullscreen |
| `0-9` | Switch capture device |
| `ESC` | Quit |

## Build

### Prerequisites
- Windows 10/11
- [MSYS2](https://www.msys2.org/) with MinGW64
- Required packages: `mingw-w64-x86_64-gcc`
- `d3dcompiler_47.dll` (included in Windows 10/11, or copy from `C:\Windows\System32\`)

### Compile
```bash
gcc -O2 -std=c11 -Wall -Isrc -o WuBuCapture.exe \
  src/wubu_capture_mf.c \
  src/wubu_panel.c \
  src/wubu_qdc.c \
  src/wubu_rev.c \
  src/wubu_telem.c \
  src/wubu_stat.c \
  src/wubu_mode.c \
  -lmfplat -lmf -lmfreadwrite -lmfuuid \
  -ld3d11 -ldxgi -ld3dcompiler \
  -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid -lm
```

Or simply run `build.bat` from an MSYS2 MinGW64 shell.

### Run
```bash
./WuBuCapture.exe
```

> **Note**: `d3dcompiler_47.dll` must be in the same directory or in PATH. Copy from `C:\Windows\System32\` if missing.

## Architecture

```
Media Foundation SourceReader (capture thread)
  ↓ YUY2 frame
  ↓ Lock-free double-buffer swap
  ↓ SetEvent

Main thread (MsgWaitForMultipleObjects)
  ↓ wubu_panel: resolve monitor under window
  ↓ wubu_qdc: read true rational refresh via QueryDisplayConfig
  ↓ wubu_stat: measure source interval from capture timestamps
  ↓ D3D11 UpdateSubresource upload (no Map/Unmap)
  ↓ Fullscreen quad + bilinear sampler
  ↓ Present(0) — paced by measured source interval, gated on new frame

Modules (src/):
  wubu_capture_mf.c  — main loop, capture, render, present
  wubu_panel.c       — display enumeration, window/cursor monitor selection
  wubu_qdc.c         — exact rational refresh rate via QueryDisplayConfig
  wubu_rev.c         — telemetry logging
  wubu_telem.c       — CSV export
  wubu_stat.c        — interval statistics (p50/p95/p99)
  wubu_mode.c        — display mode helpers
```

## Measured Performance (DISPLAY22 @ 99.982 Hz)

| Mode | fps | sd | worst gap |
|------|-----|----|-----------|
| Windowed | 60.0 | 0.14–0.40 ms | 18.36 ms |
| Fullscreen | 60.03 | 0.006 ms | 16.70 ms |

## License

WaefreBeorn Umbrella License v3.0
