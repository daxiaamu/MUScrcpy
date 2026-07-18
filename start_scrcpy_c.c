#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define APP_VERSION L"2.3.3"
#define APP_TITLE L"MU投屏 " APP_VERSION L" daxiaamu.com"
#define APP_MUTEX L"Daxiaamu.MUScrcpy.GUI.SingleInstance"
#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DEVICE (WM_APP + 2)
#define WM_APP_STATUS (WM_APP + 3)
#define WM_APP_SCREEN (WM_APP + 4)
#define WM_APP_START_WORKERS (WM_APP + 5)
#define WM_APP_DOCK_SCRCPY (WM_APP + 6)
#define WM_APP_DEVICE_STATE (WM_APP + 7)
#define DOCK_TIMER_ID 1
#define IDC_WAKE 1101
#define IDC_COMPAT 1102
#define IDC_STAY_AWAKE 1103
#define IDC_ALWAYS_ON_TOP 1104
#define IDC_QUALITY 1105
#define IDC_QUALITY_HELP 1106
#define IDC_LOG 1201
#define IDC_LOG_SCROLL 1202
#define IDM_QUALITY_HELP 2100
#define IDM_CHECK_UPDATE 2101
#define IDM_ABOUT 2102
#define IDC_ABOUT_LINK 2201

#define CLR_BG RGB(246,247,249)
#define CLR_CARD RGB(255,255,255)
#define CLR_TEXT RGB(31,35,41)
#define CLR_MUTED RGB(102,110,120)
#define CLR_BORDER RGB(218,222,228)
#define CLR_ACCENT RGB(241,82,81)
#define CLR_ACCENT_HOT RGB(246,103,102)
#define CLR_ACCENT_PRESSED RGB(211,58,58)
#define CLR_GREEN RGB(22,163,74)
#define CLR_LOG_BG RGB(25,31,42)
#define CLR_LOG_TEXT RGB(210,221,235)

typedef struct {
    wchar_t model[160], market_name[160], serial[160], android[80], slot[80], kernel[200];
    wchar_t build_display[240], rom_version[160], ota_version[240];
} DeviceInfo;

static HINSTANCE app_instance;
static HWND main_window, about_window, model_view, serial_view, android_view, slot_view, kernel_view, system_view;
static HWND wake_button, compat_check, stay_awake_check, always_on_top_check, quality_label, quality_combo, quality_help, log_view, log_scroll, status_view;
static HFONT font_normal, font_bold, font_log, font_about_title, font_link;
static HBRUSH brush_bg, brush_card, brush_log;
static HANDLE stop_event, worker_handle, screen_worker_handle, mutex_handle, scrcpy_process;
static CRITICAL_SECTION process_lock;
static DWORD scrcpy_process_id;
static HWND docked_scrcpy_window;
static int dock_side = 1, dock_retry_count;
static BOOL dock_retry_bring_forward;
static volatile LONG compat_mode;
static volatile LONG stay_awake_mode = 1;
static volatile LONG screen_is_off;
static volatile LONG always_on_top_mode;
static volatile LONG quality_mode;
static volatile LONG workers_started;
static wchar_t bin_dir[MAX_PATH];
static wchar_t scrcpy_version[80]=L"未检测";
static wchar_t current_status[200]=L"正在初始化";
static int scroll_drag_offset = -1;
static BOOL scroll_hover;
static HWND hot_button;
static WNDPROC button_window_proc;
static BOOL quality_combo_hover;
static BOOL suppress_outside_click_up;
static int normal_font_height = 16;
static int info_row_height = 40;
static int main_card_bottom = 295;
static int quality_label_width = 38, quality_combo_width = 90, quality_help_width = 76;
static int stay_awake_width = 84, compat_width = 84, always_on_top_width = 84;
static int option_gap = 8;
static BOOL device_info_fresh;
static int about_title_height = 38;

enum { QUALITY_AUTO, QUALITY_SMOOTH, QUALITY_BALANCED, QUALITY_HIGH, QUALITY_ULTRA };

static void load_preferences(void) {
    HKEY key;
    DWORD value = 1, type = 0, size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\daxiaamu\\MUScrcpy", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"KeepAwake", NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS || type != REG_DWORD) value = 1;
        RegCloseKey(key);
    }
    InterlockedExchange(&stay_awake_mode, value ? 1 : 0);
}

static BOOL save_keep_awake(BOOL enabled) {
    HKEY key;
    DWORD disposition, value = enabled ? 1 : 0;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\daxiaamu\\MUScrcpy", 0, NULL, 0,
                                  KEY_SET_VALUE, NULL, &key, &disposition);
    if (result != ERROR_SUCCESS) return FALSE;
    result = RegSetValueExW(key, L"KeepAwake", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static wchar_t *wide_copy(const wchar_t *s) {
    size_t size = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *p = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, size);
    if (p) memcpy(p, s, size);
    return p;
}

static void trim(wchar_t *s) {
    wchar_t *p = s;
    size_t n;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') ++p;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r' || s[n-1] == L'\n')) s[--n] = 0;
}

static void post_log(const wchar_t *message) {
    SYSTEMTIME t;
    wchar_t line[1400];
    GetLocalTime(&t);
    _snwprintf(line, ARRAYSIZE(line)-1, L"[%02u:%02u:%02u] %ls\r\n", t.wHour, t.wMinute, t.wSecond, message);
    line[ARRAYSIZE(line)-1] = 0;
    if (main_window) PostMessageW(main_window, WM_APP_LOG, 0, (LPARAM)wide_copy(line));
}

static void post_status(const wchar_t *message) {
    if (main_window) PostMessageW(main_window, WM_APP_STATUS, 0, (LPARAM)wide_copy(message));
}

static void utf8_or_acp_to_wide(const char *input, DWORD length, wchar_t *output, size_t capacity) {
    int n;
    output[0] = 0;
    if (!input || !length || capacity < 2) return;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int)length, output, (int)capacity-1);
    if (!n) n = MultiByteToWideChar(CP_ACP, 0, input, (int)length, output, (int)capacity-1);
    if (n > 0) output[n] = 0;
}

static BOOL initialize_bin_directory(void) {
    wchar_t module[MAX_PATH], *slash, adb[MAX_PATH], scrcpy[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, module, ARRAYSIZE(module));
    if (!n || n >= ARRAYSIZE(module)) return FALSE;
    slash = wcsrchr(module, L'\\');
    if (slash) *slash = 0;
    _snwprintf(bin_dir, ARRAYSIZE(bin_dir)-1, L"%ls\\bin", module);
    _snwprintf(adb, ARRAYSIZE(adb)-1, L"%ls\\adb.exe", bin_dir);
    _snwprintf(scrcpy, ARRAYSIZE(scrcpy)-1, L"%ls\\scrcpy.exe", bin_dir);
    return GetFileAttributesW(adb) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(scrcpy) != INVALID_FILE_ATTRIBUTES;
}

static void detect_scrcpy_version(void) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe = NULL, write_pipe = NULL;
    wchar_t exe[MAX_PATH], command[MAX_PATH+32], output[512], *version, *end;
    char bytes[1024];
    DWORD used = 0, read = 0, wait_result;
    ZeroMemory(&sa,sizeof(sa));sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
    if(!CreatePipe(&read_pipe,&write_pipe,&sa,0))return;
    SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0);
    _snwprintf(exe,ARRAYSIZE(exe)-1,L"%ls\\scrcpy.exe",bin_dir);
    _snwprintf(command,ARRAYSIZE(command)-1,L"\"%ls\" --version",exe);
    command[ARRAYSIZE(command)-1]=0;
    ZeroMemory(&si,sizeof(si));ZeroMemory(&pi,sizeof(pi));si.cb=sizeof(si);
    si.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.wShowWindow=SW_HIDE;
    si.hStdOutput=write_pipe;si.hStdError=write_pipe;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    if(!CreateProcessW(NULL,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,bin_dir,&si,&pi)){
        CloseHandle(read_pipe);CloseHandle(write_pipe);return;
    }
    CloseHandle(write_pipe);write_pipe=NULL;
    wait_result=WaitForSingleObject(pi.hProcess,3000);
    if(wait_result==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,2);WaitForSingleObject(pi.hProcess,1000);}
    while(used<sizeof(bytes)-1&&ReadFile(read_pipe,bytes+used,(DWORD)(sizeof(bytes)-1-used),&read,NULL)&&read)used+=read;
    bytes[used]=0;utf8_or_acp_to_wide(bytes,used,output,ARRAYSIZE(output));
    version=wcsstr(output,L"scrcpy ");
    if(version){
        size_t length;
        version+=7;while(*version==L' '||*version==L'\t')++version;
        end=version;while(*end&&*end!=L' '&&*end!=L'\t'&&*end!=L'\r'&&*end!=L'\n'&&*end!=L'<')++end;
        length=(size_t)(end-version);if(length>=ARRAYSIZE(scrcpy_version))length=ARRAYSIZE(scrcpy_version)-1;
        if(length){memcpy(scrcpy_version,version,length*sizeof(wchar_t));scrcpy_version[length]=0;}
    }
    CloseHandle(read_pipe);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
}

static BOOL run_adb(const wchar_t *args, wchar_t *output, size_t capacity, DWORD timeout) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe = NULL, write_pipe = NULL;
    wchar_t adb[MAX_PATH], command[1400];
    char bytes[4096];
    DWORD used = 0, read = 0, wait_result, available, started;
    BOOL success = FALSE, timed_out = FALSE;
    output[0] = 0;
    ZeroMemory(&sa, sizeof(sa)); sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return FALSE;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    _snwprintf(adb, ARRAYSIZE(adb)-1, L"%ls\\adb.exe", bin_dir);
    _snwprintf(command, ARRAYSIZE(command)-1, L"\"%ls\" %ls", adb, args);
    ZeroMemory(&si, sizeof(si)); ZeroMemory(&pi, sizeof(pi)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe; si.hStdError = write_pipe; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, bin_dir, &si, &pi)) {
        CloseHandle(write_pipe); write_pipe = NULL;
        started = GetTickCount();
        for (;;) {
            wait_result = WaitForSingleObject(pi.hProcess, 100);
            if (wait_result != WAIT_TIMEOUT) break;
            if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0 || GetTickCount() - started >= timeout) {
                timed_out = TRUE;
                TerminateProcess(pi.hProcess, 2);
                WaitForSingleObject(pi.hProcess, 1000);
                break;
            }
        }
        while (used < sizeof(bytes)-1 && PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL) && available) {
            DWORD wanted = available > sizeof(bytes)-1-used ? (DWORD)(sizeof(bytes)-1-used) : available;
            if (!ReadFile(read_pipe, bytes+used, wanted, &read, NULL) || !read) break;
            used += read;
        }
        bytes[used] = 0;
        utf8_or_acp_to_wide(bytes, used, output, capacity); trim(output);
        success = !timed_out && wait_result != WAIT_FAILED;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (write_pipe) CloseHandle(write_pipe);
    CloseHandle(read_pipe);
    return success;
}

static BOOL device_is_online(void) {
    wchar_t state[128];
    return run_adb(L"-d get-state", state, ARRAYSIZE(state), 3000) && wcscmp(state, L"device") == 0;
}

typedef struct {
    const wchar_t *arguments;
    wchar_t *output;
    size_t capacity;
} DeviceQuery;

static DWORD WINAPI device_query_worker(LPVOID parameter) {
    DeviceQuery *query=(DeviceQuery*)parameter;
    run_adb(query->arguments,query->output,query->capacity,4000);
    return 0;
}

static void read_device_info(DeviceInfo *d) {
    wchar_t market_enname[160], boot_slot[80], ab_update[80];
    HANDLE threads[12];
    int thread_count=0,i;
    DeviceQuery queries[]={
        {L"-d shell getprop ro.product.model",d->model,ARRAYSIZE(d->model)},
        {L"-d shell getprop ro.vendor.oplus.market.name",d->market_name,ARRAYSIZE(d->market_name)},
        {L"-d shell getprop ro.vendor.oplus.market.enname",market_enname,ARRAYSIZE(market_enname)},
        {L"-d get-serialno",d->serial,ARRAYSIZE(d->serial)},
        {L"-d shell getprop ro.build.version.release",d->android,ARRAYSIZE(d->android)},
        {L"-d shell getprop ro.boot.slot_suffix",d->slot,ARRAYSIZE(d->slot)},
        {L"-d shell getprop ro.boot.slot",boot_slot,ARRAYSIZE(boot_slot)},
        {L"-d shell getprop ro.build.ab_update",ab_update,ARRAYSIZE(ab_update)},
        {L"-d shell uname -r",d->kernel,ARRAYSIZE(d->kernel)},
        {L"-d shell getprop ro.build.display.id",d->build_display,ARRAYSIZE(d->build_display)},
        {L"-d shell getprop ro.rom.version",d->rom_version,ARRAYSIZE(d->rom_version)},
        {L"-d shell getprop ro.build.version.ota",d->ota_version,ARRAYSIZE(d->ota_version)}
    };
    ZeroMemory(d, sizeof(*d));
    ZeroMemory(market_enname, sizeof(market_enname));
    ZeroMemory(boot_slot, sizeof(boot_slot));
    ZeroMemory(ab_update, sizeof(ab_update));
    for(i=0;i<(int)ARRAYSIZE(queries);++i){
        HANDLE thread=CreateThread(NULL,0,device_query_worker,&queries[i],0,NULL);
        if(thread)threads[thread_count++]=thread;
        else device_query_worker(&queries[i]);
    }
    if(thread_count)WaitForMultipleObjects((DWORD)thread_count,threads,TRUE,INFINITE);
    for(i=0;i<thread_count;++i)CloseHandle(threads[i]);
    if(!d->market_name[0]&&market_enname[0])wcscpy(d->market_name,market_enname);
    if (!d->model[0]) wcscpy(d->model, L"未知设备");
    if (!d->serial[0]) wcscpy(d->serial, L"未知");
    if (!d->android[0]) wcscpy(d->android, L"未知");
    if (!wcscmp(d->slot, L"_a") || !wcscmp(d->slot, L"a")) wcscpy(d->slot, L"A");
    else if (!wcscmp(d->slot, L"_b") || !wcscmp(d->slot, L"b")) wcscpy(d->slot, L"B");
    else if (!d->slot[0]) {
        if (!wcscmp(boot_slot, L"_a") || !wcscmp(boot_slot, L"a")) wcscpy(d->slot, L"A");
        else if (!wcscmp(boot_slot, L"_b") || !wcscmp(boot_slot, L"b")) wcscpy(d->slot, L"B");
        else {
            if (!wcsicmp(ab_update, L"true") || !wcscmp(ab_update, L"1")) wcscpy(d->slot, L"A/B（槽位未知）");
            else wcscpy(d->slot, L"非A/B设备");
        }
    }
}

static DWORD WINAPI device_info_worker(LPVOID ignored) {
    DeviceInfo *d=(DeviceInfo*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(DeviceInfo));
    wchar_t expected_serial[160],current_serial[160];
    (void)ignored;
    if(!d)return 1;
    expected_serial[0]=0;
    if(!run_adb(L"-d get-serialno",expected_serial,ARRAYSIZE(expected_serial),3000)||!expected_serial[0]){
        HeapFree(GetProcessHeap(),0,d);return 0;
    }
    read_device_info(d);
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){HeapFree(GetProcessHeap(),0,d);return 0;}
    current_serial[0]=0;
    if(!run_adb(L"-d get-serialno",current_serial,ARRAYSIZE(current_serial),3000)||!current_serial[0]||
       wcscmp(expected_serial,current_serial)!=0||wcscmp(expected_serial,d->serial)!=0){
        HeapFree(GetProcessHeap(),0,d);return 0;
    }
    PostMessageW(main_window,WM_APP_DEVICE,0,(LPARAM)d);
    post_log(L"设备信息读取完成。");
    return 0;
}

static void terminate_scrcpy(void) {
    EnterCriticalSection(&process_lock);
    if (scrcpy_process) TerminateProcess(scrcpy_process, 0);
    LeaveCriticalSection(&process_lock);
}

typedef struct { DWORD process_id; HWND window; } WindowSearch;

typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextFn)(HANDLE);
static SetThreadDpiAwarenessContextFn set_thread_dpi_context;

static HANDLE enter_physical_dpi_context(void) {
    static BOOL initialized;
    if(!initialized){
        HMODULE user32=GetModuleHandleW(L"user32.dll");
        union { FARPROC raw; SetThreadDpiAwarenessContextFn call; } function;
        function.raw=user32?GetProcAddress(user32,"SetThreadDpiAwarenessContext"):NULL;
        set_thread_dpi_context=function.call;initialized=TRUE;
    }
    return set_thread_dpi_context?set_thread_dpi_context((HANDLE)(LONG_PTR)-4):NULL;
}

static void leave_physical_dpi_context(HANDLE previous) {
    if(set_thread_dpi_context&&previous)set_thread_dpi_context(previous);
}

static void get_visible_window_rect(HWND window, RECT *rect) {
    typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND,DWORD,PVOID,DWORD);
    static BOOL initialized;
    static DwmGetWindowAttributeFn get_attribute;
    if(!initialized){
        HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
        union { FARPROC raw; DwmGetWindowAttributeFn call; } function;
        function.raw=dwm?GetProcAddress(dwm,"DwmGetWindowAttribute"):NULL;
        get_attribute=function.call;initialized=TRUE;
    }
    GetWindowRect(window,rect);
    if(get_attribute){
        RECT visible;
        if(SUCCEEDED(get_attribute(window,9,&visible,sizeof(visible))))*rect=visible;
    }
}

static BOOL CALLBACK find_process_window(HWND window, LPARAM parameter) {
    WindowSearch *search=(WindowSearch*)parameter;
    DWORD process_id=0;
    GetWindowThreadProcessId(window,&process_id);
    if(process_id==search->process_id && IsWindowVisible(window) && GetWindow(window,GW_OWNER)==NULL) {
        search->window=window;
        return FALSE;
    }
    return TRUE;
}

static HWND find_scrcpy_window(void) {
    WindowSearch search;
    ZeroMemory(&search,sizeof(search));
    EnterCriticalSection(&process_lock);search.process_id=scrcpy_process_id;LeaveCriticalSection(&process_lock);
    if(!search.process_id)return NULL;
    EnumWindows(find_process_window,(LPARAM)&search);
    return search.window;
}

static int choose_dock_side(const RECT *gui_visible, int scrcpy_width, const RECT *work_area) {
    if(gui_visible->right+scrcpy_width<=work_area->right)return 1;
    if(gui_visible->left-scrcpy_width>=work_area->left)return -1;
    return work_area->right-gui_visible->right>=gui_visible->left-work_area->left?1:-1;
}

static BOOL get_window_work_area(HWND window,RECT *work_area) {
    MONITORINFO info;HMONITOR found=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    ZeroMemory(&info,sizeof(info));info.cbSize=sizeof(info);
    if(!found||!GetMonitorInfoW(found,&info))return FALSE;
    *work_area=info.rcWork;
    return TRUE;
}

static void keep_window_in_work_area(const RECT *work_area,const RECT *window_rect,
                                     const RECT *visible_rect,int *x,int *y) {
    int visible_width=visible_rect->right-visible_rect->left;
    int visible_height=visible_rect->bottom-visible_rect->top;
    int work_width=work_area->right-work_area->left;
    int work_height=work_area->bottom-work_area->top;
    int visible_x=*x+(visible_rect->left-window_rect->left);
    int visible_y=*y+(visible_rect->top-window_rect->top);
    if(visible_width<=work_width){
        if(visible_x<work_area->left)visible_x=work_area->left;
        if(visible_x+visible_width>work_area->right)visible_x=work_area->right-visible_width;
    }else visible_x=work_area->left;
    if(visible_height<=work_height){
        if(visible_y<work_area->top)visible_y=work_area->top;
        if(visible_y+visible_height>work_area->bottom)visible_y=work_area->bottom-visible_height;
    }else visible_y=work_area->top;
    *x=visible_x-(visible_rect->left-window_rect->left);
    *y=visible_y-(visible_rect->top-window_rect->top);
}

static BOOL position_scrcpy_window(HWND gui,HWND scrcpy,HWND insert_after,UINT flags) {
    RECT gui_visible,scrcpy_rect,scrcpy_visible,work_area;
    HANDLE previous_dpi_context;
    int scrcpy_width,x,y;
    previous_dpi_context=enter_physical_dpi_context();
    GetWindowRect(scrcpy,&scrcpy_rect);
    get_visible_window_rect(gui,&gui_visible);get_visible_window_rect(scrcpy,&scrcpy_visible);
    scrcpy_width=scrcpy_visible.right-scrcpy_visible.left;
    if(!get_window_work_area(gui,&work_area)){
        leave_physical_dpi_context(previous_dpi_context);return FALSE;
    }
    dock_side=choose_dock_side(&gui_visible,scrcpy_width,&work_area);
    x=dock_side>0?gui_visible.right-(scrcpy_visible.left-scrcpy_rect.left):
                   gui_visible.left-scrcpy_width-(scrcpy_visible.left-scrcpy_rect.left);
    y=scrcpy_rect.top;
    keep_window_in_work_area(&work_area,&scrcpy_rect,&scrcpy_visible,&x,&y);
    SetWindowPos(scrcpy,insert_after,x,y,0,0,flags|SWP_NOSIZE);
    leave_physical_dpi_context(previous_dpi_context);
    return TRUE;
}

static BOOL dock_scrcpy_window(HWND gui, BOOL bring_forward) {
    HWND scrcpy=find_scrcpy_window();
    if(!scrcpy||!IsWindow(scrcpy)||IsIconic(gui))return FALSE;
    if(IsIconic(scrcpy))ShowWindow(scrcpy,SW_SHOWNOACTIVATE);
    if(!position_scrcpy_window(gui,scrcpy,HWND_TOP,SWP_NOACTIVATE|SWP_SHOWWINDOW))return FALSE;
    docked_scrcpy_window=scrcpy;
    if(bring_forward){
        SetWindowPos(scrcpy,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetWindowPos(gui,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetForegroundWindow(gui);
    }
    return TRUE;
}

static void forward_scrcpy_output(HANDLE process, HANDLE pipe) {
    char bytes[1024];
    wchar_t text[1200];
    DWORD available, read;
    for (;;) {
        while (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available) {
            DWORD wanted = available > sizeof(bytes)-1 ? sizeof(bytes)-1 : available;
            if (!ReadFile(pipe, bytes, wanted, &read, NULL) || !read) break;
            utf8_or_acp_to_wide(bytes, read, text, ARRAYSIZE(text)); trim(text);
            if (text[0]) post_log(text);
        }
        if (WaitForSingleObject(process, 100) != WAIT_TIMEOUT) break;
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
    }
}

static int choose_auto_quality(void) {
    SYSTEM_INFO system_info;
    MEMORYSTATUSEX memory;
    wchar_t display_size[256], *colon;
    int width = 0, height = 0, longest = 0;
    ZeroMemory(&system_info,sizeof(system_info)); GetNativeSystemInfo(&system_info);
    ZeroMemory(&memory,sizeof(memory)); memory.dwLength=sizeof(memory); GlobalMemoryStatusEx(&memory);
    run_adb(L"-d shell wm size",display_size,ARRAYSIZE(display_size),4000);
    colon=wcsrchr(display_size,L':');
    if(colon && swscanf(colon+1,L"%dx%d",&width,&height)==2) longest=width>height?width:height;
    if(system_info.dwNumberOfProcessors<=4 || memory.ullTotalPhys<6ULL*1024*1024*1024) return QUALITY_SMOOTH;
    if(system_info.dwNumberOfProcessors>=12 && memory.ullTotalPhys>=12ULL*1024*1024*1024 && (!longest || longest<=3000)) return QUALITY_HIGH;
    return QUALITY_BALANCED;
}

static void append_quality_options(wchar_t *command, int quality) {
    if(quality==QUALITY_SMOOTH) wcscat(command,L" --max-size=1280 --max-fps=30 --video-bit-rate=4M");
    else if(quality==QUALITY_BALANCED) wcscat(command,L" --max-size=1920 --max-fps=45 --video-bit-rate=8M");
    else if(quality==QUALITY_HIGH) wcscat(command,L" --max-size=2560 --max-fps=60 --video-bit-rate=12M");
    else if(quality==QUALITY_ULTRA) wcscat(command,L" --max-fps=120 --video-bit-rate=20M");
}

static void launch_scrcpy(void) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe, write_pipe;
    wchar_t exe[MAX_PATH], command[1000];
    BOOL compatible = InterlockedCompareExchange(&compat_mode, 0, 0) != 0;
    BOOL stay_awake = InterlockedCompareExchange(&stay_awake_mode, 0, 0) != 0;
    BOOL always_on_top = InterlockedCompareExchange(&always_on_top_mode, 0, 0) != 0;
    int selected_quality = (int)InterlockedCompareExchange(&quality_mode, 0, 0);
    int effective_quality = selected_quality==QUALITY_AUTO?choose_auto_quality():selected_quality;
    ZeroMemory(&sa, sizeof(sa)); sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    _snwprintf(exe, ARRAYSIZE(exe)-1, L"%ls\\scrcpy.exe", bin_dir);
    _snwprintf(command, ARRAYSIZE(command)-1, L"\"%ls\"", exe);
    command[ARRAYSIZE(command)-1] = 0;
    if (compatible) wcscat(command,L" -d --render-driver=software --no-audio");
    if (stay_awake) wcscat(command,L" --stay-awake");
    if (always_on_top) wcscat(command,L" --always-on-top");
    append_quality_options(command,effective_quality);
    ZeroMemory(&si, sizeof(si)); ZeroMemory(&pi, sizeof(pi)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe; si.hStdError = write_pipe; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if(selected_quality==QUALITY_AUTO) {
        if(effective_quality==QUALITY_SMOOTH) post_log(L"自动画质已选择：流畅。");
        else if(effective_quality==QUALITY_HIGH) post_log(L"自动画质已选择：高清。");
        else post_log(L"自动画质已选择：均衡。");
    }
    if (compatible && stay_awake) post_log(L"正在以兼容模式启动 scrcpy（保持亮屏）…");
    else if (compatible) post_log(L"正在以兼容模式启动 scrcpy…");
    else if (stay_awake) post_log(L"正在启动 scrcpy（保持亮屏）…");
    else post_log(L"正在无参数启动 scrcpy…");
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, bin_dir, &si, &pi)) {
        post_log(L"scrcpy.exe 启动失败，请检查 bin 目录。"); CloseHandle(read_pipe); CloseHandle(write_pipe); Sleep(1000); return;
    }
    CloseHandle(write_pipe);
    EnterCriticalSection(&process_lock); scrcpy_process = pi.hProcess; scrcpy_process_id=pi.dwProcessId; LeaveCriticalSection(&process_lock);
    PostMessageW(main_window,WM_APP_DOCK_SCRCPY,0,0);
    post_status(compatible ? L"兼容模式投屏中" : L"正在投屏");
    forward_scrcpy_output(pi.hProcess, read_pipe);
    WaitForSingleObject(pi.hProcess, 1000);
    EnterCriticalSection(&process_lock); scrcpy_process = NULL; scrcpy_process_id=0; LeaveCriticalSection(&process_lock);
    docked_scrcpy_window=NULL;
    CloseHandle(read_pipe); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) post_log(L"scrcpy 已退出，准备重新连接。");
}

static DWORD WINAPI launcher_worker(LPVOID ignored) {
    BOOL waiting_logged = FALSE, info_requested = FALSE;
    (void)ignored;
    if (!initialize_bin_directory()) {
        post_status(L"缺少运行文件"); post_log(L"未找到 bin\\adb.exe 或 bin\\scrcpy.exe。"); return 1;
    }
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        if (!device_is_online()) {
            info_requested=FALSE;
            if (!waiting_logged) {
                PostMessageW(main_window,WM_APP_DEVICE_STATE,FALSE,0);
                post_status(L"等待设备连接"); post_log(L"等待 USB 设备，请确认已开启 USB 调试。"); waiting_logged = TRUE;
            }
            WaitForSingleObject(stop_event, 1000); continue;
        }
        waiting_logged = FALSE;
        if(!info_requested){
            HANDLE info_thread=CreateThread(NULL,0,device_info_worker,NULL,0,NULL);
            if(info_thread){CloseHandle(info_thread);info_requested=TRUE;}
        }
        post_log(L"设备已连接，正在并行启动投屏并读取设备信息。");launch_scrcpy();
        if (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) WaitForSingleObject(stop_event, 350);
    }
    return 0;
}

static DWORD WINAPI wake_worker(LPVOID ignored) {
    wchar_t output[256];
    (void)ignored; post_log(L"正在发送点亮屏幕指令…");
    if (run_adb(L"-d shell input keyevent 224", output, ARRAYSIZE(output), 5000)) post_log(L"点亮屏幕指令已发送。");
    else post_log(L"点亮失败，请检查设备连接和 USB 调试授权。");
    return 0;
}

static DWORD WINAPI screen_monitor_worker(LPVOID ignored) {
    LONG previous = -1;
    (void)ignored;
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        wchar_t state[512];
        LONG off = 0;
        if (bin_dir[0] && run_adb(L"-d shell \"dumpsys power | grep -E 'mWakefulness=|Display Power: state=|mScreenOn='\"",
                                  state, ARRAYSIZE(state), 4000)) {
            if (wcsstr(state,L"mWakefulness=Asleep") || wcsstr(state,L"mWakefulness=Dozing") ||
                wcsstr(state,L"state=OFF") || wcsstr(state,L"mScreenOn=false")) off = 1;
            else if (wcsstr(state,L"mWakefulness=Awake") || wcsstr(state,L"state=ON") ||
                     wcsstr(state,L"mScreenOn=true")) off = 0;
        }
        if (off != previous) {
            previous = off;
            PostMessageW(main_window,WM_APP_SCREEN,(WPARAM)off,0);
        }
        WaitForSingleObject(stop_event,500);
    }
    return 0;
}

static HWND create_text(HWND parent, const wchar_t *text) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,
                             0,0,0,0,parent,NULL,app_instance,NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font_normal, TRUE); return h;
}

static LRESULT CALLBACK modern_button_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    switch(message){
    case WM_MOUSEMOVE:
        if(hot_button!=hwnd){
            TRACKMOUSEEVENT tracking;HWND old=hot_button;hot_button=hwnd;
            if(old)InvalidateRect(old,NULL,TRUE);
            InvalidateRect(hwnd,NULL,TRUE);
            ZeroMemory(&tracking,sizeof(tracking));tracking.cbSize=sizeof(tracking);
            tracking.dwFlags=TME_LEAVE;tracking.hwndTrack=hwnd;TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if(hot_button==hwnd){hot_button=NULL;InvalidateRect(hwnd,NULL,TRUE);}
        break;
    case WM_ENABLE:InvalidateRect(hwnd,NULL,TRUE);break;
    }
    return CallWindowProcW(button_window_proc,hwnd,message,wparam,lparam);
}

static void style_modern_button(HWND button){
    WNDPROC proc=(WNDPROC)SetWindowLongPtrW(button,GWLP_WNDPROC,(LONG_PTR)modern_button_proc);
    if(!button_window_proc)button_window_proc=proc;
}

static HWND create_button(HWND parent, const wchar_t *text, int id) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                             0,0,0,0,parent,(HMENU)(INT_PTR)id,app_instance,NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font_normal, TRUE);style_modern_button(h);return h;
}

static HWND create_checkbox(HWND parent, const wchar_t *text, int id) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                             0,0,0,0,parent,(HMENU)(INT_PTR)id,app_instance,NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font_normal, TRUE);style_modern_button(h);
    return h;
}

static LRESULT CALLBACK quality_list_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,
                                          UINT_PTR subclass_id,DWORD_PTR data){
    LRESULT result=DefSubclassProc(hwnd,message,wparam,lparam);(void)subclass_id;(void)data;
    if(message==WM_NCPAINT||message==WM_SHOWWINDOW){
        HDC dc=GetWindowDC(hwnd);RECT r;HPEN pen;HGDIOBJ old_pen,old_brush;
        if(dc){
            GetWindowRect(hwnd,&r);OffsetRect(&r,-r.left,-r.top);
            pen=CreatePen(PS_SOLID,1,CLR_ACCENT);old_pen=SelectObject(dc,pen);
            old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,0,0,r.right,r.bottom);
            SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);ReleaseDC(hwnd,dc);
        }
    }
    return result;
}

static LRESULT CALLBACK quality_combo_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,
                                           UINT_PTR subclass_id,DWORD_PTR data){
    (void)subclass_id;(void)data;
    switch(message){
    case WM_MOUSEMOVE:
        if(!quality_combo_hover){
            TRACKMOUSEEVENT tracking;quality_combo_hover=TRUE;InvalidateRect(hwnd,NULL,TRUE);
            ZeroMemory(&tracking,sizeof(tracking));tracking.cbSize=sizeof(tracking);
            tracking.dwFlags=TME_LEAVE;tracking.hwndTrack=hwnd;TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:quality_combo_hover=FALSE;InvalidateRect(hwnd,NULL,TRUE);break;
    case WM_SETFOCUS:case WM_KILLFOCUS:case WM_ENABLE:InvalidateRect(hwnd,NULL,TRUE);break;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT r,text_rect;wchar_t text[40]=L"";
        BOOL focused=GetFocus()==hwnd,dropped=(BOOL)SendMessageW(hwnd,CB_GETDROPPEDSTATE,0,0);
        COLORREF border=(focused||dropped)?CLR_ACCENT:(quality_combo_hover?RGB(174,185,199):CLR_BORDER);
        HBRUSH brush;HPEN pen=CreatePen(PS_SOLID,1,border);
        HGDIOBJ old_brush,old_pen=SelectObject(dc,pen);int selected;
        GetClientRect(hwnd,&r);brush=CreateSolidBrush(CLR_BG);FillRect(dc,&r,brush);DeleteObject(brush);
        brush=CreateSolidBrush(CLR_CARD);old_brush=SelectObject(dc,brush);RoundRect(dc,r.left,r.top,r.right,r.bottom,6,6);
        SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(pen);DeleteObject(brush);
        selected=(int)SendMessageW(hwnd,CB_GETCURSEL,0,0);
        if(selected!=CB_ERR)SendMessageW(hwnd,CB_GETLBTEXT,selected,(LPARAM)text);
        text_rect=r;text_rect.left+=8;text_rect.right-=27;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,CLR_TEXT);
        SelectObject(dc,font_normal);DrawTextW(dc,text,-1,&text_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        pen=CreatePen(PS_SOLID,1,CLR_MUTED);old_pen=SelectObject(dc,pen);
        MoveToEx(dc,r.right-18,r.top+(r.bottom-r.top)/2-2,NULL);
        LineTo(dc,r.right-14,r.top+(r.bottom-r.top)/2+2);LineTo(dc,r.right-10,r.top+(r.bottom-r.top)/2-2);
        SelectObject(dc,old_pen);DeleteObject(pen);EndPaint(hwnd,&ps);return 0;
    }
    }
    return DefSubclassProc(hwnd,message,wparam,lparam);
}

static LRESULT CALLBACK device_text_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,
                                         UINT_PTR subclass_id,DWORD_PTR data){
    (void)wparam;(void)lparam;(void)subclass_id;(void)data;
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){
        PAINTSTRUCT ps;RECT client,label_rect,value_rect;HDC dc=BeginPaint(hwnd,&ps),buffer_dc;
        HBITMAP bitmap,old_bitmap;HFONT old_font;wchar_t text[800],*value,*line_break;
        int width,height;
        GetClientRect(hwnd,&client);width=client.right;height=client.bottom;
        GetWindowTextW(hwnd,text,ARRAYSIZE(text));line_break=wcsstr(text,L"\r\n");
        if(line_break){*line_break=0;value=line_break+2;}else value=L"";
        buffer_dc=CreateCompatibleDC(dc);bitmap=CreateCompatibleBitmap(dc,width>0?width:1,height>0?height:1);
        old_bitmap=(HBITMAP)SelectObject(buffer_dc,bitmap);FillRect(buffer_dc,&client,brush_card);
        old_font=(HFONT)SelectObject(buffer_dc,font_normal);SetBkMode(buffer_dc,TRANSPARENT);
        label_rect=client;label_rect.bottom=normal_font_height+2;SetTextColor(buffer_dc,CLR_TEXT);
        DrawTextW(buffer_dc,text,-1,&label_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        value_rect=client;value_rect.top=normal_font_height+3;
        SetTextColor(buffer_dc,device_info_fresh?CLR_TEXT:RGB(148,156,168));
        DrawTextW(buffer_dc,value,-1,&value_rect,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
        SelectObject(buffer_dc,old_font);BitBlt(dc,0,0,width,height,buffer_dc,0,0,SRCCOPY);
        SelectObject(buffer_dc,old_bitmap);DeleteObject(bitmap);DeleteDC(buffer_dc);EndPaint(hwnd,&ps);return 0;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,device_text_proc,1);
    return DefSubclassProc(hwnd,message,wparam,lparam);
}

static int measure_device_view_height(HWND view,int width){
    wchar_t text[800],*value,*line_break;RECT calculated={0,0,width,0};HDC dc;HFONT previous_font;
    int value_height=normal_font_height,total;
    GetWindowTextW(view,text,ARRAYSIZE(text));line_break=wcsstr(text,L"\r\n");
    value=line_break?line_break+2:L"";
    dc=GetDC(view);
    if(dc){
        previous_font=(HFONT)SelectObject(dc,font_normal);
        if(value[0]&&DrawTextW(dc,value,-1,&calculated,DT_CALCRECT|DT_LEFT|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX))
            value_height=calculated.bottom-calculated.top;
        SelectObject(dc,previous_font);ReleaseDC(view,dc);
    }
    if(value_height<normal_font_height)value_height=normal_font_height;
    total=normal_font_height+3+value_height+3;
    return total>info_row_height?total:info_row_height;
}

static void layout(HWND hwnd) {
    RECT r; int width, card_width, half, y, log_height, controls_y, controls_height, log_y;
    int content_left,content_width,column_gap,left_width,right_width,right_x;
    int row_height,model_height,serial_height,android_height,slot_height;
    int system_height,kernel_height;
    int help_x,combo_x,label_x,options_x;
    BOOL show_wake = InterlockedCompareExchange(&screen_is_off,0,0) != 0;
    GetClientRect(hwnd,&r);width=r.right;card_width=width-40;
    content_left=34;content_width=card_width-28;column_gap=18;half=(content_width-column_gap)/2;
    help_x=width-20-quality_help_width;combo_x=help_x-10-quality_combo_width;label_x=combo_x-6-quality_label_width;
    MoveWindow(status_view,20,24,label_x-36,24,TRUE);
    MoveWindow(quality_label,label_x,19,quality_label_width,28,TRUE);
    MoveWindow(quality_combo,combo_x,19,quality_combo_width,180,TRUE);
    MoveWindow(quality_help,help_x,19,quality_help_width,28,TRUE);
    left_width=half;right_width=half;right_x=content_left+left_width+column_gap;y=74;
    model_height=measure_device_view_height(model_view,left_width);
    serial_height=measure_device_view_height(serial_view,right_width);row_height=model_height>serial_height?model_height:serial_height;
    MoveWindow(model_view,content_left,y,left_width,row_height,TRUE);MoveWindow(serial_view,right_x,y,right_width,row_height,TRUE);y+=row_height+6;
    android_height=measure_device_view_height(android_view,left_width);
    slot_height=measure_device_view_height(slot_view,right_width);row_height=android_height>slot_height?android_height:slot_height;
    MoveWindow(android_view,content_left,y,left_width,row_height,TRUE);MoveWindow(slot_view,right_x,y,right_width,row_height,TRUE);y+=row_height+6;
    if(IsWindowVisible(system_view)){
        system_height=measure_device_view_height(system_view,content_width);
        MoveWindow(system_view,content_left,y,content_width,system_height,TRUE);y+=system_height+6;
    }
    kernel_height=measure_device_view_height(kernel_view,content_width);
    MoveWindow(kernel_view,content_left,y,content_width,kernel_height,TRUE);y+=kernel_height;
    main_card_bottom=y+10;
    controls_y=main_card_bottom+12;controls_height=normal_font_height+12;
    if(controls_height<42)controls_height=42;
    log_y=controls_y+controls_height+16;
    ShowWindow(wake_button,SW_SHOW);
    EnableWindow(wake_button,show_wake);
    MoveWindow(wake_button,20,controls_y,130,controls_height,TRUE);
    options_x=width-20-always_on_top_width;
    MoveWindow(always_on_top_check,options_x,controls_y,always_on_top_width,controls_height,TRUE);
    options_x-=option_gap+compat_width;MoveWindow(compat_check,options_x,controls_y,compat_width,controls_height,TRUE);
    options_x-=option_gap+stay_awake_width;MoveWindow(stay_awake_check,options_x,controls_y,stay_awake_width,controls_height,TRUE);
    log_height=r.bottom-log_y-20;if(log_height<1)log_height=1;
    MoveWindow(log_view,20,log_y,card_width-16,log_height,TRUE);
    MoveWindow(log_scroll,20+card_width-16,log_y,16,log_height,TRUE);
}

static BOOL get_log_thumb_rect(HWND scrollbar, RECT *thumb, int *visible_lines, int *total_lines) {
    RECT client,log_client;
    HDC dc;TEXTMETRICW metrics;
    int first,visible,total,height,thumb_height,top;
    GetClientRect(scrollbar,&client);GetClientRect(log_view,&log_client);
    dc=GetDC(log_view);SelectObject(dc,font_log);GetTextMetricsW(dc,&metrics);ReleaseDC(log_view,dc);
    visible=metrics.tmHeight?log_client.bottom/metrics.tmHeight:1;if(visible<1)visible=1;
    total=(int)SendMessageW(log_view,EM_GETLINECOUNT,0,0);if(total<1)total=1;
    if(visible_lines)*visible_lines=visible;
    if(total_lines)*total_lines=total;
    if(total<=visible)return FALSE;
    first=(int)SendMessageW(log_view,EM_GETFIRSTVISIBLELINE,0,0);
    height=client.bottom-client.top-32;if(height<=0)return FALSE;
    thumb_height=height*visible/total;
    if(thumb_height<28)thumb_height=28;
    if(thumb_height>height)thumb_height=height;
    top=16+(height-thumb_height)*first/(total-visible);
    SetRect(thumb,0,top,client.right,top+thumb_height);return TRUE;
}

static void scroll_log_from_thumb(HWND scrollbar, int mouse_y) {
    RECT client,thumb;int visible,total,track,desired,first;
    GetClientRect(scrollbar,&client);
    if(!get_log_thumb_rect(scrollbar,&thumb,&visible,&total))return;
    track=client.bottom-32-(thumb.bottom-thumb.top);if(track<=0)return;
    desired=mouse_y-scroll_drag_offset-16;if(desired<0)desired=0;if(desired>track)desired=track;
    desired=desired*(total-visible)/track;
    first=(int)SendMessageW(log_view,EM_GETFIRSTVISIBLELINE,0,0);
    SendMessageW(log_view,EM_LINESCROLL,0,desired-first);InvalidateRect(scrollbar,NULL,TRUE);
}

static LRESULT CALLBACK log_scroll_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch(message){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT client,thumb;HBRUSH brush;
        GetClientRect(hwnd,&client);brush=CreateSolidBrush(CLR_LOG_BG);FillRect(dc,&client,brush);DeleteObject(brush);
        if(get_log_thumb_rect(hwnd,&thumb,NULL,NULL)){
            HGDIOBJ old_brush,old_pen;POINT up[3]={{4,10},{8,5},{12,10}};POINT down[3];
            int inset=2;
            down[0].x=4;down[0].y=client.bottom-10;down[1].x=12;down[1].y=client.bottom-10;down[2].x=8;down[2].y=client.bottom-5;
            brush=CreateSolidBrush(RGB(151,160,174));old_brush=SelectObject(dc,brush);old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
            Polygon(dc,up,3);Polygon(dc,down,3);SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(brush);
            thumb.left+=inset;thumb.right-=inset;
            brush=CreateSolidBrush(scroll_hover||GetCapture()==hwnd?RGB(220,224,230):RGB(188,194,203));old_brush=SelectObject(dc,brush);old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
            RoundRect(dc,thumb.left,thumb.top,thumb.right,thumb.bottom,12,12);
            SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(brush);
        }
        EndPaint(hwnd,&ps);return 0;
    }
    case WM_LBUTTONDOWN:{
        RECT thumb,client;int y=(short)HIWORD(lparam);
        if(!get_log_thumb_rect(hwnd,&thumb,NULL,NULL))return 0;
        GetClientRect(hwnd,&client);
        if(y<16){SendMessageW(log_view,EM_LINESCROLL,0,-3);InvalidateRect(hwnd,NULL,TRUE);return 0;}
        if(y>=client.bottom-16){SendMessageW(log_view,EM_LINESCROLL,0,3);InvalidateRect(hwnd,NULL,TRUE);return 0;}
        if(y>=thumb.top&&y<=thumb.bottom)scroll_drag_offset=y-thumb.top;
        else{scroll_drag_offset=(thumb.bottom-thumb.top)/2;scroll_log_from_thumb(hwnd,y);}
        SetCapture(hwnd);return 0;
    }
    case WM_MOUSEMOVE:
        if(!scroll_hover){TRACKMOUSEEVENT tracking;ZeroMemory(&tracking,sizeof(tracking));tracking.cbSize=sizeof(tracking);tracking.dwFlags=TME_LEAVE;tracking.hwndTrack=hwnd;TrackMouseEvent(&tracking);scroll_hover=TRUE;InvalidateRect(hwnd,NULL,TRUE);}
        if(GetCapture()==hwnd&&(wparam&MK_LBUTTON)){scroll_log_from_thumb(hwnd,(short)HIWORD(lparam));return 0;}
        break;
    case WM_MOUSELEAVE:scroll_hover=FALSE;InvalidateRect(hwnd,NULL,TRUE);return 0;
    case WM_LBUTTONUP:if(GetCapture()==hwnd)ReleaseCapture();scroll_drag_offset=-1;return 0;
    case WM_MOUSEWHEEL:SendMessageW(log_view,WM_MOUSEWHEEL,wparam,lparam);InvalidateRect(hwnd,NULL,TRUE);return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

static LRESULT CALLBACK log_edit_subclass(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data){
    LRESULT result=DefSubclassProc(hwnd,message,wparam,lparam);(void)id;(void)data;
    if(message==WM_MOUSEWHEEL||message==WM_KEYDOWN||message==WM_VSCROLL)InvalidateRect(log_scroll,NULL,TRUE);
    return result;
}

static void draw_quality_item(const DRAWITEMSTRUCT *item) {
    wchar_t text[40]=L"";RECT r=item->rcItem;BOOL selected=(item->itemState&ODS_SELECTED)!=0;
    HBRUSH brush=CreateSolidBrush(selected?CLR_ACCENT:CLR_CARD);
    FillRect(item->hDC,&r,brush);DeleteObject(brush);
    if(item->itemID!=(UINT)-1)SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,(LPARAM)text);
    else GetWindowTextW(item->hwndItem,text,ARRAYSIZE(text));
    SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,selected?RGB(255,255,255):CLR_TEXT);SelectObject(item->hDC,font_normal);
    r.left+=10;DrawTextW(item->hDC,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
}

static BOOL option_is_checked(UINT id){
    if(id==IDC_STAY_AWAKE)return InterlockedCompareExchange(&stay_awake_mode,0,0)!=0;
    if(id==IDC_COMPAT)return InterlockedCompareExchange(&compat_mode,0,0)!=0;
    if(id==IDC_ALWAYS_ON_TOP)return InterlockedCompareExchange(&always_on_top_mode,0,0)!=0;
    return FALSE;
}

static void draw_option_checkbox(const DRAWITEMSTRUCT *item){
    RECT r=item->rcItem,box,text_rect;BOOL checked=option_is_checked(item->CtlID);
    BOOL disabled=(item->itemState&ODS_DISABLED)!=0,hot=item->hwndItem==hot_button;
    COLORREF border=disabled?RGB(205,210,217):(hot?RGB(142,155,171):CLR_BORDER);
    COLORREF fill=checked?(disabled?RGB(156,162,170):CLR_ACCENT):CLR_BG;
    COLORREF text_color=disabled?RGB(156,162,170):(hot?CLR_TEXT:CLR_MUTED);
    HBRUSH brush;HPEN pen;FillRect(item->hDC,&r,brush_bg);
    brush=CreateSolidBrush(fill);pen=CreatePen(PS_SOLID,1,checked?fill:border);
    HGDIOBJ old_brush=SelectObject(item->hDC,brush),old_pen=SelectObject(item->hDC,pen);
    int side=16,left=r.left+1,top=r.top+(r.bottom-r.top-side)/2;
    SetRect(&box,left,top,left+side,top+side);RoundRect(item->hDC,box.left,box.top,box.right,box.bottom,4,4);
    if(checked){
        HPEN check_pen=CreatePen(PS_SOLID,2,RGB(255,255,255));SelectObject(item->hDC,check_pen);
        MoveToEx(item->hDC,box.left+4,box.top+8,NULL);LineTo(item->hDC,box.left+7,box.top+11);LineTo(item->hDC,box.left+13,box.top+5);
        SelectObject(item->hDC,pen);DeleteObject(check_pen);
    }
    SelectObject(item->hDC,old_pen);SelectObject(item->hDC,old_brush);DeleteObject(pen);DeleteObject(brush);
    text_rect=r;text_rect.left=box.right+7;SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,text_color);
    SelectObject(item->hDC,font_normal);
    {
        wchar_t text[40];GetWindowTextW(item->hwndItem,text,ARRAYSIZE(text));
        DrawTextW(item->hDC,text,-1,&text_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }
    if(item->itemState&ODS_FOCUS){RECT focus=text_rect;InflateRect(&focus,-1,-7);DrawFocusRect(item->hDC,&focus);}
}

static void draw_owner_button(const DRAWITEMSTRUCT *item) {
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL hot=item->hwndItem==hot_button;
    COLORREF color = disabled ? RGB(203,211,222) : (pressed ? CLR_ACCENT_PRESSED : (hot?CLR_ACCENT_HOT:CLR_ACCENT));
    COLORREF text_color=disabled?RGB(75,85,99):RGB(255,255,255);
    HBRUSH brush;HPEN pen;HGDIOBJ old_brush,old_pen;RECT r=item->rcItem; wchar_t text[80];
    if(item->CtlID==IDC_QUALITY_HELP){
        COLORREF fill=pressed?RGB(224,228,234):(hot?RGB(235,238,243):CLR_BG);
        FillRect(item->hDC,&r,brush_bg);
        brush=CreateSolidBrush(fill);pen=CreatePen(PS_SOLID,1,fill);
        old_brush=SelectObject(item->hDC,brush);old_pen=SelectObject(item->hDC,pen);
        RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,6,6);
        SelectObject(item->hDC,old_pen);SelectObject(item->hDC,old_brush);DeleteObject(pen);DeleteObject(brush);
        SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,CLR_MUTED);SelectObject(item->hDC,font_normal);
        DrawTextW(item->hDC,L"帮助  ▾",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if(item->itemState&ODS_FOCUS){InflateRect(&r,-4,-4);DrawFocusRect(item->hDC,&r);}return;
    }
    {
        FillRect(item->hDC,&r,brush_bg);
        brush=CreateSolidBrush(color);pen=CreatePen(PS_SOLID,1,color);old_brush=SelectObject(item->hDC,brush);old_pen=SelectObject(item->hDC,pen);
        RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,6,6);
        SelectObject(item->hDC,old_pen);SelectObject(item->hDC,old_brush);DeleteObject(pen);DeleteObject(brush);
    }
    SetBkMode(item->hDC,TRANSPARENT); SetTextColor(item->hDC,text_color); SelectObject(item->hDC,font_normal);
    GetWindowTextW(item->hwndItem,text,ARRAYSIZE(text)); DrawTextW(item->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(item->itemState&ODS_FOCUS){InflateRect(&r,-4,-4);DrawFocusRect(item->hDC,&r);}
}

static void open_update_page(HWND owner){
    if((INT_PTR)ShellExecuteW(owner,L"open",L"https://optool.daxiaamu.com/muscrcpy",NULL,NULL,SW_SHOWNORMAL)<=32)
        MessageBoxW(owner,L"无法打开更新页面。",L"打开失败",MB_OK|MB_ICONERROR);
}

typedef struct {int divider_y,component_y,homepage_y,content_height;} AboutLayout;

static AboutLayout get_about_layout(void){
    AboutLayout layout;
    int header_bottom=26+48;
    int text_bottom=24+about_title_height+2+normal_font_height;
    if(text_bottom>header_bottom)header_bottom=text_bottom;
    layout.divider_y=header_bottom+16;
    layout.component_y=layout.divider_y+16;
    layout.homepage_y=layout.component_y+normal_font_height+12;
    layout.content_height=layout.homepage_y+normal_font_height+26;
    return layout;
}

static LRESULT CALLBACK about_link_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,
                                        UINT_PTR subclass_id,DWORD_PTR data){
    (void)lparam;(void)subclass_id;(void)data;
    if(message==WM_SETCURSOR){SetCursor(LoadCursor(NULL,IDC_HAND));return TRUE;}
    if(message==WM_KEYDOWN&&(wparam==VK_RETURN||wparam==VK_SPACE)){
        open_update_page(GetParent(hwnd));return 0;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,about_link_proc,1);
    return DefSubclassProc(hwnd,message,wparam,lparam);
}

static LRESULT CALLBACK about_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    HWND owner;
    switch(message){
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW*)lparam)->lpCreateParams);return TRUE;
    case WM_CREATE:{
        RECT client;AboutLayout layout=get_about_layout();HWND link;
        GetClientRect(hwnd,&client);
        link=CreateWindowW(L"STATIC",L"检查更新  →",WS_CHILD|WS_VISIBLE|WS_TABSTOP|SS_NOTIFY|SS_CENTERIMAGE,
                           118,layout.homepage_y-2,client.right-146,normal_font_height+6,
                           hwnd,(HMENU)IDC_ABOUT_LINK,app_instance,NULL);
        SendMessageW(link,WM_SETFONT,(WPARAM)(font_link?font_link:font_normal),TRUE);
        SetWindowSubclass(link,about_link_proc,1,0);
        return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;RECT client,text_rect;HDC dc=BeginPaint(hwnd,&ps);HICON icon;
        HFONT previous_font;HPEN pen,previous_pen;AboutLayout layout=get_about_layout();
        wchar_t version_text[80],component_text[120];
        GetClientRect(hwnd,&client);FillRect(dc,&client,brush_bg);SetBkMode(dc,TRANSPARENT);
        icon=(HICON)LoadImageW(app_instance,MAKEINTRESOURCEW(1),IMAGE_ICON,48,48,LR_DEFAULTCOLOR|LR_SHARED);
        if(icon)DrawIconEx(dc,28,26,icon,48,48,0,NULL,DI_NORMAL);
        previous_font=(HFONT)SelectObject(dc,font_about_title?font_about_title:font_bold);SetTextColor(dc,CLR_TEXT);
        SetRect(&text_rect,92,24,client.right-28,24+about_title_height+4);
        DrawTextW(dc,L"MU投屏",-1,&text_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,font_normal);SetTextColor(dc,CLR_MUTED);
        _snwprintf(version_text,ARRAYSIZE(version_text)-1,L"版本 %ls",APP_VERSION);version_text[ARRAYSIZE(version_text)-1]=0;
        SetRect(&text_rect,92,24+about_title_height+2,client.right-28,24+about_title_height+2+normal_font_height+2);
        DrawTextW(dc,version_text,-1,&text_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        pen=CreatePen(PS_SOLID,1,CLR_BORDER);previous_pen=(HPEN)SelectObject(dc,pen);
        MoveToEx(dc,28,layout.divider_y,NULL);LineTo(dc,client.right-28,layout.divider_y);
        SelectObject(dc,previous_pen);DeleteObject(pen);
        SetTextColor(dc,CLR_MUTED);SetRect(&text_rect,28,layout.component_y,108,layout.component_y+normal_font_height+2);
        DrawTextW(dc,L"内置组件",-1,&text_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SetTextColor(dc,CLR_TEXT);_snwprintf(component_text,ARRAYSIZE(component_text)-1,L"scrcpy %ls",scrcpy_version);
        component_text[ARRAYSIZE(component_text)-1]=0;SetRect(&text_rect,118,layout.component_y,client.right-28,layout.component_y+normal_font_height+2);
        DrawTextW(dc,component_text,-1,&text_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SetTextColor(dc,CLR_MUTED);SetRect(&text_rect,28,layout.homepage_y,108,layout.homepage_y+normal_font_height+2);
        DrawTextW(dc,L"版本更新",-1,&text_rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,previous_font);EndPaint(hwnd,&ps);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==IDC_ABOUT_LINK&&HIWORD(wparam)==STN_CLICKED){open_update_page(hwnd);return 0;}
        if(LOWORD(wparam)==IDCANCEL){DestroyWindow(hwnd);return 0;}break;
    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:
        DestroyWindow(hwnd);return 0;
    case WM_ACTIVATE:
        if(LOWORD(wparam)==WA_INACTIVE){
            HWND next=(HWND)lparam,owner_window=(HWND)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(next!=owner_window&&!IsChild(hwnd,next))DestroyWindow(hwnd);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam,TRANSPARENT);
        SetTextColor((HDC)wparam,GetDlgCtrlID((HWND)lparam)==IDC_ABOUT_LINK?CLR_ACCENT:CLR_TEXT);
        return (LRESULT)brush_bg;
    case WM_CLOSE:DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        owner=(HWND)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        if(about_window==hwnd)about_window=NULL;
        if(owner)SetForegroundWindow(owner);
        return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

static void show_about_window(HWND owner){
    static BOOL registered;WNDCLASSW wc;RECT owner_rect,desired;DWORD style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
    AboutLayout layout;int width,height,x=CW_USEDEFAULT,y=CW_USEDEFAULT;HWND dialog;
    if(about_window&&IsWindow(about_window)){SetForegroundWindow(about_window);return;}
    if(!registered){
        ZeroMemory(&wc,sizeof(wc));wc.lpfnWndProc=about_proc;wc.hInstance=app_instance;
        wc.lpszClassName=L"MUScrcpyAboutWindow";wc.hIcon=NULL;
        wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        registered=TRUE;
    }
    layout=get_about_layout();SetRect(&desired,0,0,360,layout.content_height);
    AdjustWindowRectEx(&desired,style,FALSE,WS_EX_DLGMODALFRAME);
    width=desired.right-desired.left;height=desired.bottom-desired.top;
    if(GetWindowRect(owner,&owner_rect)){
        x=owner_rect.left+((owner_rect.right-owner_rect.left)-width)/2;
        y=owner_rect.top+((owner_rect.bottom-owner_rect.top)-height)/2;
    }
    dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,L"MUScrcpyAboutWindow",L"关于 MU投屏",
                           style,x,y,width,height,
                           owner,NULL,app_instance,owner);
    if(!dialog)return;
    about_window=dialog;ShowWindow(dialog,SW_SHOW);UpdateWindow(dialog);
}

static void refresh_status_text(void){
    wchar_t line[280];
    BOOL projecting=!wcscmp(current_status,L"正在投屏")||!wcscmp(current_status,L"兼容模式投屏中");
    if(projecting&&InterlockedCompareExchange(&screen_is_off,0,0))
        _snwprintf(line,ARRAYSIZE(line)-1,L"●  %ls  ·  手机屏幕已熄灭",current_status);
    else if(projecting)
        _snwprintf(line,ARRAYSIZE(line)-1,L"●  %ls",current_status);
    else
        _snwprintf(line,ARRAYSIZE(line)-1,L"%ls",current_status);
    line[ARRAYSIZE(line)-1]=0;SetWindowTextW(status_view,line);InvalidateRect(status_view,NULL,TRUE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        LOGFONTW lf;HDC screen_dc=GetDC(NULL);int dpi=screen_dc?GetDeviceCaps(screen_dc,LOGPIXELSY):96;if(screen_dc)ReleaseDC(NULL,screen_dc);
        ZeroMemory(&lf,sizeof(lf)); lf.lfHeight=-MulDiv(9,dpi,72);lf.lfQuality=CLEARTYPE_QUALITY;wcscpy(lf.lfFaceName,L"Segoe UI");
        font_normal=CreateFontIndirectW(&lf);lf.lfUnderline=TRUE;font_link=CreateFontIndirectW(&lf);lf.lfUnderline=FALSE;
        lf.lfWeight=FW_SEMIBOLD; font_bold=CreateFontIndirectW(&lf);
        lf.lfHeight=-MulDiv(17,dpi,72);font_about_title=CreateFontIndirectW(&lf);
        lf.lfHeight=-15; lf.lfWeight=FW_NORMAL; wcscpy(lf.lfFaceName,L"Consolas"); font_log=CreateFontIndirectW(&lf);
        {
            HDC dc=GetDC(hwnd);HFONT previous_font;
            if(dc){
                TEXTMETRICW metrics;SIZE text_size;int candidate;previous_font=(HFONT)SelectObject(dc,font_normal);
                if(GetTextMetricsW(dc,&metrics))normal_font_height=metrics.tmHeight;
                if(GetTextExtentPoint32W(dc,L"\x753b\x8d28",2,&text_size)){
                    candidate=text_size.cx+8;if(candidate>quality_label_width)quality_label_width=candidate;
                    candidate=text_size.cx+36;if(candidate>quality_combo_width)quality_combo_width=candidate;
                }
                if(GetTextExtentPoint32W(dc,L"\x5e2e\x52a9  \x25be",5,&text_size)){
                    candidate=text_size.cx+12;if(candidate>quality_help_width)quality_help_width=candidate;
                }
                if(GetTextExtentPoint32W(dc,L"\x4fdd\x6301\x4eae\x5c4f",4,&text_size)){
                    stay_awake_width=text_size.cx+27;
                }
                if(GetTextExtentPoint32W(dc,L"\x517c\x5bb9\x6a21\x5f0f",4,&text_size)){
                    compat_width=text_size.cx+27;
                }
                if(GetTextExtentPoint32W(dc,L"\x6295\x5c4f\x7f6e\x9876",4,&text_size)){
                    always_on_top_width=text_size.cx+27;
                }
                SelectObject(dc,font_about_title);
                if(GetTextMetricsW(dc,&metrics))about_title_height=metrics.tmHeight;
                SelectObject(dc,previous_font);ReleaseDC(hwnd,dc);
            }
            info_row_height=normal_font_height*2+6;
            if(info_row_height<40)info_row_height=40;
            option_gap=normal_font_height/2;
            if(option_gap<6)option_gap=6;
            if(option_gap>14)option_gap=14;
        }
        brush_bg=CreateSolidBrush(CLR_BG); brush_card=CreateSolidBrush(CLR_CARD); brush_log=CreateSolidBrush(CLR_LOG_BG);
        status_view=create_text(hwnd,L"正在初始化"); model_view=create_text(hwnd,L"设备型号\r\n—");
        serial_view=create_text(hwnd,L"设备序列号\r\n—"); android_view=create_text(hwnd,L"Android 版本\r\n—");
        slot_view=create_text(hwnd,L"当前槽位\r\n—"); kernel_view=create_text(hwnd,L"内核版本\r\n—");
        system_view=create_text(hwnd,L"系统版本\r\n—");ShowWindow(system_view,SW_HIDE);
        SetWindowSubclass(model_view,device_text_proc,1,0);SetWindowSubclass(serial_view,device_text_proc,1,0);
        SetWindowSubclass(android_view,device_text_proc,1,0);SetWindowSubclass(slot_view,device_text_proc,1,0);
        SetWindowSubclass(kernel_view,device_text_proc,1,0);
        SetWindowSubclass(system_view,device_text_proc,1,0);
        wake_button=create_button(hwnd,L"点亮屏幕",IDC_WAKE);
        stay_awake_check=create_checkbox(hwnd,L"保持亮屏",IDC_STAY_AWAKE);
        compat_check=create_checkbox(hwnd,L"兼容模式",IDC_COMPAT);
        always_on_top_check=create_checkbox(hwnd,L"投屏置顶",IDC_ALWAYS_ON_TOP);
        quality_label=create_text(hwnd,L"画质");
        SetWindowLongPtrW(quality_label,GWL_STYLE,GetWindowLongPtrW(quality_label,GWL_STYLE)|SS_CENTERIMAGE);
        quality_combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
                                      0,0,0,0,hwnd,(HMENU)IDC_QUALITY,app_instance,NULL);
        SendMessageW(quality_combo,WM_SETFONT,(WPARAM)font_normal,TRUE);
        SetWindowSubclass(quality_combo,quality_combo_proc,1,0);
        {
            COMBOBOXINFO info;ZeroMemory(&info,sizeof(info));info.cbSize=sizeof(info);
            if(GetComboBoxInfo(quality_combo,&info)&&info.hwndList)SetWindowSubclass(info.hwndList,quality_list_proc,1,0);
        }
        SendMessageW(quality_combo,CB_SETITEMHEIGHT,(WPARAM)-1,normal_font_height+2>26?normal_font_height+2:26);
        SendMessageW(quality_combo,CB_SETITEMHEIGHT,0,normal_font_height+6>28?normal_font_height+6:28);
        SendMessageW(quality_combo,CB_ADDSTRING,0,(LPARAM)L"自动");
        SendMessageW(quality_combo,CB_ADDSTRING,0,(LPARAM)L"流畅");
        SendMessageW(quality_combo,CB_ADDSTRING,0,(LPARAM)L"均衡");
        SendMessageW(quality_combo,CB_ADDSTRING,0,(LPARAM)L"高清");
        SendMessageW(quality_combo,CB_ADDSTRING,0,(LPARAM)L"极致");
        SendMessageW(quality_combo,CB_SETCURSEL,QUALITY_AUTO,0);
        quality_help=create_button(hwnd,L"帮助  ▾",IDC_QUALITY_HELP);
        SendMessageW(stay_awake_check,BM_SETCHECK,
                     InterlockedCompareExchange(&stay_awake_mode,0,0)?BST_CHECKED:BST_UNCHECKED,0);
        log_view=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_LEFT|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
                                0,0,0,0,hwnd,(HMENU)IDC_LOG,app_instance,NULL);
        SendMessageW(log_view,WM_SETFONT,(WPARAM)font_log,TRUE);
        SetWindowSubclass(log_view,log_edit_subclass,1,0);
        log_scroll=CreateWindowExW(0,L"MULogScrollbar",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,(HMENU)IDC_LOG_SCROLL,app_instance,NULL);
        return 0;
    }
    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *limits=(MINMAXINFO*)lparam;
            MONITORINFO monitor_info;HMONITOR monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
            RECT window_rect,client_rect;int min_width=680,min_height=600;
            int required_client_width=20+130+normal_font_height+stay_awake_width+compat_width+
                                      always_on_top_width+option_gap*2+20;
            if(GetWindowRect(hwnd,&window_rect)&&GetClientRect(hwnd,&client_rect)){
                int nonclient_width=(window_rect.right-window_rect.left)-(client_rect.right-client_rect.left);
                if(required_client_width+nonclient_width>min_width)min_width=required_client_width+nonclient_width;
            }
            ZeroMemory(&monitor_info,sizeof(monitor_info));monitor_info.cbSize=sizeof(monitor_info);
            if(monitor&&GetMonitorInfoW(monitor,&monitor_info)){
                int work_width=monitor_info.rcWork.right-monitor_info.rcWork.left;
                int work_height=monitor_info.rcWork.bottom-monitor_info.rcWork.top;
                if(min_width>work_width)min_width=work_width;
                if(min_height>work_height)min_height=work_height;
            }
            limits->ptMinTrackSize.x=min_width;limits->ptMinTrackSize.y=min_height;
        }
        return 0;
    case WM_SIZE:
        layout(hwnd);
        RedrawWindow(hwnd,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        return 0;
    case WM_ACTIVATE:
        if(LOWORD(wparam)!=WA_INACTIVE){
            dock_retry_count=0;dock_retry_bring_forward=TRUE;
            if(!dock_scrcpy_window(hwnd,TRUE))SetTimer(hwnd,DOCK_TIMER_ID,200,NULL);
        }
        return 0;
    case WM_APP_DOCK_SCRCPY:
        dock_retry_count=0;dock_retry_bring_forward=FALSE;
        if(!dock_scrcpy_window(hwnd,FALSE))SetTimer(hwnd,DOCK_TIMER_ID,200,NULL);
        return 0;
    case WM_TIMER:
        if(wparam==DOCK_TIMER_ID){
            if(dock_scrcpy_window(hwnd,dock_retry_bring_forward)||++dock_retry_count>=30)KillTimer(hwnd,DOCK_TIMER_ID);
            return 0;
        }
        break;
    case WM_MOVE:
        if(docked_scrcpy_window&&IsWindow(docked_scrcpy_window)&&!IsIconic(hwnd)){
            position_scrcpy_window(hwnd,docked_scrcpy_window,NULL,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam)==IDC_WAKE) {
            HANDLE h=CreateThread(NULL,0,wake_worker,NULL,0,NULL); if(h)CloseHandle(h);
            InterlockedExchange(&screen_is_off,0); layout(hwnd); InvalidateRect(hwnd,NULL,TRUE);
        }
        else if(LOWORD(wparam)==IDC_QUALITY_HELP){
            HMENU menu=CreatePopupMenu();RECT button_rect;UINT selected;
            AppendMenuW(menu,MF_STRING,IDM_QUALITY_HELP,L"画质说明");
            AppendMenuW(menu,MF_SEPARATOR,0,NULL);
            AppendMenuW(menu,MF_STRING,IDM_CHECK_UPDATE,L"检查更新");
            AppendMenuW(menu,MF_STRING,IDM_ABOUT,L"关于");
            GetWindowRect(quality_help,&button_rect);
            selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTALIGN|TPM_TOPALIGN,button_rect.right,button_rect.bottom,0,hwnd,NULL);
            DestroyMenu(menu);
            if(selected==IDM_QUALITY_HELP){
                MessageBoxW(hwnd,
                    L"自动：根据电脑处理器、内存和手机分辨率自动选择。\r\n\r\n"
                    L"流畅：1280 / 30 FPS / 4M\r\n"
                    L"均衡：1920 / 45 FPS / 8M\r\n"
                    L"高清：2560 / 60 FPS / 12M\r\n"
                    L"极致：原始分辨率 / 最高 120 FPS / 20M\r\n\r\n"
                    L"切换画质后 scrcpy 会自动重启。设备编码器不支持目标帧率时，会使用设备允许的最高帧率。",
                    L"画质说明",MB_OK|MB_ICONINFORMATION);
            }else if(selected==IDM_CHECK_UPDATE){
                open_update_page(hwnd);
            }else if(selected==IDM_ABOUT){
                show_about_window(hwnd);
            }
        }
        else if (LOWORD(wparam)==IDC_COMPAT && HIWORD(wparam)==BN_CLICKED) {
            BOOL enabled = InterlockedCompareExchange(&compat_mode,0,0)==0;
            InterlockedExchange(&compat_mode,enabled?1:0);
            InvalidateRect(compat_check,NULL,TRUE);
            post_log(enabled?L"已启用兼容模式，正在以软件渲染、无音频参数重启 scrcpy。":L"已关闭兼容模式，正在以普通模式重启 scrcpy。");
            terminate_scrcpy();
        } else if (LOWORD(wparam)==IDC_STAY_AWAKE && HIWORD(wparam)==BN_CLICKED) {
            BOOL enabled = InterlockedCompareExchange(&stay_awake_mode,0,0)==0;
            InterlockedExchange(&stay_awake_mode,enabled?1:0);
            InvalidateRect(stay_awake_check,NULL,TRUE);
            if (!save_keep_awake(enabled)) post_log(L"保持亮屏设置保存失败，下次启动将使用默认值。");
            post_log(enabled?L"已开启保持亮屏，正在重启 scrcpy 使设置生效。":L"已关闭保持亮屏，正在重启 scrcpy 并恢复设备原状态。");
            terminate_scrcpy();
        } else if (LOWORD(wparam)==IDC_ALWAYS_ON_TOP && HIWORD(wparam)==BN_CLICKED) {
            BOOL enabled = InterlockedCompareExchange(&always_on_top_mode,0,0)==0;
            InterlockedExchange(&always_on_top_mode,enabled?1:0);
            InvalidateRect(always_on_top_check,NULL,TRUE);
            post_log(enabled?L"已开启投屏置顶，正在重启 scrcpy 使设置生效。":L"已关闭投屏置顶，正在重启 scrcpy 使设置生效。");
            terminate_scrcpy();
        } else if (LOWORD(wparam)==IDC_QUALITY && HIWORD(wparam)==CBN_SELCHANGE) {
            int selected=(int)SendMessageW(quality_combo,CB_GETCURSEL,0,0);
            const wchar_t *names[]={L"自动",L"流畅",L"均衡",L"高清",L"极致"};
            wchar_t log_message[120];
            if(selected<QUALITY_AUTO||selected>QUALITY_ULTRA) selected=QUALITY_AUTO;
            InterlockedExchange(&quality_mode,selected);
            _snwprintf(log_message,ARRAYSIZE(log_message)-1,L"画质已切换为“%ls”，正在重启 scrcpy。",names[selected]);
            log_message[ARRAYSIZE(log_message)-1]=0;
            post_log(log_message);terminate_scrcpy();
        } return 0;
    case WM_DRAWITEM:
        if(((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_QUALITY)draw_quality_item((const DRAWITEMSTRUCT*)lparam);
        else if(((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_STAY_AWAKE||((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_COMPAT||((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_ALWAYS_ON_TOP)draw_option_checkbox((const DRAWITEMSTRUCT*)lparam);
        else draw_owner_button((const DRAWITEMSTRUCT*)lparam);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam,TRANSPARENT);
        if((HWND)lparam==log_view){SetTextColor((HDC)wparam,CLR_LOG_TEXT);SetBkColor((HDC)wparam,CLR_LOG_BG);return(LRESULT)brush_log;}
        if((HWND)lparam==status_view){
            BOOL projecting=!wcscmp(current_status,L"正在投屏")||!wcscmp(current_status,L"兼容模式投屏中");
            COLORREF status_color=!wcscmp(current_status,L"缺少运行文件")?CLR_ACCENT_PRESSED:
                                  (projecting?(InterlockedCompareExchange(&screen_is_off,0,0)?RGB(217,119,6):CLR_GREEN):CLR_MUTED);
            SetTextColor((HDC)wparam,status_color);
        }else SetTextColor((HDC)wparam,(HWND)lparam==quality_label?CLR_MUTED:CLR_TEXT);
        if((HWND)lparam==model_view||(HWND)lparam==serial_view||(HWND)lparam==android_view||(HWND)lparam==slot_view||(HWND)lparam==kernel_view||(HWND)lparam==system_view)return(LRESULT)brush_card;
        return (LRESULT)brush_bg;
    case WM_CTLCOLOREDIT:
        if((HWND)lparam==log_view){SetTextColor((HDC)wparam,CLR_LOG_TEXT);SetBkColor((HDC)wparam,CLR_LOG_BG);return(LRESULT)brush_log;} break;
    case WM_CTLCOLORBTN:
        if((HWND)lparam==stay_awake_check||(HWND)lparam==compat_check||(HWND)lparam==always_on_top_check){SetBkMode((HDC)wparam,TRANSPARENT);SetTextColor((HDC)wparam,CLR_MUTED);return(LRESULT)brush_bg;} break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT r,card;
        GetClientRect(hwnd,&r); FillRect(dc,&r,brush_bg); SetBkMode(dc,TRANSPARENT);
        SetRect(&card,20,60,r.right-20,main_card_bottom);FillRect(dc,&card,brush_card);
        EndPaint(hwnd,&ps);
        if(InterlockedCompareExchange(&workers_started,1,0)==0)PostMessageW(hwnd,WM_APP_START_WORKERS,0,0);
        return 0;
    }
    case WM_APP_START_WORKERS:
        worker_handle=CreateThread(NULL,0,launcher_worker,NULL,0,NULL);
        screen_worker_handle=CreateThread(NULL,0,screen_monitor_worker,NULL,0,NULL);
        return 0;
    case WM_APP_LOG: {
        wchar_t *s=(wchar_t*)lparam;int length=GetWindowTextLengthW(log_view);
        if(length>60000){SendMessageW(log_view,EM_SETSEL,0,20000);SendMessageW(log_view,EM_REPLACESEL,FALSE,(LPARAM)L"");}
        SendMessageW(log_view,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);SendMessageW(log_view,EM_REPLACESEL,FALSE,(LPARAM)s);
        InvalidateRect(log_scroll,NULL,TRUE);
        if(s) HeapFree(GetProcessHeap(),0,s);
        return 0;
    }
    case WM_APP_STATUS: {
        wchar_t *s=(wchar_t*)lparam;
        if(s){wcsncpy(current_status,s,ARRAYSIZE(current_status)-1);current_status[ARRAYSIZE(current_status)-1]=0;}
        else current_status[0]=0;
        refresh_status_text();
        if(s) HeapFree(GetProcessHeap(),0,s);
        return 0;
    }
    case WM_APP_DEVICE_STATE:
        device_info_fresh=(BOOL)wparam;
        InvalidateRect(model_view,NULL,FALSE);InvalidateRect(serial_view,NULL,FALSE);
        InvalidateRect(android_view,NULL,FALSE);InvalidateRect(slot_view,NULL,FALSE);
        InvalidateRect(kernel_view,NULL,FALSE);InvalidateRect(system_view,NULL,FALSE);
        return 0;
    case WM_APP_DEVICE: {
        DeviceInfo *d=(DeviceInfo*)lparam;wchar_t text[800];
        device_info_fresh=TRUE;
        if(d->market_name[0]) _snwprintf(text,ARRAYSIZE(text)-1,L"设备型号\r\n%ls（%ls）",d->market_name,d->model);
        else _snwprintf(text,ARRAYSIZE(text)-1,L"设备型号\r\n%ls",d->model);
        SetWindowTextW(model_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"设备序列号\r\n%ls",d->serial);SetWindowTextW(serial_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"Android 版本\r\n%ls",d->android);SetWindowTextW(android_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"当前槽位\r\n%ls",d->slot);SetWindowTextW(slot_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"内核版本\r\n%ls",d->kernel);SetWindowTextW(kernel_view,text);
        wcscpy(text,L"系统版本");
        if(d->build_display[0]){wcscat(text,L"\r\n");wcscat(text,d->build_display);}
        if(d->rom_version[0]){wcscat(text,L"\r\n");wcscat(text,d->rom_version);}
        if(d->ota_version[0]){wcscat(text,L"\r\n");wcscat(text,d->ota_version);}
        if(d->build_display[0]||d->rom_version[0]||d->ota_version[0]){SetWindowTextW(system_view,text);ShowWindow(system_view,SW_SHOW);}
        else ShowWindow(system_view,SW_HIDE);
        layout(hwnd);
        InvalidateRect(model_view,NULL,FALSE);InvalidateRect(serial_view,NULL,FALSE);
        InvalidateRect(android_view,NULL,FALSE);InvalidateRect(slot_view,NULL,FALSE);
        InvalidateRect(kernel_view,NULL,FALSE);InvalidateRect(system_view,NULL,FALSE);
        HeapFree(GetProcessHeap(),0,d);return 0;
    }
    case WM_APP_SCREEN:
        InterlockedExchange(&screen_is_off,(LONG)wparam);
        refresh_status_text();layout(hwnd);InvalidateRect(hwnd,NULL,TRUE);return 0;
    case WM_CLOSE:
        KillTimer(hwnd,DOCK_TIMER_ID);
        EnableWindow(hwnd,FALSE);SetEvent(stop_event);terminate_scrcpy();
        if(worker_handle)WaitForSingleObject(worker_handle,3000);
        if(screen_worker_handle)WaitForSingleObject(screen_worker_handle,3000);
        DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        if(worker_handle) CloseHandle(worker_handle);
        if(screen_worker_handle) CloseHandle(screen_worker_handle);
        DeleteObject(font_normal);DeleteObject(font_bold);DeleteObject(font_log);DeleteObject(font_about_title);DeleteObject(font_link);
        DeleteObject(brush_bg);DeleteObject(brush_card);DeleteObject(brush_log);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command_line,int show) {
    WNDCLASSEXW wc;WNDCLASSW scroll_class;MSG message;HWND existing;RECT work_area;
    int initial_width=680,initial_height=660;
    (void)previous;(void)command_line;app_instance=instance;
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetDpiAwareFn)(void);
        union { FARPROC raw; SetDpiAwareFn call; } set_dpi_aware;
        set_dpi_aware.raw = user32 ? GetProcAddress(user32, "SetProcessDPIAware") : NULL;
        if (set_dpi_aware.call) set_dpi_aware.call();
    }
    mutex_handle=CreateMutexW(NULL,TRUE,APP_MUTEX);if(!mutex_handle)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){existing=FindWindowW(L"MUScrcpyGuiWindow",APP_TITLE);if(existing){if(IsIconic(existing))ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}CloseHandle(mutex_handle);return 0;}
    load_preferences();
    InitializeCriticalSection(&process_lock);stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);InitCommonControls();
    if(initialize_bin_directory())detect_scrcpy_version();
    ZeroMemory(&wc,sizeof(wc));wc.cbSize=sizeof(wc);wc.style=CS_HREDRAW|CS_VREDRAW;wc.hInstance=instance;wc.lpfnWndProc=window_proc;wc.lpszClassName=L"MUScrcpyGuiWindow";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hIcon=LoadIcon(instance,MAKEINTRESOURCE(1));wc.hIconSm=wc.hIcon;wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassExW(&wc))return 1;
    ZeroMemory(&scroll_class,sizeof(scroll_class));scroll_class.hInstance=instance;scroll_class.lpfnWndProc=log_scroll_proc;
    scroll_class.lpszClassName=L"MULogScrollbar";scroll_class.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassW(&scroll_class))return 1;
    if(SystemParametersInfoW(SPI_GETWORKAREA,0,&work_area,0)){
        int work_width=work_area.right-work_area.left,work_height=work_area.bottom-work_area.top;
        if(initial_width>work_width)initial_width=work_width;
        if(initial_height>work_height)initial_height=work_height;
    }
    main_window=CreateWindowExW(0,wc.lpszClassName,APP_TITLE,WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,
                              initial_width,initial_height,NULL,NULL,instance,NULL);
    if(!main_window) return 1;
    ShowWindow(main_window,show);UpdateWindow(main_window);
    while(GetMessageW(&message,NULL,0,0)>0){
        BOOL mouse_down=message.message==WM_LBUTTONDOWN||message.message==WM_RBUTTONDOWN||message.message==WM_MBUTTONDOWN||
                        message.message==WM_NCLBUTTONDOWN||message.message==WM_NCRBUTTONDOWN||message.message==WM_NCMBUTTONDOWN;
        BOOL mouse_up=message.message==WM_LBUTTONUP||message.message==WM_RBUTTONUP||message.message==WM_MBUTTONUP||
                      message.message==WM_NCLBUTTONUP||message.message==WM_NCRBUTTONUP||message.message==WM_NCMBUTTONUP;
        if(suppress_outside_click_up&&mouse_up){suppress_outside_click_up=FALSE;continue;}
        if(about_window&&IsWindow(about_window)){
            if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){DestroyWindow(about_window);continue;}
            if(mouse_down&&message.hwnd!=about_window&&!IsChild(about_window,message.hwnd)){
                DestroyWindow(about_window);suppress_outside_click_up=TRUE;continue;
            }
            if(IsDialogMessageW(about_window,&message))continue;
        }
        TranslateMessage(&message);DispatchMessageW(&message);
    }
    CloseHandle(stop_event);CloseHandle(mutex_handle);DeleteCriticalSection(&process_lock);return(int)message.wParam;
}
