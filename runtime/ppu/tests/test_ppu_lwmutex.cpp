// Exercise the real context-aware handlers without game code or a GPU.
#include "../ppu_sysprx.cpp"
#include <atomic>
#include <thread>
#include <cassert>
#include <chrono>

extern "C" { uint8_t* vm_base = nullptr; }
// Lock profiling is optional; it does not participate in synchronization.
extern "C" { volatile long long g_gcm_ref_pub_count = 0; }
extern "C" unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
extern "C" unsigned ppu_thread_prof_pc(unsigned) { return 0; }
static std::atomic<uint32_t> words[4096];
extern "C" uint32_t vm_read32(uint64_t a) { return words[a/4].load(); }
extern "C" uint64_t vm_read64(uint64_t a) { return (uint64_t(vm_read32(a))<<32)|vm_read32(a+4); }
extern "C" void vm_write32(uint64_t a,uint32_t v) { words[a/4].store(v); }
extern "C" void vm_write64(uint64_t a,uint64_t v) { vm_write32(a,v>>32); vm_write32(a+4,v); }

static ppu_context context(uint32_t tid, uint32_t mutex=0x100) {
    ppu_context c={}; c.thread_id=tid; c.gpr[3]=mutex; return c;
}
int main() {
    auto c=context(1); sys_lwmutex_create(&c);
    c=context(1); sys_lwmutex_lock(&c); assert(c.gpr[3]==0);
    auto other=context(2); sys_lwmutex_trylock(&other);
    assert(other.gpr[3]!=0); // Must not acquire somebody else's lock.
    assert(vm_read32(0x100)==1);
    other=context(2); other.gpr[4]=10000;
    sys_lwmutex_lock(&other); assert(uint32_t(other.gpr[3])==0x8001000B);
    assert(vm_read32(0x100)==1);
    c=context(1); sys_lwmutex_lock(&c); assert(vm_read32(0x10c)==2);
    c=context(1); sys_lwmutex_unlock(&c); assert(vm_read32(0x10c)==1);
    c=context(1); sys_lwmutex_unlock(&c);

    std::atomic<int> inside{0}, failures{0};
    auto worker=[&](int tid) {
        for(int i=0;i<1000;i++) {
            auto t=context(tid); sys_lwmutex_lock(&t);
            if(inside.fetch_add(1)!=0) failures++;
            std::this_thread::yield();
            inside--;
            t=context(tid); sys_lwmutex_unlock(&t);
        }
    };
    std::thread a(worker,1), b(worker,2); a.join(); b.join();
    assert(failures==0);

    // Existing polling lwcond fallback must release the lock for another thread.
    vm_write32(0x208, 0xA5A5A5A5); // The next guest object begins after 8 bytes.
    c=context(1,0x200); c.gpr[4]=0x100; sys_lwcond_create(&c);
    assert(vm_read32(0x200)==0x100); // PS3 pointers are 32-bit even on the PPU.
    assert(vm_read32(0x204)!=0);
    assert(vm_read32(0x208)==0xA5A5A5A5);
    c=context(1); sys_lwmutex_lock(&c);
    std::atomic<bool> ready{false}, got{false};
    std::thread signaler([&] {
        ready=true;
        auto t=context(2); t.gpr[4]=200000; sys_lwmutex_lock(&t);
        if(t.gpr[3]==0) { got=true; t=context(2); sys_lwmutex_unlock(&t); }
    });
    while(!ready) std::this_thread::yield();
    for(int i=0;i<100 && !got;i++) {
        c=context(1,0x200); sys_lwcond_wait(&c);
    }
    bool released=got.load();
    c=context(1); sys_lwmutex_unlock(&c); signaler.join();
    assert(released);
    puts("PASS: PPU lightweight mutex exclusion, timeout, recursion, contention and polling wait");
}
