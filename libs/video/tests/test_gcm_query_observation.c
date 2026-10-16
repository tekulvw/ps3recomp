#define main report_fixture_main
#include "test_gcm_report_publication.c"
#undef main
static uint64_t capture=2;
static unsigned events;
static struct{uint64_t owner,generation,words[7];} observed[8];
uint64_t ps3_critical_capture_id(void){return capture;}
int ps3_critical_enabled(void){return 1;}
void ps3_critical_emit_capture(uint64_t id,uint32_t kind,uint64_t owner,uint64_t span,const uint64_t*w,uint32_t n){
 assert(id==2&&kind==22&&n==7&&events<8);
 observed[events].owner=owner;observed[events].generation=span;
 memcpy(observed[events++].words,w,n*sizeof(*w));
}
int main(void){
 s_query_observe=1;
 gcm_query_observe(0x1800,0x01000430,4);assert(events==1&&observed[0].words[3]==0);
 gcm_query_observe(0x17cc,1,8);gcm_query_observe(0x1814,7,12);
 gcm_query_observe(0x1800,0x01000430,16);
 assert(events==3&&observed[2].words[2]==1&&observed[2].words[3]==1&&observed[2].words[4]==1);
 gcm_query_observe(0x17c8,1,20);gcm_query_observe(0x1824,7,24);gcm_query_observe(0x1824,9,28);
 gcm_query_observe(0x1800,0x01000440,32);
 assert(events==5&&observed[4].generation==1&&observed[4].words[3]==3&&observed[4].words[4]==2&&observed[4].words[6]==1);
 gcm_query_observe(0x17cc,0,36);assert(events==6&&observed[5].words[2]==0);
 capture=1;gcm_query_observe(0x17c8,0,40);assert(events==6);
 assert(vm_base==NULL);puts("PASS query observations: unknown state, enable/clear generations, draw words, capture gate; no guest writes");
}
