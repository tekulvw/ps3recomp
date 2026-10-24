/* Real resolver/cache: coverage ranges are immutable within a registration
 * generation, but every selected byte epoch and ownership key remains live. */
#define main coverage_unused_mfc_main
#include "coverage_fixture.h"
#undef main
unsigned coverage_full_calls;
static unsigned coverage_scans;
static int allocation_fail;
void* coverage_malloc(size_t size)
{
    if(allocation_fail){allocation_fail=0;return NULL;}
    return malloc(size);
}
static void primary(spu_context* c){(void)c;}
static void overlay(spu_context* c){(void)c;}
static void task(spu_context* c){(void)c;}
static void region(spu_context* c){(void)c;}
static void verified_a(spu_context* c){(void)c;}
static void verified_b(spu_context* c){(void)c;}
static const uint8_t bytes_a[8]={0x11,2,3,4,5,6,7,8};
static const uint8_t bytes_b[4]={0x22,3,4,5};
static spu_verified_code_segment overlap_segments[2]={{0x10000,8,bytes_a},{0x10100,4,bytes_b}};
static int publish_armed;
void coverage_publish_observe(void)
{
    ++coverage_scans;
    if(publish_armed){publish_armed=0;CHECK(spu_verified_overlay_register(321,332,overlap_segments,2));}
}
static void expect(spu_context* c,uint32_t pc,spu_lifted_fn fn)
{
    CHECK(spu_lifted_lookup(c,pc)==fn);
    CHECK(spu_lifted_lookup_full(c,pc)==fn);
}
static spu_context* fresh(int image)
{
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=NULL);spu_context_init(c,0);
    c->image_id=image;return c;
}
static void ownership(void)
{
    const uint32_t a=0xb000;
    spu_begin_image(311);spu_register_function(a,primary);spu_register_function(a+4,primary);
    spu_begin_image(312);spu_register_function(a,overlay);
    spu_begin_image(314);spu_register_function(a,region);spu_begin_image(0);
    spu_context* c=fresh(311);expect(c,a,primary);expect(c,a,primary);
    c->resident_ovl=312;expect(c,a,overlay);c->resident_task=313;expect(c,a,overlay);
    spu_begin_image(313);spu_register_function(a,task);spu_begin_image(0);expect(c,a,task);
    spu_overlay_register_region(0x600000,0x100,314);
    spu_overlay_note_get(c,0x600000,c->ls+a,16);expect(c,a,region);expect(c,a+4,NULL);
    spu_overlay_note_get(c,0x610000,c->ls+a,16);expect(c,a,task);
    spu_overlay_note_get(c,0x600000,c->ls+a,16);expect(c,a,region);
    spu_context_init(c,0);c->image_id=311;expect(c,a,primary);
    spu_overlay_note_get(c,0x600000,c->ls+a,16);expect(c,a,region);
    spu_context_init(c,0);c->image_id=311;
    spu_overlay_note_get(c,0x600000,c->ls+a+0x200,16);
    expect(c,a,primary); /* same numeric residency count, different lifetime */
    c->ls=calloc(1,SPU_LS_SIZE);CHECK(c->ls!=NULL);expect(c,a,primary);
    free(c->ls);c->ls=c->ls_store;free(c);
}
int main(void)
{
    ownership();
    spu_begin_image(321);spu_register_function(0x10000,primary);
    spu_begin_image(331);spu_register_function(0x10000,verified_a);spu_register_function(0x10004,verified_a);
    spu_begin_image(332);spu_register_function(0x10000,verified_b);spu_register_function(0x10004,verified_b);
    spu_begin_image(0);
    spu_context* c=fresh(321);c->policy_mode=1;memcpy(c->ls+0x10000,bytes_a,8);
    expect(c,0x10000,primary);
    spu_verified_code_segment a={0x10000,8,bytes_a};
    CHECK(spu_verified_overlay_register(321,331,&a,1));
    expect(c,0x10000,verified_a);
#ifdef SPU_DCACHE_GENERATION_TEST
    unsigned scans=coverage_scans;
    expect(c,0x10000,verified_a);CHECK(coverage_scans==scans);
#endif
    /* Caller-owned descriptors and bytes never become the registered ranges. */
    a.lsa=0x20000;a.bytes=NULL;
    unsigned count=s_verified_count;
#ifdef SPU_DCACHE_GENERATION_TEST
    uint32_t generation=__atomic_load_n(&s_verified_generation,__ATOMIC_ACQUIRE);
    CHECK(!(generation&1));
#endif
    CHECK(!spu_verified_overlay_register(321,331,overlap_segments,2)); /* reject in-place replacement */
    CHECK(!spu_verified_overlay_register(321,333,NULL,0));
    allocation_fail=1;
    CHECK(!spu_verified_overlay_register(321,333,overlap_segments,2));
    CHECK(!allocation_fail);
    CHECK(s_verified_count==count);expect(c,0x10000,verified_a);
#ifdef SPU_DCACHE_GENERATION_TEST
    CHECK(__atomic_load_n(&s_verified_generation,__ATOMIC_ACQUIRE)==generation);
    if(spu_dcache_mode()){
        unsigned calls=coverage_full_calls;
        __atomic_add_fetch(&s_verified_generation,1,__ATOMIC_RELEASE); /* publication in progress */
        CHECK(spu_lifted_lookup(c,0x10000)==verified_a && coverage_full_calls>calls);
        spu_dcache_ent* odd=&t_dcache.ent[(((0x10000>>2)^c->dcache_id)*0x9E3779B1u)>>23];
        CHECK(odd->vregistration!=__atomic_load_n(&s_verified_generation,__ATOMIC_ACQUIRE));
        __atomic_add_fetch(&s_verified_generation,1,__ATOMIC_RELEASE);
    }
#endif
#ifdef SPU_DCACHE_MIDPUBLICATION_TEST
    publish_armed=1;
    expect(c,0x10004,verified_a); /* observer publishes overlap during coverage derivation */
    CHECK(!publish_armed);
    spu_dcache_ent* e=&t_dcache.ent[(((0x10004>>2)^c->dcache_id)*0x9E3779B1u)>>23];
    CHECK(!e->fn || e->id!=c->dcache_id || e->lsa!=0x10004); /* do not publish mixed snapshot */
#else
    CHECK(spu_verified_overlay_register(321,332,overlap_segments,2));
#endif
    CHECK(s_verified_count==count+1);
    /* The tail DMA changes ONLY overlapping image B's epoch. Omitting B from
     * the target's mask incorrectly preserves A instead of rejecting ambiguity. */
    overlap_segments[0].lsa=0x20000;overlap_segments[1].lsa=0x20100;
    expect(c,0x10000,verified_a);expect(c,0x10000,verified_a);
    memcpy(c->ls+0x10100,bytes_b,4);spu_overlay_note_get(c,0x700000,c->ls+0x10100,4);
    expect(c,0x10000,NULL);
    /* Misses are not cached. Restoring unmatched tail bytes is observable even
     * without another DMA epoch; the prior successful entry was invalidated. */
    memset(c->ls+0x10100,0,4);expect(c,0x10000,verified_a);
    c->policy_mode=0;expect(c,0x10000,NULL);c->policy_mode=1;expect(c,0x10000,verified_a);
    uint8_t* raw=calloc(1,SPU_LS_SIZE);CHECK(raw!=NULL);memcpy(raw,c->ls,SPU_LS_SIZE);
    c->ls=raw;expect(c,0x10000,NULL);c->ls=c->ls_store;free(raw);
    /* Subsequent caller mutations to source descriptors did not remove overlap. */
    memcpy(c->ls+0x10100,bytes_b,4);spu_overlay_note_get(c,0x700004,c->ls+0x10100,4);
    expect(c,0x10000,NULL);
    free(c);
    CHECK(!atomic_load(&s_dcache_shadow_bad));
    printf("DCACHE_COVERAGE mode=%d passed=%d failed=%d\n",spu_dcache_mode(),g_pass,g_fail);
    return g_fail?1:0;
}
