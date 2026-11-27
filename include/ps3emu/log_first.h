/*
 * ps3recomp - log a diagnostic only the first few times, or ever more rarely
 */
#ifndef PS3RECOMP_LOG_FIRST_H
#define PS3RECOMP_LOG_FIRST_H

/* True for the first `limit` evaluations of the counter `ctr` (a static variable of the call site), then false:
 *     static int _n = 0; if (PS3_LOG_FIRST(_n, 8)) fprintf(stderr, ...);
 * The counter stops at the limit instead of counting on, so a hot path cannot wrap it past 2^31 and start
 * printing on every call again. */
#define PS3_LOG_FIRST(ctr, limit) ((ctr) < (limit) && ++(ctr))

/* For a report that repeats as a count grows (a census, a running total): true when the running count `n`
 * (from 1) is step, 2*step, 4*step, ... below step*cap, and then at every multiple of step*cap. The report
 * starts dense and settles at one per step*cap events, so a long session keeps its log for the rest:
 *     if (ps3_log_sparse(++s_total, 1024, 1024)) fprintf(stderr, ...);   (1Ki, 2Ki, 4Ki, ... 512Ki, then every 1Mi)
 * `cap` is a power of two; step 0 or cap 0 is never true. */
static inline int ps3_log_sparse(unsigned long long n, unsigned long long step, unsigned long long cap)
{
    if (!step || !cap || !n || n % step) return 0;
    const unsigned long long k = n / step;
    if (k >= cap) return k % cap == 0;
    return (k & (k - 1)) == 0;
}

#endif /* PS3RECOMP_LOG_FIRST_H */
