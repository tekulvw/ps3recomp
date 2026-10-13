#!/usr/bin/env python3
"""Run the real setter NID and attachment creation path against a synthetic VM."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires POSIX C toolchain')
    raise SystemExit(0)
FIXTURE = r'''
#include "libs/spurs/cellSpurs.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
static unsigned entered, left;
static int reject_admission;
int spu_taskset_lifecycle_create(uint32_t ea,int taskset2) {(void)ea;assert(!taskset2);return 1;}
int spu_taskset_lifecycle_enter(uint32_t ea) {(void)ea;++entered;return reject_admission ? -1 : 1;}
void spu_taskset_lifecycle_leave(uint32_t ea) {(void)ea;++left;}
static uint32_t expected_task;
static uint32_t expected_flags=3, expected_result=0;
static int inject_terminal;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {return ea==0x3020;}
void spu_lockline_lock(void) {
    /* Model an authentic guest publisher winning between reserve and CAS.
       This only fires after the new task has been made enabled. */
    if (inject_terminal && vm_read32(0x3020)==1 &&
        spurs_bitset_test(0x4000+CSTS_ENABLED,expected_task)) {
        uint32_t value=ps3_bswap32(13), result=ps3_bswap32(37);
        uint32_t semaphore=ps3_bswap32(0x44445555);
        memcpy(vm_base+0x3020,&value,4);
        memcpy(vm_base+0x3024,&semaphore,4);
        memcpy(vm_base+0x3028,&result,4);
        inject_terminal=0;
    }
}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}
static _Alignas(128) uint8_t memory[0x10000];
#define PTR(type, ea) ((type*)(uintptr_t)(ea))
static void expect(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); abort(); }
}
static void* setter;
void ps3_hle_register(unsigned nid,const char* name,void* fn) {
    (void)name; expect(nid==0xA121A224u,"setter NID"); setter=fn;
}
void register_container_fixture(void);
static uint32_t call(uint64_t attr,uint64_t container) {
    typedef int32_t (*Entry)(CellSpursTaskAttribute*,CellSpursTaskExitCode*);
    return (uint32_t)((Entry)setter)(PTR(CellSpursTaskAttribute,attr),PTR(CellSpursTaskExitCode,container));
}
static unsigned dispatched;
static uint64_t expected_helper;
size_t spu_elf_image_size(const uint8_t* image,size_t max) {(void)image;(void)max;return 16;}
int spu_workload_dispatch_async(const uint8_t* image,uint32_t size,uint32_t context) {
    (void)image;(void)size;(void)context; ++dispatched;
    uint32_t ti=spurs_taskset_taskinfo_ea(0x4000,expected_task);
    uint32_t record=0x4000+0x1980+expected_task*16;
    expect(vm_read64(record)==expected_helper,"authentic descriptor retained before dispatch");
    expect(vm_read64(record+8)==0x3000,"container retained before dispatch");
    expect(vm_read64(ti+TI_ELF)==0x9001,"task ELF exit metadata flag");
    expect(vm_read32(0x3020)==expected_flags,"container is submitted and pending completion");
    expect(vm_read64(0x3050)==0x4000 && vm_read8(0x3039)==expected_task,"container owner binding");
    expect(vm_read32(0x3028)==expected_result,"creation does not manufacture an exit result");
    return 0; /* Adapter return success is deliberately NOT a completion oracle. */
}
int main(void) {
    register_container_fixture(); expect(setter!=NULL,"generated setter NID registered");
    vm_base=memory; ppu_vm_size=sizeof(memory); s_inst[0].ea=0x1000;
    CellSpursTaskAttribute* attr=PTR(CellSpursTaskAttribute,0x2000);
    CellSpursTasksetAttribute* tsa=PTR(CellSpursTasksetAttribute,0x2100);
    CellSpursTaskset* ts=PTR(CellSpursTaskset,0x4000);
    CellSpurs* spurs=PTR(CellSpurs,0x1000);
    memset(memory,0xA5,sizeof(memory));
    expect(call(0,0x3001)==0x80410911u,"null attr precedes container alignment");
    expect(call(0x2001,0)==0x80410911u,"null container precedes attr alignment");
    expect(call(0x2001,0x3000)==0x80410910u,"attribute alignment");
    expect(call(0x2000,0x3040)==0x80410910u,"container requires 128 alignment");
    for(unsigned i=0;i<sizeof(memory);i++) expect(memory[i]==0xA5,"invalid pointers have no writes");
    expect(cellSpursTaskAttributeInitialize(attr)==0,"public task attr initializer");
    expect(call(0x100002000ull,0x100003000ull)==0,"guest pointers use low32 EA");
    expect(vm_read64(0x2040)==0x3000,"setter stores zero-extended container as BE64 at +40");
    expect(memory[0x203F]==0 && memory[0x2048]==0,"setter field canaries");
    ((CellSpursTaskAttribute*)(memory+0x2000))->revision=2;
    expect(call(0x2000,0x3080)==0x80410902u,"host-layout invalid revision");
    expect(vm_read64(0x2040)==0x3000,"invalid revision preserves attachment");
    expect(call(0xFFF8,0x3000)==0x8041090Du,"partial attr backing rejected before read");
    ((CellSpursTaskAttribute*)(memory+0x2000))->revision=1;
    expect(call(0x2000,0x10000)==0x8041090Du,"container backing rejected before access");
    expect(_cellSpursTaskAttributeInitialize(attr,1,0,0x9000,0,0,NULL,NULL)==0,"real versioned init");
    expect(vm_read64(0x2040)==0,"reinit clears binding");
    expect(cellSpursTaskExitCodeInitialize(PTR(CellSpursTaskExitCode,0x3000))==0,"container init");
    expect(call(0x2000,0x3000)==0,"versioned init and setter share host revision");
    expect(_cellSpursTasksetAttributeInitialize(tsa,1,0,0,0,0)==0,"taskset attr init");
    expect(cellSpursTasksetAttributeSetTasksetSize(tsa,0x2900)==0,"extended size");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,tsa)==0,"extended creation");
    reject_admission=1;
    expect((uint32_t)cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0x8041090Fu,"attribute creation retains retired-generation admission");
    expect((uint32_t)cellSpursCreateTask(ts,PTR(CellSpursTaskId,0x2200),NULL,NULL,0,0,0)==0x8041090Fu,"plain creation shares retired-generation admission");
    expect(vm_read32(0x3020)==0 && vm_read64(0x4000+CSTS_ENABLED)==0 && dispatched==0,"admission rejection precedes binding and task mutation");
    expect(entered==2 && left==0,"rejected admissions do not release unowned references");
    reject_admission=0;
    expected_helper=0x1234000Full; vm_write64(0x5900,expected_helper);
    expected_task=0;
    expect(cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0,"attached task creation");
    expect(dispatched==1 && vm_read32(0x2200)==0,"task0 created and dispatched");
    expect(vm_read32(0x3020)==3 && vm_read32(0x3028)==0,"dispatch return does not publish completion");
    expect(memory[0x2FFF]==0xA5 && memory[0x3080]==0xA5,"container canaries");
    /* No authentic helper is installed in a fresh HLE taskset. Its zero
       descriptor must remain visible, not be replaced with invented code. */
    expected_task=1; expected_helper=0; vm_write64(0x5900,0);
    expect(cellSpursTaskExitCodeInitialize(PTR(CellSpursTaskExitCode,0x3000))==0,"fresh pending container");
    expect(cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0,"second attached task");
    expect(dispatched==2 && vm_read32(0x2200)==1,"second slot id retained");
    expected_task=2; expected_flags=15; expected_result=37;
    expect(cellSpursTaskExitCodeInitialize(PTR(CellSpursTaskExitCode,0x3000))==0,"reset before concurrent publisher");
    inject_terminal=1;
    expect(cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0,"submission CAS survives concurrent publisher");
    expect(!inject_terminal && vm_read32(0x3020)==15 && vm_read32(0x3028)==37 && vm_read32(0x3024)==0x44445555,"publication preserves terminal flags/result/semaphore");
    expect(dispatched==3,"third task dispatched after intact publication");
    vm_write64(0x2040,0x3001);
    ((CellSpursTaskAttribute*)(memory+0x2000))->eaElf=0;
    expect((uint32_t)cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0x80410910u,"consumer rechecks mutated container alignment");
    vm_write64(0x2040,0x3000);
    ((CellSpursTaskAttribute*)(memory+0x2000))->eaElf=0x9000;
    /* Base storage cannot hold exit records; reject before task/container writes. */
    expect(cellSpursCreateTaskset(spurs,ts,0,NULL,0)==0,"base taskset");
    uint8_t before_ts[0x1900], before_container[128];
    memcpy(before_ts,memory+0x4000,sizeof(before_ts));
    memcpy(before_container,memory+0x3000,sizeof(before_container));
    expect((uint32_t)cellSpursCreateTaskWithAttribute(ts,PTR(CellSpursTaskId,0x2200),attr)==0x80410909u,"base taskset cannot attach container");
    expect(!memcmp(before_ts,memory+0x4000,sizeof(before_ts)) && !memcmp(before_container,memory+0x3000,sizeof(before_container)),"size rejection has no task or container mutation");
    expect(dispatched==3,"rejected task is not dispatched");
    expect(entered==left+2,"all accepted creation paths release admission references");
    puts("PASS: real initializer/setter/creation attachment, pending-state and native admission checks");
}
'''
with tempfile.TemporaryDirectory(prefix='spurs-exit-container-') as name:
    tmp=Path(name); fixture=tmp/'fixture.c'; fixture.write_text(FIXTURE)
    generated=tmp/'nids.cpp'
    subprocess.run([os.sys.executable,str(ROOT/'tools/gen_hle_nids.py'),'cellSpurs','--out',str(generated)],check=True)
    entries=[line for line in generated.read_text().splitlines() if 'ps3_hle_register(0xA121A224u,' in line]
    registration=tmp/'registration.c'
    registration.write_text('void ps3_hle_register(unsigned,const char*,void*);\n'
        'void cellSpursTaskAttributeSetExitCodeContainer(void);\n'
        'void register_container_fixture(void) {\n'+ '\n'.join(entries)+'\n}\n')
    binary=tmp/'fixture'
    linker=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections','-I'+str(ROOT),'-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'runtime/spu'),str(fixture),str(registration),str(ROOT/'libs/spurs/spurs_taskset.c'),
        *linker,'-pthread','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,cwd=tmp)
