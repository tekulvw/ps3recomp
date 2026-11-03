/* Consumer-owned ordered reports. Tickets represent genuine backend completion;
 * this queue never manufactures completion and never runs on GPU callbacks.
 *
 * Early mode (opt-in, early_mode=1): a record whose guest effect does not
 * depend on GPU results may be applied at its stream position, but only when
 * no earlier record is still waiting, so guest-visible order never changes.
 * Such a record stays queued until its genuine completion purely to verify
 * the backend: it is never applied twice, and a failure still latches. A
 * waiting progress then waits only for records not yet applied; capacity
 * pressure still waits for everything. */
#ifndef RSX_DEFERRED_REPORTS_H
#define RSX_DEFERRED_REPORTS_H
#include <stdint.h>
#include <stdio.h>
#define RSX_DEFERRED_REPORT_CAPACITY 1024u
#define RSX_DEFERRED_REPORT_BATCH 32u
typedef struct rsx_deferred_report {
 uint64_t ticket,generation,vm,capture,start_ns;
 uint64_t enqueued_ns,issued_ns; /* diagnostic clock (RSX_ENGINE_PHASES); 0 when off */
 uint32_t ea,argument,context,index,main_memory,operation; /* 0 report;1 label;2 completion receipt only */
 uint32_t early; /* guest effect already applied at its stream position */
} rsx_deferred_report;
typedef struct rsx_deferred_report_queue {
 rsx_deferred_report records[RSX_DEFERRED_REPORT_CAPACITY];
 uint32_t head,count,unissued,failed,max_pending;
 uint64_t generation,issued,published;
 uint64_t (*clock)(void); /* diagnostic: stamps issue times when set */
 uint32_t batch;           /* unissued records that force an issue; 0 = RSX_DEFERRED_REPORT_BATCH */
 uint32_t early_mode;      /* 1: waits cover only records whose effect is not applied */
 uint32_t gated;           /* queued records whose guest effect still awaits completion */
 uint64_t applied_early;   /* records applied at their stream position (diagnostic) */
} rsx_deferred_report_queue;
#define RSX_REPORTS_NO_WAIT 0
#define RSX_REPORTS_WAIT 1     /* every record, or in early mode every unapplied one */
#define RSX_REPORTS_WAIT_ALL 2 /* every record, applied or not */
typedef struct rsx_deferred_report_ops {
 void *user;
 uint64_t (*issue)(void*);
 int (*status)(void*,uint64_t,int); /* -1 failed;0 pending;1 genuine success */
 int (*publish)(void*,const rsx_deferred_report*);
} rsx_deferred_report_ops;
/* Consumer-owned, first failure only. Preserve the failing record and all
 * queue progress; this witness must never grant completion or retry effects. */
static int rsx_reports_fail(rsx_deferred_report_queue *q,const rsx_deferred_report *r,
 const char *reason,uint64_t ticket,int status,int wait,int status_queried){
 if(!q->failed){
  q->failed=1;
  fprintf(stderr,"[deferred reports failure] reason=%s ticket=%llu status=%d status_queried=%d wait=%d generation=%llu record_generation=%llu head=%u count=%u unissued=%u issued=%llu published=%llu ea=%08X operation=%u context=%08X\n",
   reason,(unsigned long long)ticket,status,status_queried,wait,(unsigned long long)q->generation,
   (unsigned long long)(r?r->generation:0),q->head,q->count,q->unissued,
   (unsigned long long)q->issued,(unsigned long long)q->published,
   r?r->ea:0,r?r->operation:0,r?r->context:0);
 }
 return 0;
}
static int rsx_reports_issue(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o){
 if(q->failed)return 0;if(!q->unissued)return 1;
 uint64_t ticket=o->issue(o->user);if(!ticket)return rsx_reports_fail(q,q->count?&q->records[q->head]:NULL,"issue-zero",0,0,0,0);
 for(uint32_t i=0;i<q->count;i++){rsx_deferred_report *r=&q->records[(q->head+i)%RSX_DEFERRED_REPORT_CAPACITY];if(!r->ticket){r->ticket=ticket;r->issued_ns=q->clock?q->clock():0;}}
 q->unissued=0;q->issued++;return 1;
}
static int rsx_reports_progress(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,int wait,int issue){
 if(q->failed)return 0;if(issue&&!rsx_reports_issue(q,o))return 0;
 while(q->count){rsx_deferred_report *r=&q->records[q->head];if(!r->ticket)break;
  /* An applied-only queue needs no blocking wait except under capacity. */
  const int block=wait==RSX_REPORTS_WAIT_ALL||(wait&&(!q->early_mode||q->gated));
  int ready=o->status(o->user,r->ticket,block);if(!ready){if(block)return rsx_reports_fail(q,r,"blocking-status-pending",r->ticket,ready,block,1);break;}
  if(ready!=1)return rsx_reports_fail(q,r,"backend-status",r->ticket,ready,block,1);
  if(r->generation!=q->generation)return rsx_reports_fail(q,r,"generation-mismatch",r->ticket,ready,block,1);
  if(!r->early&&!o->publish(o->user,r))return rsx_reports_fail(q,r,"publish-validation",r->ticket,ready,block,1);
  if(!r->early&&q->gated)q->gated--;
  q->head=(q->head+1)%RSX_DEFERRED_REPORT_CAPACITY;q->count--;q->published++;
 }
 return 1;
}
static int rsx_reports_push(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,rsx_deferred_report r,int may_apply){
 if(q->failed)return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY&&!rsx_reports_progress(q,o,RSX_REPORTS_WAIT_ALL,1))return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY)return rsx_reports_fail(q,&q->records[q->head],"capacity-after-blocking-progress",q->records[q->head].ticket,0,1,0);
 r.ticket=0;r.generation=q->generation;r.early=0;
 /* Apply now only behind already-applied records: stream order is kept. */
 if(may_apply&&q->early_mode&&!q->gated){
  if(!o->publish(o->user,&r))return rsx_reports_fail(q,&r,"early-publish-validation",0,0,0,0);
  r.early=1;q->applied_early++;
 }
 q->records[(q->head+q->count)%RSX_DEFERRED_REPORT_CAPACITY]=r;
 q->count++;q->unissued++;if(!r.early)q->gated++;if(q->count>q->max_pending)q->max_pending=q->count;
 if(q->unissued>=(q->batch?q->batch:RSX_DEFERRED_REPORT_BATCH)&&!rsx_reports_issue(q,o))return 0;
 return rsx_reports_progress(q,o,0,0);
}
static int rsx_reports_enqueue(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,rsx_deferred_report r){
 return rsx_reports_push(q,o,r,0);
}
/* Caller asserts the record's guest effect needs no GPU result. Outside early
 * mode, or behind an unapplied record, this is an ordinary enqueue. */
static int rsx_reports_enqueue_early(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,rsx_deferred_report r){
 return rsx_reports_push(q,o,r,1);
}
/* Only effects still to be applied can touch guest memory later. */
static int rsx_reports_overlap(const rsx_deferred_report_queue *q,uint64_t ea,uint64_t bytes){
 if(!bytes||ea>UINT32_MAX||bytes>UINT64_C(0x100000000)-ea)return 0;
 for(uint32_t i=0;i<q->count;i++){const rsx_deferred_report *r=&q->records[(q->head+i)%RSX_DEFERRED_REPORT_CAPACITY];if(r->main_memory&&!r->early&&ea<(uint64_t)r->ea+(r->operation?4u:16u)&&(uint64_t)r->ea<ea+bytes)return 1;}
 return 0;
}
#endif
