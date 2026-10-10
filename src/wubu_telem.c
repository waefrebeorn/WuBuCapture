/*
 * wubu_telem.c — frame-timing telemetry: record, analyse, export.
 * See wubu_telem.h for the rationale. Self-contained C11, no globals.
 *
 * DESIGN NOTES
 * The store is a RING so that a stall keeps its original timestamp. An
 * averaging counter would have folded a 76 ms spike into a 1 ms mean and the
 * spike is the entire problem. Overwrites are counted, never hidden: if the
 * ring wraps, any "worst run in this window" claim is unsound and the count
 * says so.
 */
#include "wubu_telem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define WUBU_TELEM_DEFAULT_CAP 16384u   /* ~4.5 min at 60 fps */

typedef struct {
    double t_ms;
    double cap_iv_ms;
    double pres_iv_ms;
    double lat_ms;
    int    new_frame;
} record;

struct wubu_telem {
    record *ring;
    size_t  cap;
    size_t  head;          /* next write slot */
    size_t  count;         /* valid entries, <= cap */
    unsigned long overwritten;
    /*
     * The ring is written by the render thread and exported by a writer thread.
     * Without this lock, the exporter walks a ring that is mutating underneath
     * it -- and worse, the export itself stalls the render loop. Measured: a
     * synchronous 600 KB export every 10 s produced frame dips at EXACTLY
     * 10.000 s intervals, i.e. the telemetry was manufacturing the stutter it
     * was supposed to measure. Exports are now off-thread and locked.
     */
    CRITICAL_SECTION lock;
    /*
     * Preallocated snapshot buffer. Allocated ONCE and reused.
     *
     * A fresh malloc(512 KB) per export is not free: the first touch faults in
     * 128 pages, and those faults happen while the render thread is waiting on
     * the lock. That produced dips at exactly the export interval even after
     * the export moved off-thread. Reusing a warm buffer makes the in-lock work
     * a pure memcpy with no faults and no allocator.
     */
    record *snap;
};

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

wubu_telem *wubu_telem_create(size_t capacity) {
    if (capacity == 0) capacity = WUBU_TELEM_DEFAULT_CAP;
    wubu_telem *t = (wubu_telem *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->ring = (record *)calloc(capacity, sizeof(record));
    if (!t->ring) { free(t); return NULL; }
    t->cap = capacity;
    t->snap = (record *)malloc(capacity * sizeof(record));
    InitializeCriticalSection(&t->lock);
    return t;
}

void wubu_telem_destroy(wubu_telem *t) {
    if (!t) return;
    DeleteCriticalSection(&t->lock);
    free(t->ring);
    free(t->snap);
    free(t);
}

void wubu_telem_record(wubu_telem *t, double t_ms,
                       double cap_iv_ms, double pres_iv_ms, double lat_ms,
                       int new_frame) {
    if (!t) return;
    EnterCriticalSection(&t->lock);
    if (t->count == t->cap) t->overwritten++;
    record *r = &t->ring[t->head];
    r->t_ms       = t_ms;
    r->cap_iv_ms  = cap_iv_ms;
    r->pres_iv_ms = pres_iv_ms;
    r->lat_ms     = lat_ms;
    r->new_frame  = new_frame ? 1 : 0;
    t->head = (t->head + 1) % t->cap;
    if (t->count < t->cap) t->count++;
    LeaveCriticalSection(&t->lock);
}

void wubu_telem_clear(wubu_telem *t) {
    if (!t) return;
    EnterCriticalSection(&t->lock);
    t->head = 0;
    t->count = 0;
    t->overwritten = 0;
    memset(t->ring, 0, t->cap * sizeof(record));
    LeaveCriticalSection(&t->lock);
}

size_t wubu_telem_count(const wubu_telem *t)          { return t ? t->count : 0; }
unsigned long wubu_telem_overwritten(const wubu_telem *t) { return t ? t->overwritten : 0; }

/* Oldest-to-newest index into the ring. */
static const record *at(const wubu_telem *t, size_t i) {
    size_t start = (t->head + t->cap - t->count) % t->cap;
    return &t->ring[(start + i) % t->cap];
}

static double field_of(const record *r, int field) {
    switch (field) {
        case WUBU_TELEM_CAP_IV:  return r->cap_iv_ms;
        case WUBU_TELEM_PRES_IV: return r->pres_iv_ms;
        case WUBU_TELEM_LAT:     return r->lat_ms;
        default: return 0.0;
    }
}

/* Gather valid values (skipping empty slots) into out; returns how many. */
static size_t gather(const wubu_telem *t, int field, double *out) {
    size_t n = 0;
    for (size_t i = 0; i < t->count; i++) {
        double v = field_of(at(t, i), field);
        if (v > 0.0) out[n++] = v;      /* 0 means "not sampled this frame" */
    }
    return n;
}

double wubu_telem_percentile(const wubu_telem *t, int field, double q) {
    if (!t || t->count == 0) return 0.0;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    double *v = (double *)malloc(t->count * sizeof(double));
    if (!v) return 0.0;
    size_t n = gather(t, field, v);
    if (n == 0) { free(v); return 0.0; }
    qsort(v, n, sizeof(double), cmp_double);
    /* Nearest-rank: the smallest value at or above the requested fraction. */
    size_t idx = (size_t)(q * (double)(n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    double r = v[idx];
    free(v);
    return r;
}

void wubu_telem_stats(const wubu_telem *t, int field,
                      double *mean, double *min, double *max, double *stddev) {
    if (mean)   *mean   = 0.0;
    if (min)    *min    = 0.0;
    if (max)    *max    = 0.0;
    if (stddev) *stddev = 0.0;
    if (!t || t->count == 0) return;

    double *v = (double *)malloc(t->count * sizeof(double));
    if (!v) return;
    size_t n = gather(t, field, v);
    if (n == 0) { free(v); return; }

    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += v[i];
    double m = s / (double)n;
    double ss = 0.0;
    for (size_t i = 0; i < n; i++) { double d = v[i] - m; ss += d * d; }

    qsort(v, n, sizeof(double), cmp_double);
    if (mean)   *mean   = m;
    if (min)    *min    = v[0];
    if (max)    *max    = v[n - 1];
    if (stddev) *stddev = sqrt(ss / (double)n);
    free(v);
}

double wubu_telem_low_fps(const wubu_telem *t, int field, double slow_percent) {
    /* N% low is defined over the SLOWEST tail: 1% low uses the 99th
       percentile of frame time, then inverted to FPS. */
    if (slow_percent <= 0.0 || slow_percent >= 1.0) return 0.0;
    double p = wubu_telem_percentile(t, field, 1.0 - slow_percent);
    return p > 0.0 ? 1000.0 / p : 0.0;
}

size_t wubu_telem_count_over(const wubu_telem *t, int field, double threshold_ms) {
    if (!t || threshold_ms <= 0.0) return 0;
    size_t c = 0;
    for (size_t i = 0; i < t->count; i++)
        if (field_of(at(t, i), field) > threshold_ms) c++;
    return c;
}

size_t wubu_telem_longest_dip_run(const wubu_telem *t, int field, double threshold_ms) {
    if (!t || threshold_ms <= 0.0) return 0;
    size_t best = 0, cur = 0;
    for (size_t i = 0; i < t->count; i++) {
        double v = field_of(at(t, i), field);
        if (v > threshold_ms) { cur++; if (cur > best) best = cur; }
        else cur = 0;
    }
    return best;
}

/*
 * SNAPSHOT, THEN WRITE.
 *
 * Writing straight from the ring while holding the lock was still wrong: the
 * writer held the lock for the whole multi-hundred-KB write, so the render
 * thread blocked in EnterCriticalSection and the dips kept arriving at exactly
 * the export interval. Moving the file IO to another thread is not enough --
 * the LOCK is what stalls the pacer.
 *
 * So: memcpy the ring out under the lock (microseconds), release it, then do
 * all the formatting and file IO with no lock held. The render thread now only
 * ever waits for a memcpy.
 */
int wubu_telem_export_csv(wubu_telem *t, const char *path) {
    if (!t || !path) return 0;

    record *snap = t->snap;
    size_t n = 0;
    EnterCriticalSection(&t->lock);
    n = t->count;
    if (n) for (size_t i = 0; i < n; i++) snap[i] = *at(t, i);
    LeaveCriticalSection(&t->lock);

    FILE *f = fopen(path, "wb");
    if (!f) { free(snap); return 0; }

    fprintf(f, "t_ms,cap_iv_ms,pres_iv_ms,lat_ms,new_frame\n");
    for (size_t i = 0; i < n; i++) {
        const record *r = &snap[i];
        fprintf(f, "%.3f,%.4f,%.4f,%.4f,%d\n",
                r->t_ms, r->cap_iv_ms, r->pres_iv_ms, r->lat_ms, r->new_frame);
    }
    int ok = (ferror(f) == 0);
    fclose(f);
    return ok;
}

void wubu_telem_summary(const wubu_telem *t, char *out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = 0;
    if (!t || t->count == 0) { snprintf(out, outLen, "telem: no samples"); return; }

    double pcap50 = wubu_telem_percentile(t, WUBU_TELEM_CAP_IV,  0.50);
    double ppres50 = wubu_telem_percentile(t, WUBU_TELEM_PRES_IV, 0.50);
    double ppres95 = wubu_telem_percentile(t, WUBU_TELEM_PRES_IV, 0.95);
    double ppres99 = wubu_telem_percentile(t, WUBU_TELEM_PRES_IV, 0.99);
    double ppres999 = wubu_telem_percentile(t, WUBU_TELEM_PRES_IV, 0.999);
    double pcap99 = wubu_telem_percentile(t, WUBU_TELEM_CAP_IV, 0.99);
    double plat99 = wubu_telem_percentile(t, WUBU_TELEM_LAT, 0.99);

    double low1   = wubu_telem_low_fps(t, WUBU_TELEM_PRES_IV, 0.01);
    double low01  = wubu_telem_low_fps(t, WUBU_TELEM_PRES_IV, 0.001);

    /* A dip is anything past 1.5x the median interval: perceptually a skipped
       frame, and the threshold the ETW cross-check also uses. */
    double thr = ppres50 * 1.5;
    size_t dips = wubu_telem_count_over(t, WUBU_TELEM_PRES_IV, thr);
    size_t run  = wubu_telem_longest_dip_run(t, WUBU_TELEM_PRES_IV, thr);
    size_t cdips = wubu_telem_count_over(t, WUBU_TELEM_CAP_IV, pcap50 * 1.5);

    snprintf(out, outLen,
        "n%zu pres p50 %.2f p95 %.2f p99 %.2f p99.9 %.2f | 1%%low %.1f 0.1%%low %.1f fps"
        " | dips %zu (run%zu) cap p50 %.2f p99 %.2f capdips %zu | lat p99 %.2f",
        t->count, ppres50, ppres95, ppres99, ppres999, low1, low01,
        dips, run, pcap50, pcap99, cdips, plat99);
}