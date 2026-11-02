#include "original_work_observer.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#if defined(PS3RECOMP_ORIGINAL_OBSERVER) && PS3RECOMP_ORIGINAL_OBSERVER
enum { UNARMED, ARMING, ARMED, DRAINING, SEALED };
enum { EXPECTED = 1, ACTIVE, RETIRED };
typedef struct worker_record {
  uint64_t id, generation, transaction;
  _Atomic unsigned state;
  uint64_t count[PS3_ORIGINAL_MAX_TARGETS][PS3_ORIGINAL_ROUTES];
} worker_record;
static ps3_original_config config;
static ps3_original_source sources[PS3_ORIGINAL_MAX_SOURCES];
static ps3_original_provider providers[PS3_ORIGINAL_MAX_PROVIDERS];
static ps3_original_target targets[PS3_ORIGINAL_MAX_TARGETS];
static worker_record workers[PS3_ORIGINAL_MAX_WORKERS];
static _Atomic unsigned phase, faults, workers_seen, worker_count;
static _Atomic uint64_t lost_records;
static uint64_t drain_boundary;
static atomic_flag control = ATOMIC_FLAG_INIT;
static _Thread_local int current_worker = -1;
static _Thread_local int resolving;
static void fault(unsigned value) {
  atomic_fetch_or_explicit(&faults, value, memory_order_relaxed);
}
static int nonzero(const uint8_t *bytes) {
  unsigned value = 0;
  for (unsigned i = 0; i < 32; ++i)
    value |= bytes[i];
  return value != 0;
}
static int source_index(uint64_t id) {
  for (unsigned i = 0; i < config.source_count; ++i)
    if (sources[i].id == id)
      return (int)i;
  return -1;
}
static int provider_index(uint64_t id) {
  for (unsigned i = 0; i < config.provider_count; ++i)
    if (providers[i].id == id)
      return (int)i;
  return -1;
}
static worker_record *active_worker(void) {
  if (resolving) {
    fault(PS3_ORIGINAL_BAD_SOURCE);
    return NULL;
  }
  if (current_worker < 0) {
    fault(PS3_ORIGINAL_MISSING_WORKER);
    return NULL;
  }
  worker_record *w = &workers[current_worker];
  if (atomic_load_explicit(&w->state, memory_order_acquire) != ACTIVE) {
    fault(PS3_ORIGINAL_LATE_WORK);
    return NULL;
  }
  return w;
}
int ps3_original_arm(const ps3_original_config *c) {
  if (!c || c->struct_size != sizeof(*c) || c->abi_version != 1 ||
      c->reserved || !c->resolve || !c->source_count ||
      c->source_count > PS3_ORIGINAL_MAX_SOURCES || !c->provider_count ||
      c->provider_count > PS3_ORIGINAL_MAX_PROVIDERS || !c->target_count ||
      c->target_count > PS3_ORIGINAL_MAX_TARGETS || !c->sources ||
      !c->providers || !c->targets || !c->counter_limit ||
      !nonzero(c->manifest_sha256))
    return -1;
  for (unsigned i = 0; i < c->source_count; ++i) {
    const ps3_original_source *s = &c->sources[i];
    if (!s->id || !s->generation || !s->provider_mask ||
        (s->provider_mask >> c->provider_count) || !nonzero(s->code_sha256))
      return -1;
    for (unsigned j = 0; j < i; ++j)
      if (c->sources[j].id == s->id)
        return -1;
  }
  for (unsigned i = 0; i < c->provider_count; ++i) {
    const ps3_original_provider *p = &c->providers[i];
    if (!p->id || p->reserved || !p->route_mask || (p->route_mask & ~63u) ||
        !nonzero(p->source_sha256))
      return -1;
    for (unsigned j = 0; j < i; ++j)
      if (c->providers[j].id == p->id)
        return -1;
  }
  for (unsigned i = 0; i < c->target_count; ++i) {
    const ps3_original_target *t = &c->targets[i];
    int found = 0;
    unsigned available_routes = 0;
    if (!t->id || t->reserved || t->low >= t->high || !t->route_mask ||
        (t->route_mask & ~63u))
      return -1;
    for (unsigned j = 0; j < c->source_count; ++j)
      if (c->sources[j].id == t->source) {
        found = 1;
        for (unsigned p = 0; p < c->provider_count; ++p)
          if (c->sources[j].provider_mask & (UINT64_C(1) << p))
            available_routes |= c->providers[p].route_mask;
      }
    if (!found || (t->route_mask & ~available_routes))
      return -1;
    for (unsigned j = 0; j < i; ++j)
      if (c->targets[j].id == t->id)
        return -1;
  }
  if (atomic_load_explicit(&workers_seen, memory_order_acquire)) {
    fault(PS3_ORIGINAL_LATE_ARM);
    return -1;
  }
  unsigned expected = UNARMED;
  if (!atomic_compare_exchange_strong_explicit(&phase, &expected, ARMING,
                                               memory_order_acq_rel,
                                               memory_order_acquire))
    return -1;
  config = *c;
  memcpy(sources, c->sources, c->source_count * sizeof(*sources));
  memcpy(providers, c->providers, c->provider_count * sizeof(*providers));
  memcpy(targets, c->targets, c->target_count * sizeof(*targets));
  config.sources = sources;
  config.providers = providers;
  config.targets = targets;
  atomic_store_explicit(&phase, ARMED, memory_order_release);
  if (atomic_load_explicit(&workers_seen, memory_order_acquire)) {
    fault(PS3_ORIGINAL_LATE_ARM);
    return -1;
  }
  return 0;
}
int ps3_original_enroll(uint64_t id, uint64_t generation) {
  if (!id || !generation ||
      atomic_flag_test_and_set_explicit(&control, memory_order_acquire))
    return -1;
  int result = -1;
  if (atomic_load_explicit(&phase, memory_order_acquire) == ARMED) {
    unsigned count = atomic_load_explicit(&worker_count, memory_order_relaxed),
             i;
    for (i = 0; i < count; ++i)
      if (workers[i].id == id && workers[i].generation == generation)
        break;
    if (i == count && count < PS3_ORIGINAL_MAX_WORKERS) {
      workers[count].id = id;
      workers[count].generation = generation;
      atomic_store_explicit(&workers[count].state, EXPECTED,
                            memory_order_release);
      atomic_store_explicit(&worker_count, count + 1, memory_order_release);
      result = 0;
    } else
      fault(PS3_ORIGINAL_MISSING_WORKER);
  } else
    fault(PS3_ORIGINAL_LATE_WORK);
  atomic_flag_clear_explicit(&control, memory_order_release);
  return result;
}
int ps3_original_worker_register(uint64_t id, uint64_t generation) {
  unsigned state = atomic_load_explicit(&phase, memory_order_acquire);
  atomic_store_explicit(&workers_seen, 1, memory_order_release);
  if (state != ARMED || current_worker >= 0) {
    fault(state < ARMED ? PS3_ORIGINAL_LATE_ARM : PS3_ORIGINAL_MISSING_WORKER);
    return -1;
  }
  unsigned count = atomic_load_explicit(&worker_count, memory_order_acquire);
  for (unsigned i = 0; i < count; ++i)
    if (workers[i].id == id && workers[i].generation == generation) {
      unsigned expected = EXPECTED;
      if (atomic_compare_exchange_strong_explicit(&workers[i].state, &expected,
                                                  ACTIVE, memory_order_acq_rel,
                                                  memory_order_acquire)) {
        current_worker = (int)i;
        return 0;
      }
      break;
    }
  fault(PS3_ORIGINAL_MISSING_WORKER);
  return -1;
}
int ps3_original_worker_retire(void) {
  worker_record *w = active_worker();
  if (!w)
    return -1;
  if (w->transaction)
    fault(PS3_ORIGINAL_BAD_SCOPE);
  atomic_store_explicit(&w->state, RETIRED, memory_order_release);
  return 0;
}
int ps3_original_transaction_begin(uint64_t id) {
  worker_record *w = active_worker();
  if (!w)
    return -1;
  if (!id || w->transaction) {
    fault(PS3_ORIGINAL_BAD_SCOPE);
    return -1;
  }
  w->transaction = id;
  return 0;
}
int ps3_original_transaction_end(uint64_t id) {
  worker_record *w = active_worker();
  if (!w)
    return -1;
  if (!id || w->transaction != id) {
    fault(PS3_ORIGINAL_BAD_SCOPE);
    return -1;
  }
  w->transaction = 0;
  return 0;
}
static void increment(uint64_t *value) {
  if (*value == config.counter_limit)
    fault(PS3_ORIGINAL_OVERFLOW);
  else
    ++*value;
}
void ps3_original_step(const void *context, uint64_t pc, unsigned route,
                       const char *provider_name) {
  unsigned state = atomic_load_explicit(&phase, memory_order_acquire);
  if (state < ARMED) {
    atomic_store_explicit(&workers_seen, 1, memory_order_release);
    fault(PS3_ORIGINAL_LATE_ARM);
    return;
  }
  if (state == SEALED) {
    fault(PS3_ORIGINAL_LATE_WORK);
    return;
  }
  worker_record *w = active_worker();
  if (!w)
    return;
  if (route >= PS3_ORIGINAL_OUTSIDE) {
    fault(PS3_ORIGINAL_BAD_ROUTE);
    return;
  }
  if (resolving || !provider_name) {
    fault(PS3_ORIGINAL_BAD_SOURCE);
    return;
  }
  const int saved_errno = errno;
  ps3_original_identity identity = {0};
  resolving = 1;
  int resolved = config.resolve(context, pc, route, provider_name, &identity);
  resolving = 0;
  errno = saved_errno;
  int s = source_index(identity.source), p = provider_index(identity.provider);
  if (resolved || s < 0 || p < 0 ||
      sources[s].generation != identity.generation ||
      !(sources[s].provider_mask & (UINT64_C(1) << p))) {
    fault(PS3_ORIGINAL_BAD_SOURCE);
    return;
  }
  if (!(providers[p].route_mask & (1u << route))) {
    fault(PS3_ORIGINAL_BAD_ROUTE);
    return;
  }
  for (unsigned i = 0; i < config.target_count; ++i)
    if (targets[i].source == identity.source && pc >= targets[i].low &&
        pc < targets[i].high) {
      if (!(targets[i].route_mask & (1u << route))) {
        fault(PS3_ORIGINAL_BAD_ROUTE);
        continue;
      }
      increment(&w->count[i][route]);
      if (!w->transaction)
        increment(&w->count[i][PS3_ORIGINAL_OUTSIDE]);
    }
}
void ps3_original_lost(uint64_t count) {
  if (!count)
    return;
  fault(PS3_ORIGINAL_LOSS);
  uint64_t old = atomic_load_explicit(&lost_records, memory_order_relaxed);
  for (;;) {
    uint64_t next = count > UINT64_MAX - old ? UINT64_MAX : old + count;
    if (next == UINT64_MAX && count > UINT64_MAX - old)
      fault(PS3_ORIGINAL_OVERFLOW);
    if (atomic_compare_exchange_weak_explicit(&lost_records, &old, next,
                                              memory_order_relaxed,
                                              memory_order_relaxed))
      return;
  }
}
int ps3_original_begin_drain(uint64_t boundary) {
  if (atomic_flag_test_and_set_explicit(&control, memory_order_acquire))
    return -4;
  int result = -1;
  if (atomic_load_explicit(&phase, memory_order_acquire) == ARMED &&
      boundary >= config.covered_from) {
    drain_boundary = boundary;
    atomic_store_explicit(&phase, DRAINING, memory_order_release);
    result = 0;
  }
  atomic_flag_clear_explicit(&control, memory_order_release);
  return result;
}
int ps3_original_seal(ps3_original_report *out) {
  if (!out || out->struct_size != sizeof(*out) || out->abi_version != 1)
    return -1;
  unsigned state = atomic_load_explicit(&phase, memory_order_acquire);
  if (state != DRAINING && state != SEALED)
    return -1;
  unsigned count = atomic_load_explicit(&worker_count, memory_order_acquire);
  if (!count)
    fault(PS3_ORIGINAL_MISSING_WORKER);
  for (unsigned i = 0; i < count; ++i) {
    unsigned status =
        atomic_load_explicit(&workers[i].state, memory_order_acquire);
    if (status == ACTIVE)
      return -4;
    if (status == EXPECTED)
      fault(PS3_ORIGINAL_MISSING_WORKER);
  }
  atomic_store_explicit(&phase, SEALED, memory_order_release);
  *out = (ps3_original_report){
      .struct_size = sizeof(*out),
      .abi_version = 1,
      .faults = atomic_load_explicit(&faults, memory_order_acquire),
      .workers = count,
      .covered_from = config.covered_from,
      .drained_through = drain_boundary,
      .lost_records =
          atomic_load_explicit(&lost_records, memory_order_relaxed)};
  memcpy(out->manifest_sha256, config.manifest_sha256, 32);
  return 0;
}
int ps3_original_count(uint64_t id, uint64_t generation, uint64_t target,
                       unsigned route, uint64_t *out) {
  if (!out || route >= PS3_ORIGINAL_ROUTES ||
      atomic_load_explicit(&phase, memory_order_acquire) != SEALED)
    return -1;
  unsigned count = atomic_load_explicit(&worker_count, memory_order_acquire);
  for (unsigned i = 0; i < count; ++i)
    if (workers[i].id == id && workers[i].generation == generation) {
      for (unsigned j = 0; j < config.target_count; ++j)
        if (targets[j].id == target) {
          *out = workers[i].count[j][route];
          return 0;
        }
    }
  return -1;
}
#else
/* Control APIs are queryable in off builds; generated step macros disappear. */
int ps3_original_arm(const ps3_original_config *c) {
  (void)c;
  return -2;
}
int ps3_original_enroll(uint64_t w, uint64_t g) {
  (void)w;
  (void)g;
  return -2;
}
int ps3_original_worker_register(uint64_t w, uint64_t g) {
  (void)w;
  (void)g;
  return -2;
}
int ps3_original_worker_retire(void) { return -2; }
int ps3_original_transaction_begin(uint64_t t) {
  (void)t;
  return -2;
}
int ps3_original_transaction_end(uint64_t t) {
  (void)t;
  return -2;
}
void ps3_original_step(const void *c, uint64_t p, unsigned r, const char *s) {
  (void)c;
  (void)p;
  (void)r;
  (void)s;
}
void ps3_original_lost(uint64_t n) { (void)n; }
int ps3_original_begin_drain(uint64_t b) {
  (void)b;
  return -2;
}
int ps3_original_seal(ps3_original_report *r) {
  (void)r;
  return -2;
}
int ps3_original_count(uint64_t w, uint64_t g, uint64_t t, unsigned r,
                       uint64_t *o) {
  (void)w;
  (void)g;
  (void)t;
  (void)r;
  (void)o;
  return -2;
}
#endif
