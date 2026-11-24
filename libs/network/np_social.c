/*
 * ps3recomp - NP social provider: the installed provider, the event queue it
 * feeds and its pending lookups. Behaviour: docs/np-social-provider.md.
 *
 * Nothing here runs until ps3_np_social_set_provider() is called; the NP Basic
 * and Lookup imports check np_social_get() first and keep their old answers
 * while it is NULL.
 */

#include "np_social_internal.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define SOCIAL_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define SOCIAL_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define SOCIAL_LOCK()   pthread_mutex_lock(&s_lock)
#define SOCIAL_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

static ps3_np_social_provider s_provider;
static volatile int s_installed;

/* Events: a ring. The first s_announced entries from s_head have been
 * announced to the title's handler and wait for sceNpBasicGetEvent. */
static np_social_event s_queue[NP_SOCIAL_QUEUE_MAX];
static unsigned s_head, s_count, s_announced;

/* Lookups. */
#define LOOKUP_MAX 32
typedef struct {
    uint64_t request;   /* 0: free */
    int      done;
    int32_t  result;
    char     online_id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
} np_social_lookup;
static np_social_lookup s_lookups[LOOKUP_MAX];
static uint64_t s_next_request = 1;

/* One stderr line per kind of refusal, so a misbehaving provider is visible
 * without flooding the log. */
static void refuse_once(int kind, const char* why)
{
    static int said[8];
    if (kind < 0 || kind >= 8 || said[kind]) return;
    said[kind] = 1;
    fprintf(stderr, "[np-social] event refused: %s (further ones are not logged)\n", why);
}

int np_social_valid_id(const char* online_id)
{
    if (!online_id) return 0;
    size_t n = strnlen(online_id, PS3_NP_SOCIAL_ONLINE_ID_MAX + 1);
    return n >= 1 && n <= PS3_NP_SOCIAL_ONLINE_ID_MAX;
}

int ps3_np_social_set_provider(const ps3_np_social_provider* provider)
{
    if (!provider || provider->abi != PS3_NP_SOCIAL_ABI || s_installed) return 1;
    s_provider = *provider;
    s_installed = 1;
    np_basic_social_register();
    np_lookup_social_register();
    fprintf(stderr, "[np-social] provider installed\n");
    return 0;
}

const ps3_np_social_provider* np_social_get(void)
{
    return s_installed ? &s_provider : NULL;
}

int ps3_np_social_push_event(int32_t event, int32_t ret_code, const char* online_id,
                             const uint8_t* data, uint32_t size)
{
    if (!s_installed) { refuse_once(0, "no provider installed"); return 1; }
    if (!np_social_valid_id(online_id)) { refuse_once(1, "online ID must be 1 to 16 bytes"); return 1; }
    if (size > PS3_NP_SOCIAL_PRESENCE_MAX || (size && !data)) {
        refuse_once(2, "data must be 0 to 128 bytes");
        return 1;
    }
    SOCIAL_LOCK();
    if (s_count == NP_SOCIAL_QUEUE_MAX) {
        SOCIAL_UNLOCK();
        refuse_once(3, "queue full (256 events)");
        return 1;
    }
    np_social_event* e = &s_queue[(s_head + s_count) % NP_SOCIAL_QUEUE_MAX];
    memset(e, 0, sizeof(*e));
    e->event = event;
    e->ret_code = ret_code;
    memcpy(e->online_id, online_id, strnlen(online_id, PS3_NP_SOCIAL_ONLINE_ID_MAX));
    e->size = size;
    if (size) memcpy(e->data, data, size);
    s_count++;
    SOCIAL_UNLOCK();
    return 0;
}

int np_social_announce(int32_t* event, int32_t* ret_code)
{
    int found = 0;
    SOCIAL_LOCK();
    if (s_announced < s_count) {
        const np_social_event* e = &s_queue[(s_head + s_announced) % NP_SOCIAL_QUEUE_MAX];
        *event = e->event;
        *ret_code = e->ret_code;
        s_announced++;
        found = 1;
    }
    SOCIAL_UNLOCK();
    return found;
}

int np_social_take(np_social_event* out)
{
    int found = 0;
    SOCIAL_LOCK();
    if (s_announced > 0) {
        *out = s_queue[s_head];
        s_head = (s_head + 1) % NP_SOCIAL_QUEUE_MAX;
        s_count--;
        s_announced--;
        found = 1;
    }
    SOCIAL_UNLOCK();
    return found;
}

void np_social_handler(int registered)
{
    if (!s_installed) return;
    if (!registered) {
        SOCIAL_LOCK();
        s_announced = 0;
        SOCIAL_UNLOCK();
    }
    if (s_provider.handler_changed) s_provider.handler_changed(s_provider.user, registered);
}

uint64_t np_social_lookup_start(const char* online_id)
{
    (void)online_id;
    uint64_t request = 0;
    SOCIAL_LOCK();
    for (int i = 0; i < LOOKUP_MAX; i++) {
        if (s_lookups[i].request) continue;
        memset(&s_lookups[i], 0, sizeof(s_lookups[i]));
        request = s_next_request++;
        s_lookups[i].request = request;
        break;
    }
    SOCIAL_UNLOCK();
    return request;
}

static np_social_lookup* find_lookup(uint64_t request)
{
    if (!request) return NULL;
    for (int i = 0; i < LOOKUP_MAX; i++)
        if (s_lookups[i].request == request) return &s_lookups[i];
    return NULL;
}

void ps3_np_social_complete_lookup(uint64_t request, int32_t result, const char* online_id)
{
    if (result == PS3_NP_SOCIAL_LOOKUP_FOUND && !np_social_valid_id(online_id))
        result = PS3_NP_SOCIAL_LOOKUP_FAILED;
    SOCIAL_LOCK();
    np_social_lookup* l = find_lookup(request);
    if (l && !l->done) {
        l->done = 1;
        l->result = result;
        if (result == PS3_NP_SOCIAL_LOOKUP_FOUND)
            memcpy(l->online_id, online_id, strnlen(online_id, PS3_NP_SOCIAL_ONLINE_ID_MAX));
    }
    SOCIAL_UNLOCK();
}

int np_social_lookup_poll(uint64_t request, int32_t* result, char online_id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1])
{
    int state = -1;
    SOCIAL_LOCK();
    np_social_lookup* l = find_lookup(request);
    if (l) {
        state = l->done;
        if (l->done) {
            *result = l->result;
            memcpy(online_id, l->online_id, sizeof(l->online_id));
        }
    }
    SOCIAL_UNLOCK();
    return state;
}

void np_social_lookup_drop(uint64_t request)
{
    SOCIAL_LOCK();
    np_social_lookup* l = find_lookup(request);
    if (l) memset(l, 0, sizeof(*l));
    SOCIAL_UNLOCK();
}
