/* Win32 owns controls; only the shared guest pump owns OSK completion events. */
#include "osk_native.h"
#if defined(_WIN32) && defined(PS3RECOMP_NATIVE_OSK_WINDOWS) && !defined(PS3RECOMP_DISABLE_NATIVE_OSK)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
static SRWLOCK s_queue_lock=SRWLOCK_INIT;
static HWND s_parent,s_dialog,s_edit;
static _Atomic(DWORD) s_owner;
static OskNativeRequest s_request,s_current;
static int s_open_pending;
static uint64_t s_close_pending,s_confirm_pending,s_generation;
static _Atomic(int) s_available,s_input_active;
int osk_native_available(void){return atomic_load(&s_available);}
int osk_native_win32_input_active(void){return atomic_load(&s_input_active);}
static int on_owner(void){return s_owner && GetCurrentThreadId()==s_owner;}
static void destroy_dialog(void){if(s_dialog)DestroyWindow(s_dialog);}
static void finish(int result){
    if(!s_dialog || !osk_native_is_open(s_generation))return;
    WCHAR text[513]={0};int length=0;
    if(result==0){length=GetWindowTextW(s_edit,text,513);if(length<0)length=0;}
    osk_native_did_finish(s_generation,result,(const uint16_t*)text,(uint32_t)length);
    destroy_dialog();
}
static LRESULT CALLBACK dialog_proc(HWND window,UINT message,WPARAM w,LPARAM l){
    switch(message){
    case WM_COMMAND:
        if(LOWORD(w)==IDOK){finish(0);return 0;}
        if(LOWORD(w)==IDCANCEL){finish(1);return 0;}
        break;
    case DM_GETDEFID:return MAKELONG(IDOK,DC_HASDEFID);
    case WM_CLOSE:finish(1);return 0;
    case WM_DESTROY:
        if(window==s_dialog){s_dialog=NULL;s_edit=NULL;osk_native_host_closed(s_generation);}
        return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
void osk_native_win32_set_window(void* value){
    HWND parent=(HWND)value;
    DWORD owner=parent?GetWindowThreadProcessId(parent,NULL):s_owner;
    if(owner && owner!=GetCurrentThreadId())return;
    atomic_store(&s_available,0);
    AcquireSRWLockExclusive(&s_queue_lock);
    uint64_t pending=s_open_pending?s_request.token:0;
    if(pending>s_close_pending)s_close_pending=pending;
    if(s_generation>s_close_pending)s_close_pending=s_generation;
    s_open_pending=0;s_confirm_pending=0;s_parent=parent;
    ReleaseSRWLockExclusive(&s_queue_lock);
    if(pending)osk_native_host_closed(pending);
    if(s_generation)osk_native_host_closed(s_generation);
    destroy_dialog();s_generation=0;atomic_store(&s_input_active,0);
    s_owner=parent?owner:0;
    atomic_store(&s_available,parent && IsWindow(parent));
}
static void wake_owner(void){
    AcquireSRWLockShared(&s_queue_lock);HWND parent=s_parent;ReleaseSRWLockShared(&s_queue_lock);
    if(parent)PostMessageW(parent,WM_NULL,0,0);
}
void osk_native_open(const OskNativeRequest* request){
    if(!osk_native_is_pending(request->token))return;
    AcquireSRWLockExclusive(&s_queue_lock);
    if(!atomic_load(&s_available)){ReleaseSRWLockExclusive(&s_queue_lock);osk_native_host_closed(request->token);return;}
    if(request->token<=s_close_pending || request->token<s_request.token){ReleaseSRWLockExclusive(&s_queue_lock);return;}
    s_request=*request;s_open_pending=1;
    atomic_store(&s_input_active,1);ReleaseSRWLockExclusive(&s_queue_lock);wake_owner();
}
void osk_native_close(uint64_t token){
    AcquireSRWLockExclusive(&s_queue_lock);
    if(token>s_close_pending)s_close_pending=token;
    ReleaseSRWLockExclusive(&s_queue_lock);wake_owner();
}
void osk_native_confirm(uint64_t token){
    if(!osk_native_is_open(token))return;
    AcquireSRWLockExclusive(&s_queue_lock);
    if(token>s_confirm_pending)s_confirm_pending=token;
    ReleaseSRWLockExclusive(&s_queue_lock);wake_owner();
}
static int create_dialog(const OskNativeRequest* request){
    WNDCLASSW cls={0};cls.lpfnWndProc=dialog_proc;cls.hInstance=GetModuleHandleW(NULL);
    cls.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);cls.lpszClassName=L"ps3recomp_osk";
    RegisterClassW(&cls);
    RECT owner;GetWindowRect(s_parent,&owner);s_current=*request;s_generation=request->token;
    s_dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,cls.lpszClassName,L"Game text input",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,owner.left+40,owner.top+60,430,190,s_parent,NULL,cls.hInstance,NULL);
    if(!s_dialog)return 0;
    HWND prompt=CreateWindowExW(0,L"STATIC",(const WCHAR*)request->message,WS_CHILD|WS_VISIBLE,
        16,12,390,44,s_dialog,NULL,cls.hInstance,NULL);
    DWORD style=WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL;
    if(request->panels&0x20000000u)style|=ES_PASSWORD;
    if(!request->limit)style|=ES_READONLY;
    s_edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",(const WCHAR*)request->initial,style,
        16,62,390,25,s_dialog,(HMENU)(uintptr_t)1001,cls.hInstance,NULL);
    HWND ok=CreateWindowExW(0,L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
        226,106,84,28,s_dialog,(HMENU)(uintptr_t)IDOK,cls.hInstance,NULL);
    HWND cancel=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        322,106,84,28,s_dialog,(HMENU)(uintptr_t)IDCANCEL,cls.hInstance,NULL);
    if(!prompt || !s_edit || !ok || !cancel){destroy_dialog();return 0;}
    HWND controls[]={prompt,s_edit,ok,cancel};
    for(unsigned i=0;i<4;i++)SendMessageW(controls[i],WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    SendMessageW(s_edit,EM_SETLIMITTEXT,request->limit?request->limit:1,0);
    HWND foreground=GetForegroundWindow();int owned=foreground==s_parent;
    ShowWindow(s_dialog,owned?SW_SHOW:SW_SHOWNOACTIVATE);
    if(owned)SetFocus(s_edit);
    atomic_store(&s_input_active,1);osk_native_did_open(request->token);return 1;
}
void osk_native_win32_pump(void){
    if(!on_owner())return;
    if(!s_parent || !IsWindow(s_parent)){osk_native_win32_set_window(NULL);return;}
    OskNativeRequest request;int opening;
    AcquireSRWLockExclusive(&s_queue_lock);
    opening=s_open_pending;request=s_request;s_open_pending=0;
    uint64_t closing=s_close_pending,confirming=s_confirm_pending;s_confirm_pending=0;
    ReleaseSRWLockExclusive(&s_queue_lock);
    /* A later closed generation also retires every older queued UI. Keeping
     * the maximum prevents a stale close from replacing a newer cancellation. */
    if(closing && opening && closing>=request.token){opening=0;atomic_store(&s_input_active,0);}
    if(closing && s_generation && closing>=s_generation){destroy_dialog();s_generation=0;atomic_store(&s_input_active,0);}
    if(opening && osk_native_is_pending(request.token)){
        if(!osk_native_available() || !create_dialog(&request)){
            osk_native_host_closed(request.token);atomic_store(&s_input_active,0);
        }
    }
    /* The mailbox confirm is the OK command itself, delivered to the dialog
     * as the button would deliver it. A simulated BM_CLICK depends on the
     * dialog being active and able to capture the mouse, which a game window
     * behind another application's foreground is not. */
    if(confirming){
        const int match=s_dialog && confirming==s_generation && osk_native_is_open(confirming);
        if(match)SendMessageW(s_dialog,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),(LPARAM)GetDlgItem(s_dialog,IDOK));
        printf("[cellOskDialog] native confirm token=%llu %s\n",(unsigned long long)confirming,
            !match?"ignored (no matching open dialog)":s_dialog?"not completed":"completed");
        fflush(stdout);
    }
}
int osk_native_win32_handle_message(void* value){
    return on_owner() && s_dialog && IsDialogMessageW(s_dialog,(MSG*)value);
}
#endif
