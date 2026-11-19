/* The FIFO walker's effect seam and the decode | record split.
 *
 * Included by cellGcmSys.c just before gcm_rsx_process_fifo_pass. Two things
 * live here:
 *
 * 1. The walker's EFFECTS, lifted out of the pass into static functions: the
 *    method-packet loop (gcm_fx_methods), the NV406E semaphore packet
 *    (gcm_fx_sema), the flip word (gcm_fx_flip), the NV0039 copy, the flips a
 *    resync writes off, the fences a resync rescues, and the `get`
 *    publication. The pass calls them through gcm_fx_* wrappers: with the
 *    split off the wrapper is the call itself, in the same order the pass
 *    made it before; with it on, the wrapper copies the words into a ring and
 *    the record thread makes the same call, in the same order, later.
 *
 * 2. PS3RECOMP_RSX_SPLIT_WALKER=1 (opt-in; design in mag-recomp
 *    local/notes/d2-split-walker-design.md): the frame-clock thread keeps the
 *    fetch (word loads, control flow, checked-REF packet validation, parks,
 *    resyncs, NV0039 recognition); a record thread owns the register-file
 *    draw engine, the backend, the deferred-report queue, the REF queue, label
 *    writes, NV0039 and 2D guest writes, flips-reached, the flip receipt and
 *    the present. Every release the guest may act on (labels, REF, reports,
 *    flip completion) is therefore made after the record thread has copied
 *    the draws before it out of guest memory, as the single thread did.
 *
 *    `get` is the one release the decoder would make early. A title that
 *    treats `get` progress as "executed" (polls get == put before rewriting
 *    vertex data) would then overwrite data a queued draw still reads, so a
 *    published `get` is held back: the decoder queues its publications with
 *    the ring position they belong to, and the record thread publishes each
 *    one once it has consumed to within PS3RECOMP_RSX_SPLIT_WALKER_GET_LEAD
 *    words of that position (default 256 words = 1 KiB, the order of the
 *    hardware's prefetch). The command words themselves are copied, so the
 *    guest reusing ring bytes behind `get` cannot change what is recorded.
 *
 *    The decoder waits only when the ring is full (the pass ends, the frame
 *    clock's idle wait runs); the record thread waits only when it is empty.
 *    There is no call from the decoder into the engine or the backend.
 *
 *    The record thread holds the consumer lease (s_gcm_fifo_lock) while it
 *    applies records and gives it up after any record when another thread
 *    is waiting for it (guest report waits, mapping transitions, the host's
 *    present/pump lease). Mapping transitions and terminate drain the ring
 *    first (gcm_split_drain). */

/* The ring's wake-ups: a native SRWLOCK + CONDITION_VARIABLE on Windows, a
 * pthread mutex + condition variable elsewhere, behind four wrappers of this
 * file's own (no pthread-name macros, so the GCM fixtures' own shims and the
 * POSIX win32_compat layer are not involved). The thread is a Win32 thread
 * on Windows and a pthread elsewhere. */
#ifdef _WIN32
typedef struct { SRWLOCK lock; CONDITION_VARIABLE cv; } gcm_split_wait;
static void gcm_split_wait_init(gcm_split_wait* w) { InitializeSRWLock(&w->lock); InitializeConditionVariable(&w->cv); }
static void gcm_split_wait_lock(gcm_split_wait* w) { AcquireSRWLockExclusive(&w->lock); }
static void gcm_split_wait_unlock(gcm_split_wait* w) { ReleaseSRWLockExclusive(&w->lock); }
static void gcm_split_wait_sleep(gcm_split_wait* w) { SleepConditionVariableSRW(&w->cv, &w->lock, INFINITE, 0); }
static void gcm_split_wait_wake(gcm_split_wait* w) { AcquireSRWLockExclusive(&w->lock); WakeAllConditionVariable(&w->cv); ReleaseSRWLockExclusive(&w->lock); }
#else
#include <pthread.h>
#include <sched.h>
typedef struct { pthread_mutex_t lock; pthread_cond_t cv; } gcm_split_wait;
static void gcm_split_wait_init(gcm_split_wait* w) { pthread_mutex_init(&w->lock, NULL); pthread_cond_init(&w->cv, NULL); }
static void gcm_split_wait_lock(gcm_split_wait* w) { pthread_mutex_lock(&w->lock); }
static void gcm_split_wait_unlock(gcm_split_wait* w) { pthread_mutex_unlock(&w->lock); }
static void gcm_split_wait_sleep(gcm_split_wait* w) { pthread_cond_wait(&w->cv, &w->lock); }
static void gcm_split_wait_wake(gcm_split_wait* w) { pthread_mutex_lock(&w->lock); pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->lock); }
#endif

/* ---- the effects --------------------------------------------------------- */

extern s32 cellGcmSetFlipCommand(u32 bufferId);
static rsx_state s_legacy_state;
static int       s_legacy_state_inited;
/* The position an effect belongs to in the command stream: the io offset of
 * the packet header the decoder was at. Direct path: s_fifo_getoff itself. */

/* Applies one method packet. `words` is the packet's data as copied by the
 * decoder (split), or NULL (direct: each word is read from guest memory at
 * its mapped address, as before). Returns 0 when the packet is done, 1 when
 * the pass must stop at this packet and come back to it (a report dependency
 * or inline-image barrier failed): the direct path returns from the pass, the
 * record thread retries the record. */
#if defined(__GNUC__) || defined(__clang__)
#define GCM_FX_INLINE static inline __attribute__((always_inline))
#else
#define GCM_FX_INLINE static inline
#endif
/* Inlined at both call sites: the pass's direct call is then the per-word
 * loop it had before the seam, with no call per packet. */
GCM_FX_INLINE int gcm_fx_methods(u32 type, u32 subch, u32 method, u32 count, u32 io,
                                 const u32* words, int plain_walk, int legacy_state_live,
                                 int checked, int diag)
{
    if(count && (subch==4u||subch==5u) && method<=0x7fcu &&
       (method>=0x400u || (type==0u && (u64)method+(u64)(count-1u)*4u>=0x400u)) &&
       !gcm_inline_image_barrier())return 1;
    u32 dea = 0;
    for (u32 i = 0; i < count; i++) {
        /* Plain walk: within one 1 MB IO page the next word's address
         * is the previous one's plus 4 (no method on that path can
         * remap IO), so the table is consulted once per page. */
        const u32 dio = io + 4 + i * 4;
        dea = (plain_walk && dea && (dio & 0xFFFFFu)) ? dea + 4u : gcm_io2ea(dio);
        if (!dea) break;
        u32 m = (type == 0) ? method + i * 4 : method;
        const u32 v = words ? words[i] : vm_read32(dea);
        /* Fast path: forwards the word and nothing else. Any per-method
         * effect the general path below gains must be matched by adding
         * that method to gcm_walk_special, or this path skips it. */
        if (plain_walk && (subch == 0 || (subch == 1 && !s_walk_s1_2d)) && !gcm_walk_special(m)) {
            if (diag && s_gph_on > 0) {
                const uint64_t e0 = gph_now();
                rsx_draw_engine_method_at((subch << 13) | m, v, dea);
                s_gph.eng_ns += gph_now() - e0; s_gph.eng_n++;
            } else
                rsx_draw_engine_method_at((subch << 13) | m, v, dea);
            s_walk_plain_words++;
            continue;
        }
        if(s_query_observe && subch==0 && (m==0x17c8u||m==0x17ccu||m==0x1800u||m==0x1814u||m==0x1824u))
            gcm_query_observe(m,v,dio);
        /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
         * splits into subchannel 7 / method 0x0B00 -- so it was landing
         * in the 2D engine path and being dropped as unrecognised. It
         * is not a 2D method at all: it tells the RSX to raise a
         * USER_CMD interrupt, which is what ps1_netemu's flip path
         * blocks on. */
        { static int sd = -1; static unsigned long s7 = 0;
          if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
          if (sd && subch == 7 && (PS3_LOG_FIRST(s7, 16)))
              fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n", m, v); }
        if (subch == 7 && m == 0x0B00u) {
            if(gcm_reports_live()&&!gcm_reports_dependency())return 1;
            extern void rsx_raise_user_cmd(u32 arg);
            rsx_raise_user_cmd(v);
            /* A title that registered its handler with the HLE
             * cellGcmSetUserHandler has no lv2 interrupt thread
             * reading that event; its handler runs from the pump.
             * The `continue` skips the 0xEB00 case below, so queue
             * it for the pump here. */
            if (s_user_handler_opd) cellGcmQueueUserCommand(v);
            continue;
        }
        /* GCM_SUBCH1_3D=1: treat subchannel 1 as the 3D object too.
         * The title issues NV4097 methods on it -- 0x1A80..0x1AC0 is
         * SET_TEXTURE_OFFSET for units 4-6 -- and the 2D path silently
         * discards whatever it does not recognise, so that state never
         * reaches the renderer. SET_OBJECT binds are not tracked, which
         * is what makes the subchannel-to-engine mapping a guess. */
        /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
         * title binds to a subchannel is what says which engine that
         * subchannel drives; comparing subchannel 1 against subchannel 0
         * turns the routing below from a guess into a reading. */
        { static int od = -1;
          if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
          if (od && m == 0) {
              static unsigned seen[8]; static int have[8];
              const unsigned h = v;
              if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                  have[subch] = 1; seen[subch] = h;
                  printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                         subch, h, 10); fflush(stdout);
              } } }
        /* The subchannel is a binding slot, not an engine selector: a
         * title may bind NV4097 to something other than subchannel 0.
         * Twisted Metal uses subchannel 1, and caner (canersaka) hit
         * the same thing in Yakuza Dead Souls, where SPU-built command
         * lists bind NV4097 elsewhere -- his rsx_live_draw.c masks the
         * subchannel out of the method for exactly this reason.
         *
         * gcm_2d_method only ever handles subchannels 2..7 (NV3062,
         * NV308A/NV309E, NV3089), so anything arriving on 1 was being
         * dropped on the floor -- for this title that included
         * SET_SHADER_PROGRAM, leaving one stale fragment program bound
         * for every draw in the game. Treat the subchannels the 2D path
         * does not claim as 3D. GCM_SUBCH1_2D=1 restores the old split.
         */
        gcm_walk_settings();
        const int s1_2d = s_walk_s1_2d;
        /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
         * non-zero subchannel -- what each binding slot actually carries. */
        { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
          if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
          const u32 cm3 = m & 0x1FFCu;
          /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
          { static u32 vw = 1;
            if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
            if (vw && v == vw) {
                static int vn; if (PS3_LOG_FIRST(vn, 64))
                    printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, io, 10);
            } }
          /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
          if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                               cm3 == 0x1A20)) {
              if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
              if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, v, 10);
          }
          if (sg && subch) {
              if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
              if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, v, 10);
          } }
        /* Mirror the whole method stream into the live NV4097->D3D12
         * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
         * It wants the raw method with its subchannel bits -- it
         * canonicalises with & 0x1FFC itself. */
        { const int live = s_walk_live;
          /* Close report work before raw dispatch can expose an
           * effect; the legacy handler below runs too late. */
          if(gcm_reports_live()&&(((subch<<13)|m)==0xE920u||
             ((subch<<13)|m)==0xE924u||((subch<<13)|m)==0xEB00u||
             ((subch<<13)|m)==0xEB04u||((subch==0u||subch==1u)&&(m==0x50u||m==0x104u)))&&
             !gcm_reports_dependency())return 1;
          if (live) rsx_live_draw_method_at((subch << 13) | m, v,dea); }
        /* GCM_METHOD_HIST=<secs>: every <secs>, the most frequent
         * (subchannel, method) pairs dispatched since the last report --
         * what a frame is actually made of when nothing draws. */
        { static int hs = -1; static u32 cnt[8][0x800]; static unsigned long long t0;
          if (hs < 0) { const char* e = getenv("GCM_METHOD_HIST"); hs = e ? atoi(e) : 0; }
          if (hs > 0) {
              cnt[subch & 7][(m & 0x1FFCu) >> 2]++;
              extern unsigned long long ps3_ms_now(void);
              const unsigned long long now = ps3_ms_now();
              if (!t0) t0 = now;
              if (now - t0 >= (unsigned long long)hs * 1000u) {
                  t0 = now;
                  fprintf(stderr, "[mhist]");
                  for (int top = 0; top < 16; top++) {
                      u32 bs = 0, bm = 0, bc = 0;
                      for (u32 sc = 0; sc < 8; sc++) for (u32 mi = 0; mi < 0x800; mi++)
                          if (cnt[sc][mi] > bc) { bc = cnt[sc][mi]; bs = sc; bm = mi; }
                      if (!bc) break;
                      fprintf(stderr, " %u:%04X=%u", bs, bm << 2, bc);
                      cnt[bs][bm] = 0;
                  }
                  fprintf(stderr, "\n");
                  memset(cnt, 0, sizeof cnt);
              } } }

        /* And into the platform-neutral register-file draw engine,
         * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
         * It wants the raw method with its subchannel bits for the
         * same reason the live engine does, and masks them itself. */
        { const int eng = s_walk_eng;
          if (eng) {
              const uint64_t e0=s_gph_on>0?gph_now():0;
              rsx_draw_engine_method_at((subch << 13) | m, v,dea);
              if(e0){s_gph.eng_ns+=gph_now()-e0;s_gph.eng_n++;}
          } }

        /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
         * for any run, live engine or not, so the method stream and the
         * backend's draw callbacks can be compared in one measurement. */
        { static int fdbg = -1;
          if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
          if (fdbg) {
              static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
              const u32 cm = m & 0x1FFCu;
              if      (cm == 0x1808u) n_be++;
              else if (cm == 0x1814u) n_va++;
              else if (cm == 0x1824u) n_ia++;
              if (((++tick) % 200000) == 0)
                  printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                         n_be, n_va, n_ia), fflush(stdout);
          } }

        /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
         * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
         * truncates them.  Reconstruct the full address the same way
         * the draw-engine call above does. */
        { const u32 mfull = (subch << 13) | m;
        if (mfull == 0xEB00u || mfull == 0xEB04u) {
            cellGcmQueueUserCommand(v);
        } else if (mfull == 0xE920u || mfull == 0xE924u) {
            cellGcmSetFlipCommand(v & 7u);
        } else if (subch == 0 || (subch == 1 && !s1_2d)) {
            /* A method this branch acts on must be in gcm_walk_special
             * too, or PS3RECOMP_GCM_FAST_WALK=1 never brings it here. */
            if(m==0x50u && !((s_early_fifo_sync||checked)?gcm_reports_dependency():gcm_reports_sync_barrier()))return 1;
            if(m==0x1d6cu)s_backend_label_offset=v;
            if(m==0x1a4u)s_backend_label_dma=v;
            if((m==0x1d70u||m==0x1d74u)&&gcm_reports_can_defer()){
                u32 value=v;
                if(m==0x1d70u)value=(value&0xff00ff00u)|((value>>16)&0xffu)|((value&0xffu)<<16);
                u32 label_ea=VM_HLE_INJECT_BASE+(s_backend_label_offset&0xffffffu);
                if(gcm_reports_live()&&s_backend_label_dma==0xFEED0001u){
                    label_ea=gcm_io2ea(s_backend_label_offset);
                    /* Legacy HOST_BUFFER skips an unmapped write. */
                    if(!label_ea){if(!gcm_reports_sync_barrier())return 1;continue;}
                }
                if(!gcm_defer_label(label_ea,value,m,dea))return 1;
                continue;
            }
            if((m==0x1d70u||m==0x1d74u)&&!gcm_reports_sync_barrier())return 1;
            if (legacy_state_live || gcm_legacy_method_has_effect(m))
            { const uint64_t l0=s_gph_on>0?gph_now():0;
              rsx_process_method(&s_legacy_state, m, v);
              if(l0){s_gph.legacy_ns+=gph_now()-l0;s_gph.legacy_n++;} }
            /* NV406E_SET_REFERENCE: queue the fence value for PACED
             * publication (gcm_ref_publish below) instead of letting a
             * later fence in the same batch overwrite it. */
            if (m == 0x50) {
                if(checked && (count!=1 || method!=0x50)){gcm_checked_ref_fail("malformed-reference",io);continue;}
                if (checked &&
                    !(s_early_fifo_sync ? gcm_early_reference_receipt() : gcm_checked_backend_flush()))
                    gcm_checked_ref_fail(s_early_fifo_sync ? "early-reference-receipt" : "backend-completion", io);
                else gcm_ref_push_at(g_rsx_last_reference, io);
            }
            if (m == 0x1A8u) {
                u32 dma = v;
                s_report_dma = dma == 0xFEED0000u ? GCM_REPORT_DMA_LOCAL : dma;
            }
            if (m == 0x1800u) {
                u32 argument=v;
                gcm_observer_event(io,"report",argument);
                gcm_get_report(argument, io);
            }
        } else
            gcm_2d_method(subch, m, v);
        }
    }
    return 0;
}

/* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
 *
 * These were falling through to rsx_process_method as "unknown method" --
 * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
 * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
 * RSX is supposed to write and never sees it move, so it never appends its
 * next FIFO segment and the GPU sits on a park forever.
 *
 * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
 * the method, so the next pass re-reads it -- which is what the hardware does
 * while it waits. Semaphore offsets index the same label window
 * cellGcmGetLabelAddress hands out.
 *
 * Returns 1 when the pass stops here (sem_blocked), 0 when the packet is
 * consumed. `get_io`: the stream position to publish as `get` before a
 * RELEASE's label is applied (0: do not); the record thread passes 0 when a
 * later publication is already out (gcm_split_get_may_publish). */
static int gcm_fx_sema(u32 type, u32 method, u32 count, u32 io, const u32* words, u32 get_io)
{
    int sem_blocked = 0;
    for (u32 i = 0; i < count; i++) {
        u32 dea = gcm_io2ea(io + 4 + i * 4);
        if (!dea) break;
        u32 m = (type == 0) ? method + i * 4 : method;
        u32 v = words ? words[i] : vm_read32(dea);
        u32 la = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
        { static int sn = 0, recdbg = -1; if (gcm_env_flag(&recdbg, "GCM_RECDBG") && PS3_LOG_FIRST(sn, 12))
            fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X\n", m, v, s_sema_offset); }
        /* Early FIFO sync: every release an ACQUIRE can depend on
         * has already been applied, so no GPU wait is needed. */
        if(m==0x68u&&!(s_early_fifo_sync?gcm_reports_dependency():gcm_reports_sync_barrier())){sem_blocked=1;break;}
        /* PS3_FTRACE: an acquire this walker sat on is satisfied now. */
        if (m == 0x68u && s_ft_sema_t0 && vm_read32(la) == v) {
            ps3_ft_span(PS3_FT_SEMA, 0, s_sema_offset, v, s_ft_sema_t0, ps3_ft_now());
            s_ft_sema_t0 = 0;
        }
        if (m == 0x64u)      s_sema_offset = v;
        else if(m==0x6cu && gcm_reports_can_defer()){
            if(!gcm_defer_label_get(la,v,m,dea,get_io)){sem_blocked=1;break;}
        }
        else if (m == 0x6Cu) {
            if(!gcm_reports_sync_barrier()){sem_blocked=1;break;}
            /* The RSX has fetched past a RELEASE before its
             * semaphore write lands, so a guest that sees the
             * label must also see `get` past the command. `get`
             * was published only at a JUMP and at the end of the
             * pass: Ratchet & Clank: Tools of Destruction waits
             * for a label, then reads `get` once and spins until
             * its own `put` equals it, which a snapshot from
             * before the RELEASE never does. GCM_GET_EQ_PUT keeps
             * publishing put at the end of the pass instead. */
            if (get_io)
                vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get_io);
            atomic_thread_fence(memory_order_release);
            vm_write32(la, v);
            { static int rn = 0; if (s_sema_offset != 0x400u && PS3_LOG_FIRST(rn, 40))   /* TEMP */
                fprintf(stderr, "[SEMA-REL] get=0x%08X off=0x%X val=0x%08X\n", io, s_sema_offset, v); }
        }
        else if (m == 0x68u && vm_read32(la) != v) {
            /* GCM_SEMA_ACQUIRE=1 makes ACQUIRE actually block, which is what
             * the hardware does. Off by default: a title whose label nothing
             * ever writes would wedge the drain permanently, and RELEASE on
             * its own is the half that unblocks a waiting guest. */
            static int blk = -1; static unsigned blk_ms;
            if (blk < 0) { const char* e = getenv("GCM_SEMA_ACQUIRE"); blk = e ? atoi(e) : 0;
                           const char* t = getenv("GCM_SEMA_ACQUIRE_MS"); blk_ms = t ? (unsigned)atoi(t) : 0; }
            if (blk) {
                /* GCM_SEMA_ACQUIRE_MS=<ms>: a bounded wait. The hardware waits
                 * for ever; an emulated producer that never comes (a boot-time
                 * label nobody services yet) would wedge the drain, so after
                 * <ms> on the same acquire it is consumed and said so. inFAMOUS 2
                 * acquires a label its SPU lighting writes each frame: waiting
                 * here is what orders the RSX's sampling after that write. */
                static u32 b_off, b_val; static unsigned long long b_t0;
                extern unsigned long long ps3_ms_now(void);
                const unsigned long long now = ps3_ms_now();
                if (!b_t0 || b_off != s_sema_offset || b_val != v) { b_off = s_sema_offset; b_val = v; b_t0 = now; }
                if (!blk_ms || now - b_t0 < blk_ms) {
                    if (PS3_FT_ON() && !s_ft_sema_t0) s_ft_sema_t0 = ps3_ft_now();
                    sem_blocked = 1; break;
                }
                { static int tn; if (PS3_LOG_FIRST(tn, 12))
                    fprintf(stderr, "[SEMA] acquire off=0x%X == 0x%08X gave up after %u ms (label 0x%08X)\n",
                            s_sema_offset, v, blk_ms, vm_read32(la)); }
                b_t0 = 0;
            }
        }
    }
    return sem_blocked;
}

/* A flip word the decoder consumed (s_fifo_getoff already past it). */
static void gcm_fx_flip(u32 w)
{
    /* Count the flip WORDS the title actually writes into the FIFO,
     * separately from the presents that result. When a title stops
     * appearing on screen the first question is which side stopped:
     * this counter still climbing while presents do not is a runtime
     * problem, both stopping together is the guest's. */
    { static unsigned long long flips = 0; static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      ++flips;
      if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
          fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                  flips, 10); }
    if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                       ps3_ft_span(PS3_FT_FLIP_FIFO, 0, w & 7u, 0, t, t); }
    rsx_fifo_busy_flip();
    if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
        /* Queued by _cellGcmSetFlipCommand, which already did the
         * guest-visible half of the request. */
        gcm_set_current_display_buffer(w & 7u);
        gcm_flip_receipt_publish(&s_flip_receipts,w & 7u,1);
        /* Not past `queued`: a resync may already have written off
         * this flip. */
        if (s_fifo_flips_reached != s_fifo_flips_queued) s_fifo_flips_reached++;
    } else {
        cellGcmSetFlipCommand(w & 0xFFu);
    }
    gcm_frame_bound();
}

/* A resync wrote a queued flip off (it will never be reached): one more
 * (set_all = 0), or every flip queued so far (set_all = 1, with the count
 * the decoder saw). */
static void gcm_fx_flips_written_off(int set_all, u32 queued)
{
    if (set_all) s_fifo_flips_reached = queued;
    else if (s_fifo_flips_reached != s_fifo_flips_queued) s_fifo_flips_reached++;
}

/* ---- the split ------------------------------------------------------------ */

enum {
    GCM_RQ_PAD = 0,      /* [hdr] filler to the ring's end */
    GCM_RQ_METHODS,      /* [hdr][raw word][io][ea0][count data words] */
    GCM_RQ_SEMA,         /* [hdr][raw word][io][count data words] */
    GCM_RQ_FLIP,         /* [hdr][word] */
    GCM_RQ_FLIPS_OFF,    /* [hdr][set_all][queued] */
    GCM_RQ_REF_RESCUE,   /* [hdr][value][io] */
    GCM_RQ_NV0039,       /* [hdr][io][in_dma][out_dma][v0..v7] */
    GCM_RQ_PASS_END,     /* [hdr][issue] */
    GCM_RQ_STOP,         /* [hdr] */
};
/* hdr: kind (8 bits) | checked (bit 8) | plain_walk (bit 9) | legacy (bit 10)
 *      | diag (bit 11) | payload words (bits 12..31) */
#define GCM_RQ_HDR(kind, flags, n) ((u32)(kind) | ((u32)(flags) << 8) | ((u32)(n) << 12))
#define GCM_RQ_KIND(h)  ((h) & 0xFFu)
#define GCM_RQ_FLAGS(h) (((h) >> 8) & 0xFu)
#define GCM_RQ_LEN(h)   ((h) >> 12)
#define GCM_RQ_F_CHECKED 1u
#define GCM_RQ_F_PLAIN   2u
#define GCM_RQ_F_LEGACY  4u
#define GCM_RQ_F_DIAG    8u
#define GCM_RQ_MAX_PAYLOAD 0xFFFFFu

static struct {
    u32* buf;
    u64  cap;                          /* words, power of two */
    u32  get_lead;                     /* PS3RECOMP_RSX_SPLIT_WALKER_GET_LEAD, words */
    int  get_decode;                   /* PS3RECOMP_RSX_SPLIT_WALKER_GET=decode: the decoder publishes `get` once the words are copied */
    _Atomic u64 head, tail;            /* consumed / published word indices (monotone) */
    _Atomic int rec_sleeping, dec_sleeping, stop, started, idle;
    gcm_split_wait rec_wait, dec_wait;   /* the record thread's / the decoder's sleep */
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    /* Pending `get` publications: the ring position the decoder was at and
     * the value; the record thread publishes one once head >= pos - lead. */
    struct { u64 pos, draw_fence; u32 value; } getq[64];
    u64 draw_fence;                    /* ring position after the newest record that reads guest memory */
    /* Test seam: called by the record thread before each record (NULL in production). */
    void (*test_hook)(const u32* record, u64 pos);
    _Atomic u32 getq_head, getq_tail;
    u64 get_pub_pos;                   /* ring position of the newest published entry */
    /* Diagnostics (PS3RECOMP_RSX_SPLIT_WALKER_STATS=1): every 2 s. */
    int stats_on;
    unsigned long long records, words, empty_sleeps, retries, lease_yields, get_pubs, get_holds, get_hold_words_max;
    unsigned long long full_stops;     /* decoder-side: passes ended or waits taken on a full ring */
    uint64_t busy_ns, yield_ns, stats_at;
} s_rq;

static int gcm_split_on(void)
{
    if (s_split < 0) {
        const char* e = getenv("PS3RECOMP_RSX_SPLIT_WALKER");
        int want = e && !strcmp(e, "1");
        if (want) {
            const char* why = NULL;
            if (!s_deferred_reports) why = "requires PS3RECOMP_RSX_DEFERRED_REPORTS=1";
            else if (s_live_report_batch) why = "not with PS3RECOMP_GCM_LIVE_REPORT_BATCH";
            else if (rsx_live_draw_enabled()) why = "not with RSX_LIVE_DRAW";
            else if (!rsx_draw_engine_enabled()) why = "requires the register-file draw engine";
            else if (rsx_get_backend() != NULL) why = "not with a vtable backend";
            if (why) { fprintf(stderr, "[rsx split walker] refused: %s; walking on one thread\n", why); want = 0; }
        }
        if (want) {
            const char* mb = getenv("PS3RECOMP_RSX_SPLIT_WALKER_RING_MB");
            long n = mb ? strtol(mb, NULL, 0) : 8; if (n < 1) n = 1; if (n > 256) n = 256;
            u64 cap = 1; while (cap < (u64)n * 262144ull) cap <<= 1;
            const char* ld = getenv("PS3RECOMP_RSX_SPLIT_WALKER_GET_LEAD");
            long lead = ld ? strtol(ld, NULL, 0) : 256; if (lead < 0) lead = 0;
            const char* st = getenv("PS3RECOMP_RSX_SPLIT_WALKER_STATS");
            /* PS3RECOMP_RSX_SPLIT_WALKER_GET=decode: `get` is the fetch pointer and
             * the decoder publishes it as soon as it has copied the words behind
             * it (ring reuse is then safe; a title that treats `get` as "executed"
             * before reusing data is not). `record` (default): the record thread
             * publishes it, bounded by the lead and never past an unconsumed
             * record that reads guest memory. */
            const char* gm = getenv("PS3RECOMP_RSX_SPLIT_WALKER_GET");
            const int get_decode = gm && !strcmp(gm, "decode");
            s_rq.buf = (u32*)malloc(cap * sizeof(u32));
            if (!s_rq.buf) { fprintf(stderr, "[rsx split walker] refused: no memory for a %llu-word ring\n", (unsigned long long)cap); want = 0; }
            else {
                s_rq.cap = cap; s_rq.get_lead = (u32)lead; s_rq.get_decode = get_decode; s_rq.stats_on = st && !strcmp(st, "1");
                gcm_split_wait_init(&s_rq.rec_wait); gcm_split_wait_init(&s_rq.dec_wait);
                fprintf(stderr, "[rsx split walker] on: decode on the frame clock, record on its own thread; ring %llu MiB, get %s\n",
                        (unsigned long long)(cap * 4 >> 20), get_decode ? "published by the decoder" : "published by the record thread, bounded");
                if (!get_decode) fprintf(stderr, "[rsx split walker] get lead %ld words past the record thread, never past an unconsumed draw\n", lead);
            }
        }
        s_split = want;
    }
    return s_split;
}

static uint64_t gcm_split_now_ns(void) { return gcm_bound_now(); }   /* QPC / CLOCK_MONOTONIC, defined above */
static void gcm_split_sleep_us(long us)
{
#ifdef _WIN32
    Sleep(us >= 1000 ? (DWORD)(us / 1000) : 1u);
#else
    const struct timespec ts = { 0, us * 1000L }; nanosleep(&ts, NULL);
#endif
}
static inline void gcm_split_pause(void)
{
#if defined(__GNUC__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
    __builtin_ia32_pause();
#endif
}

/* ---- decoder side ---- */

/* Room for n words at the tail, contiguous; NULL when the ring is full. */
static u32* gcm_rq_reserve(u32 n)
{
    for (;;) {
        const u64 tail = atomic_load_explicit(&s_rq.tail, memory_order_relaxed);
        const u64 head = atomic_load_explicit(&s_rq.head, memory_order_acquire);
        const u64 used = tail - head;
        const u64 off  = tail & (s_rq.cap - 1);
        const u64 to_end = s_rq.cap - off;
        if (to_end >= n) {
            if (s_rq.cap - used < n) return NULL;
            return s_rq.buf + off;
        }
        /* Pad to the end, then start over. */
        if (s_rq.cap - used < to_end + n) return NULL;
        s_rq.buf[off] = GCM_RQ_HDR(GCM_RQ_PAD, 0, (u32)(to_end - 1));
        atomic_store_explicit(&s_rq.tail, tail + to_end, memory_order_release);
    }
}
static void gcm_rq_commit(u32 n)
{
    atomic_store_explicit(&s_rq.tail, atomic_load_explicit(&s_rq.tail, memory_order_relaxed) + n, memory_order_release);
    if (atomic_load_explicit(&s_rq.rec_sleeping, memory_order_acquire)) {
        gcm_split_wait_wake(&s_rq.rec_wait);
    }
}
/* The decoder's `get` publication: queued with the ring position it belongs
 * to. 0 = queued; -1 = the side queue is full (stop the pass as for a full
 * ring: the record thread drains it as it goes). */
static int gcm_split_publish_get(u32 value)
{
    const u32 t = atomic_load_explicit(&s_rq.getq_tail, memory_order_relaxed);
    const u32 h = atomic_load_explicit(&s_rq.getq_head, memory_order_acquire);
    if (t - h >= 64u) return -1;
    s_rq.getq[t % 64u].pos = atomic_load_explicit(&s_rq.tail, memory_order_relaxed);
    s_rq.getq[t % 64u].draw_fence = s_rq.draw_fence;
    s_rq.getq[t % 64u].value = value;
    atomic_store_explicit(&s_rq.getq_tail, t + 1u, memory_order_release);
    if (atomic_load_explicit(&s_rq.rec_sleeping, memory_order_acquire)) {
        gcm_split_wait_wake(&s_rq.rec_wait);
    }
    return 0;
}

/* ---- record side ---- */

/* Publish every queued `get` the record thread has consumed to within the
 * lead of. Called between records and before sleeping. */
static void gcm_split_get_flush(u64 head)
{
    for (;;) {
        const u32 h = atomic_load_explicit(&s_rq.getq_head, memory_order_relaxed);
        const u32 t = atomic_load_explicit(&s_rq.getq_tail, memory_order_acquire);
        if (h == t) return;
        const u64 pos = s_rq.getq[h % 64u].pos;
        /* Never past a record that reads guest memory (a draw, an NV0039 or
         * 2D transfer) the record thread has not consumed; past the rest by
         * at most the lead. */
        if (head < s_rq.getq[h % 64u].draw_fence || pos > head + s_rq.get_lead) {
            if (s_rq.stats_on) { s_rq.get_holds++; if (pos - head > s_rq.get_hold_words_max) s_rq.get_hold_words_max = pos - head; }
            return;
        }
        vm_write32(GCM_CONTROL_GUEST_ADDR + 4, s_rq.getq[h % 64u].value);
        s_rq.get_pub_pos = pos; s_rq.get_pubs++;
        atomic_store_explicit(&s_rq.getq_head, h + 1u, memory_order_release);
    }
}
/* May a RELEASE at ring position `pos` write `get` (= the position after it)?
 * Not when a later publication is already out: `get` never moves backwards
 * in stream order. */
static int gcm_split_get_may_publish(u64 pos) { return pos >= s_rq.get_pub_pos; }

/* Apply one record. Returns 0 done, 1 retry later (the pass would have
 * returned at this point and re-read the packet on its next tick). */
static int gcm_split_apply(const u32* r, u64 pos)
{
    const u32 h = r[0], kind = GCM_RQ_KIND(h), flags = GCM_RQ_FLAGS(h);
    s_checked_ref_pass = (flags & GCM_RQ_F_CHECKED) ? 1 : 0;
    switch (kind) {
    case GCM_RQ_PAD: return 0;
    case GCM_RQ_METHODS: {
        const u32 w = r[1], io = r[2];
        const u32 count = (w >> 18) & 0x7FF, method = w & 0x1FFCu, subch = (w >> 13) & 7, type = w >> 29;
        return gcm_fx_methods(type, subch, method, count, io, r + 4,
                              (flags & GCM_RQ_F_PLAIN) != 0, (flags & GCM_RQ_F_LEGACY) != 0,
                              (flags & GCM_RQ_F_CHECKED) != 0, (flags & GCM_RQ_F_DIAG) != 0);
    }
    case GCM_RQ_SEMA: {
        const u32 w = r[1], io = r[2];
        const u32 count = (w >> 18) & 0x7FF, method = w & 0x1FFCu, type = w >> 29;
        const u32 get_io = (!gcm_get_eq_put() && !s_rq.get_decode && gcm_split_get_may_publish(pos)) ? io + 4 + count * 4 : 0;
        return gcm_fx_sema(type, method, count, io, r + 3, get_io);
    }
    case GCM_RQ_FLIP: {
        /* Present at the flip, on this thread: the engine's flip-time
         * present, after which the host's present of the same flip is
         * skipped (rsx_draw_engine_present_at_flip). Then the guest half. */
        const u32 w = r[1];
        if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER || (w & 0xFFFF0000u) == 0xFEAD0000u)
            rsx_draw_engine_present_at_flip(w & 7u);
        gcm_fx_flip(w);
        return 0;
    }
    case GCM_RQ_FLIPS_OFF: gcm_fx_flips_written_off((int)r[1], r[2]); return 0;
    case GCM_RQ_REF_RESCUE: gcm_ref_push_at(r[1], r[2]); return 0;
    case GCM_RQ_NV0039: nv0039_copy(r[2], r[3], r + 4, r[1]); return 0;
    case GCM_RQ_PASS_END:
        AcquireSRWLockExclusive(&s_ref_pub_lock);
        gcm_ref_publish_one();
        ReleaseSRWLockExclusive(&s_ref_pub_lock);
        gcm_reports_progress(0, (int)r[1]);
        return 0;
    case GCM_RQ_STOP: return 0;
    }
    return 0;
}

static void gcm_split_stats(uint64_t now)
{
    if (!s_rq.stats_on || now - s_rq.stats_at < 2000000000ull) return;
    const double dt = s_rq.stats_at ? (now - s_rq.stats_at) / 1e9 : 0.0;
    s_rq.stats_at = now;
    const u64 head = atomic_load(&s_rq.head), tail = atomic_load(&s_rq.tail);
    fprintf(stderr, "[rsx split walker] records=%llu words=%llu busy=%.1f%% yield=%.1f%% ring=%llu/%llu words full_stops=%llu empty_sleeps=%llu retries=%llu lease_yields=%llu get_pubs=%llu get_holds=%llu get_hold_max=%llu lease_waiters=%d\n",
            s_rq.records, s_rq.words, dt > 0 ? 100.0 * s_rq.busy_ns / 1e9 / dt : 0.0, dt > 0 ? 100.0 * s_rq.yield_ns / 1e9 / dt : 0.0,
            (unsigned long long)(tail - head), (unsigned long long)s_rq.cap,
            s_rq.full_stops, s_rq.empty_sleeps, s_rq.retries, s_rq.lease_yields, s_rq.get_pubs, s_rq.get_holds, s_rq.get_hold_words_max,
            atomic_load(&s_lease_waiters));
    s_rq.busy_ns = 0; s_rq.yield_ns = 0; s_rq.get_hold_words_max = 0;
}

/* The lease, held while records are applied; given up when another thread
 * wants it. The waiter count is raised by cellGcm_rsx_consumer_begin on
 * other threads while the split is on. */
static void gcm_split_lease_yield(void)
{
    const uint64_t t0 = s_rq.stats_on ? gcm_split_now_ns() : 0;
    cellGcm_rsx_consumer_end();
    s_rq.lease_yields++;
    /* Let the waiter take it: one yield of this thread's slice, then back.
     * A waiter that did not get in by then takes it at the next record. */
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
    cellGcm_rsx_consumer_begin();
    if (t0) s_rq.yield_ns += gcm_split_now_ns() - t0;
}

#ifdef _WIN32
static DWORD WINAPI gcm_split_record_main(LPVOID unused)
#else
static void* gcm_split_record_main(void* unused)
#endif
{
    (void)unused;
    s_on_record_thread = 1;
    for (;;) {
        /* Wait for records: a short spin, then sleep without the lease. */
        u64 head = atomic_load_explicit(&s_rq.head, memory_order_relaxed);
        u64 tail = atomic_load_explicit(&s_rq.tail, memory_order_acquire);
        if (head == tail) {
            int spins = 0;
            while (head == tail && spins++ < 2000) {
                gcm_split_pause();
                tail = atomic_load_explicit(&s_rq.tail, memory_order_acquire);
            }
            if (head == tail) {
                gcm_split_get_flush(head);
                gcm_split_wait_lock(&s_rq.rec_wait);
                atomic_store_explicit(&s_rq.rec_sleeping, 1, memory_order_release);
                atomic_store_explicit(&s_rq.idle, 1, memory_order_release);
                while (atomic_load_explicit(&s_rq.tail, memory_order_acquire) == head &&
                       atomic_load_explicit(&s_rq.getq_tail, memory_order_acquire) == atomic_load_explicit(&s_rq.getq_head, memory_order_relaxed) &&
                       !atomic_load_explicit(&s_rq.stop, memory_order_acquire)) {
                    s_rq.empty_sleeps++;
                    gcm_split_wait_sleep(&s_rq.rec_wait);
                }
                atomic_store_explicit(&s_rq.rec_sleeping, 0, memory_order_release);
                atomic_store_explicit(&s_rq.idle, 0, memory_order_release);
                gcm_split_wait_unlock(&s_rq.rec_wait);
                if (atomic_load_explicit(&s_rq.stop, memory_order_acquire) &&
                    atomic_load_explicit(&s_rq.tail, memory_order_acquire) == head) break;
                continue;
            }
        }
        const uint64_t t0 = gcm_split_now_ns();
        cellGcm_rsx_consumer_begin();
        int stop = 0;
        while (head != tail) {
            gcm_split_get_flush(head);
            const u32* r = s_rq.buf + (head & (s_rq.cap - 1));
            const u32 len = GCM_RQ_LEN(r[0]) + 1u;
            if (s_rq.test_hook) s_rq.test_hook(r, head);
            if (GCM_RQ_KIND(r[0]) == GCM_RQ_STOP) { stop = 1; }
            else if (gcm_split_apply(r, head)) {
                /* The pass would have returned here and come back on its
                 * next tick: give the lease up and retry in a moment. */
                s_rq.retries++;
                cellGcm_rsx_consumer_end();
                gcm_split_sleep_us(1000);
                cellGcm_rsx_consumer_begin();
                /* The pass's tick retried recovery before walking: same here. */
                if (s_reports.failed || s_reports_recover_pending) (void)gcm_reports_progress(0, 0);
                tail = atomic_load_explicit(&s_rq.tail, memory_order_acquire);
                continue;
            }
            head += len;
            atomic_store_explicit(&s_rq.head, head, memory_order_release);
            s_rq.records++; s_rq.words += len;
            if (atomic_load_explicit(&s_rq.dec_sleeping, memory_order_acquire)) {
                gcm_split_wait_wake(&s_rq.dec_wait);
            }
            if (atomic_load_explicit(&s_lease_waiters, memory_order_acquire) > 0) gcm_split_lease_yield();
            if (stop) break;
            tail = atomic_load_explicit(&s_rq.tail, memory_order_acquire);
        }
        gcm_split_get_flush(head);
        cellGcm_rsx_consumer_end();
        const uint64_t t1 = gcm_split_now_ns();
        s_rq.busy_ns += t1 - t0;
        if (PS3_FT_ON() && t1 - t0 >= ps3_ft_min_ns()) ps3_ft_span(PS3_FT_RECORD, 0, 0, 0, t0, t1);
        gcm_split_stats(t1);
        if (stop) break;
    }
    atomic_store_explicit(&s_rq.idle, 1, memory_order_release);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int gcm_split_start(void)
{
    if (atomic_load_explicit(&s_rq.started, memory_order_acquire)) return 1;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, gcm_split_record_main, NULL, 0, NULL);
    if (!t) { fprintf(stderr, "[rsx split walker] record thread not started; walking on one thread\n"); s_split = 0; return 0; }
    s_rq.thread = t;
#else
    if (pthread_create(&s_rq.thread, NULL, gcm_split_record_main, NULL)) {
        fprintf(stderr, "[rsx split walker] record thread not started; walking on one thread\n"); s_split = 0; return 0;
    }
#endif
    atomic_store_explicit(&s_rq.started, 1, memory_order_release);
    return 1;
}

/* Wait until the record thread has applied everything queued and is between
 * records. For the callers that change what the decoder reads (IO mappings,
 * the FIFO position) or end the renderer. No-op with the split off. */
void gcm_split_drain(void)
{
    if (s_split != 1 || !atomic_load_explicit(&s_rq.started, memory_order_acquire) || s_on_record_thread) return;
    /* A caller already holding the lease (a consumer body calling a public
     * entry, e.g. the flagged map calling the plain one) gives it up for the
     * wait: the record thread needs it to apply what is queued. */
    const unsigned held = s_consumer_depth;
    for (unsigned d = held; d; d--) cellGcm_rsx_consumer_end();
    for (;;) {
        const u64 head = atomic_load_explicit(&s_rq.head, memory_order_acquire);
        const u64 tail = atomic_load_explicit(&s_rq.tail, memory_order_acquire);
        if (head == tail && atomic_load_explicit(&s_rq.idle, memory_order_acquire)) break;
        if (atomic_load_explicit(&s_rq.rec_sleeping, memory_order_acquire)) {
            gcm_split_wait_wake(&s_rq.rec_wait);
        }
        gcm_split_sleep_us(100);
    }
    for (unsigned d = held; d; d--) cellGcm_rsx_consumer_begin();
}

/* Stop the record thread: drain, queue STOP, join. */
void gcm_split_stop(void)
{
    if (s_split != 1 || !atomic_load_explicit(&s_rq.started, memory_order_acquire)) return;
    gcm_split_drain();
    u32* r;
    while (!(r = gcm_rq_reserve(1))) gcm_split_sleep_us(100);
    r[0] = GCM_RQ_HDR(GCM_RQ_STOP, 0, 0);
    atomic_store_explicit(&s_rq.stop, 1, memory_order_release);
    gcm_rq_commit(1);
    gcm_split_wait_wake(&s_rq.rec_wait);
#ifdef _WIN32
    WaitForSingleObject(s_rq.thread, INFINITE); CloseHandle(s_rq.thread);
#else
    pthread_join(s_rq.thread, NULL);
#endif
    atomic_store_explicit(&s_rq.started, 0, memory_order_release);
    atomic_store_explicit(&s_rq.stop, 0, memory_order_release);
}

/* ---- the seam the pass calls ---------------------------------------------- */

/* Each returns 0 when the pass may go on, 1 when it must stop at this packet
 * (direct: the effect asked for a retry; split: the ring is full). */
GCM_FX_INLINE int gcm_seam_methods(u32 w, u32 io, u32 count, int plain_walk,
                                   int legacy_state_live, int checked, int diag)
{
    const u32 type = w >> 29, method = w & 0x1FFCu, subch = (w >> 13) & 7;
    if (s_split != 1)
        return gcm_fx_methods(type, subch, method, count, io, NULL, plain_walk, legacy_state_live, checked, diag);
    const u32 ea0 = count ? gcm_io2ea(io + 4) : 0;   /* the first data word's address, for the record */
    u32* r = gcm_rq_reserve(4 + count);
    if (!r) return 1;
    r[0] = GCM_RQ_HDR(GCM_RQ_METHODS, (checked ? GCM_RQ_F_CHECKED : 0) | (plain_walk ? GCM_RQ_F_PLAIN : 0) |
                      (legacy_state_live ? GCM_RQ_F_LEGACY : 0) | (diag ? GCM_RQ_F_DIAG : 0), 3 + count);
    r[1] = w; r[2] = io; r[3] = ea0;
    /* A packet whose effect reads guest memory on the record thread -- a
     * draw (BEGIN_END, DRAW_ARRAYS, DRAW_INDEX_ARRAY), or any 2D-engine
     * packet (a blit reads its source) -- fences the published `get`. */
    { int reads = subch >= 2u;
      if (!reads && type == 0u) reads = method <= 0x1824u && method + count * 4u > 0x1808u;
      else if (!reads) reads = method == 0x1808u || method == 0x1814u || method == 0x1824u;
      if (reads) s_rq.draw_fence = atomic_load_explicit(&s_rq.tail, memory_order_relaxed) + 4 + count; }
    /* The words, read as the direct path would: through the IO table at
     * each page (a packet may cross one), contiguous within it. */
    u32 dea = 0;
    for (u32 i = 0; i < count; i++) {
        const u32 dio = io + 4 + i * 4;
        dea = (dea && (dio & 0xFFFFFu)) ? dea + 4u : gcm_io2ea(dio);
        if (!dea) { /* unmapped mid-packet: the direct loop stops there too */
            r[0] = GCM_RQ_HDR(GCM_RQ_METHODS, GCM_RQ_FLAGS(r[0]), 3 + i);
            /* Shrink: the words after i are not part of the record. */
            const u32 shrunk = 4 + i;
            gcm_rq_commit(shrunk);
            return 0;
        }
        r[4 + i] = vm_read32(dea);
    }
    gcm_rq_commit(4 + count);
    return 0;
}
static int gcm_seam_sema(u32 w, u32 io, u32 count, int checked, int* blocked)
{
    const u32 type = w >> 29, method = w & 0x1FFCu;
    if (s_split != 1) {
        *blocked = gcm_fx_sema(type, method, count, io, NULL, gcm_get_eq_put() ? 0 : io + 4 + count * 4);
        return 0;
    }
    *blocked = 0;
    u32* r = gcm_rq_reserve(3 + count);
    if (!r) return 1;
    r[0] = GCM_RQ_HDR(GCM_RQ_SEMA, checked ? GCM_RQ_F_CHECKED : 0, 2 + count);
    r[1] = w; r[2] = io;
    u32 n = 0;
    for (; n < count; n++) {
        const u32 dea = gcm_io2ea(io + 4 + n * 4);
        if (!dea) break;
        r[3 + n] = vm_read32(dea);
    }
    r[0] = GCM_RQ_HDR(GCM_RQ_SEMA, checked ? GCM_RQ_F_CHECKED : 0, 2 + n);
    gcm_rq_commit(3 + n);
    return 0;
}
static int gcm_seam_flip(u32 w, int checked)
{
    if (s_split != 1) {
        /* Direct: the host presents from the receipt, as before. */
        gcm_fx_flip(w);
        return 0;
    }
    u32* r = gcm_rq_reserve(2);
    if (!r) return 1;
    r[0] = GCM_RQ_HDR(GCM_RQ_FLIP, checked ? GCM_RQ_F_CHECKED : 0, 1); r[1] = w;
    gcm_rq_commit(2);
    return 0;
}
/* Flips a resync writes off and fences it rescues cannot stop the pass (the
 * resync is already under way): they wait for ring room. */
static void gcm_seam_flips_written_off(int set_all)
{
    if (s_split != 1) { gcm_fx_flips_written_off(set_all, s_fifo_flips_queued); return; }
    u32* r;
    while (!(r = gcm_rq_reserve(3))) { s_rq.full_stops++; gcm_split_sleep_us(100); }
    r[0] = GCM_RQ_HDR(GCM_RQ_FLIPS_OFF, gcm_checked_ref_mode() ? GCM_RQ_F_CHECKED : 0, 2); r[1] = (u32)set_all; r[2] = s_fifo_flips_queued;
    gcm_rq_commit(3);
}
static void gcm_seam_ref_rescue(u32 value, u32 io)
{
    if (s_split != 1) { gcm_ref_push_at(value, io); return; }
    u32* r;
    while (!(r = gcm_rq_reserve(3))) { s_rq.full_stops++; gcm_split_sleep_us(100); }
    r[0] = GCM_RQ_HDR(GCM_RQ_REF_RESCUE, gcm_checked_ref_mode() ? GCM_RQ_F_CHECKED : 0, 2); r[1] = value; r[2] = io;
    gcm_rq_commit(3);
}
/* The NV0039 copy nv0039_packet recognised. Returns 1 when the ring is full
 * (the packet is not consumed; nv0039_packet reports "not yet"). */
static int gcm_seam_nv0039(u32 io, const u32 v[8])
{
    if (s_split != 1) { nv0039_copy(s_nv0039.in_dma, s_nv0039.out_dma, v, io); return 0; }
    u32* r = gcm_rq_reserve(12);
    if (!r) return 1;
    r[0] = GCM_RQ_HDR(GCM_RQ_NV0039, gcm_checked_ref_mode() ? GCM_RQ_F_CHECKED : 0, 11);
    r[1] = io; r[2] = s_nv0039.in_dma; r[3] = s_nv0039.out_dma;
    memcpy(r + 4, v, 8 * sizeof(u32));
    s_rq.draw_fence = atomic_load_explicit(&s_rq.tail, memory_order_relaxed) + 12;   /* the copy reads its source */
    gcm_rq_commit(12);
    return 0;
}
static void gcm_seam_publish_get(u32 value)
{
    if (s_split != 1 || s_rq.get_decode) { vm_write32(GCM_CONTROL_GUEST_ADDR + 4, value); return; }
    while (gcm_split_publish_get(value) < 0) { s_rq.full_stops++; gcm_split_sleep_us(100); }
}
static void gcm_seam_pass_end(int issue)
{
    if (s_split != 1) {
        gcm_reports_progress(0, issue);
        return;
    }
    u32* r;
    while (!(r = gcm_rq_reserve(2))) { s_rq.full_stops++; gcm_split_sleep_us(100); }
    r[0] = GCM_RQ_HDR(GCM_RQ_PASS_END, gcm_checked_ref_mode() ? GCM_RQ_F_CHECKED : 0, 1); r[1] = (u32)issue;
    gcm_rq_commit(2);
}
