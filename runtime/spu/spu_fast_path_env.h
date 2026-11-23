/* Every environment variable the MFC command path (spu_dma.h), the DMA
 * publication observer, the channel entries (spu_wrch, spu_rdch, spu_rchcnt),
 * the overlay note and the lock guard read. PS3RECOMP_MFC_FAST=1 takes the
 * compact form of those paths only while none of these is set, so a run that
 * arms any of them keeps the full form and every diagnostic it asked for.
 *
 * tools/test_spu_fast_path.py scans those sources for getenv() names and fails
 * when one is missing here: a diagnostic added to the path without a line in
 * this list would silently stop working under the compact form. */
#ifndef SPU_FAST_PATH_ENV_H
#define SPU_FAST_PATH_ENV_H
static const char* const spu_fast_path_env[] = {
    "LBP_SKIP_NULL_DMA",
    "PS3_OUTPUT_PROVENANCE",
    "PS3_OUTPUT_PROVENANCE_TARGET",
    "PS3RECOMP_SPU_PUT_WATCH",
    "SPU_CHHIST",
    "SPU_CMDDUMP",
    "SPU_CMDHIST",
    "SPU_DBG_MBOX",
    "SPU_DMACHK",
    "SPU_DMATRACE",
    "SPU_DMATRACE_ALL",
    "SPU_DMATRACE_RAW",
    "SPU_DMA_IMAGES",
    "SPU_DMA_IMAGES_DUMP",
    "SPU_DMA_LAX",
    "SPU_DMA_PUBLICATION",
    "SPU_DMA_PUBLICATION_ATOMIC",
    "SPU_DMA_PUBLICATION_HEADER_WORDS",
    "SPU_DMA_PUBLICATION_INCOMING",
    "SPU_DMA_PUBLICATION_LIMIT",
    "SPU_DMA_PUBLICATION_MATCH_RANGE",
    "SPU_DMA_PUBLICATION_MATCH_WORD",
    "SPU_DMA_PUBLICATION_WORDS",
    "SPU_DMA_RANGE",
    "SPU_DMA_REPEAT_BACKOFF_MS",
    "SPU_DMA_REPEAT_LIMIT",
    "SPU_DMA_REPEAT_LOG",
    "SPU_DMA_REPEAT_SECS",
    "SPU_DMA_SKIP_DUMP",
    "SPU_DMA_SOURCE_F32BE",
    "SPU_DMA_WATCH",
    "SPU_DSP_IMAGE_EA",
    "SPU_DUMP_GET",
    "SPU_GETHIST",
    "SPU_H2LSRC",
    "SPU_LOCKGUARD",
    "SPU_LSDUMP",
    "SPU_LS_WATCH",
    "SPU_MBOXTRACE",
    "SPU_MFC_TRACE",
    "SPU_OVL_DIAG",
    "SPU_POISON_DMA_WATCH",
    "SPU_POLLTRACE",
    "SPU_PUTEA",
    "SPU_PUTHIST",
    "SPU_RESIDENCY_CLEAR_LOG",
    "SPU_ROWCOV",
    "SPU_SMC_HI",
    "SPU_SMC_LO",
    "SPU_SMC_WATCH",
    "SPU_STACKEA_WATCH",
    "SPU_TAGPROBE",
    "SPU_VRAMPC",
    "SPU_WATCHEA",
    "SPU_WATCHEA_EVERY",
    "SPU_WATCHLSA",
    "SPU_WHOPOLLS",
    "YDKJ_CRI_CHAIN",
    "YDKJ_DMA_IMG",
    "YDKJ_NO_CRI_R4",
    0
};
/* Every environment variable read between the entry of spu_indirect_branch
 * and its call of the resolved function, by the resolver's dispatch cache, or
 * by the dispatch histogram, plus the cache modes that must see every
 * lookup. PS3RECOMP_SPU_IB_FAST=1 takes the short dispatch entry only while
 * none of these is set (tools/test_spu_ib_fast.py checks the list) and the
 * dispatch cache is on (SPU_DISPATCH_CACHE=1 exactly, spu_fast_path_resolve). */
static const char* const spu_ib_fast_env[] = {
    "LBP_HLE_JOBDONE",
    "PS3RECOMP_SPURS_PM_RECORD",   /* the policy-run recorder sees every branch */
    "PS3RECOMP_SPURS_PM_STATS",
    "SPU_DSPDESC",
    "SPU_DUMP_OVL",
    "SPU_IBCOV",
    "SPU_IBHIST",
    "SPU_INTERP_UNLIFTED",
    "SPU_IRQTRACE",
    "SPU_JOBEXEC",
    "SPU_JOBEXEC_DUMP",
    "SPU_JOBTRACE",
    "SPU_LOCKGUARD",
    "SPU_MISS_DUMP_IMG",
    "SPU_MISS_DUMP_PCMIN",
    "SPU_OVERLAY_RESIDENCY_SHADOW",
    "SPU_OVERLAY_RESIDENCY_STATS",
    "SPU_POLLTRACE",
    "SPU_VERIFIED_LEAF_BLOCKS",
    "SPURS_PM_TRACE",
    "YDKJ_CRI_CHAIN",
    "YDKJ_NO_CRI_R4",
    "YDKJ_POLTRACE",
    "YDKJ_SPU_LSDUMP",
    0
};
#endif
