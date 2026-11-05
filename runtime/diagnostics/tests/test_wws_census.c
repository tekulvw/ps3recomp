/* Synthetic fixture for the opt-in job-manager censuses (wws_census_impl.h):
 * a fabricated workload data block, slot array and targets in a small guest
 * buffer, and a fabricated policy context whose transfers are replayed through
 * the observer. Checks what is reported, that unreadable presumed pointers are
 * survived, that nothing is written to guest memory or the context, and that
 * the observer is detached when the job-interface census is not armed. */
#include "../../spu/spu_context.h"
#include "../wws_census_impl.h"
#include <assert.h>

uint8_t* vm_base;
enum { GUEST = 1u << 20, B = 0x1000, SLOTS = 0x2000, CMD = 0x3000, GATE = 0x3100 };

static void put32(uint32_t ea, uint32_t v) { vm_base[ea] = v >> 24; vm_base[ea + 1] = v >> 16; vm_base[ea + 2] = v >> 8; vm_base[ea + 3] = v; }
static void put16(uint32_t ea, uint16_t v) { vm_base[ea] = v >> 8; vm_base[ea + 1] = (uint8_t)v; }
static char* slurp(FILE* f)
{
    fflush(f); long n = ftell(f); rewind(f);
    char* s = calloc(1, (size_t)n + 1); assert(fread(s, 1, (size_t)n, f) == (size_t)n);
    rewind(f); assert(!ftruncate(fileno(f), 0));
    return s;
}
static void expect(const char* text, const char* needle)
{
    if (!strstr(text, needle)) { fprintf(stderr, "missing: %s\n--- output ---\n%s\n", needle, text); abort(); }
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    vm_base = calloc(1, GUEST);
    FILE* f = tmpfile(); assert(f);
    wws_stream = f;
    /* Armed by hand: the environment is never read. */
    wws_env_read = 1;
    wws_qw.image = 101; wws_qw.from = 0; wws_qw.to = UINT64_MAX;
    wws_aw.image = 101; wws_aw.from = 0; wws_aw.to = UINT64_MAX;
    snprintf(wws_jobio_dir, sizeof wws_jobio_dir, "%s", argv[1]);

    put32(B + 0x20, 0x1F); put32(B + 0x30, SLOTS);
    for (int i = 0; i < 6; i++) put16(B + 0x44 + 2 * i, 0xFFFF);
    static spu_context ctx;
    ctx.ls = ctx.ls_store; ctx.policy_mode = 1; ctx.image_id = 101;
    wws_census_note_run(&ctx, 101, 0x2F00, B, 4, 0x5000);
    assert(ctx.wws_observer == &wws_observer_vtbl);

    /* Queue census. */
    wws_census_poll_once(1000);
    char* s = slurp(f);
    expect(s, "kind=container wid=4 spurs=00005000 image=101 b=00001000 hdr=00000000.");
    expect(s, "kind=array wid=4 q=0 cap=0x1F base=00002000");
    expect(s, "kind=row wid=4 q=0 h=0000.0000.FFFF.FFFF.FFFF.FFFF.FFFF.FFFF");
    free(s);
    put32(SLOTS, 0x00400001); put32(SLOTS + 4, CMD);
    put32(SLOTS + 8, 2); put32(SLOTS + 12, GATE);
    put32(SLOTS + 16, 0x00400001); put32(SLOTS + 20, 0xFFFFFF00u);   /* presumed pointer, unreadable */
    put32(CMD, 0xC0DE0001); put16(GATE + 2, 1);
    put16(B + 0x40, 3);
    static uint8_t before[GUEST]; memcpy(before, vm_base, GUEST);
    wws_census_poll_once(2000);
    assert(!memcmp(before, vm_base, GUEST));
    s = slurp(f);
    expect(s, "kind=row wid=4 q=0 h=0003.0000.");
    expect(s, "kind=post wid=4 q=0 idx=0x0000 cap=0x1F w0=00400001 w1=00003000 tgt=C0DE0001.00000000.00000000.00000000");
    expect(s, "kind=post wid=4 q=0 idx=0x0001 cap=0x1F w0=00000002 w1=00003100 tgt=00000001.");
    expect(s, "kind=post wid=4 q=0 idx=0x0002 cap=0x1F w0=00400001 w1=FFFFFF00 tgt=-");
    expect(s, "kind=target-watch ea=00003100 v=00000001.");
    free(s);
    put16(GATE + 2, 0); put32(SLOTS, 0); put32(SLOTS + 4, 0);   /* gate released, slot 0 consumed */
    put16(B + 0x42, 1); put16(B + 0x44, 0);
    wws_census_poll_once(5000);   /* 3 us after the post */
    s = slurp(f);
    expect(s, "kind=row wid=4 q=0 h=0003.0001.0000.FFFF.");
    expect(s, "kind=target ea=00003100 v=00000000.");
    expect(s, "kind=slot wid=4 q=0 idx=0x0000 after_us=3 w0=00000000 w1=00000000");
    free(s);
    put16(B + 0x40, 0);
    wws_census_poll_once(6000);
    s = slurp(f);
    expect(s, "kind=reset wid=4 q=0 from=0x0003 to=0x0000");
    free(s);

    /* Job-interface census, replaying a job's transfers through the hooks. */
    ctx.resident_ovl = 221;
    ctx.gpr[0]._u32[0] = 0x2000; ctx.pc = 0x1500;               /* policy-internal: ignored */
    ctx.wws_observer->branch(&ctx);
    ctx.pc = 0x4398; ctx.gpr[3]._u32[0] = 0x11223344;          /* policy enters the job */
    ctx.wws_observer->branch(&ctx);
    ctx.pc = 0x4400; ctx.wws_observer->branch(&ctx);           /* job-internal: ignored */
    ctx.pc = 0x1A40; ctx.gpr[0]._u32[0] = 0x4108; ctx.gpr[3]._u32[0] = 7;   /* job calls the policy */
    ctx.wws_observer->branch(&ctx);
    ctx.srr0 = 0x4410; ctx.irq_saved = 1; ctx.wws_observer->irq_taken(&ctx);
    ctx.pc = 0x0; ctx.wws_observer->branch(&ctx);              /* the handler's own transfer */
    ctx.irq_saved = 0;
    ctx.gpr[3]._u32[0] = 0x99; ctx.wws_observer->drain_return(&ctx, 0x4108);
    ctx.wws_observer->drain_return(&ctx, 0x4108);              /* no open call: ignored */
    ctx.pc = 0x2000; ctx.gpr[0]._u32[0] = 0x2000;              /* job returns to the policy */
    ctx.wws_observer->branch(&ctx);
    s = slurp(f);
    expect(s, "kind=entry ctx=");
    expect(s, "img=221 pc=0x04398 link=0x02000");
    expect(s, "r3=11223344.");
    expect(s, "kind=call ctx=");
    expect(s, "img=221 target=0x01A40 link=0x04108");
    expect(s, "kind=irq ctx=");
    expect(s, "srr0=0x04410");
    expect(s, "kind=ret ctx=");
    expect(s, "target=0x01A40 link=0x04108 dt_ns=");
    expect(s, "r3=00000099.");
    expect(s, "kind=leave ctx=");
    expect(s, "entry=0x04398 target=0x02000 link=0x02000");
    assert(!strstr(s, "pc=0x01500") && !strstr(s, "0x04400"));
    free(s);
    wws_api_summary("test");
    s = slurp(f);
    expect(s, "kind=entry img=221 target=0x04398 n=1");
    expect(s, "kind=call img=221 target=0x01A40 n=1");
    expect(s, "kind=ret img=221 target=0x01A40 n=1");
    expect(s, "kind=leave img=221 target=0x02000 n=1");
    expect(s, "kind=irq img=221 target=0x04410 n=1");
    expect(s, "kind=irq img=221 target=0x80000000 n=1");
    free(s);
    /* The first run of job image 221 was recorded. */
    char path[1024]; snprintf(path, sizeof path, "%s/job-0000.jobio", argv[1]);
    FILE* r = fopen(path, "rb"); assert(r); fclose(r);

    /* Another policy image: no observer, no container. */
    static spu_context other;
    other.ls = other.ls_store; other.policy_mode = 1; other.image_id = 7;
    wws_census_note_run(&other, 7, 0x1000, 0x8000, 2, 0x5000);
    assert(other.wws_observer == NULL && wws_nconts == 1);
    /* The job-interface window closes: the context is detached at its next run. */
    wws_adone = 1;
    wws_census_note_run(&ctx, 101, 0x2F00, B, 4, 0x5000);
    assert(ctx.wws_observer == NULL);
    puts("PASS: queue census (posts, targets, slot reuse, reset, unreadable pointer); job interface (entry, call, irq, ret, leave, summary, job I/O); detach");
    return 0;
}
