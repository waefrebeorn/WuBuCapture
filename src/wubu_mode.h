/*
 * wubu_mode.h — exact display mode / refresh-rate detection for WuBuCapture.
 * Opaque, self-contained, C11. No dependency on capture or D3D code.
 *
 * WHY: dmDisplayFrequency truncates (59.94 Hz reads back as 59). Pacing from
 * the integer is a systematic phase error and is a proven cause of frame dips.
 * Never derive a pacing period from dmDisplayFrequency; use
 * wubu_mode_period_ms_for_window().
 */
#ifndef WUBU_MODE_H
#define WUBU_MODE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact refresh (Hz) of the monitor owning hwnd, or 0.0 if undeterminable. */
double wubu_mode_exact_hz_for_window(HWND hwnd);

/* Windowed period (ms) from the exact rate; 0.0 if undeterminable. */
double wubu_mode_period_ms_for_window(HWND hwnd);

/* "\\.\DISPLAYnn" for the monitor owning hwnd. Returns 0 on failure. */
int wubu_mode_device_for_window(HWND hwnd, wchar_t *out, size_t outLen);

/* |panel_hz - source_hz| in hundredths of a Hz. */
int wubu_mode_source_drift_hz(double panel_hz, double source_hz);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_MODE_H */