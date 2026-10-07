/* ps3_log.h -- gate for high-volume diagnostic logging.
 *
 * Chatty per-wait / per-round logs stay FULL when stderr is redirected
 * (debug runs capture to a file) or PS3_VERBOSE is set, and go quiet on a
 * live console: conhost flushes stall the emitting threads, and the ~20k
 * [WAIT] lines/minute of an LBP intro run showed up as harsh ~1 Hz hitches
 * in user-visible runs while file-redirected runs played smoothly.
 */
#ifndef PS3_LOG_H
#define PS3_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define PS3_ISATTY_STDERR() _isatty(_fileno(stderr))
#else
#include <unistd.h>
#define PS3_ISATTY_STDERR() isatty(fileno(stderr))
#endif

#ifdef __cplusplus
static inline int ps3_log_verbose(void)
#else
static __inline int ps3_log_verbose(void)
#endif
{
    /* PS3_VERBOSE, when set, decides -- INCLUDING "0" to force quiet. Without it
     * the old default stands: a redirected stderr means someone is capturing a
     * log, so be verbose.
     *
     * That default has a sharp edge worth knowing about. The per-event lines are
     * emitted from every guest thread through one FILE lock, and Windows locks
     * are not fair, so at the ~6k lines/s a SPURS audio loop produces a thread
     * can starve on the lock for seconds. Tokyo Jungle presents four frames in
     * 40 s with the log discarded and one with it redirected to a file -- the
     * logging changes what the title does. Measure timing-sensitive behaviour
     * with PS3_VERBOSE=0. */
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_VERBOSE");
        if (e) v = (e[0] && e[0] != '0') ? 1 : 0;
        else   v = !PS3_ISATTY_STDERR();
    }
    return v;
}


/* High-frequency trace policy.
 *
 * Redirecting stderr is intentionally NOT enough to make hot per-event traces
 * full-rate.  Those call sites can execute tens of thousands of times per
 * second and contend on the process-wide FILE lock.  Default is sampled;
 * explicitly request full fidelity with PS3_HOTLOG=full (or 1), or silence hot
 * sites with PS3_HOTLOG=off (or 0).  Low-rate/error diagnostics are unaffected.
 */
#ifdef __cplusplus
static inline int ps3_log_hot_mode(void)
#else
static __inline int ps3_log_hot_mode(void)
#endif
{
    enum { PS3_HOTLOG_OFF = 0, PS3_HOTLOG_THROTTLE = 1, PS3_HOTLOG_FULL = 2 };
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("PS3_HOTLOG");
        if (!e || !*e || strcmp(e, "throttle") == 0 || strcmp(e, "sample") == 0)
            mode = PS3_HOTLOG_THROTTLE;
        else if (e[0] == '0' || strcmp(e, "off") == 0 || strcmp(e, "quiet") == 0)
            mode = PS3_HOTLOG_OFF;
        else if (e[0] == '1' || strcmp(e, "full") == 0 || strcmp(e, "verbose") == 0)
            mode = PS3_HOTLOG_FULL;
        else
            mode = PS3_HOTLOG_THROTTLE;
    }
    return mode;
}

#endif /* PS3_LOG_H */
