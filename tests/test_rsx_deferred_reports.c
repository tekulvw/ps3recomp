#include "../libs/video/rsx_deferred_reports.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t issued,complete;
static unsigned published,waits;
static int failed;
static uint64_t issue(void *p){(void)p;return ++issued;}
static int status(void *p,uint64_t ticket,int wait){(void)p;if(failed)return -1;if(wait){waits++;complete=ticket;}return ticket<=complete;}
static int publish(void *p,const rsx_deferred_report *r){(void)p;assert(r->ticket<=complete);assert(r->argument==published);published++;return 1;}
int main(void){
 rsx_deferred_report_queue q={0};rsx_deferred_report_ops ops={0,issue,status,publish};q.generation=1;
 for(unsigned i=0;i<RSX_DEFERRED_REPORT_BATCH;i++){rsx_deferred_report r={0};r.argument=i;assert(rsx_reports_enqueue(&q,&ops,r));}
 assert(issued==1&&published==0&&q.count==RSX_DEFERRED_REPORT_BATCH);
 assert(rsx_reports_progress(&q,&ops,0,1));assert(published==0);
 complete=1;assert(rsx_reports_progress(&q,&ops,0,1));assert(published==RSX_DEFERRED_REPORT_BATCH&&q.count==0);
 rsx_deferred_report r={0};r.argument=published;r.ea=0x100;r.main_memory=1;assert(rsx_reports_enqueue(&q,&ops,r));assert(rsx_reports_overlap(&q,0x10f,1));assert(!rsx_reports_overlap(&q,0x110,16));assert(!rsx_reports_overlap(&q,0xfffffff0,32));
 assert(rsx_reports_progress(&q,&ops,1,1));assert(waits==1&&q.count==0);
 for(unsigned i=0;i<RSX_DEFERRED_REPORT_CAPACITY+1;i++){r.argument=published+q.count;assert(rsx_reports_enqueue(&q,&ops,r));}
 assert(q.count<=RSX_DEFERRED_REPORT_CAPACITY&&waits>1);assert(rsx_reports_progress(&q,&ops,1,1));
 r.argument=published;assert(rsx_reports_enqueue(&q,&ops,r));failed=1;assert(!rsx_reports_progress(&q,&ops,1,1));assert(q.failed&&q.count==1);assert(!rsx_reports_enqueue(&q,&ops,r));
 puts("PASS ordered deferred reports: no early publication, batch/end-FIFO/pressure progress, overlap bounds, failure never completes");
}
