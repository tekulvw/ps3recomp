/* PAMF 0040/0041 metadata. All API pointers are guest effective addresses.
 * No codec execution: the reader describes resident header bytes only. */
#include "cellPamf.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <string.h>

extern u32 ppu_vm_size;
#define EA(p) ((u32)(uintptr_t)(p))
#define FAIL_ARG ((s32)CELL_PAMF_ERROR_INVALID_ARG)
#define FAIL_HEADER ((s32)CELL_PAMF_ERROR_INVALID_HEADER)
static int range(u64 a, u64 n) {
    u64 limit = ppu_vm_size ? (u64)ppu_vm_size : (1ull << 32);
    return vm_base && a && a < limit && n <= limit-a;
}
static u16 r16(u32 a) { return ((u16)vm_base[a]<<8)|vm_base[a+1]; }
static void w16(u32 a,u16 x) { vm_base[a]=(u8)(x>>8);vm_base[a+1]=(u8)x; }
static s32 type(u32 s) {
    switch(vm_base[s]) {
    case 0x1b:return 0;case 2:return 1;case 0xdc:return 2;
    case 0x80:return 3;case 0x81:return 4;case 0xdd:return 5;
    default:return -1;
    }
}
static int matches(s32 t,u8 wanted) {
    return t>=0 && (t==wanted || (wanted==20 && t<=1) || (wanted==21 && t>=2 && t<=4));
}
/* Restrict this implementation to the single-period/single-group PAMF format.
 * Validate memory and nested lengths even when VERIFY_ON was not requested. */
static s32 header(u32 h,u64 file_size,u32 attr,u64 *hs,u64 *ds) {
    if (!range(h,0x88)) return FAIL_ARG;
    if (vm_read32(h)!=CELL_PAMF_MAGIC) return (s32)CELL_PAMF_ERROR_UNKNOWN_TYPE;
    u32 v=vm_read32(h+4);
    if (v!=0x30303430 && v!=0x30303431) return (s32)CELL_PAMF_ERROR_UNSUPPORTED_VERSION;
    *hs=(u64)vm_read32(h+8)*2048;*ds=(u64)vm_read32(h+12)*2048;
    if (!*hs || *hs>(1ull<<32) || (file_size && *hs+*ds!=file_size)) return FAIL_HEADER;
    if (!vm_base[h+0x6f] || !vm_base[h+0x81]) return FAIL_HEADER;
    if (vm_base[h+0x6f]!=1 || vm_base[h+0x81]!=1) return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    u64 seq_end=0x54ull+vm_read32(h+0x50),period_end=0x74ull+vm_read32(h+0x70);
    u64 group_end=0x86ull+vm_read32(h+0x82),streams_end=0x88ull+48ull*vm_base[h+0x87];
    if (!vm_base[h+0x87] || vm_read32(h+0x6a)!=vm_base[h+0x87] ||
        streams_end>group_end || group_end>period_end || period_end>seq_end || seq_end>*hs ||
        !range(h,(attr&CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)?streams_end:*hs)) return FAIL_HEADER;
    for(u32 i=0;i<vm_base[h+0x87];i++) {
        u32 s=h+0x88+48*i;
        u64 ep=vm_read32(s+8),num=vm_read32(s+12);
        if (type(s)<0 || (num && (ep<streams_end || ep+12*num>*hs))) return FAIL_HEADER;
    }
    return CELL_OK;
}
static s32 reader(u32 r,u32 *h,u32 *s) {
    if(!range(r,128)) return FAIL_ARG;
    *h=vm_read32(r+0x24);
    u64 hs,ds;s32 rc=header(*h,0,vm_read32(r+0x10),&hs,&ds);
    if(rc) return FAIL_HEADER;
    u32 i=vm_read32(r+0x20);
    if(vm_read32(r+0x28)!=*h+0x50 || vm_read32(r+0x2c)!=*h+0x70 ||
       vm_read32(r+0x30)!=*h+0x82 || i>=vm_base[*h+0x87] ||
       vm_read32(r+0x34)!=*h+0x88+48*i) return FAIL_HEADER;
    *s=vm_read32(r+0x34);return CELL_OK;
}
s32 cellPamfReaderInitialize(CellPamfReader* p,void* addr,u64 fileSize,u32 attribute) {
    u32 r=EA(p),h=EA(addr);u64 hs,ds;
    if(!range(r,128) || (attribute&~3u)) return FAIL_ARG;
    s32 rc=header(h,fileSize,attribute,&hs,&ds);if(rc)return rc;
    /* Refuse overlap so reader creation cannot corrupt its backing header. */
    if((u64)r<(u64)h+hs && (u64)h<(u64)r+128)return FAIL_ARG;
    memset(vm_base+r,0,128);vm_write64(r,hs);vm_write64(r+8,ds);
    vm_write32(r+0x10,attribute);w16(r+0x16,vm_read32(h+4)==0x30303431?41:40);
    vm_write32(r+0x24,h);vm_write32(r+0x28,h+0x50);vm_write32(r+0x2c,h+0x70);
    vm_write32(r+0x30,h+0x82);vm_write32(r+0x34,h+0x88);return CELL_OK;
}
static s32 timestamp(CellPamfReader*p,u64*out,u32 offset) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);if(rc)return rc;
    if(!range(EA(out),8))return FAIL_ARG;
    vm_write32(EA(out),r16(h+offset));vm_write32(EA(out)+4,vm_read32(h+offset+2));return CELL_OK;
}
s32 cellPamfReaderGetPresentationStartTime(CellPamfReader*p,u64*out) {return timestamp(p,out,0x56);}
s32 cellPamfReaderGetPresentationEndTime(CellPamfReader*p,u64*out) {return timestamp(p,out,0x5c);}
u32 cellPamfReaderGetMuxRateBound(CellPamfReader*p) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);return rc?(u32)rc:vm_read32(h+0x62)&0x3fffff;
}
s32 cellPamfReaderGetNumberOfStreams(CellPamfReader*p) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);return rc?rc:vm_base[h+0x87];
}
s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader*p,u8 wanted) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);if(rc)return rc;
    s32 n=0;for(u32 i=0;i<vm_base[h+0x87];i++)n+=matches(type(h+0x88+48*i),wanted);return n;
}
s32 cellPamfReaderSetStreamWithIndex(CellPamfReader*p,u32 index) {
    u32 r=EA(p),h,s;s32 rc=reader(r,&h,&s);if(rc)return rc;
    if(index>=vm_base[h+0x87])return FAIL_ARG;
    vm_write32(r+0x20,index);vm_write32(r+0x34,h+0x88+48*index);return CELL_OK;
}
s32 cellPamfReaderSetStreamWithType(CellPamfReader*p,u8 wanted,u32 index) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);if(rc)return rc;
    for(u32 i=0;i<vm_base[h+0x87];i++)if(matches(type(h+0x88+48*i),wanted) && index--==0)
        return cellPamfReaderSetStreamWithIndex(p,i);
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}
/* SDK export used by retail: return the absolute selected stream index. */
s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader*p,u8 wanted,u8 index) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);if(rc)return rc;
    if(index>=vm_base[h+0x87])return FAIL_ARG;
    u32 found=0;
    for(u32 i=0;i<vm_base[h+0x87];i++)if(matches(type(h+0x88+48*i),wanted) && found++==index) {
        rc=cellPamfReaderSetStreamWithIndex(p,i);return rc?rc:(s32)i;
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}
s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader*p,u8 wanted,u32 channel) {
    /* LLE's channel-selector quirks are not implemented; never silently select a different stream. */
    (void)p;(void)wanted;(void)channel;return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
}
s32 cellPamfReaderGetStreamIndex(CellPamfReader*p) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);return rc?rc:(s32)vm_read32(EA(p)+0x20);
}
s32 cellPamfReaderGetStreamInfo(CellPamfReader*p,void*out,u32 size) {
    u32 h,s,o=EA(out);s32 rc=reader(EA(p),&h,&s);if(rc)return rc;
    s32 t=type(s);u32 n=t==0?32:t==1?24:(t>=2&&t<=4)?8:0;
    if(!n)return FAIL_ARG;
    if(size<n || !range(o,n))return FAIL_ARG;
    u8 b[32]={0};u32 d=s+16;
    if(t<=1) {
        b[0]=t==0?vm_base[d]:(vm_base[d]==0x44?3:vm_base[d]==0x48?1:255);
        if(t==0) {b[1]=vm_base[d+1];b[2]=vm_base[d+2]>>7;b[3]=(vm_base[d+2]>>6)&1;b[4]=(vm_base[d+2]&15)-1;b[5]=vm_base[d+3];}
        else {b[1]=vm_base[d+2]>>7;b[2]=(vm_base[d+2]>>6)&1;b[3]=vm_base[d+2]&15;b[4]=vm_base[d+3];}
        if(vm_base[d+3]==255)memcpy(b+6,vm_base+d+4,4);
        u16 w=vm_base[d+9]*16,ht=vm_base[d+11]*16;
        b[10]=w>>8;b[11]=w;b[12]=ht>>8;b[13]=ht;
        memcpy(b+14,vm_base+d+12,t==0?8:4);
        u32 q=t==0?22:18;
        if(vm_base[d+2]&0x40) {b[q]=vm_base[d+20]>>5;b[q+1]=(vm_base[d+20]>>4)&1;memcpy(b+q+2,vm_base+d+21,3);}
        if(t==0) {b[27]=vm_base[d+24]>>7;b[28]=(vm_base[d+24]>>6)&1;b[29]=(vm_base[d+24]>>4)&3;b[30]=vm_base[d+24]&3;b[31]=vm_base[d+25];}
    } else {b[3]=vm_base[d+3]&15;b[4]=vm_base[d+2]&15;if(t==3)b[7]=vm_base[d+4]>>6;}
    memcpy(vm_base+o,b,n);return CELL_OK;
}
s32 cellPamfStreamTypeToEsFilterId(u8 t,u8 ch,void*out) {
    u32 o=EA(out);if(!range(o,16)||t>5||ch>15)return FAIL_ARG;
    u32 major=t<=1?0xe0|ch:0xbd,minor=t<=1?0:ch|(t==3?0x40:t==4?0x30:t==5?0x20:0);
    vm_write32(o,major);vm_write32(o+4,minor);vm_write32(o+8,t==0);vm_write32(o+12,0);return CELL_OK;
}
u32 cellPamfReaderGetNumberOfEp(CellPamfReader*p) {
    u32 h,s;s32 rc=reader(EA(p),&h,&s);return rc?(u32)rc:vm_read32(s+12);
}
s32 cellPamfReaderGetEp(CellPamfReader*p,u32 index,CellPamfEp*out) {
    /* Legacy non-SDK entry point: no fabricated EPs from the old invented layout. */
    (void)p;(void)index;(void)out;return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
}
