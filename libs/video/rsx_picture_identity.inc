/* Included once by the serialized draw engine. Producer/descriptor callers
 * may be guest threads; only this registry lock crosses that boundary.
 * Tokens own immutable copied bytes. Descriptor, pending-command and bound
 * texture references extend lifetime independently of recycled guest memory. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_picture_identity.h"
enum { PIC_SLOTS=32,PIC_ALIASES=256,PIC_COMMANDS=4096,PIC_OFFSETS=256,PIC_BYTES=128*1024*1024 };
typedef struct {u64 token;u32 ea,location,offset,bytes,refs;u8* pixels;} PictureIdentity;
typedef struct {u32 ea;u64 token;} PictureAlias;
static PictureIdentity pic_slots[PIC_SLOTS];
static PictureAlias pic_aliases[PIC_ALIASES],pic_commands[PIC_COMMANDS];
static struct {u32 location,offset,bytes;} pic_offsets[PIC_OFFSETS];
static u32 pic_offset_count,pic_bytes;
/* Unconsumed pic_commands entries: texture-offset methods look one up on
 * every bind, and with none outstanding the 4096-entry scan finds nothing. */
static u32 pic_command_count;
static u64 pic_serial;
static int pic_failed;
static atomic_flag pic_lock=ATOMIC_FLAG_INIT;
static void pic_enter(void){while(atomic_flag_test_and_set_explicit(&pic_lock,memory_order_acquire)){} }
static void pic_leave(void){atomic_flag_clear_explicit(&pic_lock,memory_order_release);}
static PictureIdentity* pic_find(u64 token){for(unsigned i=0;i<PIC_SLOTS;++i)if(token && pic_slots[i].token==token)return &pic_slots[i];return NULL;}
static int pic_tracked(u32 location,u32 offset){for(u32 i=0;i<pic_offset_count;++i)if(pic_offsets[i].location==location && pic_offsets[i].offset==offset)return 1;return 0;}
static int pic_requires_identity(u32 location,u32 offset,u32 bytes){pic_enter();int result=0;for(u32 i=0;i<pic_offset_count;++i)if(pic_offsets[i].location==location && (u64)offset+bytes>pic_offsets[i].offset && (u64)pic_offsets[i].offset+pic_offsets[i].bytes>offset){result=1;break;}pic_leave();return result;}
static int pic_pending_command(u32 ea){pic_enter();int result=0;if(pic_command_count)for(unsigned i=0;i<PIC_COMMANDS;++i)if(ea && pic_commands[i].ea==ea){result=1;break;}pic_leave();return result;}
static void pic_drop(u64 token){PictureIdentity* p=pic_find(token);if(!p || !p->refs){pic_failed=1;return;}if(!--p->refs){pic_bytes-=p->bytes;free(p->pixels);memset(p,0,sizeof(*p));}}
static int pic_hold(u64 token){PictureIdentity*p=pic_find(token);if(!p || p->refs==UINT32_MAX){pic_failed=1;return 0;}++p->refs;return 1;}
u64 rsx_picture_create(u32 ea,u32 location,u32 offset,u32 bytes,const u8* pixels)
{
    if(!ea || location>1 || !bytes || bytes>64*1024*1024 || !pixels || (u64)ea+bytes>0x100000000ull || (u64)offset+bytes>0x100000000ull)return 0;
    pic_enter();u64 result=0;
    if(pic_failed || bytes>PIC_BYTES-pic_bytes || pic_serial==UINT64_MAX)goto fail;
    if(!pic_tracked(location,offset)){
        if(pic_offset_count==PIC_OFFSETS)goto fail;
        pic_offsets[pic_offset_count].location=location;pic_offsets[pic_offset_count].bytes=bytes;pic_offsets[pic_offset_count++].offset=offset;
    }else{
        for(u32 i=0;i<pic_offset_count;++i)if(pic_offsets[i].location==location && pic_offsets[i].offset==offset && pic_offsets[i].bytes<bytes)pic_offsets[i].bytes=bytes;
    }
    for(unsigned i=0;i<PIC_SLOTS;++i)if(!pic_slots[i].token){
        u8* copy=malloc(bytes);if(!copy)goto fail;memcpy(copy,pixels,bytes);
        result=++pic_serial;pic_slots[i]=(PictureIdentity){result,ea,location,offset,bytes,1,copy};pic_bytes+=bytes;goto done;
    }
fail:pic_failed=1;
done:pic_leave();return result;
}
void rsx_picture_release(u64 token){if(!token)return;pic_enter();pic_drop(token);pic_leave();}
int rsx_picture_retain(u64 token){pic_enter();int result=!pic_failed && pic_hold(token);pic_leave();return result;}
int rsx_picture_bind_descriptor(u32 ea,u64 token)
{
    if(!ea)return -1;pic_enter();int result=-1,empty=-1;
    if(pic_failed && token)goto done;
    for(unsigned i=0;i<PIC_ALIASES;++i){
        if(pic_aliases[i].ea==ea){
            if(pic_aliases[i].token==token){result=1;goto done;}
            if(token && !pic_hold(token))goto done;
            pic_drop(pic_aliases[i].token);pic_aliases[i]=(PictureAlias){token?ea:0,token};result=1;goto done;
        }
        if(!pic_aliases[i].ea && empty<0)empty=(int)i;
    }
    if(!token){result=1;goto done;}
    if(empty<0 || !pic_hold(token))goto fail;
    pic_aliases[empty]=(PictureAlias){ea,token};result=1;goto done;
fail:pic_failed=1;
done:pic_leave();return result;
}
int rsx_picture_capture_command(u32 ea,u32 descriptor,u32 location,u32 offset)
{
    pic_enter();int result=-1,empty=-1;u64 token=0;
    if(pic_failed && token)goto done;
    for(unsigned i=0;i<PIC_ALIASES;++i)if(pic_aliases[i].ea==descriptor){token=pic_aliases[i].token;break;}
    if(!token){if(pic_tracked(location,offset))goto fail;result=0;goto done;}
    PictureIdentity*p=pic_find(token);
    if(!ea || !p || p->location!=location || p->offset!=offset)goto fail;
    for(unsigned i=0;i<PIC_COMMANDS;++i){
        if(pic_commands[i].ea==ea)goto fail; /* Unconsumed sidecar must never be replaced. */
        if(!pic_commands[i].ea && empty<0)empty=(int)i;
    }
    if(empty<0 || !pic_hold(token))goto fail;
    pic_commands[empty]=(PictureAlias){ea,token};++pic_command_count;result=1;goto done;
fail:pic_failed=1;
done:pic_leave();return result;
}
int rsx_picture_take_command(u32 ea,u32 location,u32 offset,u64* token)
{
    if(!token)return -1;*token=0;pic_enter();int result=-1;
    if(pic_failed)goto done;
    if(pic_command_count)for(unsigned i=0;i<PIC_COMMANDS;++i)if(ea && pic_commands[i].ea==ea){
        PictureIdentity*p=pic_find(pic_commands[i].token);
        if(!p || p->location!=location || p->offset!=offset)goto fail;
        *token=p->token;pic_commands[i]=(PictureAlias){0,0};--pic_command_count;result=1;goto done;
    }
    if(pic_tracked(location,offset))goto fail;
    result=0;goto done;
fail:pic_failed=1;
done:pic_leave();return result;
}
/* Borrow requires the caller's retained token (e.g. current texture binding).
 * The registry never releases that reference on another thread. */
const u8* rsx_picture_data(u64 token,u32 location,u32 offset,u32 bytes)
{
    pic_enter();PictureIdentity*p=pic_find(token);const u8* result=NULL;
    if(!pic_failed && p && p->location==location && p->offset==offset && bytes<=p->bytes)result=p->pixels;
    else pic_failed=1;
    pic_leave();return result;
}
int rsx_picture_ready(void){pic_enter();int result=!pic_failed;pic_leave();return result;}
int rsx_picture_live_count(void){pic_enter();int result=0;for(unsigned i=0;i<PIC_SLOTS;++i)result+=!!pic_slots[i].token;pic_leave();return result;}
int rsx_picture_reset(void){pic_enter();int result=1;for(unsigned i=0;i<PIC_SLOTS;++i)if(pic_slots[i].token){result=0;break;}if(result){memset(pic_aliases,0,sizeof(pic_aliases));memset(pic_commands,0,sizeof(pic_commands));pic_command_count=0;pic_offset_count=pic_bytes=0;pic_failed=0;}pic_leave();return result;}
