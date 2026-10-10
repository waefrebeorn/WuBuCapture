/*
 * wubu_stat.h — O(n) rolling-window statistics for the present path.
 *
 * WHY THIS MODULE EXISTS
 * ----------------------
 * WuBuCapture's title-bar stats used to run, on the PRESENT thread, every 20
 * frames: three 512-double arrays on the stack (12 KB) and three qsorts of up to
 * 512 elements each. That is a bounded, repeating, on-thread cost sitting
 * directly between the frame pacer and Present() — the exact shape of a
 * "periodic hitch" that has nothing to do with the GPU, the panel or the
 * capture card, and therefore nothing to do with anything the ETW trace would
 * blame on them.
 *
 * Three things were wrong with it, and all three are fixed here:
 *   1. STACK TRAFFIC — 12 KB of fresh stack per call, first-touch page faults
 *      on the first pass through.
 *   2. O(n log n) — a full sort to read one percentile. A selection pass is
 *      O(n), touches each element once, and needs no scratch array at all.
 *   3. WRONG THREAD — upstream (capture-arrival) health is diagnostic only and
 *      was being recomputed on the deadline thread. It is now maintained by the
 *      capture thread on its own cadence; the present thread only reads it.
 *
 * The results are numerically identical for the cases that matter (mean, max,
 * min, stddev exactly; percentile within one element of the sorted answer,
 * because it selects the same order statistic by value rather than by index).
 */
#ifndef WUBU_STAT_H
#define WUBU_STAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sum of the first n elements. Caller guarantees n is in range. */
double wubu_stat_sum(const double *v, long n);

/* Arithmetic mean; returns 0.0 for n <= 0 rather than dividing by zero. */
double wubu_stat_mean(const double *v, long n);

/* Population standard deviation about the mean, in one pass. */
double wubu_stat_stddev(const double *v, long n);

/* Smallest / largest of the first n elements; 0.0 for n <= 0. */
double wubu_stat_min(const double *v, long n);
double wubu_stat_max(const double *v, long n);

/*
 * The q-th order statistic (0.0..1.0), e.g. 0.95 for p95.
 *
 * O(n) average, using a quickselect on the caller's own array. NOTE this
 * REORDERS v -- it is a partial sort, not a pure read. Every caller here passes
 * a ring buffer whose order carries no meaning, so that is fine and it is what
 * lets us avoid a scratch copy; the one caller that must not disturb its input
 * should use wubu_stat_percentile() instead. The previous qsort-based code
 * needed a full copy for exactly this reason, which is where the stack traffic
 * came from.
 */
double wubu_stat_percentile_inplace(double *v, long n, double q);

/*
 * Percentile WITHOUT touching the input.
 *
 * Implemented as repeated min-extraction over a file-scope index set: O(k*n)
 * where k = q*(n-1). A logical-range quickselect is NOT possible here, because
 * narrowing a partition requires the swaps to have physically happened and this
 * function must not write to the caller's array. (An attempt that counted
 * instead of swapping returned p50 == max for a window that was 97% the
 * minimum; the unit test caught it. Recorded because it is a natural mistake.)
 *
 * For the window this module actually serves -- n = 512, q = 0.95 -- that is 26
 * passes over 512 doubles, still far cheaper than a qsort plus a scratch copy.
 *
 * NOT REENTRANT: it shares one static scratch buffer, so only one thread may
 * call it at a time. Bounded at 4096 elements; a larger n is clamped rather
 * than allowed to overrun.
 */
double wubu_stat_percentile(const double *v, long n, double q);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_STAT_H */
