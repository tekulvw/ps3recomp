#include "../rsx_metal_backend.m"
float cellAvconfExt_display_gamma(void){return 1.0f;}   /* identity present gamma */
#include <assert.h>

static void expect_file(const char* path, u32 pixels, const u8 rgba[4])
{
    FILE* f=fopen(path,"rb"); assert(f);
    for (u32 i=0;i<pixels;++i) { u8 actual[4]; assert(fread(actual,1,4,f)==4); assert(!memcmp(actual,rgba,4)); }
    assert(fgetc(f)==EOF); fclose(f);
}
int main(int argc, char** argv)
{
    assert(argc==2);
    @autoreleasepool {
        s_dev=MTLCreateSystemDefaultDevice(); assert(s_dev);
        s_queue=[s_dev newCommandQueue]; assert(s_queue);
        assert(!eng_init(NULL,4,4));
        u32 surface=eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,2,1,NULL,0); assert(surface);
        const float red[4]={1,0,0,1};
        eng_clear_color(NULL,surface,red);
        assert(s_eng_rec_count==1);
        assert(eng_capture_texture_rgba8(NULL,surface,argv[1]));
        assert(s_eng_rec_count==0);
        const u8 want_red[4]={255,0,0,255}; expect_file(argv[1],2,want_red);
        /* Nonuniform rows prove the helper preserves texture coordinates. */
        const u8 corners[16]={17,23,31,255, 41,43,47,255,
                              53,59,61,255, 67,71,73,255};
        u32 image=eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,2,2,1,1,0xAAE4,0xA5); assert(image);
        eng_texture_upload(NULL,image,0,0,2,2,corners,8,2);
        assert(eng_capture_texture_rgba8(NULL,image,argv[1]));
        FILE* f=fopen(argv[1],"rb"); assert(f);
        u8 actual[16]; assert(fread(actual,1,16,f)==16 && !memcmp(actual,corners,16)); fclose(f);
        /* A real BC1 GPU decode; endpoints red565 followed by all index0. */
        const u8 block[8]={0,248,0,0,0,0,0,0};
        u32 bc=eng_texture_create(NULL,RSX_BE_FMT_BC1,4,4,1,1,0xAAE4,0xA6); assert(bc);
        eng_texture_upload(NULL,bc,0,0,4,4,block,8,1);
        assert(eng_capture_texture_rgba8(NULL,bc,argv[1])); expect_file(argv[1],16,want_red);
        /* The resource's real crossbar must be included in captured samples. */
        const u8 glyph=173;
        u32 b8=eng_texture_create(NULL,RSX_BE_FMT_R8,1,1,1,1,0x02FF,0xA1); assert(b8);
        eng_texture_upload(NULL,b8,0,0,1,1,&glyph,1,1);
        assert(eng_capture_texture_rgba8(NULL,b8,argv[1]));
        const u8 want_glyph[4]={0,0,0,173}; expect_file(argv[1],1,want_glyph);
        assert(!eng_capture_texture_rgba8(NULL,0,argv[1]));
        eng_shutdown(NULL);
        puts("PASS: synchronized preceding clear, exact pixel coordinates, BC1 GPU decode and selected B8 remap");
    }
    return 0;
}
