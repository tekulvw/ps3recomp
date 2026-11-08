/* Early-release mode of the ordered report queue: applied-at-stream-position
 * records, ordering behind unapplied ones, verification-only completion. */
#include "../libs/video/rsx_deferred_reports.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t issued,complete;
static unsigned applied[16384],napplied,blocking,polls;
static int failed;
static uint64_t issue(void *p){(void)p;return ++issued;}
static int status(void *p,uint64_t ticket,int wait){
 (void)p;if(failed)return -1;
 if(wait){blocking++;if(complete<ticket)complete=ticket;}else polls++;
 return ticket<=complete;
}
/* Records carry a sequence number; publication order must equal stream order
 * and each record is applied exactly once. */
static int publish(void *p,const rsx_deferred_report *r){
 (void)p;assert(napplied<16384);
 if(napplied)assert(r->argument==applied[napplied-1]+1);
 applied[napplied++]=r->argument;return 1;
}
static rsx_deferred_report rec(unsigned seq){rsx_deferred_report r={0};r.argument=seq;r.operation=1;r.main_memory=1;r.ea=0x1000+seq*4u;return r;}
int main(void){
 rsx_deferred_report_ops ops={0,issue,status,publish};
 /* Outside early mode the early entry point is an ordinary enqueue. */
 { rsx_deferred_report_queue q={0};q.generation=1;
   assert(rsx_reports_enqueue_early(&q,&ops,rec(0)));
   assert(napplied==0&&q.gated==1&&q.applied_early==0);
   assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_WAIT,1));
   assert(napplied==1&&blocking==1&&q.count==0&&q.gated==0); }
 napplied=0;blocking=polls=0;complete=issued;
 rsx_deferred_report_queue q={0};q.generation=1;q.early_mode=1;
 /* Applied at once, before any completion; still queued for verification. */
 for(unsigned i=0;i<5;i++)assert(rsx_reports_enqueue_early(&q,&ops,rec(i)));
 assert(napplied==5&&q.count==5&&q.gated==0&&q.applied_early==5&&q.unissued==5);
 /* No earlier effect is pending: a waiting progress must not block. */
 assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_WAIT,1));
 assert(blocking==0&&q.count==5&&!q.unissued);
 /* Applied records do not hold guest destinations against VM transitions. */
 assert(!rsx_reports_overlap(&q,0x1000,64));
 /* Completion retires them without applying them a second time. */
 complete=issued;assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_NO_WAIT,0));
 assert(q.count==0&&napplied==5&&q.published==5);
 /* An unapplied record keeps every later record behind it. */
 assert(rsx_reports_enqueue(&q,&ops,rec(5)));
 assert(rsx_reports_enqueue_early(&q,&ops,rec(6)));
 assert(rsx_reports_enqueue_early(&q,&ops,rec(7)));
 assert(napplied==5&&q.gated==3&&rsx_reports_overlap(&q,0x1000+5*4u,4));
 assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_NO_WAIT,1));assert(napplied==5);
 /* A waiting progress blocks while unapplied records remain, then applies
  * them in order. */
 assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_WAIT,1));
 assert(blocking>0&&napplied==8&&q.count==0&&q.gated==0);
 /* Early application resumes once nothing unapplied precedes it. */
 assert(rsx_reports_enqueue_early(&q,&ops,rec(8)));assert(napplied==9&&q.gated==0);
 /* Receipts with nothing to apply are verification-only. */
 rsx_deferred_report receipt={0};receipt.operation=2;receipt.argument=9;
 assert(rsx_reports_enqueue_early(&q,&ops,receipt)&&napplied==10);
 assert(rsx_reports_issue(&q,&ops)&&!q.unissued);
 /* Capacity pressure still waits for every record, applied or not. */
 blocking=0;
 for(unsigned i=10;i<10+RSX_DEFERRED_REPORT_CAPACITY+4;i++)assert(rsx_reports_enqueue_early(&q,&ops,rec(i)));
 assert(blocking>0&&q.count<=RSX_DEFERRED_REPORT_CAPACITY);
 assert(napplied==10+RSX_DEFERRED_REPORT_CAPACITY+4);
 complete=issued;assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_NO_WAIT,1));
 complete=issued;assert(rsx_reports_progress(&q,&ops,RSX_REPORTS_NO_WAIT,0));assert(q.count==0);
 /* A backend failure after early application still latches: nothing later
  * is applied and the failure is never converted into completion. */
 unsigned before=napplied;
 assert(rsx_reports_enqueue_early(&q,&ops,rec(before)));assert(napplied==before+1);
 assert(rsx_reports_issue(&q,&ops));
 failed=1;assert(!rsx_reports_progress(&q,&ops,RSX_REPORTS_NO_WAIT,0));
 assert(q.failed&&q.count==1);
 assert(!rsx_reports_enqueue_early(&q,&ops,rec(before+1))&&napplied==before+1);
 /* A raised bound (PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY): applied records
  * queue up to it with no completion wait, and the bound still holds. */
 { failed=0;napplied=0;blocking=0;issued=complete=0;
   static rsx_deferred_report_queue big;memset(&big,0,sizeof big);
   big.generation=1;big.early_mode=1;big.capacity=4096;
   assert(rsx_reports_capacity(&big)==4096);
   for(unsigned i=0;i<4096;i++)assert(rsx_reports_enqueue_early(&big,&ops,rec(i)));
   assert(blocking==0&&big.count==4096&&big.capacity_waits==0&&napplied==4096);
   /* Tickets cover every record, oldest first, across many issues. */
   for(unsigned i=0;i<big.count;i++)assert(big.records[(big.head+i)%RSX_DEFERRED_REPORT_STORAGE].ticket==0||
     big.records[(big.head+i)%RSX_DEFERRED_REPORT_STORAGE].ticket==1+i/RSX_DEFERRED_REPORT_BATCH);
   assert(rsx_reports_enqueue_early(&big,&ops,rec(4096)));
   assert(blocking>0&&big.capacity_waits==1&&big.count<=4096&&napplied==4097);
   /* Completion retires the ring in order across the wrap of the storage. */
   for(unsigned round=0;round<3;round++){
     for(unsigned i=0;i<3000;i++)assert(rsx_reports_enqueue_early(&big,&ops,rec(napplied)));
     complete=issued;assert(rsx_reports_progress(&big,&ops,RSX_REPORTS_NO_WAIT,1));
     complete=issued;assert(rsx_reports_progress(&big,&ops,RSX_REPORTS_NO_WAIT,0));
     assert(big.count==0);
   }
   /* An out-of-range bound falls back to the default. */
   big.capacity=RSX_DEFERRED_REPORT_STORAGE+1;assert(rsx_reports_capacity(&big)==RSX_DEFERRED_REPORT_CAPACITY); }
 puts("PASS early-release queue: raised capacity bound;");
 puts("PASS early-release queue: stream-order application, no double publication, gated ordering, capacity, receipts, sticky failure");
}
