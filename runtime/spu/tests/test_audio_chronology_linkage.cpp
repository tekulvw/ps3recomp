/* Public C ABI and implementation header are usable from C++17. */
#include "../../diagnostics/audio_chronology_impl.h"
#include <cassert>
#ifndef _WIN32
extern "C" void SetLastError(DWORD value) { (void)value; }
#endif
int main() {
    audio_chronology_dma_token token;
    errno=EDOM;
    audio_chronology_dma_begin(&token,0x10000,32,0x20);
    assert(token.port==-1);
    audio_chronology_dma_after(&token,reinterpret_cast<const uint8_t*>(1));
    audio_chronology_dma_end(&token,reinterpret_cast<const void*>(1),0,0,0,0,0,0);
    assert(errno==EDOM);
    return 0;
}
