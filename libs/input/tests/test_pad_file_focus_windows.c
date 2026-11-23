/* Actual Windows cellPad + guest BE packets. Host focus/controller/time and
 * the keyboard (held virtual keys and the foreground layout) are deterministic
 * providers; this fixture creates no windows or real input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
static ULONGLONG fixture_ms=1000;
static int fixture_focus=2;
static int fixture_connected=1;
static unsigned focus_queries,file_opens;
static WORD physical_buttons;
static int physical_axes;
static int fixture_osk_active;
static const char* mailbox;
static int fixture_reparse;
static char fixture_target[MAX_PATH];
static HANDLE fixture_reparse_handle=INVALID_HANDLE_VALUE;
int osk_native_win32_input_active(void){return fixture_osk_active;}
static ULONGLONG WINAPI fixture_ticks(void){return fixture_ms;}
static HWND WINAPI fixture_foreground(void){++focus_queries;return fixture_focus?(HWND)(uintptr_t)1:NULL;}
static DWORD WINAPI fixture_window_pid(HWND window,DWORD* pid){(void)window;*pid=GetCurrentProcessId()+(fixture_focus==2);return 1;}
static DWORD WINAPI fixture_xinput(DWORD port,XINPUT_STATE* state){
    ZeroMemory(state,sizeof(*state));state->Gamepad.wButtons=physical_buttons;
    if(physical_axes){state->Gamepad.sThumbLX=16384;state->Gamepad.sThumbLY=-16384;state->Gamepad.sThumbRX=24576;state->Gamepad.sThumbRY=-24576;}
    return port||!fixture_connected?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS;
}
/* Keyboard: held virtual keys, and the foreground layout (1 = US QWERTY,
 * 2 = French AZERTY, whose letter positions differ). */
static unsigned char fixture_vk_held[256];
static uintptr_t fixture_layout=1;
static unsigned layout_queries;
static SHORT WINAPI fixture_async_key(int vk){return (SHORT)(vk>=0&&vk<256&&fixture_vk_held[vk]?0x8000:0);}
static HKL WINAPI fixture_keyboard_layout(DWORD thread){(void)thread;++layout_queries;return (HKL)fixture_layout;}
static UINT WINAPI fixture_map_key(UINT code,UINT type,HKL layout){
    static const struct{UINT scan;char us;}letters[]={{0x1E,'A'},{0x30,'B'},{0x2E,'C'},{0x20,'D'},{0x12,'E'},{0x21,'F'},
        {0x22,'G'},{0x23,'H'},{0x17,'I'},{0x24,'J'},{0x25,'K'},{0x26,'L'},{0x32,'M'},{0x31,'N'},{0x18,'O'},{0x19,'P'},
        {0x10,'Q'},{0x13,'R'},{0x1F,'S'},{0x14,'T'},{0x16,'U'},{0x2F,'V'},{0x11,'W'},{0x2D,'X'},{0x15,'Y'},{0x2C,'Z'}};
    assert(type==MAPVK_VSC_TO_VK);
    for(unsigned i=0;i<sizeof letters/sizeof letters[0];i++)if(letters[i].scan==code){
        char c=letters[i].us;
        if((uintptr_t)layout==2)c=c=='A'?'Q':c=='Q'?'A':c=='Z'?'W':c=='W'?'Z':c;
        return (UINT)c;
    }
    return 0;
}
static FILE* fixture_fopen(const char* path,const char* mode){++file_opens;return fopen(path,mode);}
static HANDLE WINAPI fixture_open(LPCSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD create,DWORD flags,HANDLE model){
    ++file_opens;
    const int mailbox_open=fixture_reparse&&mailbox&&!strcmp(path,mailbox);
    const int nofollow=(flags&FILE_FLAG_OPEN_REPARSE_POINT)!=0;
    HANDLE file=CreateFileA(mailbox_open&&!nofollow?fixture_target:path,access,share,security,create,flags,model);
    fixture_reparse_handle=mailbox_open&&nofollow?file:INVALID_HANDLE_VALUE;
    return file;
}
static BOOL WINAPI fixture_information(HANDLE file,LPBY_HANDLE_FILE_INFORMATION info){
    BOOL result=GetFileInformationByHandle(file,info);
    if(result&&fixture_reparse&&file==fixture_reparse_handle)info->dwFileAttributes|=FILE_ATTRIBUTE_REPARSE_POINT;
    return result;
}
#define GetTickCount64 fixture_ticks
#define GetForegroundWindow fixture_foreground
#define GetWindowThreadProcessId fixture_window_pid
#define XInputGetState fixture_xinput
#define fopen fixture_fopen
#define CreateFileA fixture_open
#define GetFileInformationByHandle fixture_information
#define GetAsyncKeyState fixture_async_key
#define GetKeyboardLayout fixture_keyboard_layout
#define MapVirtualKeyExW fixture_map_key
#include "../cellPad.c"
#undef GetTickCount64
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef XInputGetState
#undef fopen
#undef CreateFileA
#undef GetFileInformationByHandle
#undef GetAsyncKeyState
#undef GetKeyboardLayout
#undef MapVirtualKeyExW
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
static void publish(const char* text){FILE*f=fopen(mailbox,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static long queued(void){FILE*f=fopen(mailbox,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(!fclose(f));return n;}
static int report(int advance){if(advance)fixture_ms+=4;assert(cellPadGetData(0,(CellPadData*)(uintptr_t)0x100)==CELL_OK);return (int32_t)vm_read32(0x100);}
static unsigned buttons(void){return vm_read16(0x108)|(vm_read16(0x10A)<<8);}
static unsigned axis(unsigned i){static const unsigned offsets[]={6,7,4,5};return vm_read16(0x104+2*offsets[i]);}
static void axes(unsigned a,unsigned b,unsigned c,unsigned d){assert(axis(0)==a&&axis(1)==b&&axis(2)==c&&axis(3)==d);}
static void analog(unsigned sequence,unsigned reports,unsigned deadline,unsigned a,unsigned b,unsigned c,unsigned d){
    char line[256];snprintf(line,sizeof line,"A1 owned %u %u %u %u %u %u %u\n",sequence,reports,deadline,a,b,c,d);publish(line);
}
static void ack(unsigned sequence,unsigned delivered,const char*status){
    char path[1024],line[512],field[64];snprintf(path,sizeof path,"%s.ack",mailbox);
    FILE*f=fopen(path,"rb");assert(f&&fgets(line,sizeof line,f));assert(!fclose(f));
    snprintf(field,sizeof field,"\"sequence\":%u,",sequence);assert(strstr(line,field));
    snprintf(field,sizeof field,"\"delivered\":%u,",delivered);assert(strstr(line,field));
    snprintf(field,sizeof field,"\"status\":\"%s\"",status);assert(strstr(line,field));
    assert(strstr(line,"\"nonce\":\"owned\"")&&strstr(line,"\"neutral_delivered\":true"));
}
static void no_ack(void){char path[1024];snprintf(path,sizeof path,"%s.ack",mailbox);assert(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES);}
static void reparse_target(const char*command){
    snprintf(fixture_target,sizeof fixture_target,"%s.target",mailbox);
    FILE*f=fopen(fixture_target,"wb");assert(f&&fputs(command,f)>=0);assert(!fclose(f));fixture_reparse=1;
}
static void target_preserved(const char*command){
    char text[256];FILE*f=fopen(fixture_target,"rb");assert(f&&fgets(text,sizeof text,f));assert(!fclose(f));assert(!strcmp(text,command));
}
static void neutral_payload(void){
    assert(!buttons());
    for(unsigned i=4;i<=7;i++)assert(vm_read16(0x104+2*i)==128);
    for(unsigned i=8;i<=19;i++)assert(!vm_read16(0x104+2*i));
}
/* Keyboard pad: the shared Linux layout through real Windows cellPad reports. */
static void hold(int vk,int down){fixture_vk_held[vk&255]=(unsigned char)(down!=0);}
static void key_report(const char*what,unsigned want,unsigned lx,unsigned ly,unsigned rx,unsigned ry){
    assert(report(1)>0);
    if(buttons()!=want||axis(0)!=lx||axis(1)!=ly||axis(2)!=rx||axis(3)!=ry){
        printf("FAIL %s: buttons 0x%04X (want 0x%04X) axes %u %u %u %u (want %u %u %u %u)\n",what,buttons(),want,
               axis(0),axis(1),axis(2),axis(3),lx,ly,rx,ry);fflush(stdout);
        assert(0);
    }
}
static void keyboard_layout_checks(void){
    static const struct{int vk;unsigned button;}buttons_map[]={
        {'X',CELL_PAD_CTRL_CROSS},{'Z',CELL_PAD_CTRL_SQUARE},{'C',CELL_PAD_CTRL_CIRCLE},{'V',CELL_PAD_CTRL_TRIANGLE},
        {VK_RETURN,CELL_PAD_CTRL_START},{VK_SPACE,CELL_PAD_CTRL_SELECT},
        {'Q',CELL_PAD_CTRL_L1},{'E',CELL_PAD_CTRL_R1},{'R',CELL_PAD_CTRL_L2},{'T',CELL_PAD_CTRL_R2},
        {'F',CELL_PAD_CTRL_L3},{'G',CELL_PAD_CTRL_R3},
        {VK_UP,CELL_PAD_CTRL_UP},{VK_DOWN,CELL_PAD_CTRL_DOWN},{VK_LEFT,CELL_PAD_CTRL_LEFT},{VK_RIGHT,CELL_PAD_CTRL_RIGHT}};
    for(unsigned i=0;i<sizeof buttons_map/sizeof buttons_map[0];i++){
        hold(buttons_map[i].vk,1);key_report("button",buttons_map[i].button,128,128,128,128);
        hold(buttons_map[i].vk,0);key_report("button release",0,128,128,128,128);
    }
    static const struct{int vk;unsigned lx,ly,rx,ry;}sticks[]={
        {'W',128,0,128,128},{'S',128,255,128,128},{'A',0,128,128,128},{'D',255,128,128,128},
        {'I',128,128,128,0},{'K',128,128,128,255},{'J',128,128,0,128},{'L',128,128,255,128}};
    for(unsigned i=0;i<sizeof sticks/sizeof sticks[0];i++){
        hold(sticks[i].vk,1);key_report("stick",0,sticks[i].lx,sticks[i].ly,sticks[i].rx,sticks[i].ry);
        hold(sticks[i].vk,0);key_report("stick release",0,128,128,128,128);
    }
    /* Both sticks at once, diagonals, opposing keys cancel and release restores. */
    hold('W',1);hold('D',1);hold('I',1);hold('L',1);key_report("two sticks",0,255,0,255,0);
    hold('S',1);hold('J',1);key_report("opposing",0,255,128,128,0);
    hold('W',0);hold('L',0);key_report("opposing release",0,255,255,0,0);
    hold('S',0);hold('D',0);hold('I',0);hold('J',0);key_report("sticks released",0,128,128,128,128);
    /* Several buttons, and the old Windows-only keys no longer do anything. */
    hold('X',1);hold('C',1);hold('Q',1);key_report("chord",CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_L1,128,128,128,128);
    hold('X',0);hold('C',0);hold('Q',0);
    hold('1',1);hold('2',1);hold(VK_TAB,1);key_report("retired keys",0,128,128,128,128);
    hold('1',0);hold('2',0);hold(VK_TAB,0);
    /* Arrow keys drive only the d-pad. */
    hold(VK_UP,1);hold(VK_LEFT,1);key_report("arrows",CELL_PAD_CTRL_UP|CELL_PAD_CTRL_LEFT,128,128,128,128);
    hold(VK_UP,0);hold(VK_LEFT,0);key_report("arrows released",0,128,128,128,128);
}
static void keyboard_mode(const char*mode){
    if(!strcmp(mode,"keyboard-off")){
        fixture_focus=1;hold('X',1);hold('W',1);assert(report(1)>=0);
        assert(!buttons());assert(!s_host_state[0].connected);return;
    }
    if(!strcmp(mode,"keyboard-azerty")){
        fixture_layout=2;fixture_focus=1;
        /* Physical positions: W's key types Z, Z's types W, A's types Q, Q's types A. */
        hold('Z',1);key_report("azerty W position",0,128,0,128,128);hold('Z',0);
        hold('W',1);key_report("azerty Z position",CELL_PAD_CTRL_SQUARE,128,128,128,128);hold('W',0);
        hold('Q',1);key_report("azerty A position",0,0,128,128,128);hold('Q',0);
        hold('A',1);key_report("azerty Q position",CELL_PAD_CTRL_L1,128,128,128,128);hold('A',0);
        hold('X',1);key_report("azerty X",CELL_PAD_CTRL_CROSS,128,128,128,128);hold('X',0);
        /* Switching the foreground layout re-resolves the keys. */
        fixture_layout=1;hold('W',1);key_report("back to US",0,128,0,128,128);hold('W',0);
        key_report("released",0,128,128,128,128);assert(layout_queries>0);return;
    }
    if(!strcmp(mode,"keyboard-controller")){
        /* With a controller on port 0 the keyboard merges into it: buttons add,
         * and a stick follows its keys while one is held. */
        fixture_connected=1;fixture_focus=1;physical_buttons=XINPUT_GAMEPAD_B;
        key_report("controller alone",CELL_PAD_CTRL_CIRCLE,128,128,128,128);
        hold('X',1);key_report("keyboard button adds",CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_CROSS,128,128,128,128);
        physical_axes=1;assert(report(1)>0);
        const unsigned clx=axis(0),cly=axis(1),crx=axis(2),cry=axis(3);
        assert(clx!=128&&cly!=128&&crx!=128&&cry!=128);
        hold('W',1);key_report("held left stick follows the keys",CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_CROSS,128,0,crx,cry);
        hold('S',1);key_report("opposing keys still own the stick",CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_CROSS,128,128,crx,cry);
        hold('W',0);hold('S',0);hold('L',1);key_report("right stick keys",CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_CROSS,clx,cly,255,128);
        hold('L',0);hold('X',0);key_report("released: controller exactly",CELL_PAD_CTRL_CIRCLE,clx,cly,crx,cry);
        /* Pressure: a controller button keeps its reading, a keyboard button reads full. */
        assert(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_OK);
        hold('R',1);key_report("keyboard L2 with controller",CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_L2,clx,cly,crx,cry);
        assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_L2)==255&&vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CIRCLE)==255);
        hold('R',0);
        /* Another window in front: the keyboard adds nothing, the controller still plays. */
        hold('X',1);fixture_focus=2;key_report("unfocused keyboard, controller kept",CELL_PAD_CTRL_CIRCLE,clx,cly,crx,cry);
        fixture_focus=1;key_report("held across focus stays ignored",CELL_PAD_CTRL_CIRCLE,clx,cly,crx,cry);
        hold('X',0);physical_axes=0;
        /* Unplugging leaves the keyboard alone on the port. */
        fixture_connected=0;hold('V',1);
        key_report("unplugged",CELL_PAD_CTRL_TRIANGLE,128,128,128,128);hold('V',0);return;
    }
    if(!strcmp(mode,"keyboard-controller-off")){
        /* PAD_NO_KEYBOARD: a controller's report is untouched by held keys. */
        fixture_connected=1;fixture_focus=1;physical_buttons=XINPUT_GAMEPAD_B;hold('X',1);hold('W',1);
        key_report("keyboard off beside controller",CELL_PAD_CTRL_CIRCLE,128,128,128,128);return;
    }
    /* "keyboard": foreign foreground ignores keys and a key held across the
     * focus change stays ignored until released. */
    hold('X',1);key_report("foreign focus",0,128,128,128,128);
    fixture_focus=0;key_report("no foreground",0,128,128,128,128);
    fixture_focus=1;key_report("held across focus",0,128,128,128,128);
    hold('X',0);key_report("released",0,128,128,128,128);
    hold('X',1);key_report("fresh press",CELL_PAD_CTRL_CROSS,128,128,128,128);hold('X',0);
    keyboard_layout_checks();
    /* Focus loss releases everything; the native dialog owns input like focus. */
    hold('W',1);hold('X',1);key_report("held",CELL_PAD_CTRL_CROSS,128,0,128,128);
    fixture_focus=2;key_report("focus lost",0,128,128,128,128);
    fixture_focus=1;key_report("still held after focus",0,128,128,128,128);
    hold('W',0);hold('X',0);key_report("released",0,128,128,128,128);
    hold(VK_RETURN,1);fixture_osk_active=1;assert(report(1)>0);assert(!buttons());
    fixture_osk_active=0;key_report("Return that closed the dialog",0,128,128,128,128);
    hold(VK_RETURN,0);key_report("released",0,128,128,128,128);
    hold(VK_RETURN,1);key_report("fresh Return",CELL_PAD_CTRL_START,128,128,128,128);hold(VK_RETURN,0);
    /* Digital pressure for pressure-enabled reports. */
    assert(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_OK);
    hold('R',1);hold('X',1);key_report("pressure",CELL_PAD_CTRL_L2|CELL_PAD_CTRL_CROSS,128,128,128,128);
    assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_L2)==255&&vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255);
    assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CIRCLE)==0);
    hold('R',0);hold('X',0);key_report("released",0,128,128,128,128);
}
int main(int argc,char**argv){
    assert(argc==3);const char*mode=argv[1];mailbox=argv[2];
    static uint8_t memory[65536];vm_base=memory;ppu_vm_size=sizeof memory;
    const int is_keyboard=!strncmp(mode,"keyboard",8);
    assert(!_putenv_s("PAD_FILE",strcmp(mode,"no-file")&&!is_keyboard?mailbox:""));
    assert(!_putenv_s("PAD_FILE_REQUIRE_FOCUS",!strcmp(mode,"legacy")?"":(!strcmp(mode,"zero")||!strcmp(mode,"analog-no-focus"))?"0":"1"));
    const int is_analog=!strncmp(mode,"analog",6);
    assert(!_putenv_s("PAD_FILE_ANALOG_NONCE",is_analog&&strcmp(mode,"analog-off")?"owned":""));
    if(!strcmp(mode,"no-controller")||!strcmp(mode,"analog-no-controller")){fixture_connected=0;assert(!_putenv_s("PAD_NO_KEYBOARD","1"));}
    if(is_keyboard){fixture_connected=0;assert(!_putenv_s("PAD_NO_KEYBOARD",!strcmp(mode,"keyboard-off")||!strcmp(mode,"keyboard-controller-off")?"1":""));}
    /* The unfocused-mailbox modes count focus queries; the keyboard (which also
     * asks, beside a controller) stays out of that count. */
    if(!strcmp(mode,"legacy")||!strcmp(mode,"zero")||!strcmp(mode,"no-file"))assert(!_putenv_s("PAD_NO_KEYBOARD","1"));
    if(!strcmp(mode,"long-path")){char path[600];memset(path,'x',sizeof(path)-1);path[sizeof(path)-1]=0;assert(!_putenv_s("PAD_FILE",path));}
    assert(cellPadInit(7)==CELL_OK);publish("0x4000 3\n");
    if(is_keyboard){
        keyboard_mode(mode);
    }else if(is_analog){
        fixture_focus=1;
        if(!strcmp(mode,"analog-reparse")){
            const char*command="A1 owned 1 2 5000 0 255 17 240\n";reparse_target(command);
            fixture_focus=2;assert(report(0)>0);axes(128,128,128,128);target_preserved(command);no_ack();
            fixture_focus=1;assert(report(1)>0);axes(128,128,128,128);target_preserved(command);no_ack();
            fixture_reparse=0;analog(1,2,5000,0,255,17,240);assert(report(1)>0);assert(!queued());axes(128,128,128,128);
            analog(2,1,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(report(1)>0);axes(128,128,128,128);ack(2,1,"consumed");
        }else if(!strcmp(mode,"analog-off")||!strcmp(mode,"analog-no-focus")){
            analog(1,2,5000,0,255,17,240);assert(report(0)>0);axes(128,128,128,128);assert(queued()>0);no_ack();
        }else if(!strcmp(mode,"analog-invalid")){
            const char*bad[]={"A1 foreign 1 2 5000 0 255 17 240\n","A1 owned 0 2 5000 0 255 17 240\n",
                "A1 owned 1 121 5000 0 255 17 240\n","A1 owned 1 2 1000 0 255 17 240\n",
                "A1 owned 1 2 20000 0 255 17 240\n","A1 owned 1 2 5000 -1 255 17 240\n",
                "A1 owned 1 2 5000 256 255 17 240\n","A1 owned 1 2 5000 0 255 17\n",
                "A1 owned 1 2 5000 0 255 17 240 extra\n","A1 owned 1 0 5000 0 255 17 240\n"};
            for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){publish(bad[i]);assert(report(i!=0)>=0);axes(128,128,128,128);assert(queued()>0);no_ack();}
            char truncated[300];memset(truncated,' ',sizeof truncated);memcpy(truncated,"A1 owned 1 2 5000 0 255 17 240",28);
            FILE*f=fopen(mailbox,"wb");assert(f&&fwrite(truncated,1,sizeof truncated,f)==sizeof truncated);fclose(f);
            assert(report(1)>0);axes(128,128,128,128);assert(queued()==sizeof truncated);no_ack();
            f=fopen(mailbox,"wb");const char embedded[]="A1 owned 1 2 5000 0 255 17 240\0junk";assert(f&&fwrite(embedded,1,sizeof embedded,f)==sizeof embedded);fclose(f);
            assert(report(1)>0);axes(128,128,128,128);assert(queued()==sizeof embedded);no_ack();
            analog(1,1,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(report(1)>0);axes(128,128,128,128);ack(1,1,"consumed");
        }else if(!strcmp(mode,"analog-focus")||!strcmp(mode,"analog-osk")){
            analog(1,4,5000,0,255,17,240);assert(report(0)>0);axes(0,255,17,240);no_ack();
            analog(2,2,5000,200,128,128,128);
            if(!strcmp(mode,"analog-osk"))fixture_osk_active=1;else fixture_focus=2;
            assert(!report(0));axes(128,128,128,128);assert(!queued());no_ack();
            assert(report(1)>0);axes(128,128,128,128);ack(1,1,"canceled");
            analog(3,2,5000,200,128,128,128);fixture_focus=1;fixture_osk_active=0;
            assert(report(1)>0);axes(128,128,128,128);assert(!queued());ack(1,1,"canceled");
            analog(4,1,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(report(1)>0);axes(128,128,128,128);ack(4,1,"consumed");
        }else if(!strcmp(mode,"analog-locked")){
            analog(1,4,5000,0,255,17,240);
            HANDLE locked=CreateFileA(mailbox,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);assert(locked!=INVALID_HANDLE_VALUE);
            assert(report(0)>0);axes(128,128,128,128);assert(queued()>0);no_ack();
            assert(CloseHandle(locked));assert(report(1)>0);axes(0,255,17,240);assert(!queued());
            analog(2,2,5000,200,128,128,128);
            locked=CreateFileA(mailbox,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);assert(locked!=INVALID_HANDLE_VALUE);
            fixture_focus=2;assert(!report(0));axes(128,128,128,128);assert(queued()>0);
            fixture_focus=1;assert(report(1)>0);axes(128,128,128,128);ack(1,1,"canceled");assert(queued()>0);
            assert(CloseHandle(locked));assert(report(1)>0);axes(128,128,128,128);assert(!queued());
            analog(3,1,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(report(1)>0);axes(128,128,128,128);ack(3,1,"consumed");
        }else if(!strcmp(mode,"analog-mixed")){
            physical_buttons=XINPUT_GAMEPAD_B;physical_axes=1;assert(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_OK);
            publish("0x4000 3\n");assert(report(1)>0&&buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
            analog(1,2,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(buttons()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255);
            assert(vm_read16(0x104+2*CELL_PAD_BTN_OFFSET_PRESS_CIRCLE)==255);
            assert(report(1)>0);axes(0,255,17,240);assert(report(1)>0);axes(128,128,128,128);ack(1,2,"consumed");
            assert(report(1)>0&&buttons()==CELL_PAD_CTRL_CIRCLE);assert(axis(0)!=128&&axis(2)!=128);
        }else{
            analog(1,2,5000,0,255,17,240);assert(report(0)>0);axes(0,255,17,240);assert(!queued());no_ack();
            for(unsigned i=0;i<100;i++){assert(!report(0));axes(0,255,17,240);no_ack();}
            assert(report(1)>0);axes(0,255,17,240);no_ack();assert(!report(0));axes(128,128,128,128);no_ack();
            assert(report(1)>0);axes(128,128,128,128);ack(1,2,"consumed");
            analog(1,2,5000,0,255,17,240);assert(report(1)>0);axes(128,128,128,128);assert(queued()>0);ack(1,2,"consumed");
            analog(2,120,(unsigned)fixture_ms+8,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            assert(report(1)>0);axes(128,128,128,128);ack(2,1,"expired");
            analog(3,100,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            analog(4,0,5000,128,128,128,128);assert(!report(0));axes(128,128,128,128);ack(2,1,"expired");
            assert(report(1)>0);axes(128,128,128,128);ack(4,0,"released");
            analog(5,2,5000,0,255,17,240);assert(report(1)>0);axes(0,255,17,240);
            analog(6,1,5000,200,128,128,128);assert(report(1)>0);axes(0,255,17,240);assert(queued()>0);
            assert(report(1)>0);axes(128,128,128,128);assert(queued()>0);ack(5,2,"consumed");
            assert(report(1)>0);axes(200,128,128,128);assert(!queued());
            assert(report(1)>0);axes(128,128,128,128);ack(6,1,"consumed");
        }
    }else if(!strcmp(mode,"reparse")){
        const char*command="0x4000 3\n";reparse_target(command);fixture_focus=1;
        assert(report(0)>0&&!buttons());target_preserved(command);
        fixture_focus=2;assert(!report(0)&&!buttons());target_preserved(command);
    }else if(!strcmp(mode,"osk-owner")){
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
