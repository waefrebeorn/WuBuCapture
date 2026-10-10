/*
 * wubu_telem.h — frame-timing telemetry: record, analyse, export.
 *
 * A proper timing suite, not ad-hoc counters. Built to the standard frame-time
 * analysis practice:
 *   - a fixed-capacity RING of per-frame records so a stall is preserved with
 *     its exact timestamp instead of being averaged away (research on frame
 *     time graphs is explicit that averages hide the spikes that ARE the
 *     stutter; a block of spikes close together is what the eye sees)
 *   - PERCENTILES (p50/p95/p99/p99.9), because a mean cannot express a tail
 *     and a tail is what stutter looks like
 *   - a HISTOGRAM on export, because clusters reveal the mode: a bimodal
 *     distribution means alternating frame durations, a single spike means one
 *     stall, and those need completely different fixes
 *   - CSV so the data can leave this process and be compared across runs
 *
 * Opaque type, self-contained C11, no dependency on capture or D3D code.
 */
#ifndef WUBU_TELEM_H
#define WUBU_TELEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field selectors for percentile/histogram queries. */
enum {
    WUBU_TELEM_CAP_IV = 0,   /* ms between arriving capture samples (upstream) */
    WUBU_TELEM_PRES_IV = 1,  /* ms between our Present() calls (our pacing)  */
    WUBU_TELEM_LAT = 2,      /* ms from capture arrival to drawing the frame */
    WUBU_TELEM_FIELD_COUNT
};

typedef struct wubu_telem wubu_telem;

/* capacity == 0 selects a sensible default (about 4 minutes at 60fps). */
wubu_telem *wubu_telem_create(size_t capacity);
void        wubu_telem_destroy(wubu_telem *t);

/* Record one frame. new_frame != 0 means this Present carried fresh data. */
void wubu_telem_record(wubu_telem *t, double t_ms,
                       double cap_iv_ms, double pres_iv_ms, double lat_ms,
                       int new_frame);

/* Number of valid records currently held. */
size_t wubu_telem_count(const wubu_telem *t);

/* Records dropped by the ring since start: non-zero means the buffer is too
   small to hold the interval you care about, and any gap analysis is unsound. */
unsigned long wubu_telem_overwritten(const wubu_telem *t);

/* Percentile (q in 0..1) of a field, in ms. 0.0 when no samples. */
double wubu_telem_percentile(const wubu_telem *t, int field, double q);

/* Mean / min / max / stddev of a field, in ms. */
void   wubu_telem_stats(const wubu_telem *t, int field,
                       double *mean, double *min, double *max, double *stddev);

/*
 * Count intervals of a field strictly greater than `threshold_ms`.
 * This is the "how many bad frames are in this window" question, which is the
 * one that actually tracks a regression.
 */
size_t wubu_telem_count_over(const wubu_telem *t, int field, double threshold_ms);

/* Drop every record and reset the timebase. Used after warm-up so the ring
   contains steady state only, never device-init transients. */
void wubu_telem_clear(wubu_telem *t);

/*
 * "N% low FPS", the standard stutter metric used by CapFrameX / OCAT /
 * FrameView. It is the SAME data as a frame-time percentile, inverted:
 *   1% low   FPS = 1000 / p99   frame time
 *   0.1% low FPS = 1000 / p99.9 frame time
 * A 1% low of 32 fps and a p99 frame time of 31.25 ms are one fact stated two
 * ways. Report both, because "low fps" is how dips are felt and "p99 ms" is
 * how they get diagnosed.
 *
 * slow_percent is the SLOW tail fraction: 1.0 for a 1% low, 0.1 for 0.1% low.
 * Returns 0.0 with no samples.
 */
double wubu_telem_low_fps(const wubu_telem *t, int field, double slow_percent);

/*
 * Longest run of consecutive intervals over `threshold_ms`: a cluster of dips
 * is far more visible than the same number spread out, and a cluster is what
 * the eye reads as stutter rather than as a random hiccup.
 */
size_t wubu_telem_longest_dip_run(const wubu_telem *t, int field, double threshold_ms);

/*
 * Write the whole ring as CSV with a header row:
 *   t_ms,cap_iv_ms,pres_iv_ms,lat_ms,new_frame
 * Returns 1 on success. Used by the graph tool.
 */
int wubu_telem_export_csv(wubu_telem *t, const char *path);

/* Human-readable one-line summary of every field, for the title bar. */
void wubu_telem_summary(const wubu_telem *t, char *out, size_t outLen);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_TELEM_H */