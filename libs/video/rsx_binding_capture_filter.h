/* Diagnostic-only consumer selector. No rendering state is changed. */
#ifndef RSX_BINDING_CAPTURE_FILTER_H
#define RSX_BINDING_CAPTURE_FILTER_H
#include "ps3emu/ps3types.h"
#include <string.h>
#define RSX_BINDING_CAPTURE_MAX 8u
typedef struct { u64 fp_hash; u32 control; int active; } rsx_binding_capture_filter;
/* Strict bounded hexadecimal fields; signs, prefixes and whitespace are errors. */
static inline int rsx_binding_capture_hex(const char** cursor, unsigned digits, u64* out)
{
    const char* p=*cursor; u64 v=0; unsigned n=0;
    while ((*p>='0' && *p<='9') || (*p>='a' && *p<='f') || (*p>='A' && *p<='F')) {
        if (++n>digits) return 0;
        unsigned d=*p<='9' ? (unsigned)(*p-'0') : *p<='F' ? (unsigned)(*p-'A'+10) : (unsigned)(*p-'a'+10);
        v=(v<<4)|d; ++p;
    }
    if (!n) return 0;
    *cursor=p; *out=v; return 1;
}
/* NULL/empty means legacy behavior; otherwise one hash:control or * per offset.
 * Return active count, or -1 for malformed configuration. Parse transactionally. */
static inline int rsx_binding_capture_parse(const char* text, u32 count,
    rsx_binding_capture_filter out[RSX_BINDING_CAPTURE_MAX])
{
    rsx_binding_capture_filter tmp[RSX_BINDING_CAPTURE_MAX]={{0}};
    if (!count || count>RSX_BINDING_CAPTURE_MAX) return -1;
    if (!text || !*text) { memcpy(out,tmp,sizeof tmp); return 0; }
    unsigned size=0; while (text[size] && size<256) ++size;
    if (size==256) return -1;
    const char* p=text; int active=0;
    for (u32 i=0;i<count;++i) {
        if (*p=='*') ++p;
        else {
            u64 hash,control;
            if (!rsx_binding_capture_hex(&p,16,&hash) || !hash || *p++!=':' ||
                !rsx_binding_capture_hex(&p,8,&control)) return -1;
            tmp[i].fp_hash=hash; tmp[i].control=(u32)control; tmp[i].active=1; ++active;
        }
        if (i+1<count) { if (*p++!=',') return -1; }
        else if (*p) return -1;
    }
    memcpy(out,tmp,sizeof tmp); return active;
}
static inline int rsx_binding_capture_matches(const rsx_binding_capture_filter* f,
    int valid_program, u64 hash, u32 control)
{
    return !f->active || (valid_program && f->fp_hash==hash && f->control==control);
}
#endif
