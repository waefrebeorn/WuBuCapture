/*
 * wubu_qdc.c -- read the TRUE fractional refresh rate via QueryDisplayConfig.
 *
 * See wubu_qdc.h for why this exists and what it is worth. The short version:
 * EnumDisplaySettings only exposes the integer dmDisplayFrequency, which
 * truncates this box's 59.94 Hz panel to 59, and the fractional DEVMODE field
 * is provably unreadable here (devfreq_diag.c). QueryDisplayConfig returns the
 * rate as an exact DISPLAYCONFIG_RATIONAL instead.
 */
#include "wubu_qdc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>
#include <stdlib.h>

/*
 * -- THE API CONTRACT, LEARNED THE HARD WAY --------------------------------
 * The original body here used the "call QueryDisplayConfig with NULL arrays to
 * learn the sizes" pattern. That pattern DOES NOT EXIST for this API and returns
 * ERROR_INVALID_PARAMETER (87) for every flag combination -- which looks exactly
 * like an ABI or permissions fault and sent the investigation down a wrong path.
 *
 * The documented contract, from
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig :
 *
 *   pathArray     -- "This parameter cannot be NULL."
 *   modeInfoArray -- "This parameter cannot be NULL."
 *
 *   "...As the GetDisplayConfigBufferSizes function can only determine the
 *    required array size at a particular moment in time, it is possible that
 *    between calls to GetDisplayConfigBufferSizes and QueryDisplayConfig the
 *    system configuration will change and the provided array sizes will no
 *    longer be sufficient... the caller should call GetDisplayConfigBufferSizes
 *    again to get the new array sizes."
 *
 *   "If the QDC_ALL_PATHS or QDC_ONLY_ACTIVE_PATHS flag is set in the Flags
 *    parameter, the currentTopologyId parameter must be set to NULL; otherwise,
 *    QueryDisplayConfig returns ERROR_INVALID_PARAMETER."
 *
 * Correct sequence: GetDisplayConfigBufferSizes -> calloc both -> QueryDisplayConfig
 * with a NULL topology id -> on ERROR_INSUFFICIENT_BUFFER, re-size and retry.
 */
double wubu_qdc_refresh_hz(void *hwnd, unsigned *out_num, unsigned *out_den) {
    UINT32 n_paths = 0, n_modes = 0;
    LONG rc;

    /*
     * QDC_ONLY_ACTIVE_PATHS: the mode each display is RUNNING now, not every
     * mode it could run. currentTopologyId is NULL because the flag is not
     * QDC_DATABASE_CURRENT, which the docs require.
     */
    const UINT32 flags = QDC_ONLY_ACTIVE_PATHS;

    double hz = 0.0;

    /* Bounded retry, 3 passes: this runs on the startup path and a display that
       keeps reconfiguring must not hang us. */
    for (int attempt = 0; attempt < 3 && hz <= 0.0; attempt++) {
        rc = GetDisplayConfigBufferSizes(flags, &n_paths, &n_modes);
        if (rc != 0) return 0.0;
        if (n_paths == 0) return 0.0;

        DISPLAYCONFIG_PATH_INFO *paths =
            (DISPLAYCONFIG_PATH_INFO *)calloc(n_paths, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes =
            (DISPLAYCONFIG_MODE_INFO *)calloc(n_modes ? n_modes : 1, sizeof *modes);
        if (!paths || !modes) { free(paths); free(modes); return 0.0; }

        rc = QueryDisplayConfig(flags, &n_paths, paths, &n_modes, modes, NULL);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            /* Display reconfigured between sizing and fetch: re-size and retry. */
            free(paths); free(modes);
            continue;
        }
        if (rc != 0) { free(paths); free(modes); return 0.0; }

        /*
         * FIND THE PATH FOR *OUR* WINDOW'S MONITOR.
         *
         * The first version ignored its hwnd argument and returned the first
         * ACTIVE path's rate. That is a live bug: with the window on a 120 Hz
         * portrait panel the integer field said 120 while this returned 59.936
         * for a different display, so the pacer locked to the wrong panel and
         * derived a 2-refresh grid for a display it was not driving.
         *
         * A SECOND attempt tried to match via DisplayConfigGetDeviceInfo's
         * monitorDevicePath against MONITORINFOEXW::szDevice. That is also wrong
         * and it failed silently -- returning 0.0 -- because the two name spaces
         * do not meet: szDevice is the \\.\DISPLAYnn that EnumDisplaySettings
         * also reports, whereas monitorDevicePath is the PHYSICAL connector path
         * (\\?\DISPLAY#...). probe_qdc.exe shows the rationals are per-path and
         * carry no \\.\DISPLAYnn name at all.
         *
         * What actually correlates: the WINDOW's pixel size against each path's
         * SOURCE mode. That is how a panel gets identified in practice, and it
         * cannot silently pick a different panel's clock when it fails -- it just
         * reports no match.
         *
         * If the window size matches nothing (e.g. the window is on a third
         * display whose source mode differs), we return 0.0 rather than guessing:
         * a wrong clock is worse than no clock, because the caller can always fall
         * back to letting the compositor pace, but cannot detect a bad number.
         */
        /* Match on WHERE THE WINDOW IS, not its size.
             *
             * Size matching was wrong and is now known to be wrong, from
             * probe_qdc2.exe output on this machine:
             *
             *   #  source mode   position      refresh
             *   0  3440x1440    (0,0)         59936/1000
             *   1  2560x1440    (-1440,-512)  119998/1000
             *   2  2560x1440    (3440,0)      119998/1000
             *
             * Our window is 1920x1080, which equals NO source mode here, so a
             * size comparison can never succeed. The source POSITION is what
             * identifies a desktop display, so we match the window's centre
             * against it: the window centre must land inside the source's desktop
             * rectangle (position .. position + width/height).
             */
        POINT wc = { 0, 0 };
        int have_pos = 0;
        if (hwnd && IsWindow((HWND)hwnd)) {
            RECT r;
            if (GetWindowRect((HWND)hwnd, &r)) {
                wc.x = (r.left + r.right) / 2;
                wc.y = (r.top + r.bottom) / 2;
                have_pos = 1;
            }
        }

        for (UINT32 i = 0; i < n_paths; i++) {
            if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;

            DISPLAYCONFIG_RATIONAL rr = paths[i].targetInfo.refreshRate;
            if (rr.Denominator == 0 || rr.Numerator == 0) continue;

            /* Select the display whose desktop rectangle contains the window. */
            if (have_pos) {
                UINT32 smi = paths[i].sourceInfo.modeInfoIdx;
                if (smi == 0xFFFFFFFF || smi >= n_modes) continue;
                if (modes[smi].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;

                LONG sx = modes[smi].sourceMode.position.x;
                LONG sy = modes[smi].sourceMode.position.y;
                LONG sw = (LONG)modes[smi].sourceMode.width;
                LONG sh = (LONG)modes[smi].sourceMode.height;

                if (wc.x < sx || wc.x >= sx + sw || wc.y < sy || wc.y >= sy + sh)
                    continue;   /* window centre is on a different display */
            }

            hz = (double)rr.Numerator / (double)rr.Denominator;
            if (out_num) *out_num = rr.Numerator;
            if (out_den) *out_den = rr.Denominator;
            break;
        }

        free(paths); free(modes);
    }

    /*
     * Sanity gate. A refresh rate outside 20-500 Hz is not a display refresh
     * rate; it is a misread struct or an uninitialised field. Returning it
     * would be exactly the fabricated-59.94 failure this module exists to end,
     * so we return "unknown" instead and let the caller refuse to pace.
     */
    if (hz < 20.0 || hz > 500.0) return 0.0;
    return hz;
}
