/*
 * wubu_rev.c — the revolver of detected runtime constants. See wubu_rev.h.
 *
 * DESIGN
 * One fixed cylinder of chambers, all filled by wubu_rev_load(). Nothing in the
 * hot path is a bare literal: the pacer, the warm-up gate, the spin tail, the
 * ring depth and the dip threshold are all resolved from here.
 *
 * Everything derives from a SINGLE measurement -- the negotiated source frame
 * rate -- so a 30 or 120 Hz source retunes the entire program by changing one
 * detected fact, not by editing six constants and hoping they stay consistent.
 * That coupling was the actual defect: the pacer used 60 Hz while the warm-up
 * used 90 frames and the ring used 512, so they silently disagreed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "wubu_rev.h"

typedef struct {
    double        value;
    wubu_provenance prov;
} chamber_slot;

/* Set by the host before wubu_rev_load() so detection can narrate itself. */
int wubu_rev_verbose = 0;

struct wubu_rev {
    chamber_slot c[WUBU_CH_COUNT];
};

/* ---- fallback when detection fails: sane, and labelled as default ---- */
#define DEF_SOURCE_HZ      60.0
#define DEF_SOURCE_W       1920
#define DEF_SOURCE_H       1080
#define DEF_PANEL_HZ       60.0
#define DEF_WARMUP_SEC     1.5
#define DEF_RING_SEC       4.0
#define DEF_SPIN_TAIL_MS   1.2
#define DEF_EXPORT_SEC     120

static void put(wubu_rev *r, wubu_chamber c, double v, wubu_provenance p) {
    r->c[c].value = v;
    r->c[c].prov  = p;
}

wubu_rev *wubu_rev_create(void) {
    wubu_rev *r = (wubu_rev *)calloc(1, sizeof *r);
    return r;
}

void wubu_rev_destroy(wubu_rev *r) { free(r); }

int wubu_rev_load(wubu_rev *r, HWND hwnd, double src_hz, unsigned src_w, unsigned src_h) {
    if (!r) return 0;

    /* ---- 1. the one measurement everything else derives from ---- */
    double hz = (src_hz > 1.0 && src_hz < 1000.0) ? src_hz : DEF_SOURCE_HZ;
    put(r, WUBU_CH_SOURCE_HZ, hz,
        (src_hz > 1.0 && src_hz < 1000.0) ? WUBU_PROV_DETECTED : WUBU_PROV_DEFAULT);

    put(r, WUBU_CH_SOURCE_W, src_w ? (double)src_w : DEF_SOURCE_W,
        src_w ? WUBU_PROV_DETECTED : WUBU_PROV_DEFAULT);
    put(r, WUBU_CH_SOURCE_H, src_h ? (double)src_h : DEF_SOURCE_H,
        src_h ? WUBU_PROV_DETECTED : WUBU_PROV_DEFAULT);

    /* ---- 2. panel rate ----
     *
     * MEASURED ON THIS MACHINE (see src/refresh_probe.c):
     *   main panel  3440x1440 @  59 Hz   <- the window lives here
     *   portrait    1440x2560 @ 120 Hz
     *
     * Two traps this replaces:
     *  - dmDisplayFrequencyFractional DOES NOT EXIST in this MinGW DEVMODEW, and
     *    "#if defined(dmDisplayFrequencyFractional)" on a STRUCT FIELD is always
     *    false, so the "fractional" path never compiled and quietly fell through.
     *    The 59.940 Hz the app printed all session was a hardcoded fallback
     *    return value, not a measurement.
     *  - the integer field is the only real signal here, and reading the wrong
     *    display returns 120, which inverts the source/panel relationship.
     * So: read the integer field, and SAY WHICH DEVICE it came from.
     */
    double panel_hz = 0.0;
    if (hwnd) {
        /*
         * DO NOT TRUST MonitorFromWindow() HERE.
         * It returned DISPLAY23 (1440x2560 @ 120 Hz) for a window whose rect is
         * (270,481) 1280x720 -- squarely inside DISPLAY22 (0,0 3440x1392). On a
         * multi-display rig "NEAREST" is evaluated against a window that has not
         * been positioned yet, and picking the wrong monitor silently inverts the
         * source/panel relationship.
         *
         * Unambiguous method: ask which monitor actually contains the window's
         * centre point. If the window has no rect yet, fall back to NEAREST but
         * say so in the log rather than pretending it is certain.
         */
        HMONITOR mon = NULL;
        RECT wr;
        BOOL haveRect = GetWindowRect(hwnd, &wr);
        if (haveRect && wr.right > wr.left && wr.bottom > wr.top) {
            POINT ctr = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
            mon = MonitorFromPoint(ctr, MONITOR_DEFAULTTOPRIMARY);
        } else {
            mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        }
        MONITORINFOEXW mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
            DEVMODEW dm;
            memset(&dm, 0, sizeof dm);
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500) {
                panel_hz = (double)dm.dmDisplayFrequency;
                if (wubu_rev_verbose) {
                    /* Narrow logging only. This file's host logs with fprintf,
                       and C11 7.21.2 makes a stream's orientation permanent -- a
                       single fwprintf here truncated the message at the first
                       space ("monitor chosen by w"). */
                    char dev[64];
                    size_t k = 0;
                    while (mi.szDevice[k] && k < 63) { dev[k] = (char)mi.szDevice[k]; k++; }
                    dev[k] = 0;
                    fprintf(stderr, "panel detected: %s %ux%u @ %.0f Hz  (monitor chosen by %s)\n",
                            dev, dm.dmPelsWidth, dm.dmPelsHeight, panel_hz,
                            haveRect ? "window centre point"
                                     : "NEAREST fallback, window has no rect");
                }
            }
        }
    }
    put(r, WUBU_CH_PANEL_HZ, panel_hz > 0.0 ? panel_hz : DEF_PANEL_HZ,
        panel_hz > 0.0 ? WUBU_PROV_DETECTED : WUBU_PROV_DEFAULT);

    double panel_ms = 1000.0 / r->c[WUBU_CH_PANEL_HZ].value;
    put(r, WUBU_CH_PANEL_PERIOD_MS, panel_ms, WUBU_PROV_DERIVED);

    /*
     * ---- 3. pacing period = the SOURCE period, deliberately NOT the panel ----
     * dmDisplayFrequency truncates (59.94 reads as 59), so pacing from the
     * panel is a systematic 1.6% phase error that accumulates every frame.
     * The source rate is exact, so we pace from the source and stay 1:1 with it.
     */
    double pacing_ms = 1000.0 / hz;
    put(r, WUBU_CH_PACING_MS, pacing_ms, WUBU_PROV_SPEC);

    /*
     * ---- 4. spin tail, scaled to the period instead of a magic 1.2 ms ----
     * Must be long enough to cover a scheduler wake-up but short enough that
     * spinning costs nothing: ~7% of the period, clamped to a sane band.
     */
    double tail = pacing_ms * 0.07;
    if (tail < 0.30) tail = 0.30;
    if (tail > 3.00) tail = 3.00;
    put(r, WUBU_CH_SPIN_TAIL_MS, tail, WUBU_PROV_DERIVED);

    /* ---- 5. warm-up: a fixed TIME of settle time, converted to frames ---- */
    double warm_s = DEF_WARMUP_SEC;
    put(r, WUBU_CH_WARMUP_FRAMES, warm_s * hz, WUBU_PROV_DERIVED);

    /* ---- 6. ring depth: a fixed TIME of history, converted to frames ---- */
    double ring_s = DEF_RING_SEC;
    put(r, WUBU_CH_RING_FRAMES, ring_s * hz, WUBU_PROV_DERIVED);

    /* ---- 7. export interval: coarse on purpose ----
     * Proven by A/B: writing ~400 KB every 10 s produced frame dips at exactly
     * the export interval (7 periodic dips at --csv 10, 0 at --csv 200). The
     * export perturbs what it measures, so it is deliberately infrequent. */
    put(r, WUBU_CH_EXPORT_SEC, (double)DEF_EXPORT_SEC, WUBU_PROV_SPEC);

    /* ---- 8. dip threshold = 1.5x the period, the standard screening rule ---- */
    put(r, WUBU_CH_LAT_WARN_MS, pacing_ms * 1.5, WUBU_PROV_DERIVED);

    return 1;
}

double wubu_rev_get(const wubu_rev *r, wubu_chamber c, wubu_provenance *prov) {
    if (!r || c < 0 || c >= WUBU_CH_COUNT) { if (prov) *prov = WUBU_PROV_DEFAULT; return 0.0; }
    if (prov) *prov = r->c[c].prov;
    return r->c[c].value;
}

unsigned wubu_rev_get_u(const wubu_rev *r, wubu_chamber c) {
    double v = wubu_rev_get(r, c, NULL);
    if (v < 0.0) return 0;
    return (unsigned)(v + 0.5);
}

const char *wubu_rev_name(wubu_chamber c) {
    switch (c) {
        case WUBU_CH_SOURCE_HZ:       return "source_hz";
        case WUBU_CH_SOURCE_W:        return "source_w";
        case WUBU_CH_SOURCE_H:        return "source_h";
        case WUBU_CH_PANEL_HZ:        return "panel_hz";
        case WUBU_CH_PANEL_PERIOD_MS: return "panel_period_ms";
        case WUBU_CH_PACING_MS:       return "pacing_ms";
        case WUBU_CH_SPIN_TAIL_MS:    return "spin_tail_ms";
        case WUBU_CH_WARMUP_FRAMES:   return "warmup_frames";
        case WUBU_CH_RING_FRAMES:     return "ring_frames";
        case WUBU_CH_EXPORT_SEC:      return "export_sec";
        case WUBU_CH_LAT_WARN_MS:     return "dip_threshold_ms";
        default:                      return "?";
    }
}

const char *wubu_rev_prov_name(wubu_provenance p) {
    switch (p) {
        case WUBU_PROV_DETECTED: return "detected";
        case WUBU_PROV_SPEC:     return "spec";
        case WUBU_PROV_DERIVED:  return "derived";
        default:                 return "default";
    }
}

void wubu_rev_dump(const wubu_rev *r, char *out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = 0;
    if (!r) { snprintf(out, outLen, "(no revolver)"); return; }
    size_t used = 0;
    for (int i = 0; i < WUBU_CH_COUNT; i++) {
        wubu_provenance p;
        double v = wubu_rev_get(r, (wubu_chamber)i, &p);
        int n = snprintf(out + used, outLen - used, "%-18s = %10.4f  [%s]\n",
                         wubu_rev_name((wubu_chamber)i), v, wubu_rev_prov_name(p));
        if (n < 0 || (size_t)n >= outLen - used) break;
        used += (size_t)n;
    }
}