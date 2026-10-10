/*
 * wubu_panel.c — see wubu_panel.h for why this module exists and for the
 * measured evidence behind every refusal to invent a refresh rate.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "wubu_panel.h"

struct wubu_panel {
    int verbose;      /* narrate each detection decision */
};

/* Plausibility band for a display refresh. Below 24 Hz it is a projector or a
   mis-report; above 1000 Hz nothing this code can present to is real. */
#define WUBU_HZ_MIN 24.0
#define WUBU_HZ_MAX 1000.0

wubu_panel *wubu_panel_create(void) {
    wubu_panel *p = (wubu_panel *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->verbose = 0;
    return p;
}

void wubu_panel_destroy(wubu_panel *p) { free(p); }

void wubu_panel_set_verbose(wubu_panel *p, int v) { if (p) p->verbose = v; }

/*
 * Read the CURRENT mode of one device.
 *
 * Only dmDisplayFrequency is trusted, and that is a deliberate limitation, not
 * an oversight. Measured by build/devfreq_diag.exe on this machine: an
 * over-sized dmSize is clipped by EnumDisplaySettingsW (dmSize returns 188) and
 * the bytes past the end of MinGW's DEVMODEW are never written, so
 * dmDisplayFrequencyFractional cannot be read here at all. A caller that
 * invents 59.94 from 59 is fabricating a 0.06 Hz error that then accumulates as
 * a phase error every single frame.
 */
static BOOL read_device(const WCHAR *device, wubu_panel_display *out) {
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm)) return FALSE;
    if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) return FALSE;

    memset(out, 0, sizeof *out);
    wcsncpy(out->device, device, 63);
    out->device[63] = 0;
    /* ASCII copy for logging, filled by wide-char copy below. Device names are
       ASCII (\\.\DISPLAYnn), so a straight narrow copy is correct. */
    size_t n = 0;
    while (device[n] && n < 63) { out->device_a[n] = (char)device[n]; n++; }
    out->device_a[n] = 0;
    out->width      = dm.dmPelsWidth;
    out->height     = dm.dmPelsHeight;
    out->refresh_hz = dm.dmDisplayFrequency;
    return TRUE;
}

double wubu_panel_device_hz(wubu_panel *p, const WCHAR *device) {
    (void)p;
    wubu_panel_display d;
    if (!device || !read_device(device, &d)) return 0.0;
    if ((double)d.refresh_hz < WUBU_HZ_MIN || (double)d.refresh_hz > WUBU_HZ_MAX)
        return 0.0;
    return (double)d.refresh_hz;
}

int wubu_panel_scan(wubu_panel *p, wubu_panel_display *out, int maxOut) {
    DISPLAY_DEVICEW dd;
    int n = 0, count = 0;

    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    while (maxOut <= 0 || count < maxOut) {
        if (!EnumDisplayDevicesW(NULL, (UINT)n++, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        wubu_panel_display d;
        if (!read_device(dd.DeviceName, &d)) continue;
        d.is_primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? TRUE : FALSE;
        d.picked_for = WUBU_PANEL_PICK_NONE;
        out[count++] = d;

        if (p && p->verbose)
            fprintf(stderr, "[panel] %s  %ux%u @ %lu Hz%s\n",
                    d.device_a, d.width, d.height,
                    (unsigned long)d.refresh_hz,
                    d.is_primary ? "  (PRIMARY)" : "");
    }
    return count;
}

/*
 * Which monitor is the user actually looking at?
 *
 * A window's centre point is the honest answer when the window has been placed,
 * and is strictly better than MonitorFromWindow(NEAREST) -- NEAREST is evaluated
 * against a rectangle that may not have a position yet, which is how a window
 * on DISPLAY22 got paced against DISPLAY23's 120 Hz. But at F-time the user is
 * explicitly saying "put it over THERE", and the only reliable statement of
 * where "there" is, is where their pointer is.
 */
static HMONITOR pick_monitor(HWND hwnd, BOOL useCursor, wubu_panel_pick *how) {
    if (useCursor) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
            if (m) { *how = WUBU_PANEL_PICK_CURSOR; return m; }
        }
    }
    if (hwnd) {
        RECT wr;
        if (GetWindowRect(hwnd, &wr) && wr.right > wr.left && wr.bottom > wr.top) {
            POINT ctr = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
            HMONITOR m = MonitorFromPoint(ctr, MONITOR_DEFAULTTOPRIMARY);
            if (m) { *how = WUBU_PANEL_PICK_WINDOW; return m; }
        }
    }
    *how = WUBU_PANEL_PICK_PRIMARY;
    POINT origin = { 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

double wubu_panel_resolve(wubu_panel *p, HWND hwnd, BOOL useCursor,
                          wubu_panel_display *out) {
    wubu_panel_pick how = WUBU_PANEL_PICK_NONE;
    HMONITOR mon = pick_monitor(hwnd, useCursor, &how);
    if (!mon) {
        if (p && p->verbose)
            fprintf(stderr, "[panel] no monitor resolved; refusing to guess a rate\n");
        if (out) memset(out, 0, sizeof *out);
        return 0.0;
    }

    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
        if (out) memset(out, 0, sizeof *out);
        return 0.0;
    }

    wubu_panel_display d;
    memset(&d, 0, sizeof d);
    if (!read_device(mi.szDevice, &d)) {
        if (p && p->verbose) {
            /* d is unpopulated on this path, so name the device from mi.szDevice.
               That needs a narrow copy: logging a WCHAR here would reintroduce
               the mixed-orientation truncation this module exists to prevent. */
            char dev[64];
            size_t k = 0;
            while (mi.szDevice[k] && k < 63) { dev[k] = (char)mi.szDevice[k]; k++; }
            dev[k] = 0;
            fprintf(stderr, "[panel] %s: could not read its current mode\n", dev);
        }
        if (out) memset(out, 0, sizeof *out);
        return 0.0;
    }

    d.picked_for = how;
    d.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? TRUE : FALSE;

    if (p && p->verbose) {
        const char *why =
            (how == WUBU_PANEL_PICK_CURSOR) ? "under the cursor" :
            (how == WUBU_PANEL_PICK_WINDOW) ? "window centre" : "PRIMARY fallback";
        fprintf(stderr, "[panel] target %s %ux%u @ %lu Hz  (chosen by %s)\n",
                d.device_a, d.width, d.height,
                (unsigned long)d.refresh_hz, why);
        fprintf(stderr, "[panel] refresh is the INTEGER field: this SDK/OS "
                        "delivers no fractional rate, so it may be truncated\n");
    }

    if (out) *out = d;

    if ((double)d.refresh_hz < WUBU_HZ_MIN || (double)d.refresh_hz > WUBU_HZ_MAX)
        return 0.0;                  /* implausible: report unknown, not a guess */
    return (double)d.refresh_hz;
}

/*
 * How many refreshes should one source frame occupy?
 *
 * The eye is reading the pattern of REPEATS, not the average rate. When the
 * panel rate is an exact integer multiple of the source rate -- 60 on 60, or 60
 * on 120 -- every frame gets the same number of refreshes and the motion is
 * even. When it is not -- 60 fps on a 100 Hz panel is 1.667 slots per frame --
 * the repeat pattern has to be uneven, and the unevenness is the judder. That
 * is arithmetic about the display, not a defect in any particular program.
 */
unsigned wubu_panel_sync_interval(double source_hz, double panel_hz) {
    if (source_hz <= 1.0 || panel_hz <= 1.0) return 0;
    double ratio = panel_hz / source_hz;
    double nearest = floor(ratio + 0.5);
    if (nearest < 1.0) return 0;
    /*
     * TOLERANCE MUST BE DERIVED FROM THE FIELD WE ACTUALLY HAVE, NOT PICKED.
     *
     * dmDisplayFrequency is an INTEGER, so a 59.94 Hz panel is reported as 59 --
     * a truncation error of up to 1/59 = 1.70%. A flat 0.5% tolerance therefore
     * REJECTS the most important case on this machine (60 fps on the 59 Hz main
     * panel, which is genuinely 1:1) and hands the caller "no even interval"
     * when the display plainly has one. The unit test caught exactly that.
     *
     * So the band is the quantisation of the reading plus a small margin for
     * the source rate's own imprecision. 1/panel_hz is provably the worst case
     * for an integer Hz field, so the band shrinks automatically as the panel
     * rate grows -- which is correct, because that is when the truncation
     * actually stops mattering.
     */
    double tol = (1.0 / panel_hz) + 0.002;
    if (fabs(ratio - nearest) / nearest > tol) return 0;
    return (unsigned)nearest;
}

const char *wubu_panel_ratio_note(double source_hz, double panel_hz) {
    static char buf[320];
    if (source_hz <= 1.0 || panel_hz <= 1.0) {
        snprintf(buf, sizeof buf,
                 "source/panel rate unknown: cannot reason about cadence");
        return buf;
    }
    double ratio = panel_hz / source_hz;
    unsigned iv = wubu_panel_sync_interval(source_hz, panel_hz);
    if (iv)
        snprintf(buf, sizeof buf,
                 "panel %.2f Hz / source %.2f Hz = %.3f -> EVEN, %u refresh(es) per frame",
                 panel_hz, source_hz, ratio, iv);
    else {
        double low  = floor(ratio);
        double frac = ratio - low;
        snprintf(buf, sizeof buf,
                 "panel %.2f Hz / source %.2f Hz = %.3f -> NOT EVEN: %d then %d "
                 "refreshes per frame repeating (fraction %.3f). No integer sync "
                 "interval is cadence-correct here; the repeats must alternate, and "
                 "that is the judder.",
                 panel_hz, source_hz, ratio,
                 (int)low, (int)low + 1, frac);
    }
    return buf;
}
