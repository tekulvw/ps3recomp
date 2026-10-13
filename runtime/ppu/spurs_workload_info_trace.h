/* Optional, game-independent observation of cellSpursGetWorkloadInfo calls.
 * No scheduler admission changes. The API's actual returned bytes are distinct
 * from request bytes sampled before/after dispatch; those may change concurrently.
 */
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_vm_size;
namespace {
struct SpursWorkloadInfoTrace {
    struct Requests {
        bool valid=false;
        unsigned ready=0,contention=0,idle=0,signal=0,flag_receiver=0;
        uint32_t flag_value=0;
    };
    ppu_context* ctx=nullptr;
    bool active=false;
    unsigned id=0;
    uint64_t spurs=0,info=0,lr=0,tid=0,wid=0;
    Requests before;
    static bool range(uint64_t ea,uint64_t size) {
        const uint64_t limit=ppu_vm_size?uint64_t(ppu_vm_size):(uint64_t(1)<<32);
        return vm_base&&ea&&ea<=UINT32_MAX&&ea<limit&&size<=limit-ea;
    }
    static uint64_t option(const char* name,uint64_t fallback,int radix=16) {
        const char* e=std::getenv(name);
        if(!e||!*e||*e=='-'||*e=='+')return fallback;
        char* end=nullptr;errno=0;const auto n=std::strtoull(e,&end,radix);
        return errno||!end||*end?fallback:n;
    }
    static Requests requests(uint64_t ea,uint64_t w) {
        Requests r;
        // This implementation's live first-bank workload counters cover WID 0..15.
        if(w>=16||!range(ea,0x78))return r;
        r.valid=true;r.ready=vm_base[ea+w];r.idle=vm_base[ea+0x10+w];r.contention=vm_base[ea+0x20+w];
        const unsigned signal=unsigned(vm_base[ea+0x70])<<8|vm_base[ea+0x71];
        r.signal=(signal&(0x8000u>>w))?1:0;
        r.flag_receiver=vm_base[ea+0x77];
        r.flag_value=uint32_t(vm_base[ea+0x6c])<<24|uint32_t(vm_base[ea+0x6d])<<16|uint32_t(vm_base[ea+0x6e])<<8|vm_base[ea+0x6f];
        return r;
    }
    explicit SpursWorkloadInfoTrace(uint32_t nid,ppu_context* c) {
        const int saved_errno=errno;
        static const bool enabled=[] {const char* e=std::getenv("PS3_SPURS_INFO_TRACE");return e&&std::strcmp(e,"1")==0;}();
        // Disabled / unrelated imports do not dereference ctx or touch VM/I/O.
        if(!enabled||nid!=0x4E153E3Eu||!c){errno=saved_errno;return;}
        static const char* arm=std::getenv("PS3_SPURS_INFO_TRACE_ARM_FILE");
        if(arm&&*arm) {
            FILE* f=std::fopen(arm,"rb");
            if(!f){errno=saved_errno;return;}
            std::fclose(f);
        }
        static const uint64_t filter_spurs=option("PS3_SPURS_INFO_TRACE_SPURS",UINT64_MAX);
        static const uint64_t filter_wid=option("PS3_SPURS_INFO_TRACE_WID",UINT64_MAX);
        static const uint64_t lr_min=option("PS3_SPURS_INFO_TRACE_LR_MIN",0);
        static const uint64_t lr_max=option("PS3_SPURS_INFO_TRACE_LR_MAX",UINT64_MAX);
        if((filter_spurs!=UINT64_MAX&&c->gpr[3]!=filter_spurs)||
           (filter_wid!=UINT64_MAX&&c->gpr[4]!=filter_wid)||c->lr<lr_min||c->lr>lr_max) {
            errno=saved_errno;return;
        }
        static const unsigned limit=[] {auto n=option("PS3_SPURS_INFO_TRACE_LIMIT",65536,10);return unsigned(n==0?65536:n>1048576?1048576:n);}();
        static std::atomic<unsigned> admitted{0};unsigned count=admitted.load(std::memory_order_relaxed);
        while(count<limit&&!admitted.compare_exchange_weak(count,count+1,std::memory_order_relaxed)){}
        if(count>=limit){errno=saved_errno;return;}
        ctx=c;id=count;spurs=c->gpr[3];wid=c->gpr[4];info=c->gpr[5];lr=c->lr;tid=c->thread_id;
        before=requests(spurs,wid);active=true;errno=saved_errno;
    }
    ~SpursWorkloadInfoTrace() {
        if(!active)return;
        const int saved_errno=errno;
        const uint32_t result=uint32_t(ctx->gpr[3]);
        const bool valid=result==0&&range(info,0x30);
        unsigned contention=0,min=0,max=0,ready=0,idle=0,signal=0;
        if(valid){contention=vm_base[info+0x20];min=vm_base[info+0x21];max=vm_base[info+0x22];ready=vm_base[info+0x23];idle=vm_base[info+0x24];signal=vm_base[info+0x25];}
        const Requests after=requests(spurs,wid);
        // One bounded physical record: no fragmented fprintf events under contention.
        char record[1024];
        const int n=std::snprintf(record,sizeof record,
          "[spurs-info-trace] id=%u tid=%llu entry_lr=%08llX spurs=%08llX wid=%llu info=%08llX result=%08X info_valid=%u contention=%u min=%u max=%u ready=%u idle=%u signal=%u request_valid=%u before_ready=%u before_contention=%u before_idle=%u before_signal=%u before_flag_receiver=%u before_flag_value=%08X after_valid=%u after_ready=%u after_contention=%u after_idle=%u after_signal=%u after_flag_receiver=%u after_flag_value=%08X native_active=unavailable cold_admission_count=unavailable\n",
          id,(unsigned long long)tid,(unsigned long long)lr,(unsigned long long)spurs,(unsigned long long)wid,(unsigned long long)info,result,unsigned(valid),contention,min,max,ready,idle,signal,unsigned(before.valid),before.ready,before.contention,before.idle,before.signal,before.flag_receiver,before.flag_value,unsigned(after.valid),after.ready,after.contention,after.idle,after.signal,after.flag_receiver,after.flag_value);
        if(n>0&&size_t(n)<sizeof record)std::fprintf(stderr,"%s",record);
        errno=saved_errno;
    }
};
}
