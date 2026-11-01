/* Actual Windows cellPad + guest BE packets. Host focus/controller/time are
 * deterministic providers; this fixture creates no windows or real input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
static ULONGLONG fixture_ms=1000;
static int fixture_focus=2;
static int fixture_connected=1;
static unsigned focus_queries,file_opens;
static WORD physical_buttons;
static int physical_axes;
static int fixture_osk_active;
int osk_native_win32_input_active(void){return fixture_osk_active;}
static ULONGLONG WINAPI fixture_ticks(void){return fixture_ms;}
static HWND WINAPI fixture_foreground(void){++focus_queries;return fixture_focus?(HWND)(uintptr_t)1:NULL;}
static DWORD WINAPI fixture_window_pid(HWND window,DWORD* pid){(void)window;*pid=GetCurrentProcessId()+(fixture_focus==2);return 1;}
static DWORD WINAPI fixture_xinput(DWORD port,XINPUT_STATE* state){
    ZeroMemory(state,sizeof(*state));state->Gamepad.wButtons=physical_buttons;
    if(physical_axes){state->Gamepad.sThumbLX=16384;state->Gamepad.sThumbLY=-16384;state->Gamepad.sThumbRX=24576;state->Gamepad.sThumbRY=-24576;}
    return port||!fixture_connected?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS;
}
static FILE* fixture_fopen(const char* path,const char* mode){++file_opens;return fopen(path,mode);}
static HANDLE WINAPI fixture_open(LPCSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD create,DWORD flags,HANDLE model){
    ++file_opens;return CreateFileA(path,access,share,security,create,flags,model);
}
#define GetTickCount64 fixture_ticks
#define GetForegroundWindow fixture_foreground
#define GetWindowThreadProcessId fixture_window_pid
#define XInputGetState fixture_xinput
#define fopen fixture_fopen
#define CreateFileA fixture_open
#include "../cellPad.c"
#undef GetTickCount64
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef XInputGetState
#undef fopen
#undef CreateFileA
#include <assert.h>
#include <sys/stat.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t a){(void)a;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
u32 cellGcm_flip_request_count(void){return 0;}
static const char* mailbox;
static void publish(const char* text){FILE*f=fopen(mailbox,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static long queued(void){FILE*f=fopen(mailbox,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(!fclose(f));return n;}
static int report(int advance){if(advance)fixture_ms+=4;assert(cellPadGetData(0,(CellPadData*)(uintptr_t)0x100)==CELL_OK);return (int32_t)vm_read32(0x100);}
static unsigned buttons(void){return vm_read16(0x108)|(vm_read16(0x10A)<<8);}
static void neutral_payload(void){
    assert(!buttons());
    for(unsigned i=4;i<=7;i++)assert(vm_read16(0x104+2*i)==128);
    for(unsigned i=8;i<=19;i++)assert(!vm_read16(0x104+2*i));
}
int main(int argc,char**argv){
    assert(argc==3);const char*mode=argv[1];mailbox=argv[2];
    static uint8_t memory[65536];vm_base=memory;ppu_vm_size=sizeof memory;
    assert(!_putenv_s("PAD_FILE",strcmp(mode,"no-file")?mailbox:""));
    assert(!_putenv_s("PAD_FILE_REQUIRE_FOCUS",!strcmp(mode,"legacy")?"":!strcmp(mode,"zero")?"0":"1"));
    if(!strcmp(mode,"no-controller")){fixture_connected=0;assert(!_putenv_s("PAD_NO_KEYBOARD","1"));}
    if(!strcmp(mode,"long-path")){char path[600];memset(path,'x',sizeof(path)-1);path[sizeof(path)-1]=0;assert(!_putenv_s("PAD_FILE",path));}
    assert(cellPadInit(7)==CELL_OK);publish("0x4000 3\n");
    if(!strcmp(mode,"osk-owner")){
        assert(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_OK);
        fixture_focus=1;assert(report(0)>0&&buttons()==CELL_PAD_CTRL_CROSS&&!queued());
        unsigned header=vm_read16(0x106);
        publish("0x0008 4\n");fixture_osk_active=1;physical_buttons=XINPUT_GAMEPAD_B;physical_axes=1;
        assert(!report(0)&&!buttons()&&!queued());
        assert(report(1)>0);neutral_payload();assert(vm_read16(0x106)==header);
        assert(s_host_state[0].buttons==CELL_PAD_CTRL_CIRCLE && s_host_state[0].analog_lx!=128);
        fixture_osk_active=0;assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);
        assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)!=128);
        assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CIRCLE)==255);
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);
        publish("0x4000 2\n");assert(report(1)>0&&buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
        assert(report(1)>0&&buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);
    }else if(!strcmp(mode,"long-path")){
        assert(report(0)>0&&!buttons()&&queued()>0&&!file_opens);
    }else if(!strcmp(mode,"late-unfocused")){
        assert(report(0)>0&&!buttons()&&!queued());
        publish("0x4000 2\n");fixture_focus=1;
        assert(report(1)>0&&!buttons()&&!queued());
        publish("0x4000 2\n");assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
    }else if(!strcmp(mode,"locked-admission")){
        fixture_focus=1;
        HANDLE locked=CreateFileA(mailbox,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(locked!=INVALID_HANDLE_VALUE);
        assert(report(0)>0&&!buttons()&&queued()>0);
        assert(report(1)>0&&!buttons()&&queued()>0);
        assert(CloseHandle(locked));assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS&&!queued());
    }else if(!strcmp(mode,"blocked-discard")){
        HANDLE locked=CreateFileA(mailbox,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(locked!=INVALID_HANDLE_VALUE);
        assert(report(0)>0&&!buttons()&&queued()>0);
        fixture_focus=1;assert(report(1)>0&&!buttons()&&queued()>0);
        assert(CloseHandle(locked));assert(report(1)>0&&!buttons()&&!queued());
        publish("0x4000 2\n");assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);assert(report(1)>0&&!buttons());
    }else if(!strcmp(mode,"legacy")||!strcmp(mode,"zero")){
        assert(report(0)>0&&buttons()==CELL_PAD_CTRL_CROSS&&!queued());
        for(int i=0;i<100;++i)assert(!report(0));
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(report(1)>0&&!buttons());assert(!focus_queries);
    }else if(!strcmp(mode,"no-file")){
        assert(report(0)>0&&!buttons()&&queued()>0&&!focus_queries);
    }else{
        // Even an initial foreign foreground discards pending input.
        assert(report(0)>0&&!buttons()&&!queued());
        fixture_focus=1;assert(report(1)>0&&!buttons());
        publish("0x4000 3\n");assert(!report(0)&&!queued());
        for(int i=0;i<100;++i)assert(!report(0));
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(!report(0));assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        assert(report(1)>0&&!buttons());
        // Loss on a len0 drain cancels held input and a queued successor.
        publish("0x4000 4\n");assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CROSS);
        publish("0x0008 2\n");fixture_focus=2;
        assert(!report(0)&&!buttons()&&!queued());
        assert(report(1)>0&&!buttons());
        fixture_focus=1;assert(report(1)>0&&!buttons());
        // No foreground also cancels, and never resumes stale work.
        publish("0x4000 2\n");fixture_focus=0;assert(report(1)>0&&!buttons()&&!queued());
        fixture_focus=1;assert(report(1)>0&&!buttons());
        assert(!remove(mailbox));fixture_focus=0;assert(report(1)>0&&!buttons());
        assert(GetFileAttributesA(mailbox)==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND);
        fixture_focus=1;assert(report(1)>0&&!buttons());
        // The optional gate affects only mailbox input; real XInput survives.
        fixture_connected=1;physical_buttons=XINPUT_GAMEPAD_B;fixture_focus=2;publish("0x4000 2\n");
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE&&!queued());
        fixture_focus=1;assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);
        publish("0x4000 2\n");
        assert(report(1)>0&&buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
        assert(report(1)>0&&buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
        assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);
        assert(focus_queries>0);
    }
    assert(cellPadEnd()==CELL_OK);puts("PASS Windows PAD_FILE focus/packet contract");return 0;
}
