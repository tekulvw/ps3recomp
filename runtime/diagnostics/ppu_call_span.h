/* Default-off selected-dispatch span. No guest-state mutation or dereference of
 * register arguments. Complete only after the caller has drained trampolines. */
#ifndef PS3RECOMP_PPU_CALL_SPAN_H
#define PS3RECOMP_PPU_CALL_SPAN_H
#include "audio_chronology.h"
class ppu_call_span_scope {
    audio_chronology_call_token token_; /* begin initializes serial before admission */
    static unsigned& depth() { static thread_local unsigned value=0;return value; }
    void finish(unsigned abnormal) noexcept {
        if(token_.serial) { --depth();audio_chronology_call_end(&token_,abnormal); }
    }
public:
    ppu_call_span_scope(const void* context,uint32_t target) {
        audio_chronology_call_begin(&token_,context,target,depth());
        if(token_.serial)++depth();
    }
    ~ppu_call_span_scope() noexcept { finish(1); }
    void complete() noexcept { finish(0); }
    ppu_call_span_scope(const ppu_call_span_scope&)=delete;
    ppu_call_span_scope& operator=(const ppu_call_span_scope&)=delete;
};
#endif
