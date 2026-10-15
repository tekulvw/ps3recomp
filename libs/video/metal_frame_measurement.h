/* Opt-in, fixed-capacity frame ledger. Caller supplies synchronization. Every
 * rate uses the requested wall interval, including periods with no callbacks.
 * Zero-initialize state before first use. There is no GPU work, host clock
 * read, file I/O, or rendering in this unit. */
#ifndef PS3RECOMP_METAL_FRAME_MEASUREMENT_H
#define PS3RECOMP_METAL_FRAME_MEASUREMENT_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define METAL_MEASUREMENT_CAPACITY 16384u
#define METAL_MEASUREMENT_DROP_DRAWABLE 1
#define METAL_MEASUREMENT_DROP_BLIT 2
#define METAL_MEASUREMENT_DROP_SUBMISSION 3
#define METAL_MEASUREMENT_HASH_SIZE (METAL_MEASUREMENT_CAPACITY*2u)
typedef struct {
    uint64_t submission_id, guest_flip_id;
    double commit_time, presented_time;
    unsigned committed:1, completed:1, failed:1, presented:1;
} MetalMeasurementFrame;
typedef struct {
    uint64_t request_id, guest_id_floor;
    uint64_t nil_drawables, skipped_blits, submission_failures;
    double t0, t1, grace;
    unsigned active, finalized, count;
    uint64_t admitted, zero_presented_time, invalid_presented_time;
    uint64_t duplicate_callbacks, out_of_order, overflow, identity_errors;
    double last_presented;
    unsigned slots[METAL_MEASUREMENT_HASH_SIZE]; /* entry index + 1 */
    MetalMeasurementFrame frames[METAL_MEASUREMENT_CAPACITY];
} MetalFrameMeasurement;
typedef struct {
    uint64_t request_id;
    double t0, t1, seconds, verified_guest_fps;
    uint64_t admitted, committed, gpu_completed, gpu_failed, displayed;
    uint64_t unique_guest_displayed, repeated_guest_displayed, uncorrelated;
    uint64_t positive_presentations, duplicate_presented_time, ambiguous_presented_time;
    uint64_t guest_id_floor, preexisting_guest_submissions;
    uint64_t nil_drawables, skipped_blits, submission_failures;
    uint64_t unresolved, uncommitted_events, missing_presentations, zero_presented_time, invalid_presented_time;
    uint64_t duplicate_callbacks, out_of_order, overflow, identity_errors;
    int qualified;
} MetalFrameMeasurementResult;
static inline int metal_measurement_begin(MetalFrameMeasurement *m,uint64_t request,double now,double seconds,double grace) {
    if(!m || (m->active && !m->finalized) || !request || !isfinite(now) || now<=0 ||
       !isfinite(seconds) || seconds<=0 || seconds>300 || !isfinite(grace) || grace<0 || grace>30 ||
       !isfinite(now+seconds+grace))return 0;
    memset(m,0,sizeof(*m));m->active=1;m->request_id=request;m->t0=now;m->t1=now+seconds;m->grace=grace;return 1;
}
static inline int metal_measurement_receives(MetalFrameMeasurement *m,double arrival) {
    return m && m->active && !m->finalized && isfinite(arrival) && arrival>=m->t0 && arrival<m->t1+m->grace;
}
static inline MetalMeasurementFrame *metal_measurement_frame(MetalFrameMeasurement *m,uint64_t submission,uint64_t guest,double arrival) {
    if(!metal_measurement_receives(m,arrival))return NULL;
    if(!submission){++m->identity_errors;return NULL;}
    unsigned slot=(unsigned)((submission*UINT64_C(11400714819323198485))>>32)&(METAL_MEASUREMENT_HASH_SIZE-1);
    while(m->slots[slot]) {
        MetalMeasurementFrame *f=&m->frames[m->slots[slot]-1];
        if(f->submission_id==submission){if(f->guest_flip_id!=guest){++m->identity_errors;return NULL;}return f;}
        slot=(slot+1)&(METAL_MEASUREMENT_HASH_SIZE-1);
    }
    if(m->count==METAL_MEASUREMENT_CAPACITY){++m->overflow;return NULL;}
    MetalMeasurementFrame *f=&m->frames[m->count];m->slots[slot]=++m->count;
    f->submission_id=submission;f->guest_flip_id=guest;return f;
}
static inline void metal_measurement_admit(MetalFrameMeasurement *m,double now) {
    if(metal_measurement_receives(m,now) && now<m->t1)++m->admitted;
}
static inline void metal_measurement_drop(MetalFrameMeasurement *m,int kind,double now) {
    if(!metal_measurement_receives(m,now) || now>=m->t1)return;
    if(kind==METAL_MEASUREMENT_DROP_DRAWABLE)++m->nil_drawables;
    else if(kind==METAL_MEASUREMENT_DROP_BLIT)++m->skipped_blits;
    else ++m->submission_failures;
}
static inline void metal_measurement_commit(MetalFrameMeasurement *m,uint64_t submission,uint64_t guest,double now) {
    MetalMeasurementFrame *f=metal_measurement_frame(m,submission,guest,now);if(!f)return;
    if(f->committed){++m->identity_errors;return;}f->committed=1;f->commit_time=now;
}
static inline void metal_measurement_complete(MetalFrameMeasurement *m,uint64_t submission,uint64_t guest,int success,double arrival) {
    MetalMeasurementFrame *f=metal_measurement_frame(m,submission,guest,arrival);if(!f)return;
    if(f->completed || f->failed){++m->duplicate_callbacks;return;}
    if(success)f->completed=1;else f->failed=1;
}
static inline void metal_measurement_present(MetalFrameMeasurement *m,uint64_t submission,uint64_t guest,double time,double arrival) {
    if(!metal_measurement_receives(m,arrival))return;
    if(!isfinite(time) || time<0){++m->invalid_presented_time;return;}
    if(time==0){++m->zero_presented_time;return;}
    MetalMeasurementFrame *f=metal_measurement_frame(m,submission,guest,arrival);if(!f)return;
    if(f->presented) {
        ++m->duplicate_callbacks;
        if(time!=f->presented_time)++m->identity_errors;
        return;
    }
    /* Delivery order is independent of presentation order. Equal timestamps
     * across different submissions are deduplicated when the window closes. */
    if(time<m->last_presented)++m->out_of_order;
    if(time>m->last_presented)m->last_presented=time;
    f->presented=1;f->presented_time=time;
}
static inline int metal_measurement_id_compare(const void *a,const void *b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);
}
static inline int metal_measurement_time_compare(const void *a,const void *b) {
    const MetalMeasurementFrame *x=*(const MetalMeasurementFrame *const*)a;
    const MetalMeasurementFrame *y=*(const MetalMeasurementFrame *const*)b;
    if(x->presented_time!=y->presented_time)return (x->presented_time>y->presented_time)-(x->presented_time<y->presented_time);
    return (x->submission_id>y->submission_id)-(x->submission_id<y->submission_id);
}
static inline int metal_measurement_finish(MetalFrameMeasurement *m,double now,MetalFrameMeasurementResult *r) {
    if(!m || !r || !m->active || m->finalized || !isfinite(now) || now<m->t1+m->grace)return 0;
    memset(r,0,sizeof(*r));r->request_id=m->request_id;r->t0=m->t0;r->t1=m->t1;r->seconds=m->t1-m->t0;
    r->guest_id_floor=m->guest_id_floor;r->nil_drawables=m->nil_drawables;
    r->skipped_blits=m->skipped_blits;r->submission_failures=m->submission_failures;
    r->admitted=m->admitted;r->zero_presented_time=m->zero_presented_time;r->invalid_presented_time=m->invalid_presented_time;
    r->duplicate_callbacks=m->duplicate_callbacks;r->out_of_order=m->out_of_order;r->overflow=m->overflow;r->identity_errors=m->identity_errors;
    uint64_t ids[METAL_MEASUREMENT_CAPACITY];unsigned n=0,shown=0;
    MetalMeasurementFrame *presentations[METAL_MEASUREMENT_CAPACITY];
    for(unsigned i=0;i<m->count;i++) {
        MetalMeasurementFrame *f=&m->frames[i];
        if(!f->committed && (f->completed || f->failed || f->presented))++r->uncommitted_events;
        /* Leading frames committed before t0 cannot enter verified guest FPS. */
        if(!f->committed || f->commit_time<m->t0 || f->commit_time>=m->t1)continue;
        ++r->committed;if(f->completed)++r->gpu_completed;if(f->failed)++r->gpu_failed;
        if(!f->completed && !f->failed)++r->unresolved;
        if(!f->guest_flip_id)++r->uncorrelated;
        else if(f->guest_flip_id<=m->guest_id_floor)++r->preexisting_guest_submissions;
        if(!f->presented)++r->missing_presentations;
        if(f->presented && f->presented_time>=m->t0 && f->presented_time<m->t1) {
            presentations[shown++]=f;
        }
    }
    r->positive_presentations=shown;
    qsort(presentations,shown,sizeof(*presentations),metal_measurement_time_compare);
    for(unsigned i=0;i<shown;) {
        MetalMeasurementFrame *first=presentations[i];unsigned end=i+1;
        int ambiguous=0,all_completed=first->completed;
        while(end<shown && presentations[end]->presented_time==first->presented_time) {
            if(presentations[end]->guest_flip_id!=first->guest_flip_id)ambiguous=1;
            if(!presentations[end]->completed)all_completed=0;
            ++end;
        }
        ++r->displayed;r->duplicate_presented_time+=end-i-1;
        if(ambiguous)++r->ambiguous_presented_time;
        /* Choosing an arbitrary guest from a conflicting timestamp tie can
         * inflate unique guest work across groups. Exclude the entire tie. */
        else if(all_completed && first->guest_flip_id>m->guest_id_floor)ids[n++]=first->guest_flip_id;
        i=end;
    }
    qsort(ids,n,sizeof(*ids),metal_measurement_id_compare);
    for(unsigned i=0;i<n;i++) {if(!i || ids[i]!=ids[i-1])++r->unique_guest_displayed;else ++r->repeated_guest_displayed;}
    r->verified_guest_fps=r->unique_guest_displayed/r->seconds;
    r->qualified=!r->overflow && !r->identity_errors && !r->gpu_failed && !r->unresolved && !r->uncommitted_events && !r->uncorrelated && !r->skipped_blits && !r->submission_failures &&
        !r->zero_presented_time && !r->invalid_presented_time && r->displayed>0;
    m->finalized=1;return 1;
}
#endif
