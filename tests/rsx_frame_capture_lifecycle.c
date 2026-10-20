/* Actual finite-capture include with a neutral backend; synthetic files only. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned callbacks;
static int capture(void*user,uint32_t handle,const char*path)
{
 (void)user;assert(handle==77);++callbacks;
 FILE*f=fopen(path,"wb");assert(f);assert(fwrite("RGBA",1,4,f)==4);fclose(f);return 1;
}
static struct {int(*capture_texture_rgba8)(void*,uint32_t,const char*);void*user;} backend={capture,NULL};
static struct {unsigned frames;struct {unsigned handle,location,offset,w,h;}surfaces[1];typeof(backend)*be;}g={0,{{77,0,128,1,1}},&backend};
uint64_t ps3_critical_now_ns(void){static uint64_t t=100;return ++t;}
#include "rsx_frame_capture.inc"
static void write_text(const char*path,const char*text)
{FILE*f=fopen(path,"w");assert(f);fputs(text,f);fclose(f);}
int main(int argc,char**argv)
{
 assert(argc==3);
 if(!strcmp(argv[1],"off")){
  unsetenv("RSX_ENGINE_FRAME_CAPTURE");unsetenv("RSX_ENGINE_FRAME_CAPTURE_REQUEST");
  frame_capture_present(0,0);frame_capture_draw(2,0);assert(!callbacks&&!s_frame_capture.enabled);return 0;
 }
 char prefix[1024],request[1024],path[2048];snprintf(prefix,sizeof prefix,"%s/frame",argv[2]);snprintf(request,sizeof request,"%s/request",argv[2]);
 setenv("RSX_ENGINE_FRAME_CAPTURE",prefix,1);setenv("RSX_ENGINE_FRAME_CAPTURE_REQUEST",request,1);setenv("RSX_ENGINE_FRAME_CAPTURE_SESSION","neutral-session",1);
 write_text(request,"1\n");frame_capture_present(0,0);frame_capture_draw(0x1002,0);
 snprintf(path,sizeof path,"%s-1.json",prefix);write_text(path,"OLD_RECEIPT");
 frame_capture_present(0,0);assert(!callbacks);FILE*f=fopen(path,"r");assert(f);char text[4096];assert(fread(text,1,11,f)==11);fclose(f);assert(!memcmp(text,"OLD_RECEIPT",11));
 write_text(request,"2\n");frame_capture_present(0,0);frame_capture_draw(0x2002,0);
 snprintf(path,sizeof path,"%s-2.rgba",prefix);write_text(path,"OLD_PIXELS");frame_capture_present(0,0);assert(!callbacks);
 f=fopen(path,"r");assert(f);assert(fread(text,1,10,f)==10);fclose(f);assert(!memcmp(text,"OLD_PIXELS",10));
 write_text(request,"3\n");frame_capture_present(0,0);frame_capture_draw(0x3002,0);frame_capture_present(0,0);assert(callbacks==1);
 snprintf(path,sizeof path,"%s-3.json",prefix);f=fopen(path,"r");assert(f);size_t n=fread(text,1,sizeof text-1,f);text[n]=0;fclose(f);assert(strstr(text,"\"saved\":true")&&strstr(text,"neutral-session"));
 write_text(request,"4\n");frame_capture_present(0,0);frame_capture_draw(0x4002,0);frame_capture_present(0,0);assert(callbacks==2);
 write_text(request,"5\n");frame_capture_present(0,0);frame_capture_draw(0x5002,0);frame_capture_present(0,0);assert(callbacks==2);
 puts("frame capture actual helper: default off, exclusive receipt/pixel collision refusal, complete frame and four-token bound PASS");
}
