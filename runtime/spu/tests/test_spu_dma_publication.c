/* Manufactured outgoing data; real direct/list DMA path and unchanged bytes. */
#define main source_fixture_main
#include "test_spu_dma_source_f32be.c"
#undef main
static uint8_t pub_ls[SPU_LS_SIZE];
static spu_context pub_ctx;
static void* publish_thread(void* unused) {
    (void)unused;
    for (int i=0;i<20;i++) spu_dma_publication_observe(&pub_ctx,0x100,0x12000,128,0x20);
    return NULL;
}
int main(int argc, char** argv) {
    memset(g_vm_page_bitmap,255,sizeof g_vm_page_bitmap);
    pub_ctx.ls=pub_ls; pub_ctx.image_id=17; pub_ctx.pc=0x1234; pub_ctx.spu_id=2;
    for(unsigned i=0;i<32;i++) spu_ls_write32(&pub_ctx,0x100+4*i,0xAA000000+i);
    spu_ls_write32(&pub_ctx,0x120,0x12345678);
    spu_ls_write32(&pub_ctx,0x130,0x12345678); /* One report per matching transfer. */
    if(argc>1 && !strcmp(argv[1],"incoming-disabled")) {
        spu_dma_publication_observe((const spu_context*)(uintptr_t)1,0,0x12000,128,0x40);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"incoming-end")) {
        memcpy(memory+0x12000,pub_ls+0x100,16);
        assert(mfc_do_transfer(&pub_ctx,SPU_LS_SIZE-16,0x12000,16,0x40)==0);
        assert(!memcmp(pub_ls+SPU_LS_SIZE-16,memory+0x12000,16)); return 0;
    }
    if(argc>1 && !strcmp(argv[1],"incoming-small")) {
        spu_ls_write32(&pub_ctx,0x100,0x12345678); memcpy(memory+0x12000,pub_ls+0x100,4);
        memset(pub_ls+0x100,0xA5,4);
        assert(mfc_do_transfer(&pub_ctx,0x100,0x12000,4,0x40)==0);
        assert(!memcmp(pub_ls+0x100,memory+0x12000,4)); return 0;
    }
    if(argc>1 && !strcmp(argv[1],"incoming")) {
        uint8_t expected[128]; memcpy(expected,pub_ls+0x100,128);
        memcpy(memory+0x12000,expected,128); memset(pub_ls+0x100,0xA5,128);
        assert(mfc_do_transfer(&pub_ctx,0x100,0x12000,128,0x40)==0);
        assert(!memcmp(pub_ls+0x100,expected,128)); assert(!memcmp(memory+0x12000,expected,128));
        memset(pub_ls+0x100,0x5A,128);
        pub_ctx.mfc_lsa=0x100; pub_ctx.mfc_tag=1;
        spu_ls_write32(&pub_ctx,0x300,128); spu_ls_write32(&pub_ctx,0x304,0x12000);
        assert(mfc_do_list_transfer(&pub_ctx,0x300,0,8,0x44)==0);
        assert(!memcmp(pub_ls+0x100,expected,128)); assert(!memcmp(memory+0x12000,expected,128));
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"full")) {
        uint32_t at=SPU_LS_SIZE-16384;
        spu_ls_write32(&pub_ctx,SPU_LS_SIZE-4,0x12345678);
        assert(mfc_do_transfer(&pub_ctx,at,0x12000,16384,0x20)==0);
        assert(!memcmp(memory+0x12000,pub_ls+at,16384)); return 0;
    }
    if(argc>1 && !strcmp(argv[1],"disabled")) {
        spu_dma_publication_observe((const spu_context*)(uintptr_t)1,0,0x12000,16384,0x20);
        pub_ctx.ls=(uint8_t*)1;
        spu_dma_publication_observe(&pub_ctx,0,0x12000,16384,0x20);
        return 0; /* Disabled observer must not access an unreadable LS. */
    }
    uint8_t before[128]; memcpy(before,pub_ls+0x100,128);
    if(argc>1 && !strcmp(argv[1],"threads")) {
        pthread_t t[8]; for(int i=0;i<8;i++) assert(!pthread_create(&t[i],0,publish_thread,0));
        for(int i=0;i<8;i++) assert(!pthread_join(t[i],0));
    } else {
        assert(mfc_do_transfer(&pub_ctx,0x100,0x12000,128,0x20)==0);
        assert(!memcmp(memory+0x12000,before,128));
        pub_ctx.mfc_lsa=0x100; pub_ctx.mfc_tag=1;
        spu_ls_write32(&pub_ctx,0x300,128); spu_ls_write32(&pub_ctx,0x304,0x12000);
        assert(mfc_do_list_transfer(&pub_ctx,0x300,0,8,0x24)==0);
        assert(mfc_do_transfer(&pub_ctx,0x100,0x13000,128,0x20)==0);
        assert(mfc_do_transfer(&pub_ctx,SPU_LS_SIZE-16,0x12000,32,0x20)==-1);
        assert(mfc_do_transfer(&pub_ctx,0x100,0xFFFFFFF0,32,0x20)==-1);
        assert(mfc_do_transfer(&pub_ctx,0x100,0x12000,128,0x40)==0);
    }
    assert(!memcmp(pub_ls+0x100,before,128)); return 0;
}
