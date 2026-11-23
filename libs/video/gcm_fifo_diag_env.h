/* Every environment variable that arms a diagnostic the FIFO walker would
 * otherwise test on EVERY command word it decodes (gcm_rsx_process_fifo_pass):
 * the FIFO observer, the word statistics and their control-flow ring (also
 * read by the GCM_CIRCLE_DUMP report), the constant-stream histogram and the
 * engine-phase clock around each dispatch. The walker resolves this list once
 * into one word; while none of these is set the per-word path skips that work
 * behind a single test, and setting any of them runs it exactly as before.
 *
 * tools/test_gcm_fifo_lean.py scans the walker for getenv() names and fails
 * when one is read on the per-word path without a line here, so a diagnostic
 * added to that path cannot silently stop working. */
#ifndef PS3RECOMP_GCM_FIFO_DIAG_ENV_H
#define PS3RECOMP_GCM_FIFO_DIAG_ENV_H
static const char* const gcm_fifo_diag_env[] = {
    "GCM_CIRCLE_DUMP",
    "GCM_FIFO_OBSERVE",
    "GCM_FIFO_OBSERVE_LIMIT",
    "GCM_FIFO_OBSERVE_WORDS",
    "GCM_WORD_STATS",
    "RSX_ENGINE_PHASES",
    "TCONST_FIFO",
    0
};
#endif
