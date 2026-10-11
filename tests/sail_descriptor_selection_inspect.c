/* Test-only inspection translation unit. Calls still use real generated HLE. */
#include "../libs/codec/cellSail.c"
int inspect_selection(u32 desc,u32 category,u8* reader,u8* metadata) {
    if(desc>=MAX_DESCRIPTORS || category>1 || !s_descs[desc].in_use) return 0;
    if(!s_descs[desc].selections[category].valid) return 0;
    memcpy(reader,s_descs[desc].selections[category].reader,128);
    memcpy(metadata,s_descs[desc].selections[category].stream,48);
    return 1;
}
