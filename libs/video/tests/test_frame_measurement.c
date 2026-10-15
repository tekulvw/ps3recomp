/* These cases catch inflated guest FPS, shortened denominators, early callback
 * assumptions, stale-session reuse, and silently lost telemetry. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if !__has_include("metal_frame_measurement.h")
int main(void) { assert(!"metal_frame_measurement.h feature is missing"); }
#else
#include "metal_frame_measurement.h"
static MetalFrameMeasurement m;
static void begin(void) { assert(metal_measurement_begin(&m,7,10,2,1)); }
static MetalFrameMeasurementResult finish(void) {
 MetalFrameMeasurementResult r;
 assert(!metal_measurement_finish(&m,12.9,&r));
 assert(metal_measurement_finish(&m,13,&r));return r;
}
static void frame(uint64_t sub,uint64_t guest,double commit,double display) {
 metal_measurement_commit(&m,sub,guest,commit);
 metal_measurement_complete(&m,sub,guest,1,commit+.01);
 metal_measurement_present(&m,sub,guest,display,display+.02);
}
int main(void) {
 begin();frame(1,1,10.1,10.2);frame(2,1,10.3,10.4);frame(3,2,10.5,10.6);
 MetalFrameMeasurementResult r=finish();
 assert(r.committed==3 && r.gpu_completed==3 && r.displayed==3);
 assert(r.unique_guest_displayed==2 && r.repeated_guest_displayed==1);
 assert(r.seconds==2 && r.verified_guest_fps==1 && r.qualified);
 /* A presentation callback may precede GPU completion and commit bookkeeping. */
 begin();metal_measurement_present(&m,1,42,10.2,10.3);
 metal_measurement_complete(&m,1,42,1,10.4);metal_measurement_commit(&m,1,42,10.1);
 r=finish();assert(r.unique_guest_displayed==1 && r.qualified);
 /* Zero/nonfinite timestamps never turn GPU completion into display FPS. */
 begin();metal_measurement_commit(&m,1,1,10.1);metal_measurement_complete(&m,1,1,1,10.2);
 metal_measurement_present(&m,1,1,0,10.3);metal_measurement_present(&m,1,1,NAN,10.4);
 r=finish();assert(r.zero_presented_time==1 && r.invalid_presented_time==1);
 assert(r.displayed==0 && r.verified_guest_fps==0 && !r.qualified);
 /* Boot/unknown IDs, missing GPU completion, and GPU errors remain explicit. */
 begin();frame(1,0,10.1,10.2);metal_measurement_commit(&m,2,2,10.3);
 metal_measurement_present(&m,2,2,10.4,10.5);metal_measurement_commit(&m,3,3,10.6);
 metal_measurement_complete(&m,3,3,0,10.7);metal_measurement_present(&m,3,3,10.8,10.9);
 r=finish();assert(r.uncorrelated==1 && r.unresolved==1 && r.gpu_failed==1);
 assert(!r.qualified && r.unique_guest_displayed==0);
 /* Boundary timestamps and delayed callbacks use [10,12), never event endpoints. */
 begin();frame(1,1,9.9,10.0);frame(2,2,10.1,11.5);
 metal_measurement_commit(&m,3,3,11.9);metal_measurement_complete(&m,3,3,1,12.1);
 metal_measurement_present(&m,3,3,11.9,12.8);
 frame(4,4,12.0,12.0);r=finish();
 assert(r.committed==2 && r.unique_guest_displayed==2 && r.seconds==2);
 /* Arrival reordering is diagnostic; one submission still has one display event. */
 begin();frame(1,1,10.1,10.5);metal_measurement_present(&m,1,1,10.5,10.6);
 frame(2,2,10.2,10.4);r=finish();
 assert(r.duplicate_callbacks==1 && r.out_of_order==1 && r.unique_guest_displayed==2 && r.qualified);
 /* Starting while active must not reset a window. Reuse after finalization must. */
 begin();assert(!metal_measurement_begin(&m,8,11,2,1));r=finish();
 assert(r.request_id==7);assert(metal_measurement_begin(&m,8,20,2,1));
 assert(metal_measurement_finish(&m,23,&r));assert(r.request_id==8 && r.committed==0);
 /* Equal positive timestamps across nonconsecutive/reordered submissions are
  * one physical event. Conflicting guest identities make the tie ambiguous. */
 begin();frame(1,1,10.1,10.5);frame(2,2,10.2,10.6);frame(3,3,10.3,10.5);
 r=finish();assert(r.displayed==2 && r.positive_presentations==3 && r.duplicate_presented_time==1);
 assert(r.ambiguous_presented_time==1 && r.unique_guest_displayed==1 && r.qualified);
 /* Picking arbitrary guests from two ambiguous groups could claim two unique
  * guest frames when both reported events might have displayed guest 1. */
 begin();frame(1,2,10.1,10.4);frame(2,1,10.2,10.4);
 frame(3,3,10.3,10.5);frame(4,1,10.4,10.5);
 r=finish();assert(r.displayed==2 && r.ambiguous_presented_time==2 && r.unique_guest_displayed==0);
 /* A tied timestamp for the same guest remains exactly one verified event. */
 begin();frame(1,7,10.1,10.5);frame(2,7,10.2,10.5);
 r=finish();assert(r.displayed==1 && r.unique_guest_displayed==1 && r.duplicate_presented_time==1 && r.qualified);
 /* One submission claiming two different positive timestamps is inconsistent. */
 begin();frame(1,1,10.1,10.5);metal_measurement_present(&m,1,1,10.6,10.7);
 r=finish();assert(r.identity_errors==1 && r.displayed==1 && !r.qualified);
 /* A callback without its real commit marker cannot silently disappear. */
 begin();metal_measurement_complete(&m,1,1,1,10.2);metal_measurement_present(&m,1,1,10.3,10.4);
 r=finish();assert(r.uncommitted_events==1 && !r.qualified && r.unique_guest_displayed==0);
 /* Capacity exhaustion and inconsistent IDs invalidate the result explicitly. */
 begin();for(unsigned i=1;i<=METAL_MEASUREMENT_CAPACITY+1;i++)metal_measurement_commit(&m,i,i,10.1);
 r=finish();assert(r.overflow>0 && !r.qualified);
 begin();metal_measurement_commit(&m,1,1,10.1);metal_measurement_complete(&m,1,2,1,10.2);
 r=finish();assert(r.identity_errors>0 && !r.qualified);
 /* Re-presenting a guest ID that existed before t0 cannot count as new work. */
 begin();m.guest_id_floor=10;frame(1,10,10.1,10.2);frame(2,11,10.3,10.4);
 r=finish();assert(r.preexisting_guest_submissions==1 && r.unique_guest_displayed==1);
 begin();metal_measurement_drop(&m,METAL_MEASUREMENT_DROP_DRAWABLE,10.2);
 metal_measurement_drop(&m,METAL_MEASUREMENT_DROP_BLIT,10.3);
 metal_measurement_drop(&m,METAL_MEASUREMENT_DROP_SUBMISSION,10.4);
 r=finish();assert(r.nil_drawables==1 && r.skipped_blits==1 && r.submission_failures==1 && !r.qualified);
 assert(!metal_measurement_begin(&m,1,NAN,2,1));assert(!metal_measurement_begin(&m,1,10,0,1));
 puts("Frame measurement boundaries, correlation, errors, and lifecycle passed");
}

#endif
