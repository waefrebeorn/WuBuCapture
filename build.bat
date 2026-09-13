:: WuBuCapture Build Script for Windows/MinGW64
:: Run from MSYS2 MinGW64 shell

@echo off
echo Building WuBuCapture...

gcc -O2 -std=c11 -o WuBuCapture.exe WuBuCapture.c ^
  -lmfplat -lmf -lmfreadwrite -lmfuuid ^
  -ld3d11 -ldxgi -ld3dcompiler ^
  -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED!
    exit /b 1
)

echo Build successful: WuBuCapture.exe

:: Copy required DLL if not present
if not exist d3dcompiler_47.dll (
    echo Copying d3dcompiler_47.dll...
    copy C:\Windows\System32\d3dcompiler_47.dll . 2>nul
    if %ERRORLEVEL% NEQ 0 (
        echo WARNING: Could not copy d3dcompiler_47.dll - may need to copy manually
    )
)

echo Done. Run WuBuCapture.exe
