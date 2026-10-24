#include "../rsx_picture_identity.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#ifndef RSX_PICTURE_HAS_RETAIN
int rsx_picture_retain(u64 token){(void)token;return 0;}
#endif
#ifdef RSX_HAS_PICTURE_IDENTITIES
#include "../rsx_picture_identity.inc"
#else
u64 rsx_picture_create(u32 ea,u32 loc,u32 off,u32 bytes,const u8*p){(void)ea;(void)loc;(void)off;(void)bytes;(void)p;return 0;}
#endif
#ifdef RSX_HAS_PICTURE_IDENTITIES
static u64 threaded_token;
static void* reader(void*arg){u32 n=(u32)(uintptr_t)arg;u32 desc=0x3000+n*0x100,cmd=0x4004+n*0x100;
    for(unsigned i=0;i<10000;++i){u64 out=0;assert(rsx_picture_bind_descriptor(desc,threaded_token)==1);assert(rsx_picture_capture_command(cmd,desc,1,0x2000)==1);assert(rsx_picture_take_command(cmd,1,0x2000,&out)==1 && out==threaded_token);const u8*p=rsx_picture_data(out,1,0x2000,32);assert(p && p[0]==0x17 && p[31]==0x17);assert(rsx_picture_bind_descriptor(desc,0)==1);rsx_picture_release(out);}return NULL;}
static void* producer(void*arg){(void)arg;u8 pixels[32];memset(pixels,0x89,32);for(unsigned i=0;i<10000;++i){u64 t=rsx_picture_create(0x1000,1,0x2000,32,pixels);assert(t);rsx_picture_release(t);}return NULL;}
#endif
int main(int argc,char**argv){
    assert(argc==2);
    u8 a[32],b[32];memset(a,0x17,32);memset(b,0x89,32);
#ifdef RSX_HAS_PICTURE_IDENTITIES
    if(!strcmp(argv[1],"threaded")){
        threaded_token=rsx_picture_create(0x1000,1,0x2000,32,a);assert(threaded_token);pthread_t t[3];
        assert(!pthread_create(&t[0],NULL,reader,(void*)(uintptr_t)0));assert(!pthread_create(&t[1],NULL,reader,(void*)(uintptr_t)1));assert(!pthread_create(&t[2],NULL,producer,NULL));
        for (unsigned i=0; i<3; ++i) { assert(!pthread_join(t[i],NULL)); }
        assert(rsx_picture_live_count()==1);rsx_picture_release(threaded_token);assert(rsx_picture_live_count()==0 && rsx_picture_reset());puts("PASS30000 concurrent producer/descriptor/command lifetime iterations");return 0;
    }
    if(!strcmp(argv[1],"capacity")){
        u64 tokens[32];for(unsigned i=0;i<32;++i){tokens[i]=rsx_picture_create(0x1000,1,0x2000,32,a);assert(tokens[i]);}
        assert(!rsx_picture_create(0x1000,1,0x2000,32,a));assert(!rsx_picture_ready());for(unsigned i=0;i<32;++i)rsx_picture_release(tokens[i]);assert(!rsx_picture_live_count() && rsx_picture_reset());puts("PASS32 snapshot bound and safe owner cleanup");return 0;
    }
    if(!strcmp(argv[1],"aliases") || !strcmp(argv[1],"commands")){
        u64 token=rsx_picture_create(0x1000,1,0x2000,32,a);assert(token);
        if(!strcmp(argv[1],"aliases")){
            for(unsigned i=0;i<256;++i)assert(rsx_picture_bind_descriptor(0x3000+i*40,token)==1);
            assert(rsx_picture_bind_descriptor(0x3000+256*40,token)<0);assert(!rsx_picture_ready());
            for(unsigned i=0;i<256;++i)assert(rsx_picture_bind_descriptor(0x3000+i*40,0)==1);
            rsx_picture_release(token);assert(!rsx_picture_live_count() && rsx_picture_reset());
        }else{
            assert(rsx_picture_bind_descriptor(0x3000,token)==1);
            for(unsigned i=0;i<4096;++i)assert(rsx_picture_capture_command(0x4004+i*40,0x3000,1,0x2000)==1);
            assert(rsx_picture_capture_command(0x4004+4096*40,0x3000,1,0x2000)<0);assert(!rsx_picture_ready());
            assert(rsx_picture_bind_descriptor(0x3000,0)==1);rsx_picture_release(token);assert(rsx_picture_live_count()==1 && !rsx_picture_reset());
        }
        puts("PASS bounded alias/command capacity, failure latch and resolved vs quarantined ownership");return 0;
    }
#endif
    u64 first=rsx_picture_create(0x1000,1,0x2000,32,a);assert(first);
#ifdef RSX_HAS_PICTURE_IDENTITIES
    assert(rsx_picture_retain(first)==1);rsx_picture_release(first);
    assert(rsx_picture_bind_descriptor(0x3000,first)==1);
    assert(rsx_picture_capture_command(0x4004,0x3000,1,0x2000)==1);assert(pic_pending_command(0x4004));
    rsx_picture_release(first);
    u64 second=rsx_picture_create(0x1000,1,0x2000,32,b);assert(second && second!=first);
    /* Suppressed UI retains an old descriptor despite same-address reuse. */
    assert(rsx_picture_capture_command(0x5004,0x3000,1,0x2000)==1);
    assert(rsx_picture_bind_descriptor(0x3000,second)==1);
    assert(rsx_picture_capture_command(0x6004,0x3000,1,0x2000)==1);
    rsx_picture_release(second);u64 token=0;
    assert(rsx_picture_take_command(0x4004,1,0x2000,&token)==1 && token==first);
    assert(!memcmp(rsx_picture_data(token,1,0x2000,32),a,32));rsx_picture_release(token);
    assert(rsx_picture_take_command(0x5004,1,0x2000,&token)==1 && token==first);
    assert(!memcmp(rsx_picture_data(token,1,0x2000,32),a,32));rsx_picture_release(token);
    assert(rsx_picture_take_command(0x6004,1,0x2000,&token)==1 && token==second);
    assert(!memcmp(rsx_picture_data(token,1,0x2000,32),b,32));rsx_picture_release(token);
    assert(rsx_picture_bind_descriptor(0x3000,0)==1);assert(rsx_picture_live_count()==0);
    assert(rsx_picture_take_command(0x7004,1,0x7777,&token)==0 && !token);
    assert(rsx_picture_take_command(0x7004,0,0x2000,&token)==0);
    assert(!pic_requires_identity(0,0x2008,1));assert(pic_requires_identity(1,0x2008,1));
    assert(pic_requires_identity(1,0x1fff,2));assert(!pic_requires_identity(1,0x1fff,1));
    assert(rsx_picture_take_command(0x7004,1,0x2000,&token)<0);assert(!rsx_picture_ready());
    assert(rsx_picture_reset()==1);
    token=rsx_picture_create(0x1000,1,0x2000,32,a);assert(token);
    assert(rsx_picture_bind_descriptor(0x3000,token)==1);
    assert(rsx_picture_capture_command(0x4004,0x3000,1,0x2000)==1);
    assert(rsx_picture_capture_command(0x4004,0x3000,1,0x2000)<0);assert(!rsx_picture_ready());
    rsx_picture_release(token);assert(rsx_picture_bind_descriptor(0x3000,0)==1);assert(rsx_picture_live_count()==1);assert(rsx_picture_reset()==0);
    puts("PASS immutable old queued and future descriptor identities across same-EA reuse; unknown/reused command rejection");
#endif
}
