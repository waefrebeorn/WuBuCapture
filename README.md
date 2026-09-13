# WuBuCapture

Ultra-low latency capture card viewer for Windows 11. DirectShow/Media Foundation → D3D11 with GPU-accelerated display.

## Features

- **Sub-frame latency**: DXGI tearing present (no vsync wait), lock-free triple-buffered SPSC queue
- **GPU-accelerated**: D3D11 fullscreen quad with bilinear scaling, swap chain resizing for fullscreen
- **Device selection**: Hot-switch between capture devices with number keys (0-9)
- **Borderless fullscreen**: Press `F` for seamless fullscreen on any monitor
- **YUY2 native**: Zero-copy from capture card to GPU texture

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
gcc -O2 -std=c11 -o WuBuCapture.exe WuBuCapture.c \
  -lmfplat -lmf -lmfreadwrite -lmfuuid \
  -ld3d11 -ldxgi -ld3dcompiler \
  -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid
```

### Run
```bash
./WuBuCapture.exe
```

> **Note**: `d3dcompiler_47.dll` must be in the same directory or in PATH. Copy from `C:\Windows\System32\` if missing.

## Architecture

```
Media Foundation SourceReader (capture thread)
  ↓ YUY2 frame (zero-copy)
  ↓ Lock-free buffer swap
  ↓ SetEvent
Main thread (MsgWaitForMultipleObjects)
  ↓ D3D11 dynamic texture upload (CPU YUV→RGB)
  ↓ Fullscreen quad + bilinear sampler
  ↓ Present(0, DXGI_PRESENT_ALLOW_TEARING)
```

## License

WaefreBeorn Umbrella License v3.0
