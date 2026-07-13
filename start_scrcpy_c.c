#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define APP_VERSION L"2.3"
#define APP_TITLE L"MU投屏 " APP_VERSION L" daxiaamu.com"
#define APP_MUTEX L"Daxiaamu.MUScrcpy.GUI.SingleInstance"
#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DEVICE (WM_APP + 2)
#define WM_APP_STATUS (WM_APP + 3)
#define WM_APP_SCREEN (WM_APP + 4)
#define WM_APP_START_WORKERS (WM_APP + 5)
#define WM_APP_DOCK_SCRCPY (WM_APP + 6)
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
#define IDC_ABOUT_LOGO 2201
#define IDC_ABOUT_TITLE 2202
#define IDC_ABOUT_INFO 2203
#define IDC_ABOUT_UPDATE 2204
#define IDC_ABOUT_SCRCPY 2205

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
static HFONT font_normal, font_bold, font_log, font_about_title;
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
    (void)ignored;
    if(!d)return 1;
    read_device_info(d);
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){HeapFree(GetProcessHeap(),0,d);return 0;}
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

static BOOL dock_scrcpy_window(HWND gui, BOOL bring_forward) {
    HWND scrcpy=find_scrcpy_window();
    RECT gui_rect,gui_visible,scrcpy_rect,scrcpy_visible;
    MONITORINFO monitor_info;
    int scrcpy_width,x,y;
    if(!scrcpy||!IsWindow(scrcpy)||IsIconic(gui))return FALSE;
    if(IsIconic(scrcpy))ShowWindow(scrcpy,SW_SHOWNOACTIVATE);
    GetWindowRect(gui,&gui_rect);GetWindowRect(scrcpy,&scrcpy_rect);
    get_visible_window_rect(gui,&gui_visible);get_visible_window_rect(scrcpy,&scrcpy_visible);
    scrcpy_width=scrcpy_visible.right-scrcpy_visible.left;
    ZeroMemory(&monitor_info,sizeof(monitor_info));monitor_info.cbSize=sizeof(monitor_info);
    GetMonitorInfoW(MonitorFromWindow(gui,MONITOR_DEFAULTTONEAREST),&monitor_info);
    if(gui_visible.right+scrcpy_width<=monitor_info.rcWork.right)dock_side=1;
    else if(gui_visible.left-scrcpy_width>=monitor_info.rcWork.left)dock_side=-1;
    else dock_side=(monitor_info.rcWork.right-gui_visible.right>=gui_visible.left-monitor_info.rcWork.left)?1:-1;
    x=dock_side>0?gui_visible.right-(scrcpy_visible.left-scrcpy_rect.left):
                   gui_visible.left-scrcpy_width-(scrcpy_visible.left-scrcpy_rect.left);
    y=scrcpy_rect.top;
    SetWindowPos(scrcpy,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
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
    if (compatible) wcscat(command,L" …5994 tokens truncated…PtrW(hwnd,GWLP_USERDATA);
            if(next!=owner_window&&!IsChild(hwnd,next))DestroyWindow(hwnd);
        }
        return 0;
    case WM_DRAWITEM:
        if(wparam==IDC_ABOUT_UPDATE){draw_owner_button((const DRAWITEMSTRUCT*)lparam);return TRUE;}break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam,TRANSPARENT);
        SetTextColor((HDC)wparam,(GetDlgCtrlID((HWND)lparam)==IDC_ABOUT_INFO||GetDlgCtrlID((HWND)lparam)==IDC_ABOUT_SCRCPY)?CLR_MUTED:CLR_TEXT);
        return (LRESULT)brush_bg;
    case WM_CLOSE:DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        owner=(HWND)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        if(hot_button&&IsChild(hwnd,hot_button))hot_button=NULL;
        if(about_window==hwnd)about_window=NULL;
        if(owner)SetForegroundWindow(owner);
        return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

static void show_about_window(HWND owner){
    static BOOL registered;WNDCLASSW wc;RECT owner_rect;int width=380,height=330,x=CW_USEDEFAULT,y=CW_USEDEFAULT;HWND dialog;
    if(about_window&&IsWindow(about_window)){SetForegroundWindow(about_window);return;}
    if(!registered){
        ZeroMemory(&wc,sizeof(wc));wc.lpfnWndProc=about_proc;wc.hInstance=app_instance;
        wc.lpszClassName=L"MUScrcpyAboutWindow";wc.hIcon=LoadIcon(app_instance,MAKEINTRESOURCE(1));
        wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        registered=TRUE;
    }
    if(GetWindowRect(owner,&owner_rect)){
        x=owner_rect.left+((owner_rect.right-owner_rect.left)-width)/2;
        y=owner_rect.top+((owner_rect.bottom-owner_rect.top)-height)/2;
    }
    dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,L"MUScrcpyAboutWindow",L"关于 MU投屏",
                           WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,x,y,width,height,
                           owner,NULL,app_instance,owner);
    if(!dialog)return;
    about_window=dialog;ShowWindow(dialog,SW_SHOW);UpdateWindow(dialog);
}

static void refresh_status_text(void){
    wchar_t line[280];
    if(InterlockedCompareExchange(&screen_is_off,0,0))
        _snwprintf(line,ARRAYSIZE(line)-1,L"●  %ls  ·  手机屏幕已熄灭",current_status);
    else
        _snwprintf(line,ARRAYSIZE(line)-1,L"●  %ls",current_status);
    line[ARRAYSIZE(line)-1]=0;SetWindowTextW(status_view,line);InvalidateRect(status_view,NULL,TRUE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        LOGFONTW lf;HDC screen_dc=GetDC(NULL);int dpi=screen_dc?GetDeviceCaps(screen_dc,LOGPIXELSY):96;if(screen_dc)ReleaseDC(NULL,screen_dc);
        ZeroMemory(&lf,sizeof(lf)); lf.lfHeight=-MulDiv(9,dpi,72);lf.lfQuality=CLEARTYPE_QUALITY;wcscpy(lf.lfFaceName,L"Segoe UI");
        font_normal=CreateFontIndirectW(&lf); lf.lfWeight=FW_SEMIBOLD; font_bold=CreateFontIndirectW(&lf);
        lf.lfHeight=-MulDiv(17,dpi,72);font_about_title=CreateFontIndirectW(&lf);
        lf.lfHeight=-15; lf.lfWeight=FW_NORMAL; wcscpy(lf.lfFaceName,L"Consolas"); font_log=CreateFontIndirectW(&lf);
        brush_bg=CreateSolidBrush(CLR_BG); brush_card=CreateSolidBrush(CLR_CARD); brush_log=CreateSolidBrush(CLR_LOG_BG);
        status_view=create_text(hwnd,L"●  正在初始化"); model_view=create_text(hwnd,L"设备型号\r\n—");
        serial_view=create_text(hwnd,L"设备序列号\r\n—"); android_view=create_text(hwnd,L"Android 版本\r\n—");
        slot_view=create_text(hwnd,L"当前槽位\r\n—"); kernel_view=create_text(hwnd,L"内核版本\r\n—");
        system_view=create_text(hwnd,L"系统版本\r\n—");ShowWindow(system_view,SW_HIDE);
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
        SendMessageW(quality_combo,CB_SETITEMHEIGHT,(WPARAM)-1,26);SendMessageW(quality_combo,CB_SETITEMHEIGHT,0,28);
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
        ((MINMAXINFO*)lparam)->ptMinTrackSize.x=700;
        ((MINMAXINFO*)lparam)->ptMinTrackSize.y=600;
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
            RECT gui_visible,scrcpy_rect,scrcpy_visible;int x,visible_width;
            get_visible_window_rect(hwnd,&gui_visible);GetWindowRect(docked_scrcpy_window,&scrcpy_rect);
            get_visible_window_rect(docked_scrcpy_window,&scrcpy_visible);
            visible_width=scrcpy_visible.right-scrcpy_visible.left;
            x=dock_side>0?gui_visible.right-(scrcpy_visible.left-scrcpy_rect.left):
                           gui_visible.left-visible_width-(scrcpy_visible.left-scrcpy_rect.left);
            SetWindowPos(docked_scrcpy_window,NULL,x,scrcpy_rect.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
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
            wchar_t message[120];
            if(selected<QUALITY_AUTO||selected>QUALITY_ULTRA) selected=QUALITY_AUTO;
            InterlockedExchange(&quality_mode,selected);
            _snwprintf(message,ARRAYSIZE(message)-1,L"画质已切换为“%ls”，正在重启 scrcpy。",names[selected]);
            post_log(message);terminate_scrcpy();
        } return 0;
    case WM_DRAWITEM:
        if(((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_QUALITY)draw_quality_item((const DRAWITEMSTRUCT*)lparam);
        else if(((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_STAY_AWAKE||((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_COMPAT||((const DRAWITEMSTRUCT*)lparam)->CtlID==IDC_ALWAYS_ON_TOP)draw_option_checkbox((const DRAWITEMSTRUCT*)lparam);
        else draw_owner_button((const DRAWITEMSTRUCT*)lparam);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wparam,TRANSPARENT);
        if((HWND)lparam==log_view){SetTextColor((HDC)wparam,CLR_LOG_TEXT);SetBkColor((HDC)wparam,CLR_LOG_BG);return(LRESULT)brush_log;}
        SetTextColor((HDC)wparam,(HWND)lparam==status_view?
                     (InterlockedCompareExchange(&screen_is_off,0,0)?RGB(217,119,6):CLR_GREEN):
                     ((HWND)lparam==quality_label?CLR_MUTED:CLR_TEXT));
        if((HWND)lparam==model_view||(HWND)lparam==serial_view||(HWND)lparam==android_view||(HWND)lparam==slot_view||(HWND)lparam==kernel_view||(HWND)lparam==system_view)return(LRESULT)brush_card;
        return (LRESULT)brush_bg;
    case WM_CTLCOLOREDIT:
        if((HWND)lparam==log_view){SetTextColor((HDC)wparam,CLR_LOG_TEXT);SetBkColor((HDC)wparam,CLR_LOG_BG);return(LRESULT)brush_log;} break;
    case WM_CTLCOLORBTN:
        if((HWND)lparam==stay_awake_check||(HWND)lparam==compat_check||(HWND)lparam==always_on_top_check){SetBkMode((HDC)wparam,TRANSPARENT);SetTextColor((HDC)wparam,CLR_MUTED);return(LRESULT)brush_bg;} break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT r,card;
        GetClientRect(hwnd,&r); FillRect(dc,&r,brush_bg); SetBkMode(dc,TRANSPARENT);
        SetRect(&card,28,60,r.right-28,295);FillRect(dc,&card,brush_card);
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
    case WM_APP_DEVICE: {
        DeviceInfo *d=(DeviceInfo*)lparam;wchar_t text[800];
        if(d->market_name[0]) _snwprintf(text,ARRAYSIZE(text)-1,L"设备型号\r\n%ls（%ls）",d->market_name,d->model);
        else _snwprintf(text,ARRAYSIZE(text)-1,L"设备型号\r\n%ls",d->model);
        SetWindowTextW(model_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"设备序列号\r\n%ls",d->serial);SetWindowTextW(serial_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"Android 版本\r\n%ls",d->android);SetWindowTextW(android_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"当前槽位\r\n%ls",d->slot);SetWindowTextW(slot_view,text);
        _snwprintf(text,ARRAYSIZE(text)-1,L"内核版本\r\n%ls",d->kernel);SetWindowTextW(kernel_view,text);
        wcscpy(text,L"系统版本\r\n");
        if(d->build_display[0]) wcscat(text,d->build_display);
        if(d->rom_version[0]){if(d->build_display[0])wcscat(text,L"    |    ");wcscat(text,d->rom_version);}
        if(d->ota_version[0]){if(d->build_display[0]||d->rom_version[0])wcscat(text,L"    |    ");wcscat(text,d->ota_version);}
        if(d->build_display[0]||d->rom_version[0]||d->ota_version[0]){SetWindowTextW(system_view,text);ShowWindow(system_view,SW_SHOW);}
        else ShowWindow(system_view,SW_HIDE);
        HeapFree(GetProcessHeap(),0,d);return 0;
    }
    case WM_APP_SCREEN:
        InterlockedExchange(&screen_is_off,(LONG)wparam);
        refresh_status_text();layout(hwnd);InvalidateRect(hwnd,NULL,TRUE);return 0;
    case WM_CLOSE:
        KillTimer(hwnd,DOCK_TIMER_ID);EnableWindow(hwnd,FALSE);SetEvent(stop_event);terminate_scrcpy();
        if(worker_handle)WaitForSingleObject(worker_handle,3000);
        if(screen_worker_handle)WaitForSingleObject(screen_worker_handle,3000);
        DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        if(worker_handle) CloseHandle(worker_handle);
        if(screen_worker_handle) CloseHandle(screen_worker_handle);
        DeleteObject(font_normal);DeleteObject(font_bold);DeleteObject(font_log);DeleteObject(font_about_title);
        DeleteObject(brush_bg);DeleteObject(brush_card);DeleteObject(brush_log);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command_line,int show) {
    WNDCLASSEXW wc;WNDCLASSW scroll_class;MSG message;HWND existing;
    (void)previous;(void)command_line;app_instance=instance;
    mutex_handle=CreateMutexW(NULL,TRUE,APP_MUTEX);if(!mutex_handle)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){existing=FindWindowW(L"MUScrcpyGuiWindow",APP_TITLE);if(existing){if(IsIconic(existing))ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}CloseHandle(mutex_handle);return 0;}
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetDpiAwareFn)(void);
        union { FARPROC raw; SetDpiAwareFn call; } set_dpi_aware;
        set_dpi_aware.raw = user32 ? GetProcAddress(user32, "SetProcessDPIAware") : NULL;
        if (set_dpi_aware.call) set_dpi_aware.call();
    }
    load_preferences();
    InitializeCriticalSection(&process_lock);stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);InitCommonControls();
    if(initialize_bin_directory())detect_scrcpy_version();
    ZeroMemory(&wc,sizeof(wc));wc.cbSize=sizeof(wc);wc.style=CS_HREDRAW|CS_VREDRAW;wc.hInstance=instance;wc.lpfnWndProc=window_proc;wc.lpszClassName=L"MUScrcpyGuiWindow";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hIcon=LoadIcon(instance,MAKEINTRESOURCE(1));wc.hIconSm=wc.hIcon;wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassExW(&wc))return 1;
    ZeroMemory(&scroll_class,sizeof(scroll_class));scroll_class.hInstance=instance;scroll_class.lpfnWndProc=log_scroll_proc;
    scroll_class.lpszClassName=L"MULogScrollbar";scroll_class.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassW(&scroll_class))return 1;
    main_window=CreateWindowExW(0,wc.lpszClassName,APP_TITLE,WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,700,660,NULL,NULL,instance,NULL);
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