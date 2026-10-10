/*
 * wubu_qdc.h -- TRUE fractional display refresh rate via QueryDisplayConfig.
 *
 * WHY THIS MODULE EXISTS
 * -----------------------
 * The whole hitch investigation stalled on one number. WuBuMedia's panel module
 * reads the refresh rate with EnumDisplaySettingsW, which only exposes the
 * INTEGER dmDisplayFrequency. On this box that reports 59 for what is actually
 * a 59.94 Hz (60000/1001) panel, because the fractional field
 * dmDisplayFrequencyFractional does not exist in MinGW's DEVMODEW and the OS
 * clips an over-sized request to 188 bytes.
 *
 * That truncation was PROVEN, not assumed: src/devfreq_diag.c poisons a DEVMODE
 * with 0xAA, requests an over-sized dmSize, and observes the poison untouched
 * past the struct end. So EnumDisplaySettings simply cannot carry the true rate
 * on this toolchain. A whole session was spent treating 59 as the truth.
 *
 * THE FIX IS A DIFFERENT API, NOT A BIGGER STRUCT.
 * QueryDisplayConfig returns each display path's refresh rate as an exact
 * DISPLAYCONFIG_RATIONAL -- a numerator/denominator pair. That is the fractional
 * value as a RATIONAL, with no DEVMODE involved and therefore no 188-byte clip.
 * 60000/1001 is representable exactly; 59.94 is not.
 *
 * WHY THE 59-vs-59.94 ERROR CAUSED THE HITCHES
 * --------------------------------------------
 * Pacing to 59 Hz gives a 16.9492 ms period. The panel's true period is
 * 1001/60000 s = 16.6833 ms. That is 0.2659 ms of error PER SLOT, and it
 * accumulates: the phase error reaches a full blank (16.68 ms) every
 *
 *     16.6833 / 0.2659 ~= 63 slots ~= 1.05 seconds
 *
 * So the app misses a blank roughly once per second, forever. Measured dip rate
 * was 30-47 per 60 s, which is the same order of magnitude. It also explains the
 * signature exactly: a missed blank shows up as a 1.5-1.9x period gap followed
 * ~15 ms later by a short catch-up frame.
 *
 * This is a HYPOTHESIS with a numeric prediction, not a conclusion. wubu_qdc
 * exists so the prediction can be tested against the real rational instead of
 * against 59.
 *
 * CAVEAT, stated up front: this reads the rate the OS reports for the ACTIVE
 * mode. It is not a substitute for measuring scanout. If the returned rational
 * is not 60000/1001, that is itself the finding and the pacing arithmetic must
 * be redone against whatever it actually says.
 */
#ifndef WUBU_QDC_H
#define WUBU_QDC_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve the TRUE refresh rate, in Hz, of the display currently driving
 * `hwnd`'s monitor.
 *
 * Returns > 0.0 on success. Returns 0.0 if the rate could not be established --
 * an explicit "unknown", never a plausible-looking substitute. Callers must
 * treat 0.0 as unknown and refuse to invent a value; that rule is what ended
 * the previous session's fabricated-59.94 episode.
 *
 * On success *out_num and *out_den receive the raw rational (expected
 * 60000/1001) so the caller can log exactly what the OS claimed rather than a
 * float derived from it. Either pointer may be NULL.
 */
double wubu_qdc_refresh_hz(void *hwnd, unsigned *out_num, unsigned *out_den);

#ifdef __cplusplus
}
#endif
#endif /* WUBU_QDC_H */
