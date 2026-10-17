#include "../job_io_recorder.h"
#include "../../spu/spu_context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Records two transfers on an armed context, ignores a second context, and
 * verifies the file layout: header, entry state, events with data, exit state. */
int main(int argc,char **argv){
 assert(argc==2);
 static spu_context a,b;static uint8_t lsa_store[SPU_LS_SIZE],lsb_store[SPU_LS_SIZE];
 a.ls=lsa_store;b.ls=lsb_store;a.pc=0x12345;lsa_store[0x100]=0xAB;
 assert(!ps3_jobio_is(&a));
 assert(!ps3_jobio_begin(&a,77,argv[1]));
 assert(ps3_jobio_is(&a)&&!ps3_jobio_is(&b));
 assert(ps3_jobio_begin(&b,1,argv[1])==2);
 uint8_t get[4]={1,2,3,4},put[2]={9,8};
 ps3_jobio_event(&a,PS3_JOBIO_GET,0x40,0x200,0x10000000ull,4,get,0);
 ps3_jobio_event(&b,PS3_JOBIO_GET,0x40,0,0,4,get,0);
 ps3_jobio_event(&a,PS3_JOBIO_PUT,0x20,0x300,0x20000000ull,2,put,0);
 ps3_jobio_event(&a,PS3_JOBIO_ZERO,0x2000,0,0x30000000ull,128,NULL,0);
 lsa_store[0x100]=0xCD;
 assert(ps3_jobio_end(&b)==-1);
 assert(!ps3_jobio_end(&a)&&!ps3_jobio_is(&a));
 char path[1024];snprintf(path,sizeof path,"%s/job-0000.jobio",argv[1]);
 FILE *f=fopen(path,"rb");assert(f);
 uint32_t head[8];uint64_t times[2];assert(fread(head,sizeof head,1,f)==1&&fread(times,sizeof times,1,f)==1);
 assert(head[0]==0x4a4f4249u&&head[1]==1&&head[2]==77&&head[3]==0x12345&&head[4]==SPU_LS_SIZE&&head[6]==0);
 assert(head[7]==3*28+4+2);
 uint8_t *entry=malloc(SPU_LS_SIZE);assert(fread(entry,SPU_LS_SIZE,1,f)==1&&entry[0x100]==0xAB);
 fseek(f,(long)sizeof a.gpr,SEEK_CUR);
 uint32_t ev[7];uint8_t data[4];
 assert(fread(ev,28,1,f)==1&&ev[0]==PS3_JOBIO_GET&&ev[3]==0x10000000u&&ev[4]==4);assert(fread(data,4,1,f)==1&&!memcmp(data,get,4));
 assert(fread(ev,28,1,f)==1&&ev[0]==PS3_JOBIO_PUT&&ev[4]==2);assert(fread(data,2,1,f)==1&&!memcmp(data,put,2));
 assert(fread(ev,28,1,f)==1&&ev[0]==PS3_JOBIO_ZERO&&ev[4]==128);
 uint8_t *exitls=malloc(SPU_LS_SIZE);assert(fread(exitls,SPU_LS_SIZE,1,f)==1&&exitls[0x100]==0xCD);
 fclose(f);free(entry);free(exitls);puts("PASS");return 0;
}
