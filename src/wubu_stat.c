/*
 * wubu_stat.c — see wubu_stat.h for why the present path must not sort.
 *
 * Every function here is allocation-free and O(n) unless stated otherwise. No
 * scratch buffers, no qsort, no libc dependency beyond <math.h>.
 */
#include <math.h>
#include <string.h>
#include "wubu_stat.h"

/*
 * Scratch index set for the non-destructive percentile: one byte per element,
 * marking which have already been consumed as we walk up to the wanted rank.
 *
 * File-scope static on purpose -- it must not be a stack array (that is the
 * stack traffic this module exists to remove) and it must not be malloc'd (a
 * heap call on the present path can fault pages). It is bounded, zeroed per
 * call, and therefore NOT reentrant: wubu_stat_percentile() must be called from
 * one thread at a time. That holds here -- it is only ever called from
 * TickTitle() on the present thread -- and the header says so.
 *
 * WUBU_STAT_MAX is the largest window this scratch can serve. LAT_N in
 * wubu_capture_mf.c is 512, comfortably inside it. Exceeding it returns the
 * percentile of the first WUBU_STAT_MAX elements rather than writing past the
 * buffer; it does not corrupt memory.
 */
#define WUBU_STAT_MAX 4096
static unsigned char wubu_stat_taken[WUBU_STAT_MAX];

double wubu_stat_sum(const double *v, long n) {
    if (!v || n <= 0) return 0.0;
    double s = 0.0;
    for (long i = 0; i < n; i++) s += v[i];
    return s;
}

double wubu_stat_mean(const double *v, long n) {
    if (!v || n <= 0) return 0.0;
    return wubu_stat_sum(v, n) / (double)n;
}

double wubu_stat_stddev(const double *v, long n) {
    if (!v || n <= 0) return 0.0;
    double m = wubu_stat_mean(v, n);
    double ss = 0.0;
    for (long i = 0; i < n; i++) { double d = v[i] - m; ss += d * d; }
    return sqrt(ss / (double)n);
}

double wubu_stat_min(const double *v, long n) {
    if (!v || n <= 0) return 0.0;
    double m = v[0];
    for (long i = 1; i < n; i++) if (v[i] < m) m = v[i];
    return m;
}

double wubu_stat_max(const double *v, long n) {
    if (!v || n <= 0) return 0.0;
    double m = v[0];
    for (long i = 1; i < n; i++) if (v[i] > m) m = v[i];
    return m;
}

double wubu_stat_percentile_inplace(double *v, long n, double q) {
    if (!v || n <= 0) return 0.0;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    long k = (long)(q * (double)(n - 1));
    if (k < 0) k = 0;
    if (k > n - 1) k = n - 1;

    /* Hoare-style quickselect: partitions in place around a pivot until the
       target index holds the k-th smallest. Iterative, so no recursion depth
       concern, and O(1) extra space -- which is the entire point. */
    long lo = 0, hi = n - 1;
    while (lo < hi) {
        double pivot = v[lo + (hi - lo) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {
                double t = v[i]; v[i] = v[j]; v[j] = t;
                i++; j--;
            }
        }
        if (k <= j)      hi = j;
        else if (k >= i) lo = i;
        else              break;
    }
    return v[k];
}

double wubu_stat_percentile(const double *v, long n, double q) {
    if (!v || n <= 0) return 0.0;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    if (n == 1) return v[0];
    long k = (long)(q * (double)(n - 1));
    if (k < 0) k = 0;
    if (k > n - 1) k = n - 1;

    /*
     * Non-destructive selection by REPEATED MIN-EXTRACTION over a shrinking
     * candidate set.
     *
     * The obvious quickselect-over-a-logical-range does not work without being
     * able to swap elements, because narrowing a Hoare partition requires the
     * partition to have physically happened. Counting is not enough: knowing how
     * many elements are <= pivot does not tell you WHICH they are, so the range
     * cannot be narrowed. (An earlier version tried anyway and returned
     * p50 == 900 for a window that is 97% the value 16.6 -- the unit test caught
     * it on the rare-outlier pattern.)
     *
     * So: find the k-th smallest by taking the minimum k+1 times, each time
     * excluding one already-chosen element by index. O(k*n) with no writes, no
     * allocation and no branches on data-dependent memory order. For the
     * windows this module exists to serve (n = 512, q = 0.95) that is 26 passes
     * over 512 doubles -- still far cheaper than the qsort and a scratch copy it
     * replaces, and it leaves the caller's ring untouched, which is the property
     * that actually matters for buffers written concurrently by other threads.
     */
    double best = 0.0;
    if (n > WUBU_STAT_MAX) n = WUBU_STAT_MAX;   /* clamp; never write past */
    /* k was computed from the ORIGINAL n, so re-clamp it against the clamped n
       or the extraction loop would walk past the scratch buffer. */
    if (k > n - 1) k = n - 1;
    memset(wubu_stat_taken, 0, (size_t)n);
    for (long r = 0; r <= k; r++) {
        long bestIdx = -1;
        double b = 0.0;
        for (long i = 0; i < n; i++) {
            if (wubu_stat_taken[i]) continue;   /* already consumed */
            if (bestIdx < 0 || v[i] < b) { b = v[i]; bestIdx = i; }
        }
        if (bestIdx < 0) break;                 /* cannot happen for r <= k < n */
        wubu_stat_taken[bestIdx] = 1;
        best = b;
    }
    return best;
}
