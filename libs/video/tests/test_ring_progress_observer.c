/* Actual GCM queue/publication with a synthetic backend receipt. No GPU claim. */
#define main existing_gcm_fixture_main
#include "test_gcm_report_publication.c"
#undef main
static struct { uint32_t kind,ea,value;uint64_t ticket,aux; } observed[64];
static unsigned observed_count;
static void observe(uint32_t kind,uint32_t ea,uint32_t value,uint64_t ticket,uint64_t aux){
    assert(observed_count<64);
    observed[observed_count].kind=kind;observed[observed_count].ea=ea;
    observed[observed_count].value=value;observed[observed_count].ticket=ticket;
    observed[observed_count++].aux=aux;
}
static unsigned count_kind(uint32_t kind){unsigned n=0;for(unsigned i=0;i<observed_count;i++)n+=observed[i].kind==kind;return n;}
int main(void){
    vm_base=mmap(NULL,0x4000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_hle_inject_base=0x3000000;ppu_vm_size=0x4000000;
    s_deferred_reports=1;fixture_live=0;s_reports.generation=1;
    uint32_t ea=VM_HLE_INJECT_BASE+0x4b0;raw32(ea,0x40145624);
    /* Observer disabled: original queue/receipt semantics still apply. */
    assert(gcm_defer_label(ea,0x80142adc,0x1d74,0x30142adc));
    assert(gcm_reports_progress(0,1));uint64_t first=fixture_issued;
    assert(vm_read32(ea)==0x40145624&&!observed_count);
    /* Arming after issue must preserve the left-censored pending ticket. */
    rsx_ring_progress_observer_set(ea,observe);
    assert(gcm_reports_progress(0,0));
    assert(count_kind(RSX_RING_PENDING_SNAPSHOT)==1);
    assert(observed[0].ticket==first&&observed[0].value==0x80142adc);
    assert(!count_kind(RSX_RING_VM_PUBLISH)&&vm_read32(ea)==0x40145624);
    fixture_completed=first;assert(gcm_reports_progress(0,0));
    assert(count_kind(RSX_RING_METAL_COMPLETE)==1&&count_kind(RSX_RING_VM_PUBLISH)==1);
    assert(vm_read32(ea)==0x80142adc);
    assert(observed[1].kind==RSX_RING_METAL_COMPLETE&&observed[2].kind==RSX_RING_VM_PUBLISH);
    assert(observed[2].ticket==first);
    /* Filter unrelated labels while retaining their real publications. */
    unsigned before=observed_count;
    assert(gcm_defer_label(ea+16,0x12345678,0x1d74,0x30143000));
    assert(gcm_reports_progress(0,1));fixture_completed=fixture_issued;
    assert(gcm_reports_progress(0,0)&&vm_read32(ea+16)==0x12345678);
    assert(observed_count==before);
    /* New target label gets an actual issue ticket before publication. */
    assert(gcm_defer_label(ea,0xc0146000,0x1d74,0x30145ffc));
    assert(count_kind(RSX_RING_DECODE)==1&&count_kind(RSX_RING_ENQUEUE)==1);
    assert(gcm_reports_progress(0,1));
    assert(count_kind(RSX_RING_TICKET_BIND)==1&&vm_read32(ea)==0x80142adc);
    fixture_completed=fixture_issued;assert(gcm_reports_progress(0,0));
    assert(vm_read32(ea)==0xc0146000&&count_kind(RSX_RING_VM_PUBLISH)==2);
    /* A consumer-observed backend completion is reported once per ticket. */
    unsigned complete=count_kind(RSX_RING_METAL_COMPLETE);
    rsx_ring_progress_metal_completed(fixture_issued,4);
    assert(count_kind(RSX_RING_METAL_COMPLETE)==complete);
    rsx_ring_progress_observer_set(0,NULL);before=observed_count;
    assert(gcm_defer_label(ea,0x00147000,0x1d74,0x30146ffc));
    assert(gcm_reports_progress(0,1));fixture_completed=fixture_issued;
    assert(gcm_reports_progress(0,0)&&vm_read32(ea)==0x00147000);
    assert(observed_count==before);
    munmap(vm_base,0x4000000);puts("ring observer actual GCM ordering/filter checks passed");return 0;
}
