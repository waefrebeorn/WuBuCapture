/*
 * wubu_panel.h — display topology and TRUE refresh reading.
 *
 * WHY THIS MODULE EXISTS
 * ----------------------
 * WuBuCapture spent a session being lied to by its own refresh code. Three
 * separate mechanisms produced false rates, and each one silently inverted the
 * source/panel relationship and manufactured judder:
 *
 *   1. EnumDisplaySettingsW(NULL, ...) describes the PRIMARY monitor, not the
 *      monitor the window is actually on. On this three-display box that read
 *      120 Hz for a window on the 59 Hz panel.
 *   2. DEVMODEW in MinGW-w64 has NO dmDisplayFrequencyFractional member. That
 *      is a STRUCT FIELD, not a macro, so "#if defined(dmDisplayFrequency-
 *      Fractional)" is ALWAYS false. The "fractional" branch therefore never
 *      compiled, and the "59.940 Hz" the app printed for a whole session was a
 *      hardcoded `return 16.6833` fallback wearing a measurement's clothes.
 *   3. A wrong-offset read of the fractional field is worse than no read: it
 *      returns a plausible-looking number that is not the display's rate.
 *
 * MEASURED on this machine (build/devfreq_diag.exe, the poison-byte probe):
 * asking EnumDisplaySettingsW for an over-sized DEVMODEW gets the request
 * CLIPPED -- dmSize comes back 188, and 0xAA poison past the end of the
 * struct is untouched. So the fractional tail genuinely does not arrive here.
 * The integer field is the ONLY truthful signal, and this module says so out
 * loud instead of inventing precision.
 *
 * DESIGN
 * Opaque struct, minimal includes, no globals, no hidden state. One job:
 * enumerate displays and report what is provably true about each.
 */
#ifndef WUBU_PANEL_H
#define WUBU_PANEL_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the target monitor was chosen. Telemetry logs this, because "which
   monitor" was the exact question whose wrong answer cost a session. */
typedef enum {
    WUBU_PANEL_PICK_NONE = 0,   /* nothing resolved                          */
    WUBU_PANEL_PICK_WINDOW,    /* monitor containing the window centre      */
    WUBU_PANEL_PICK_CURSOR,     /* monitor under the mouse (F-time target)  */
    WUBU_PANEL_PICK_PRIMARY     /* fallback: the primary monitor             */
} wubu_panel_pick;

typedef struct wubu_panel wubu_panel;

/* An immutable snapshot of the display topology at one instant. */
typedef struct {
    WCHAR            device[64];   /* e.g. \\.\DISPLAY23                    */
    /*
     * The SAME device name as a plain ASCII string.
     *
     * This is not redundancy, it is a correctness requirement. C11 7.21.2: once a
     * stream's orientation is set (byte or wide) it cannot change, and mixing
     * fprintf and fwprintf on the same stream is undefined. The capture host
     * logs most things with fprintf while display names are wide, so a single
     * fwprintf(stderr, L"...%ls...") silently truncates the line at the first
     * space -- which is exactly how "monitor chosen by w" and
     * "source 60.000 fps -> p" appeared instead of real text. Device names are
     * ASCII by construction, so carrying an ASCII copy lets every log line use
     * one orientation and one printf family.
     */
    char             device_a[64];
    UINT             width, height;
    UINT             refresh_hz;  /* integer ONLY; see header. Never faked. */
    BOOL             is_primary;
    wubu_panel_pick  picked_for;   /* only set on the resolved target      */
} wubu_panel_display;

wubu_panel *wubu_panel_create(void);
void       wubu_panel_destroy(wubu_panel *p);

/* Narrate every detection decision to stderr. Off by default. */
void wubu_panel_set_verbose(wubu_panel *p, int v);

/*
 * Enumerate every attached display. Returns the count, or 0 on failure.
 * Safe to call repeatedly: the caller owns the returned array.
 */
int  wubu_panel_scan(wubu_panel *p, wubu_panel_display *out, int maxOut);

/*
 * Resolve the monitor to pace against, and read its TRUE integer refresh.
 *
 *   hwnd    window whose CENTRE picks the monitor (may be NULL)
 *   useCursor  when true, the monitor under the cursor wins instead -- this is
 *              the F-time path: the user is telling us which display they want
 *              by where they are looking, so we must measure THAT display and
 *              not one we guessed at startup.
 *
 * Returns the resolved refresh in Hz, or 0.0 if no display could be resolved.
 * 0.0 is a real, meaningful answer meaning "unknown" -- callers must refuse to
 * pace rather than substitute a made-up number.
 *
 * *out (optional) receives the full record, including the device name and how
 * the monitor was chosen, so the caller can log both.
 */
double wubu_panel_resolve(wubu_panel *p, HWND hwnd, BOOL useCursor,
                          wubu_panel_display *out);

/* Read one device's integer refresh by name. Returns 0.0 when unavailable. */
double wubu_panel_device_hz(wubu_panel *p, const WCHAR *device);

/*
 * The number of display refreshes each source frame should occupy, as an
 * integer sync interval.
 *
 *   panel 59 Hz  / 60 fps source -> ratio 0.98, not an integer. Interval 1.
 *   panel 60 Hz  / 60 fps source -> ratio 1.00, interval 1. (Even, judder free.)
 *   panel 120 Hz / 60 fps source -> ratio 2.00, interval 2. (Even, judder free.)
 *   panel 100 Hz / 60 fps source -> ratio 1.67, NOT integral. No integer
 *                                 interval is even; see wubu_panel_ratio_note.
 *
 * Returns 0 when the ratio is not within tolerance of a whole number, which
 * tells the caller that no integer sync interval can be cadence-correct here.
 */
unsigned wubu_panel_sync_interval(double source_hz, double panel_hz);

/* Human-readable verdict on the source/panel ratio. Never returns NULL.
   Returns ASCII (not WCHAR) so the caller can log it with fprintf without
   mixing printf orientations on stderr -- see the device_a note in the header. */
const char *wubu_panel_ratio_note(double source_hz, double panel_hz);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_PANEL_H */
