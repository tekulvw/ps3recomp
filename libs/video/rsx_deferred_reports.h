/* Consumer-owned ordered reports. Tickets represent genuine backend completion;
 * this queue never manufactures completion and never runs on GPU callbacks. */
#ifndef RSX_DEFERRED_REPORTS_H
#define RSX_DEFERRED_REPORTS_H
#include <stdint.h>
#define RSX_DEFERRED_REPORT_CAPACITY 1024u
#define RSX_DEFERRED_REPORT_BATCH 32u
typedef struct rsx_deferred_report {
 uint64_t ticket,generation,vm,capture,start_ns;
 uint32_t ea,argument,context,index,main_memory;
} rsx_deferred_report;
typedef struct rsx_deferred_report_queue {
 rsx_deferred_report records[RSX_DEFERRED_REPORT_CAPACITY];
 uint32_t head,count,unissued,failed,max_pending;
 uint64_t generation,issued,published;
} rsx_deferred_report_queue;
typedef struct rsx_deferred_report_ops {
 void *user;
 uint64_t (*issue)(void*);
 int (*status)(void*,uint64_t,int); /* -1 failed;0 pending;1 genuine success */
 int (*publish)(void*,const rsx_deferred_report*);
} rsx_deferred_report_ops;
static int rsx_reports_issue(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o){
 if(q->failed)return 0;if(!q->unissued)return 1;
 uint64_t ticket=o->issue(o->user);if(!ticket){q->failed=1;return 0;}
 for(uint32_t i=0;i<q->count;i++){rsx_deferred_report *r=&q->records[(q->head+i)%RSX_DEFERRED_REPORT_CAPACITY];if(!r->ticket)r->ticket=ticket;}
 q->unissued=0;q->issued++;return 1;
}
static int rsx_reports_progress(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,int wait,int issue){
 if(q->failed)return 0;if(issue&&!rsx_reports_issue(q,o))return 0;
 while(q->count){rsx_deferred_report *r=&q->records[q->head];if(!r->ticket)break;
  int ready=o->status(o->user,r->ticket,wait);if(!ready){if(wait){q->failed=1;return 0;}break;}
  if(ready!=1||r->generation!=q->generation||!o->publish(o->user,r)){q->failed=1;return 0;}
  q->head=(q->head+1)%RSX_DEFERRED_REPORT_CAPACITY;q->count--;q->published++;
 }
 return 1;
}
static int rsx_reports_enqueue(rsx_deferred_report_queue *q,const rsx_deferred_report_ops *o,rsx_deferred_report r){
 if(q->failed)return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY&&!rsx_reports_progress(q,o,1,1))return 0;
 if(q->count==RSX_DEFERRED_REPORT_CAPACITY){q->failed=1;return 0;}
 r.ticket=0;r.generation=q->generation;q->records[(q->head+q->count)%RSX_DEFERRED_REPORT_CAPACITY]=r;
 q->count++;q->unissued++;if(q->count>q->max_pending)q->max_pending=q->count;
 if(q->unissued>=RSX_DEFERRED_REPORT_BATCH&&!rsx_reports_issue(q,o))return 0;
 return rsx_reports_progress(q,o,0,0);
}
static int rsx_reports_overlap(const rsx_deferred_report_queue *q,uint64_t ea,uint64_t bytes){
 if(!bytes||ea>UINT32_MAX||bytes>UINT64_C(0x100000000)-ea)return 0;
 for(uint32_t i=0;i<q->count;i++){const rsx_deferred_report *r=&q->records[(q->head+i)%RSX_DEFERRED_REPORT_CAPACITY];if(r->main_memory&&ea<(uint64_t)r->ea+16&&(uint64_t)r->ea<ea+bytes)return 1;}
 return 0;
}
#endif
