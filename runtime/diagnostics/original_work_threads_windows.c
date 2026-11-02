/* Windows native factories, not a POSIX emulation. No aliases are included in
 * this implementation: all calls below reach the genuine Windows/CRT API. */
#include "original_work_threads.h"
#if defined(_WIN32) && defined(PS3RECOMP_ORIGINAL_OBSERVER) && \
    PS3RECOMP_ORIGINAL_OBSERVER
#include "original_work_observer.h"
#include <errno.h>
#include <stdlib.h>

typedef struct original_windows_start {
  LPTHREAD_START_ROUTINE windows_fn;
  unsigned (__stdcall *crt_fn)(void *);
  void *arg;
  uint64_t worker;
} original_windows_start;
static _Thread_local int registered_here;

static void retire(void) {
  if (registered_here) {
    registered_here = 0;
    (void)ps3_original_worker_retire();
  } else {
    /* A native early exit outside a wrapped worker is not a known retirement.
     * Never make that path look like a successful all-worker drain. */
    ps3_original_lost(1);
  }
}
static original_windows_start take(void *opaque) {
  const DWORD error = GetLastError();
  const int saved_errno = errno;
  original_windows_start input = *(original_windows_start *)opaque;
  free(opaque);
  registered_here = ps3_original_worker_register(input.worker, 1) == 0;
  errno = saved_errno;
  SetLastError(error);
  return input;
}
static DWORD WINAPI windows_start(void *opaque) {
  original_windows_start input = take(opaque);
  DWORD result = input.windows_fn(input.arg);
  retire();
  return result;
}
static unsigned __stdcall crt_start(void *opaque) {
  original_windows_start input = take(opaque);
  unsigned result = input.crt_fn(input.arg);
  retire();
  return result;
}
static original_windows_start *prepare(void) {
  uint64_t worker = 0;
  if (ps3_original_worker_prepare(&worker) != 1)
    return NULL;
  original_windows_start *input = malloc(sizeof(*input));
  if (!input) {
    (void)ps3_original_worker_cancel(worker);
    ps3_original_lost(1);
    return NULL;
  }
  *input = (original_windows_start){.worker = worker};
  return input;
}
HANDLE WINAPI ps3_original_CreateThread(LPSECURITY_ATTRIBUTES security,
                                        SIZE_T stack,
                                        LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                        DWORD flags, LPDWORD tid) {
  DWORD error = GetLastError();
  int saved_errno = errno;
  original_windows_start *input = prepare();
  if (input) {
    input->windows_fn = fn;
    input->arg = arg;
  }
  errno = saved_errno;
  SetLastError(error);
  HANDLE thread = CreateThread(security, stack, input ? windows_start : fn,
                               input ? input : arg, flags, tid);
  error = GetLastError();
  saved_errno = errno;
  if (!thread && input) {
    (void)ps3_original_worker_cancel(input->worker);
    free(input);
  }
  errno = saved_errno;
  SetLastError(error);
  return thread;
}
uintptr_t __cdecl ps3_original_beginthreadex(
    void *security, unsigned stack, unsigned (__stdcall *fn)(void *), void *arg,
    unsigned flags, unsigned *tid) {
  DWORD error = GetLastError();
  int saved_errno = errno;
  original_windows_start *input = prepare();
  if (input) {
    input->crt_fn = fn;
    input->arg = arg;
  }
  errno = saved_errno;
  SetLastError(error);
  uintptr_t thread = _beginthreadex(security, stack, input ? crt_start : fn,
                                   input ? input : arg, flags, tid);
  error = GetLastError();
  saved_errno = errno;
  if (!thread && input) {
    (void)ps3_original_worker_cancel(input->worker);
    free(input);
  }
  errno = saved_errno;
  SetLastError(error);
  return thread;
}
DECLSPEC_NORETURN void WINAPI ps3_original_ExitThread(DWORD code) {
  retire();
  ExitThread(code);
}
DECLSPEC_NORETURN void __cdecl ps3_original_endthreadex(unsigned code) {
  retire();
  _endthreadex(code);
  /* Some UCRT headers omit noreturn on _endthreadex. Keep the wrapper
   * terminal even with that declaration, after the single retirement above. */
  ExitThread(code);
}
#endif
