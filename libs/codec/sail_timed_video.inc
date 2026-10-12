/* Opt-in bounded retail THX and MAG intro producer. Every callback runs on the caller's
 * original guest thread, under the descriptor mutation guard, outside locks. */
#if defined(PS3RECOMP_SAIL_FFMPEG) && PS3RECOMP_SAIL_FFMPEG
extern int sail_video_open(FILE*,uint64_t,uint64_t,unsigned,SailVideoDecoder**);
extern int sail_video_next(SailVideoDecoder*,SailVideoFrame*);
extern void sail_video_close(SailVideoDecoder*);
#define SAIL_THX_BYTES (5120u*720u)
typedef struct SailTimedProfile {
    const char* name;
    u32 pictures;
} SailTimedProfile;
/* Verified retail profiles share format and cadence. Neither a basename nor
 * header duration alone admits an arbitrary frame count or terminal timestamp. */
static const SailTimedProfile s_timed_profiles[] = {
    {"thx.pam",179u}, {"mag_intro.pam",1806u}
};
typedef struct SailTimedSlot {
    u32 buffer, location, offset;
    u64 identity, retire_poll;
} SailTimedSlot;
typedef struct SailTimedVideo {
    const SailTimedProfile* profile;
    SailHeldFrame callbacks;
    SailVideoDecoder* decoder;
    SailVideoFrame pending;
    SailTimedSlot slots[4];
    int current, eof, pumping;
    u32 ordinal;
    u64 anchor, origin, final_deadline;
    u8 format[32];
} SailTimedVideo;
static SailTimedVideo s_timed[CELL_SAIL_PLAYER_MAX];

static u64 sail_timed_poll(u32 player) {
    u64 tag=0;
    sail_graphics_lock();
    for(int i=0;i<SAIL_GRAPHICS_MAX;++i)
        if(s_graphics_configs[i].player_ea==player && s_graphics_adapters[i]) {
            tag=s_graphics_frames[i].tag;break;
        }
    sail_graphics_unlock();return tag;
}
static u64 sail_timed_first_poll(u32 player) {
    u64 time=0;sail_graphics_lock();
    for(int i=0;i<SAIL_GRAPHICS_MAX;++i)if(s_graphics_adapters[i] && s_graphics_configs[i].player_ea==player){time=s_graphics_frames[i].first_poll_us;break;}
    sail_graphics_unlock();return time;
}
static s32 sail_timed_free_slot(SailTimedVideo* t,int n) {
    SailTimedSlot* p=&t->slots[n];
    if(!p->buffer)return CELL_OK;
    SailHeldFrame* h=&t->callbacks;
    vm_write32(h->scratch+32,p->buffer);
    u64 result=0;
    s32 rc=sail_held_invoke(h,h->graphics.callbacks[4],h->graphics.arg_ea,1,h->scratch+32,0,&result);
    if(rc)return rc; /* undispatched: ownership remains known and retained */
    if((s32)result<0){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
    fprintf(stderr,"[cellSail] TimedFrame free buffer=%08X identity=%llu poll=%llu\n",p->buffer,
        (unsigned long long)p->identity,(unsigned long long)sail_timed_poll(h->player));
    if(p->identity)rsx_picture_release(p->identity);
    memset(p,0,sizeof(*p));return CELL_OK;
}
static s32 sail_timed_release_locked(u32 player) {
    const int slot=sail_player_slot(player);
    if(slot<0)return SAIL_DESC_INVALID_ARG;
    SailTimedVideo* t=&s_timed[slot];SailHeldFrame* h=&t->callbacks;
    if(!h->active)return CELL_OK;
    sail_audio_hide(player);
    sail_graphics_unpublish_frame(player);
    if(h->uncertain)return SAIL_DESC_INVALID_STATE;
    for(int i=0;i<4;++i){s32 rc=sail_timed_free_slot(t,i);if(rc)return rc;}
    sail_video_frame_release(&t->pending);
    if(t->decoder){sail_video_close(t->decoder);t->decoder=NULL;}
    u64 result=0;
    if(h->made_up){
        s32 rc=sail_held_invoke(h,h->graphics.callbacks[1],h->graphics.arg_ea,0,0,0,&result);
        if(rc)return rc;
        if((s32)result<0){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
        h->made_up=0;
    }
    if(h->scratch){
        s32 rc=sail_held_invoke(h,h->free_opd,h->allocator_arg,128,h->scratch,0,&result);
        if(rc)return rc;h->scratch=0;
    }
    sail_graphics_withdraw_frame(player);memset(t,0,sizeof(*t));return CELL_OK;
}
/* AddressToOffset can auto-map; the public tables instead validate every page.
 * The common held mapping does not expose location, so preserve it unchanged. */
static s32 sail_timed_mapping(SailTimedVideo* t,SailTimedSlot* p) {
    SailHeldFrame* h=&t->callbacks;const u32 tmp=h->scratch+64;
    h->buffer=p->buffer;
    s32 rc=sail_held_mapping(h,SAIL_THX_BYTES);if(rc)return rc;
    p->offset=vm_read32(h->scratch+36);
    if(cellGcmGetConfiguration((void*)(uintptr_t)tmp))return SAIL_DESC_NOT_AVAILABLE;
    const u64 base=vm_read32(tmp),size=vm_read32(tmp+8);
    p->location=!(base && p->buffer>=base && (u64)p->buffer+SAIL_THX_BYTES<=base+size);
    return CELL_OK;
}
static s32 sail_timed_allocate(SailTimedVideo* t,unsigned requested) {
    SailHeldFrame* h=&t->callbacks;
    for(unsigned i=0;i<requested;++i)vm_write32(h->scratch+32+4*i,0);
    u64 result=0;
    s32 rc=sail_held_invoke(h,h->graphics.callbacks[3],h->graphics.arg_ea,
        SAIL_THX_BYTES,requested,h->scratch+32,&result);
    if(rc)return rc;
    unsigned count=0;u32 buffers[4]={0};
    for(unsigned i=0;i<requested;++i){buffers[i]=vm_read32(h->scratch+32+4*i);if(buffers[i])++count;}
    /* Validate before adopting or freeing any returned aliases. Invalid ownership
     * cannot be guessed, including duplicate entries on an error return. */
    for(unsigned i=0;i<requested;++i)if(buffers[i]){
        if(!sail_desc_range(buffers[i],SAIL_THX_BYTES)){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
        for(unsigned j=0;j<i;++j)if(buffers[j] && (u64)buffers[i]< (u64)buffers[j]+SAIL_THX_BYTES &&
            (u64)buffers[j]<(u64)buffers[i]+SAIL_THX_BYTES){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
        for(int j=0;j<4;++j)if(t->slots[j].buffer && (u64)buffers[i]<(u64)t->slots[j].buffer+SAIL_THX_BYTES &&
            (u64)t->slots[j].buffer<(u64)buffers[i]+SAIL_THX_BYTES){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
    }
    for(unsigned i=0;i<requested;++i)if(buffers[i]){
        int n=0;while(n<4 && t->slots[n].buffer)++n;
        if(n==4){h->uncertain=1;return SAIL_DESC_INVALID_STATE;}
        t->slots[n].buffer=buffers[i];
    }
    if((s32)result<0)return SAIL_DESC_MEMORY;
    if((result==0 && count!=requested) || (result && (result>requested || result!=count)))
        return SAIL_DESC_MEMORY;
    if(!count)return SAIL_DESC_MEMORY;
    for(int i=0;i<4;++i)if(t->slots[i].buffer && !t->slots[i].identity){
        rc=sail_timed_mapping(t,&t->slots[i]);if(rc)return rc;
    }
    return CELL_OK;
}
static int sail_timed_metadata(const SailTimedVideo* t,const SailVideoFrame* p,unsigned ordinal) {
    return p->rgba && p->ordinal==ordinal && ordinal<=t->profile->pictures &&
        p->width==1280 && p->height==720 && p->pitch==5120 && p->colorspace==1 &&
        p->color_range==1 && !p->interlaced && p->time_base_num==1 && p->time_base_den==90000 &&
        p->rate_num==30000 && p->rate_den==1001 && p->duration==3003 &&
        ((ordinal<t->profile->pictures && p->pts_valid && p->pts==90000+(s64)(ordinal-1)*3003) ||
         (ordinal==t->profile->pictures && !p->pts_valid && p->pts==INT64_MIN));
}
static s32 sail_timed_next(SailTimedVideo* t) {
    const int result=sail_video_next(t->decoder,&t->pending);
    if(result || !sail_timed_metadata(t,&t->pending,t->ordinal+1))return SAIL_DESC_NOT_AVAILABLE;
    if(t->pending.ordinal==t->profile->pictures){
        SailVideoFrame extra={0};const int tail=sail_video_next(t->decoder,&extra);
        sail_video_frame_release(&extra);
        if(tail!=SAIL_VIDEO_EOF)return SAIL_DESC_NOT_AVAILABLE;
        t->eof=1; /* terminal unknown PTS has separately verified scheduling cadence */
    }
    return CELL_OK;
}
static s32 sail_timed_publish(SailTimedVideo* t,int n) {
    SailTimedSlot* p=&t->slots[n];SailVideoFrame* frame=&t->pending;
    memcpy(vm_base+p->buffer,frame->rgba,SAIL_THX_BYTES);
    p->identity=rsx_picture_create(p->buffer,p->location,p->offset,SAIL_THX_BYTES,frame->rgba);
    if(!p->identity)return SAIL_DESC_NOT_AVAILABLE;
    const s32 rc=sail_graphics_replace_identity(t->callbacks.player,p->buffer,t->format,
        (u64)frame->pts,t->origin,frame->ordinal,p->identity);
    if(rc)return rc;
    if(t->current>=0)t->slots[t->current].retire_poll=sail_timed_poll(t->callbacks.player)+2;
    t->current=n;t->ordinal=frame->ordinal;
    fprintf(stderr,"[cellSail] TimedFrame publish movie=%s ordinal=%u buffer=%08X identity=%llu pts=%lld ptsValid=%d clock=%llu deadline=%llu terminalCadence=%d\n",
        t->profile->name,t->ordinal,p->buffer,(unsigned long long)p->identity,(long long)frame->pts,frame->pts_valid,
        (unsigned long long)ppu_system_time_us(),(unsigned long long)(t->anchor+(u64)(t->ordinal-1)*1001000/30),!frame->pts_valid);
    sail_video_frame_release(frame);return CELL_OK;
}
static s32 sail_timed_reject(int slot,u32 player,u64 token,s32 rc) {
    const s32 released=sail_timed_release_locked(player);if(released)rc=released;
    sail_descriptor_result(rc);
    if(!s_timed[slot].callbacks.active && sail_stream_lifecycle(player,token,1)==CELL_OK)
        sail_stream_notify(slot,1,0,(u64)(s64)rc,0);
    return rc;
}
static s32 sail_timed_start(u32 player,u32 command,u64 token) {
    if(!sail_descriptor_enter())return SAIL_DESC_INVALID_STATE;
    const int slot=sail_player_slot(player);
    if(slot<0)return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    SailTimedVideo* t=&s_timed[slot];SailHeldFrame* h=&t->callbacks;
    if(s_sail_players[slot].stream_event_busy || h->active)return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if(!sail_desc_range(command,40) || vm_read32(command)!=1)return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    for(unsigned i=4;i<40;i+=4)if(vm_read32(command+i))return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
    SailDescriptor* d=NULL;
    for(int i=0;i<MAX_DESCRIPTORS;++i)if(s_descs[i].in_use && s_descs[i].player==player && s_descs[i].stream_open){
        if(d)return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);d=&s_descs[i];
    }
    if(!d)return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    const char* base=strrchr(d->uri,'/');
    const SailTimedProfile* profile=NULL;
    if(base)for(unsigned i=0;i<sizeof(s_timed_profiles)/sizeof(s_timed_profiles[0]);++i)
        if(!strcmp(base+1,s_timed_profiles[i].name)){profile=&s_timed_profiles[i];break;}
    if(sail_held_enabled() || !profile || !d->media || !d->selections[1].valid ||
        d->selections[1].stream[0]!=2 || d->selections[1].stream[4]!=0xE0 || !g_ps3_guest_caller ||
        !s_sail_players[slot].callback_ea || !sail_video_available() || !cellGcm_checked_reference_ready())
        return sail_timed_reject(slot,player,token,SAIL_DESC_NOT_AVAILABLE);
    memset(t,0,sizeof(*t));t->current=-1;t->profile=profile;
    s32 rc=sail_graphics_begin_session(player,&h->graphics,&h->session);
    if(rc)return sail_timed_reject(slot,player,token,rc);
    h->active=1;h->player=player;h->alloc_opd=s_sail_players[slot].allocator_alloc_opd;
    h->free_opd=s_sail_players[slot].allocator_free_opd;h->allocator_arg=s_sail_players[slot].allocator_arg;
    const u64 offset=(u64)vm_read32(d->header_ea+8)*2048,length=(u64)vm_read32(d->header_ea+12)*2048;
    t->origin=((u64)vm_base[d->header_ea+0x56]<<40)|((u64)vm_base[d->header_ea+0x57]<<32)|vm_read32(d->header_ea+0x58);
    rc=sail_audio_start(player,d,offset,length,t->origin,h->session);if(rc)return sail_timed_reject(slot,player,token,rc);
    if(sail_video_open(d->media,offset,length,0x1E0,&t->decoder))return sail_timed_reject(slot,player,token,SAIL_DESC_NOT_AVAILABLE);
    rc=sail_timed_next(t);if(rc)return sail_timed_reject(slot,player,token,rc);
    u64 result=0;
    rc=sail_held_invoke(h,h->alloc_opd,h->allocator_arg,128,128,0,&result);
    if(rc)return sail_timed_reject(slot,player,token,rc);
    if(!result || result>UINT32_MAX || (result&127) || !sail_desc_range(result,128)){
        if(result)h->uncertain=1;
        return sail_timed_reject(slot,player,token,result?SAIL_DESC_INVALID_STATE:SAIL_DESC_MEMORY);
    }
    h->scratch=(u32)result;memset(vm_base+h->scratch,0,128);
    const u8 format[32]={1,0,8,3,5,0,2,0xD0,0,0,0x14,0,0,0,0,255,1,1,0};
    memcpy(t->format,format,32);memcpy(vm_base+h->scratch,format,32);
    rc=sail_held_invoke(h,h->graphics.callbacks[0],h->graphics.arg_ea,0,0,0,&result);
    if(rc)return sail_timed_reject(slot,player,token,rc);
    h->made_up=1;if((s32)result<0)return sail_timed_reject(slot,player,token,(s32)result);
    rc=sail_timed_allocate(t,4);if(rc)return sail_timed_reject(slot,player,token,rc);
    unsigned owned=0;for(int i=0;i<4;++i)owned+=t->slots[i].buffer!=0;
    if(owned<3)return sail_timed_reject(slot,player,token,SAIL_DESC_MEMORY);
    rc=sail_held_invoke(h,h->graphics.callbacks[2],h->graphics.arg_ea,h->scratch,h->session,0,&result);
    if(rc)return sail_timed_reject(slot,player,token,rc);
    t->anchor=ppu_system_time_us();sail_audio_activate(player,t->anchor);t->final_deadline=t->anchor+(u64)t->profile->pictures*1001000/30;
    rc=sail_timed_publish(t,0);if(rc)return sail_timed_reject(slot,player,token,rc);
    s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_RUNNING;s_sail_players[slot].stream_event_busy=1;
    sail_descriptor_result(CELL_OK);
    sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_START_TRANSITION,0);
    sail_stream_notify(slot,SAIL_EV_CALL_COMPLETED,SAIL_CALL_START,0,token);
    sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_RUNNING,0);
    s_sail_players[slot].stream_event_busy=0;
    return CELL_OK;
}
static void sail_timed_pump(u32 player) {
    const int slot=sail_player_slot(player);if(slot<0)return;
    SailTimedVideo* t=&s_timed[slot];SailHeldFrame* h=&t->callbacks;
    if(!h->active || h->uncertain || t->pumping || s_sail_players[slot].stream_event_busy)return;
    if(!sail_descriptor_enter())return;
    t->pumping=1;s32 rc=CELL_OK;
    if(!cellGcm_checked_reference_ready()){rc=SAIL_DESC_NOT_AVAILABLE;goto failed;}
    if(t->ordinal==t->profile->pictures){
        if(!t->eof){rc=SAIL_DESC_NOT_AVAILABLE;goto failed;}
        const u64 selected=sail_timed_first_poll(player);
        const u64 hold_end=selected+(1001000+29)/30;
        if(!selected || ppu_system_time_us()<t->final_deadline || ppu_system_time_us()<hold_end)goto done;
        fprintf(stderr,"[cellSail] TimedVideo final duration movie=%s nominalEnd=%llu selected=%llu holdEnd=%llu clock=%llu\n",
            t->profile->name,(unsigned long long)t->final_deadline,(unsigned long long)selected,(unsigned long long)hold_end,(unsigned long long)ppu_system_time_us());
        const SailTimedProfile* completed=t->profile;
        rc=sail_timed_release_locked(player);if(rc)goto failed;
        s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_FINISHED;s_sail_players[slot].stream_event_busy=1;
        sail_descriptor_result(CELL_OK);
        sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_STOP_TRANSITION,0);
        sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_OPENED,0);
        s_sail_players[slot].stream_event_busy=0;
        fprintf(stderr,"[cellSail] TimedVideo genuine EOF cleanup movie=%s pictures=%u clock=%llu\n",completed->name,completed->pictures,(unsigned long long)ppu_system_time_us());return;
    }
    /* Retire only replaced pictures after two frame-bearing polls. Immutable
     * descriptor/command references remain independent after original free. */
    for(int i=0;i<4;++i)if(i!=t->current && t->slots[i].identity &&
        sail_timed_poll(player)>=t->slots[i].retire_poll){rc=sail_timed_free_slot(t,i);if(rc)goto failed;}
    if(!t->pending.rgba){rc=sail_timed_next(t);if(rc)goto failed;}
    if(ppu_system_time_us()<t->anchor+(u64)t->ordinal*1001000/30)goto done;
    int available=-1;
    for(int i=0;i<4;++i)if(t->slots[i].buffer && !t->slots[i].identity){available=i;break;}
    if(available<0){
        int empty=0;for(int i=0;i<4;++i)empty+=!t->slots[i].buffer;
        if(!empty)goto done; /* bounded poll backpressure, never overwrite */
        rc=sail_timed_allocate(t,1);if(rc)goto failed;
        for(int i=0;i<4;++i)if(t->slots[i].buffer && !t->slots[i].identity){available=i;break;}
    }
    if(available<0){rc=SAIL_DESC_MEMORY;goto failed;}
    rc=sail_timed_publish(t,available);if(rc)goto failed;
done:
    t->pumping=0;sail_descriptor_result(CELL_OK);return;
failed:
    t->pumping=0;
    fprintf(stderr,"[cellSail] TimedVideo failed=%08X movie=%s ordinal=%u\n",(u32)rc,t->profile?t->profile->name:"none",t->ordinal);
    sail_timed_reject(slot,player,0,rc);
}
static s32 sail_timed_stop(u32 player,u64 token) {
    if(!sail_descriptor_enter())return SAIL_DESC_INVALID_STATE;
    const int slot=sail_player_slot(player);if(slot<0)return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if(s_sail_players[slot].stream_event_busy)return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    s32 rc=sail_timed_release_locked(player);if(rc)return sail_descriptor_result(rc);
    s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_FINISHED;s_sail_players[slot].stream_event_busy=1;
    sail_descriptor_result(CELL_OK);
    sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_STOP_TRANSITION,0);
    sail_stream_notify(slot,SAIL_EV_CALL_COMPLETED,SAIL_CALL_STOP,0,token);
    sail_stream_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_OPENED,0);
    s_sail_players[slot].stream_event_busy=0;return CELL_OK;
}
#else
static s32 sail_timed_start(u32 p,u32 c,u64 t){(void)p;(void)c;(void)t;return SAIL_GRAPHICS_NOT_AVAILABLE;}
static s32 sail_timed_stop(u32 p,u64 t){(void)p;(void)t;return SAIL_GRAPHICS_NOT_AVAILABLE;}
static s32 sail_timed_release_locked(u32 p){(void)p;return CELL_OK;}
static void sail_timed_pump(u32 p){(void)p;}
#endif
