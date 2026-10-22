/* Consumer-owned ordered reports. Tickets represent genuine backend completion;
 * this queue never manufactures completion and never runs on GPU callbacks. */
#ifndef RSX_DEFERRED_REPORTS_H
#define RSX_DEFERRED_REPORTS_H
#include <stdint.h>
#include <stdio.h>
#define RSX_DEFERRED_REPORT_CAPACITY 1024u
#define RSX_DEFERRED_REPORT_BATCH 32u
typedef struct rsx_deferred_report {
 uint64_t ticket,generation,vm,capture,start_ns;
 uint64_t enqueued_ns,issued_ns; /* diagnostic clock (RSX_ENGINE_PHASES); 0 when off */
 uint32_t ea,argument,context,index,main_memory,operation; /* 0 report;1 label */
} rsx_deferred_report;
typedef struct rsx_deferred_report_queue {
 rsx_deferred_report records[RSX_DEFERRED_REPORT_CAPACITY];
 uint32_t head,count,unissued,failed,max_pending;
 uint64_t generation,issued,published;
 uint64_t (*clock)(void); /* diagnostic: stamps issue times when set */
 uint32_t batch;           /* unissued records that force an issue; 0 = RSX_DEFERRED_REPORT_BATCH */
} rsx_deferred_report_queue;
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
  int ready=o->status(o->user,r->ticket,wait);if(!ready){if(wait)return rsx_reports_fail(q,r,"blocking-status-pending",r->ticket,ready,wait,1);break;}
  if(ready!=1)return rsx_reports_fail(q,r,"backend-status",r->ticket,ready,wait,1);
  if(r->generation!=q->generation)return rsx_reports_fail(q,r,"generation-mismatch",r->ticket,ready,wait,1);
  if(!o->publish(o->user,r))return rsx_reports_fail(q,r,"publish-validation",r->ticket,ready,wait,1);
  q->head=(q->head+1)%RSX_DEFERRED_REPORT_CAPACITY;q->count--;q->published++;
 }
 return 1;
}
static int rsx_reports_enqueue(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,rsx_deferred_report r){
 if(q->failed)return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY&&!rsx_reports_progress(q,o,1,1))return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY)return rsx_reports_fail(q,&q->records[q->head],"capacity-after-blocking-progress",q->records[q->head].ticket,0,1,0);
 r.ticket=0;r.generation=q->generation;q->records[(q->head+q->count)%RSX_DEFERRED_REPORT_CAPACITY]=r;
 q->count++;q->unissued++;if(q->count>q->max_pending)q->max_pending=q->count;
 if(q->unissued>=(q->batch?q->batch:RSX_DEFERRED_REPORT_BATCH)&&!rsx_reports_issue(q,o))return 0;
 return rsx_reports_progress(q,o,0,0);
}
static int rsx_reports_overlap(const rsx_deferred_report_queue *q,uint64_t ea,uint64_t bytes){
 if(!bytes||ea>UINT32_MAX||bytes>UINT64_C(0x100000000)-ea)return 0;
 for(uint32_t i=0;i<q->count;i++){const rsx_deferred_report *r=&q->records[(q->head+i)%RSX_DEFERRED_REPORT_CAPACITY];if(r->main_memory&&ea<(uint64_t)r->ea+(r->operation?4u:16u)&&(uint64_t)r->ea<ea+bytes)return 1;}
 return 0;
}
#endif
