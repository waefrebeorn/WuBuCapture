/*
 * wubu_mode.c — exact display refresh-rate detection.
 *
 * Self-contained C11 module: opaque, no global state, no C++ syntax, no
 * dependency on the capture or D3D code. WuBuCapture and the standalone mode
 * probe both link it.
 *
 * WHY IT EXISTS
 * dmDisplayFrequency reports a TRUNCATED integer. This panel really runs at
 * 59.94 Hz (60000/1001) but the field reads 59. WuBuCapture paced to
 * 16.949 ms from that integer while the true period is 16.683 ms. That 1.6%
 * error is a systematic PHASE error which accumulates frame after frame and
 * shows up as dips and judder even while the app's own counters look perfect.
 *
 * HOW
 * DXGI_MODE_DESC.RefreshRate is a DXGI_RATIONAL, so it carries the exact
 * fraction; DEVMODE does not. We walk factory -> adapter -> output with the
 * SDK's own vtables (never hand-computed slot indices: IDXGIDeviceSubObject
 * inserts GetDevice at slot 7 and getting that wrong dereferences a device
 * pointer as a vtable) and return the highest active rate we can read.
 *
 * RULE: never derive a pacing period from dmDisplayFrequency.
 */
#define COBJMACROS 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "wubu_mode.h"

/*
 * Deliberately returns 0.0 on this toolchain.
 *
 * DXGI is the only API that carries the exact fraction, and on this MinGW it
 * cannot be called safely: DXGI_MODE_DESC lives in dxgitype.h rather than
 * dxgi.h, IDXGIOutputVtbl has no GetCurrentDisplayMode member at all, and both
 * it and IDXGIAdapterVtbl predate the interfaces they describe. Calling through
 * them produced DXGI_ERROR_NOT_FOUND for the adapters and a hard access
 * violation when the vtable slot was taken by hand. Shipping a crash for a
 * number we can derive better another way is not a trade worth making.
 *
 * Callers must treat 0.0 as "unknown" and fall back to the measured source
 * cadence, which is what actually matters for 1:1 presentation.
 */
double wubu_mode_exact_hz_for_window(HWND hwnd) {
    (void)hwnd;
    return 0.0;
}

double wubu_mode_period_ms_for_window(HWND hwnd) {
    double hz = wubu_mode_exact_hz_for_window(hwnd);
    return hz > 0.0 ? 1000.0 / hz : 0.0;
}

int wubu_mode_device_for_window(HWND hwnd, wchar_t *out, size_t outLen) {
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
        if (outLen) out[0] = 0;
        return 0;
    }
    size_t n = wcslen(mi.szDevice);
    if (n + 1 > outLen) n = outLen - 1;
    memcpy(out, mi.szDevice, n * sizeof(wchar_t));
    out[n] = 0;
    return 1;
}

/*
 * A nominally-60.000 Hz source on an NTSC-derived 59.94 Hz panel does not
 * divide evenly: they drift by ~0.06 Hz, so one frame in ~17 must be held for
 * an extra refresh. That is the difference between "even 60fps" and "one hitch
 * every 17 seconds", and it must be logged, never rounded away.
 */
int wubu_mode_source_drift_hz(double panel_hz, double source_hz) {
    if (panel_hz <= 0.0 || source_hz <= 0.0) return 0;
    double d = panel_hz - source_hz;
    if (d < 0) d = -d;
    return (int)(d * 100.0 + 0.5);
}