#include "original_work_threads.h"
#if !defined(_WIN32) && defined(PS3RECOMP_ORIGINAL_OBSERVER) &&                \
    PS3RECOMP_ORIGINAL_OBSERVER
#include "original_work_observer.h"
#include <errno.h>
#include <stdlib.h>
typedef struct original_thread_start {
  void *(*fn)(void *);
  void *arg;
  uint64_t worker;
} original_thread_start;
static void retire(void *unused) {
  (void)unused;
  (void)ps3_original_worker_retire();
}
static void *start(void *opaque) {
  int saved = errno;
  original_thread_start input = *(original_thread_start *)opaque;
  free(opaque);
  int registered = ps3_original_worker_register(input.worker, 1) == 0;
  errno = saved;
  void *result;
  pthread_cleanup_push(retire, NULL);
  result = input.fn(input.arg);
  pthread_cleanup_pop(registered);
  return result;
}
int ps3_original_pthread_create(pthread_t *thread,
                                const pthread_attr_t *attributes,
                                void *(*fn)(void *), void *arg) {
  const int entry_errno = errno;
  uint64_t worker = 0;
  if (ps3_original_worker_prepare(&worker) != 1) {
    errno = entry_errno;
    return pthread_create(thread, attributes, fn, arg);
  }
  original_thread_start *input = malloc(sizeof(*input));
  if (!input) {
    (void)ps3_original_worker_cancel(worker);
    ps3_original_lost(1);
    errno = entry_errno;
    return pthread_create(thread, attributes, fn, arg);
  }
  *input = (original_thread_start){fn, arg, worker};
  errno = entry_errno;
  int result = pthread_create(thread, attributes, start, input);
  int after_errno = errno;
  if (result) {
    (void)ps3_original_worker_cancel(worker);
    free(input);
  }
  errno = after_errno;
  return result;
}
#endif
