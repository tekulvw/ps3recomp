/* Optional host scheduling policy at PM-worker startup only. */
#ifndef SPURS_PM_DARWIN_QOS_H
#define SPURS_PM_DARWIN_QOS_H
#if defined(__APPLE__)
#include <errno.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void spurs_pm_darwin_qos(unsigned spu)
{
    int saved_errno=errno;
    /* One read per worker, before guest work or scheduler locks. Explicit 0
     * queries only, for a balanced control; absent/invalid stays silent/off. */
    const char* option=getenv("SPURS_PM_DARWIN_QOS");
    if(!option || (strcmp(option,"0") && strcmp(option,"1"))) {
        errno=saved_errno;return;
    }
    int enabled=option[0]=='1';
    uint64_t host_id=0;
    int id_rc=pthread_threadid_np(NULL,&host_id);
    qos_class_t before=QOS_CLASS_UNSPECIFIED,after=QOS_CLASS_UNSPECIFIED;
    int before_relative=0,after_relative=0;
    int before_rc=pthread_get_qos_class_np(pthread_self(),&before,&before_relative);
    int set_rc=enabled?pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0):0;
    int after_rc=pthread_get_qos_class_np(pthread_self(),&after,&after_relative);
    /* No retry or guest admission change on error. This API reports requested
     * pthread policy, not dynamic overrides, core placement or CPU frequency. */
    static atomic_uint reports;
    unsigned n=atomic_load_explicit(&reports,memory_order_relaxed);
    while(n<=128 && !atomic_compare_exchange_weak_explicit(&reports,&n,n+1,
            memory_order_relaxed,memory_order_relaxed)) {}
    if(n<128)
        fprintf(stderr,"[spurs-pm-qos] scope=pm-worker-start spu=%u host_id=%llu id_rc=%d enabled=%d query=pthread_requested requested_class=%u requested_relative=0 before_rc=%d before_class=%u before_relative=%d set_attempted=%d set_rc=%d after_rc=%d after_class=%u after_relative=%d\n",
            spu,(unsigned long long)host_id,id_rc,enabled,(unsigned)QOS_CLASS_USER_INITIATED,
            before_rc,(unsigned)before,before_relative,enabled,set_rc,
            after_rc,(unsigned)after,after_relative);
    else if(n==128)
        fprintf(stderr,"[spurs-pm-qos] telemetry_limit=128 further_startups_unreported=1\n");
    errno=saved_errno;
}
#endif
#endif
