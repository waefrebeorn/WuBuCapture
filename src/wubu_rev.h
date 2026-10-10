/*
 * wubu_rev.h — "revolver" of detected runtime constants.
 *
 * WHY A REVOLVER
 * The program was full of magic numbers (60.0, 16.6667, 90, 1.2, 512, 200) and
 * every one of them was a place where reality and the constant disagreed --
 * which is exactly how the pacing drifted, the warm-up finished 15x too early,
 * and the export interval manufactured its own frame dips.
 *
 * A revolver holds N chambers on a rotating cylinder. Here every chamber is a
 * value the program needs, and each is FILLED BY DETECTION at startup and then
 * never recomputed. Turning the cylinder resolves a chamber by name. Nothing
 * is a bare literal in the hot path any more, and every value carries its own
 * provenance so telemetry can say whether a number was measured, specified or
 * merely defaulted -- which is the difference between "it is 16.67 because we
 * measured it" and "it is 16.67 because we typed it".
 *
 * Chambers are derived from ONE measurement (the source frame rate) rather
 * than each being independently hardcoded, so a different source retunes the
 * whole program by changing one detected fact.
 */
#ifndef WUBU_REV_H
#define WUBU_REV_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wubu_rev wubu_rev;

typedef enum {
    WUBU_CH_SOURCE_HZ = 0,      /* negotiated source frame rate            */
    WUBU_CH_SOURCE_W,           /* negotiated source width                 */
    WUBU_CH_SOURCE_H,           /* negotiated source height                */
    WUBU_CH_PANEL_HZ,           /* panel refresh (integer; may truncate)   */
    WUBU_CH_PANEL_PERIOD_MS,    /* 1000 / panel Hz                         */
    WUBU_CH_PACING_MS,          /* present deadline period                 */
    WUBU_CH_SPIN_TAIL_MS,       /* sleep-then-spin tail before deadline    */
    WUBU_CH_WARMUP_FRAMES,      /* frames to ignore while pipeline settles */
    WUBU_CH_RING_FRAMES,        /* telemetry ring depth                    */
    WUBU_CH_EXPORT_SEC,         /* telemetry export interval               */
    WUBU_CH_LAT_WARN_MS,        /* 1.5x period: dip threshold              */
    WUBU_CH_COUNT
} wubu_chamber;

/* Provenance of a chamber's value. */
typedef enum {
    WUBU_PROV_DEFAULT = 0,      /* no detection available; fallback used  */
    WUBU_PROV_SPEC,             /* constant of the negotiated format      */
    WUBU_PROV_DETECTED,         /* measured from the running system       */
    WUBU_PROV_DERIVED           /* computed from another chamber          */
} wubu_provenance;

extern int wubu_rev_verbose;   /* set before load to narrate detection */

wubu_rev *wubu_rev_create(void);
void      wubu_rev_destroy(wubu_rev *r);

/*
 * Detect and load every chamber.
 *   src_hz    negotiated source rate (MF reports exact numerator/denominator)
 *   src_w/h   negotiated picture size
 *   hwnd      window whose monitor defines the panel
 * Returns 1 on success.
 */
int  wubu_rev_load(wubu_rev *r, HWND hwnd,
                   double src_hz, unsigned src_w, unsigned src_h);

/* Resolve one chamber. Returns the value; *prov (optional) gets its origin. */
double wubu_rev_get(const wubu_rev *r, wubu_chamber c, wubu_provenance *prov);

/* Convenience: resolve as an unsigned / integer count. */
unsigned wubu_rev_get_u(const wubu_rev *r, wubu_chamber c);

const char *wubu_rev_name(wubu_chamber c);
const char *wubu_rev_prov_name(wubu_provenance p);

/* One line per chamber: "name = value [provenance]". Used by --verbose. */
void wubu_rev_dump(const wubu_rev *r, char *out, size_t outLen);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_REV_H */