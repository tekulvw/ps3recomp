/* Optional real ATRAC PCM delivery; the original client remains clock owner.
 * Guest scratch is stable through the next GetFrame or stream close. */
#if defined(PS3RECOMP_SAIL_FFMPEG) && PS3RECOMP_SAIL_FFMPEG
#define SAIL_AUDIO_ADAPTER_HAS_PCM 1
static struct SailAudioState {
    SailHeldFrame owner;
    SailAudioPCM pcm;
    u32 cursor,session;
    u64 origin,anchor;
    u8 format[24];
    int active;
} s_audio[SAIL_SOUND_MAX];
static int sail_audio_enabled(void){const char* e=getenv("PS3RECOMP_SAIL_ATRAC_AUDIO");return e && !strcmp(e,"1");}
static int sail_audio_busy_locked(int i){return s_audio[i].owner.active;}
static s32 sail_audio_get_format_locked(int i,u32 out){
    if(!s_audio[i].owner.active || !s_audio[i].pcm.samples)return SAIL_SOUND_INVALID_STATE;
    memcpy(vm_base+out,s_audio[i].format,24);vm_base[out+2]=1;vm_base[out+3]=0;return CELL_OK;
}
static s32 sail_audio_time_locked(int i,u64 pts,u32 out){
    if(!s_audio[i].owner.active || !s_audio[i].pcm.samples)return SAIL_SOUND_INVALID_STATE;
    u64 bits=(pts-s_audio[i].origin)*1000;s64 numerator;memcpy(&numerator,&bits,8);
    vm_write64(out,(u64)(numerator/90));return CELL_OK;
}
static s32 sail_audio_get_frame_locked(int i,u32 samples,u32 info){
    struct SailAudioState* a=&s_audio[i];
    if(!a->active || !samples || samples>a->pcm.count-a->cursor)return CELL_SAIL_SYNC_NO_FRAME;
    const u64 now=ppu_system_time_us();
    const u64 ticks=a->pcm.pts+(u64)a->cursor*90000/a->pcm.rate;
    if(ticks>a->origin && (now<a->anchor || now-a->anchor<(ticks-a->origin)*1000/90))return CELL_SAIL_SYNC_NO_FRAME;
    /* FFmpeg 5.1 = L,R,C,LFE,BL,BR; Sail layout9 = L,C,R,BL,BR,LFE. */
    const unsigned six[6]={0,2,1,4,5,3};
    for(u32 n=0;n<samples;++n)for(u32 c=0;c<a->pcm.channels;++c){
        float value=a->pcm.samples[(u64)(a->cursor+n)*a->pcm.channels+(a->pcm.channels==6?six[c]:c)];
        u32 bits;memcpy(&bits,&value,4);vm_write32(a->owner.buffer+4*(n*a->pcm.channels+c),bits);
    }
    vm_write32(info,a->owner.buffer);vm_write32(info+4,a->session);
    vm_write32(info+8,a->cursor/samples+1);vm_write32(info+12,0);vm_write64(info+16,ticks);
    a->cursor+=samples;
    if(a->cursor==samples || a->cursor%48000<samples)
        fprintf(stderr,"[cellSail] ATRAC frame player=%08X channels=%u samples=%u consumed=%u pts=%llu clock=%llu\n",
            a->owner.player,a->pcm.channels,samples,a->cursor,(unsigned long long)ticks,(unsigned long long)now);
    return 0x214; /* NEXT | NO_SYNC: real PCM, independent bounded wall-clock cadence */
}
static void sail_audio_hide(u32 player){
    sail_sound_lock();for(int i=0;i<SAIL_SOUND_MAX;++i)if(s_audio[i].owner.player==player)s_audio[i].active=0;sail_sound_unlock();
}
static s32 sail_audio_release_locked(u32 player){
    for(int i=0;i<SAIL_SOUND_MAX;++i){
        struct SailAudioState* a=&s_audio[i];SailHeldFrame* h=&a->owner;
        if(!h->active || h->player!=player)continue;
        sail_audio_hide(player);if(h->uncertain)return SAIL_SOUND_INVALID_STATE;
        u64 result=0;s32 rc;
        if(h->made_up){
            rc=sail_held_invoke(h,s_sound_configs[i].callbacks[1],s_sound_configs[i].arg,0,0,0,&result);
            if(rc)return rc;if((s32)result<0){h->uncertain=1;return SAIL_SOUND_INVALID_STATE;}h->made_up=0;
        }
        if(h->scratch){rc=sail_held_invoke(h,h->free_opd,h->allocator_arg,128,h->scratch,0,&result);if(rc)return rc;h->scratch=0;}
        sail_sound_lock();sail_audio_pcm_release(&a->pcm);memset(a,0,sizeof(*a));sail_sound_unlock();
    }
    return CELL_OK;
}
static s32 sail_audio_start(u32 player,SailDescriptor* d,u64 offset,u64 length,u64 origin,u32 session){
    if(!sail_audio_enabled())return CELL_OK;
    int i=-1;sail_sound_lock();
    for(int n=0;n<SAIL_SOUND_MAX;++n)if(s_sound_adapters[n] && s_sound_configs[n].player==player){if(i>=0){sail_sound_unlock();return SAIL_DESC_NOT_AVAILABLE;}i=n;}
    if(i<0 || s_audio[i].owner.active){sail_sound_unlock();return SAIL_SOUND_INVALID_STATE;}
    struct SailAudioState* a=&s_audio[i];SailHeldFrame* h=&a->owner;
    h->active=1;h->player=player;a->session=session;a->origin=origin;
    sail_sound_unlock();
    if(!d->selections[0].valid || d->selections[0].stream[0]!=0xdc || d->selections[0].stream[4]!=0xbd || d->selections[0].stream[5]>9)return SAIL_DESC_NOT_AVAILABLE;
    SailAudioPCM pcm={0};
    if(sail_audio_decode_pcm(d->media,offset,length,d->selections[0].stream[5],&pcm))return SAIL_DESC_NOT_AVAILABLE;
    const char* base=strrchr(d->uri,'/');
    if(!base || !((!strcmp(base+1,"thx.pam") && pcm.channels==2 && pcm.count==290816) ||
       (!strcmp(base+1,"mag_intro.pam") && pcm.channels==6 && pcm.count==2897920)) || pcm.rate!=48000 || pcm.pts!=85470){sail_audio_pcm_release(&pcm);return SAIL_DESC_NOT_AVAILABLE;}
    const int slot=sail_player_slot(player);h->alloc_opd=s_sail_players[slot].allocator_alloc_opd;h->free_opd=s_sail_players[slot].allocator_free_opd;h->allocator_arg=s_sail_players[slot].allocator_arg;
    const u32 size=pcm.channels*8192+128;u64 result=0;
    s32 rc=sail_held_invoke(h,h->alloc_opd,h->allocator_arg,128,size,0,&result);
    if(rc){sail_audio_pcm_release(&pcm);return rc;}
    if(!result || result>UINT32_MAX || (result&127) || !sail_desc_range(result,size)){
        sail_audio_pcm_release(&pcm);if(result)h->uncertain=1;return result?SAIL_SOUND_INVALID_STATE:SAIL_SOUND_MEMORY;
    }
    h->scratch=(u32)result;h->buffer=h->scratch+128;memset(vm_base+h->scratch,0,size);
    /* Packed BE float PCM, sample count and sample rate from actual decoder. */
    const u8 format[24]={1,(u8)pcm.channels,1,0,0,0,0xbb,0x80,0,0,0,(u8)(pcm.channels==6?9:2),0};
    memcpy(a->format,format,24);memset(a->format+12,0xff,12);memcpy(vm_base+h->scratch,a->format,24);
    sail_sound_lock();a->pcm=pcm;sail_sound_unlock();
    rc=sail_held_invoke(h,s_sound_configs[i].callbacks[0],s_sound_configs[i].arg,0,0,0,&result);
    if(rc)return rc;h->made_up=1;if((s32)result<0)return (s32)result;
    rc=sail_held_invoke(h,s_sound_configs[i].callbacks[2],s_sound_configs[i].arg,h->scratch,session,0,&result);
    if(rc)return rc;
    fprintf(stderr,"[cellSail] ATRAC prepared player=%08X track=%u channels=%u samples=%u pts=%llu origin=%llu buffer=%08X\n",
        player,d->selections[0].stream[5],a->pcm.channels,a->pcm.count,(unsigned long long)a->pcm.pts,(unsigned long long)origin,h->buffer);
    return CELL_OK;
}
static void sail_audio_activate(u32 player,u64 anchor){
    sail_sound_lock();for(int i=0;i<SAIL_SOUND_MAX;++i)if(s_audio[i].owner.player==player && s_audio[i].pcm.samples){s_audio[i].anchor=anchor;s_audio[i].active=1;}sail_sound_unlock();
}
#else
static int sail_audio_busy_locked(int i){(void)i;return 0;}
static s32 sail_audio_get_frame_locked(int i,u32 s,u32 p){(void)i;(void)s;(void)p;return CELL_SAIL_SYNC_NO_FRAME;}
static s32 sail_audio_get_format_locked(int i,u32 p){(void)i;(void)p;return SAIL_SOUND_INVALID_STATE;}
static s32 sail_audio_time_locked(int i,u64 t,u32 p){(void)i;(void)t;(void)p;return SAIL_SOUND_INVALID_STATE;}
static s32 sail_audio_release_locked(u32 p){(void)p;return CELL_OK;}
#endif
