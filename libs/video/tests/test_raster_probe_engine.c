/* Neutral public-engine diagnostic controls; no title addresses or programs. */
#define main retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include <unistd.h>
static rsx_be_render_state states[16], bound;
static u32 handles[16], state_count;
static unsigned checks, fails;
static int use_indexed;
#define T(x,s) do {checks++;if(!(x)){fails++;fprintf(stderr,"FAIL %s\n",s);}}while(0)
static u32 make(void*u,const char*v,const char*p,const rsx_be_render_state*r,const rsx_vertex_layout_plan*l,u32 s,rsx_be_format f,u32 n){u32 h=stub_pipeline_create(u,v,p,r,l,s,f,n);if(h&&state_count<16){states[state_count]=*r;handles[state_count++]=h;}return h;}
static void bind(void*u,u32 h){stub_bind_pipeline(u,h);for(u32 i=0;i<state_count;i++)if(handles[i]==h)bound=states[i];}
static void check_draw(u32 expected){rsx_be_render_state original;rsx_draw_engine_decode_render_state(&g.rsx,&original);if(use_indexed){const u8 indices[6]={0,0,0,1,0,2};memcpy(vm_base+0x30000,indices,6);m(0x181C,0x30000);m(0x1820,0x10);m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);m(0x1824,2u<<24);m(M_BEGIN_END,0);}else draw_triangle();T(bound.cull_enable==expected,"selected cull state reaches actual public backend pipeline");original.cull_enable=expected;T(!memcmp(&bound,&original,sizeof original),"all other render state bytes unchanged");}
int main(int argc,char**argv){if(argc!=2)return 2;vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;g_stub_backend.pipeline_create=make;g_stub_backend.bind_pipeline=bind;engine_up();m(M_DEPTH_TEST,1);m(M_DEPTH_WRITE,1);m(0x0A6C,0x203);m(0x183C,1);m(0x1830,0x405);m(0x1834,0x901);m(0x0328,1);m(0x033C,0x7F);m(0x02F8,0x01010101);
 const char*kind=argv[1];
 use_indexed=strstr(kind,"-indexed")!=NULL;
 if(!strncmp(kind,"b-",2)){m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_1);m(M_CONTEXT_DMA_COLOR_B,DMA_LOCAL);m(M_COLOR_B_OFFSET,0x50000);if(!strcmp(kind,"b-match")){m(M_COLOR_A_OFFSET,0x50000);m(M_COLOR_B_OFFSET,0x40000);}if(!strcmp(kind,"b-location")){m(M_CONTEXT_DMA_COLOR_B,DMA_MAIN);m(M_COLOR_B_OFFSET,0x40000);}}
 if(!strcmp(kind,"location"))m(M_CONTEXT_DMA_COLOR_A,DMA_MAIN);
 if(!strcmp(kind,"offset"))m(M_COLOR_A_OFFSET,0x50000);
 if(!strcmp(kind,"depth-readonly"))m(M_DEPTH_WRITE,0);
 if(!strcmp(kind,"depth-disabled"))m(M_DEPTH_TEST,0);
 if(!strcmp(kind,"mask-zero"))m(0x0324,0);
 if(!strcmp(kind,"transition")){
  check_draw(1);const char*p=getenv("RSX_ENGINE_RASTER_PROBE_TRIGGER");FILE*f=p?fopen(p,"w"):NULL;T(f!=NULL,"fixture arms its own private trigger");if(f)fclose(f);check_draw(0);check_draw(0);rsx_draw_engine_present();check_draw(1);T(state_count==2,"frame-window switchback reuses original cached pipeline");if(p)unlink(p);
 }else if(!strcmp(kind,"frames-two")){
  check_draw(0);check_draw(0);rsx_draw_engine_present();check_draw(0);check_draw(0);rsx_draw_engine_present();check_draw(1);T(state_count==2,"two full frames then original pipeline reused");
 }else check_draw(!strncmp(kind,"match",5)||!strcmp(kind,"b-match")?0:1);
 T(stub.n_draw==(!strcmp(kind,"transition")?4:!strcmp(kind,"frames-two")?5:1),"all requested draws emitted");engine_down();free(vm_base);printf("raster probe checks=%u failures=%u\n",checks,fails);return fails?1:0;}
