:: WuBuCapture Build Script for Windows/MinGW64
:: Run from MSYS2 MinGW64 shell

@echo off
echo Building WuBuCapture (modular)...

gcc -O2 -std=c11 -Wall -Isrc -o WuBuCapture.exe ^
  src/wubu_capture_mf.c ^
  src/wubu_panel.c ^
  src/wubu_qdc.c ^
  src/wubu_rev.c ^
  src/wubu_telem.c ^
  src/wubu_stat.c ^
  src/wubu_mode.c ^
  -lmfplat -lmf -lmfreadwrite -lmfuuid ^
  -ld3d11 -ldxgi -ld3dcompiler ^
  -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid -lm

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED!
    exit /b 1
)

echo Build successful: WuBuCapture.exe

if not exist d3dcompiler_47.dll (
    echo Copying d3dcompiler_47.dll...
    copy C:\Windows\System32\d3dcompiler_47.dll . 2>nul
)

echo Done. Run WuBuCapture.exe
