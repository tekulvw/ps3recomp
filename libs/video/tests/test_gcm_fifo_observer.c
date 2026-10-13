/* Exercise the actual FIFO drain and its observation without a GPU. */
#define main checked_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"classification")) {
        gcm_observer_packet(0x18,0xFEAD0002,0);
        return 0;
    }
    setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));s_ea_address_table[0]=1;
    s_gcm_syscall_mode=1;
    /* Header precedes the observed interval; its payload consumes it. */
    raw32(0x100000,0x40240300); /* NI, nine words. */
    for(unsigned i=0;i<9;i++)raw32(0x100004+i*4,0xAA000000+i);
    raw32(0x100028,0x20000028); /* Park. */
    cellGcm_syscall_set_fifo(0x30,0);
    for(unsigned i=0;i<8;i++)cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff==0x28);assert(vm_read32(0x100018)==0xAA000005);
    raw32(0x100028,0x20000038); /* Patched branch. */
    raw32(0x100038,0x00041800);raw32(0x10003C,0x01000430);
    s_report_dma=GCM_REPORT_DMA_MAIN;s_ea_address_table[0xE0]=0x20;
    raw32(watched+12,0x00010000);cellGcm_syscall_set_fifo(0x40,0x28);
    cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff==0x40);assert(s_report_data[67].timestamp!=0);
    assert(vm_read32(watched+12)==0);return 0;
}
