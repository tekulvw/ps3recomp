/* The FIFO walker's effects, lifted out of gcm_rsx_process_fifo_pass
 * (cellGcmSys.c), which includes this file just before the pass:
 *
 *  - gcm_fx_methods: the method-packet loop (the engine, labels, references,
 *    reports, the 2D engines), inlined back into the pass at its one call;
 *  - gcm_fx_sema: an NV406E semaphore packet (OFFSET / ACQUIRE / RELEASE);
 *  - gcm_fx_flip: a flip word the pass consumed;
 *  - gcm_fx_flips_written_off: the flips a resync writes off.
 *
 * Same code, same order, same effects as when they sat inside the pass. The
 * semaphore and flip paths and their statics are cold; taking them out of a
 * thousand-line function leaves the hot per-word loop a smaller function to
 * compile. */

/* ---- the effects --------------------------------------------------------- */

extern s32 cellGcmSetFlipCommand(u32 bufferId);
static rsx_state s_legacy_state;
static int       s_legacy_state_inited;

/* Applies one method packet; each word is read from guest memory at its
 * mapped address (`words` is NULL: a caller holding a copy may pass it).
 * Returns 0 when the packet is done, 1 when the pass must stop at this packet
 * and come back to it (a report dependency or inline-image barrier failed). */
#if defined(__GNUC__) || defined(__clang__)
#define GCM_FX_INLINE static inline __attribute__((always_inline))
#else
#define GCM_FX_INLINE static inline
#endif
/* Inlined into the pass: the per-word loop it had before, no call per packet. */
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
 * RELEASE's label is applied (0: do not). */
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

