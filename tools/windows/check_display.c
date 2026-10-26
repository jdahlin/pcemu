// Original ARM64 fullscreen GDI pattern for headless VirtIO GPU acceptance.
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef __WCHAR_TYPE__ WCHAR;
typedef unsigned long long WPARAM;
typedef long long LPARAM;
typedef long long LRESULT;
typedef struct {
    int left, top, right, bottom;
} RECT;
typedef struct {
    HANDLE dc;
    int erase;
    RECT rect;
    int restore, inc;
    unsigned char reserved[32];
} PAINT;
typedef struct {
    HANDLE window;
    DWORD message;
    WPARAM wparam;
    LPARAM lparam;
    DWORD time;
    int x, y;
    DWORD private;
} MSG;
typedef struct {
    DWORD style;
    LRESULT (*proc)(HANDLE, DWORD, WPARAM, LPARAM);
    int clsExtra, wndExtra;
    HANDLE instance, icon, cursor, brush;
    const WCHAR *menu, *name;
} WNDCLASS;
#define DLL __declspec(dllimport)
DLL unsigned short RegisterClassW(const WNDCLASS *);
DLL HANDLE CreateWindowExW(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HANDLE,
                           HANDLE, HANDLE, void *);
DLL LRESULT DefWindowProcW(HANDLE, DWORD, WPARAM, LPARAM);
DLL int GetMessageW(MSG *, HANDLE, DWORD, DWORD);
DLL int TranslateMessage(const MSG *);
DLL LRESULT DispatchMessageW(const MSG *);
DLL HANDLE BeginPaint(HANDLE, PAINT *);
DLL int EndPaint(HANDLE, const PAINT *);
DLL int GetClientRect(HANDLE, RECT *);
DLL int FillRect(HANDLE, const RECT *, HANDLE);
DLL int GetSystemMetrics(int);
DLL int UpdateWindow(HANDLE);
DLL void PostQuitMessage(int);
DLL HANDLE CreateSolidBrush(DWORD);
DLL int DeleteObject(HANDLE);
DLL HANDLE GetModuleHandleW(const WCHAR *);
DLL void ExitProcess(DWORD);
DLL int EnumDisplayDevicesW(const WCHAR *, DWORD, void *, DWORD);
DLL int EnumDisplaySettingsW(const WCHAR *, DWORD, void *);
DLL long ChangeDisplaySettingsExW(const WCHAR *, void *, HANDLE, DWORD, void *);
DLL HANDLE GetStdHandle(DWORD);
DLL int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
static void text(const char *s) {
    DWORD n = 0, w;
    while (s[n])
        ++n;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0);
}
static int contains(const WCHAR *s, const char *part) {
    for (unsigned at = 0; s[at]; ++at) {
        unsigned i = 0;
        while (part[i] && s[at + i] == (unsigned char)part[i])
            ++i;
        if (!part[i])
            return 1;
    }
    return 0;
}
static void put32(unsigned char *p, DWORD value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}
static int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;
typedef struct {
    DWORD size;
    WCHAR name[32], description[128];
    DWORD flags;
    WCHAR id[128], key[128];
} DISPLAY;
typedef union {
    unsigned long long align;
    unsigned char bytes[220];
} MODE;
static DWORD get32(const unsigned char *p) {
    DWORD n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n |= (DWORD)p[i] << (i * 8);
    return n;
}
static int getMode(const WCHAR *name, MODE *mode) {
    for (unsigned i = 0; i < sizeof(mode->bytes); ++i)
        mode->bytes[i] = 0;
    mode->bytes[68] = 220;
    return EnumDisplaySettingsW(name, (DWORD)-1, mode->bytes);
}
static void number(long n) {
    char digits[12];
    unsigned at = 11;
    digits[at] = 0;
    if (n < 0) {
        text("-");
        n = -n;
    }
    do {
        digits[--at] = '0' + n % 10;
        n /= 10;
    } while (n);
    text(digits + at);
}
static void selectGpu(void) {
    DISPLAY display;
    WCHAR target[32];
    int found = 0;
    for (unsigned i = 0; i < 16; ++i) {
        display.size = sizeof(display);
        if (!EnumDisplayDevicesW(0, i, &display, 0))
            break;
        if (!contains(display.id, "DEV_1050"))
            continue;
        for (unsigned j = 0; j < 32; ++j)
            target[j] = display.name[j];
        found = 1;
        break;
    }
    if (!found) {
        text("PCEMU_GPU_DISPLAY_NOT_FOUND\r\n");
        return;
    }
    text("PCEMU_GPU_DISPLAY_FOUND\r\n");
    MODE mode;
    if (!getMode(target, &mode)) {
        text("PCEMU_GPU_MODE_NOT_FOUND\r\n");
        return;
    }
    DWORD right = get32(mode.bytes + 172);
    // Stage all positions together so moving the primary cannot create an overlap.
    for (unsigned i = 0; i < 16; ++i) {
        display.size = sizeof(display);
        if (!EnumDisplayDevicesW(0, i, &display, 0))
            break;
        if (!(display.flags & 1) || contains(display.id, "DEV_1050"))
            continue;
        MODE other;
        if (!getMode(display.name, &other))
            continue;
        put32(other.bytes + 72, 0x20);
        put32(other.bytes + 76, right);
        put32(other.bytes + 80, 0);
        right += get32(other.bytes + 172);
        ChangeDisplaySettingsExW(display.name, other.bytes, 0, 0x10000001u, 0);
    }
    put32(mode.bytes + 72, 0x20);
    put32(mode.bytes + 76, 0);
    put32(mode.bytes + 80, 0);
    long staged = ChangeDisplaySettingsExW(target, mode.bytes, 0, 0x10000011u, 0);
    long applied = staged ? staged : ChangeDisplaySettingsExW(0, 0, 0, 0, 0);
    text("PCEMU_GPU_PRIMARY_RC=");
    number(applied);
    text("\r\n");
    if (!applied)
        text("PCEMU_GPU_PRIMARY_SET_OK\r\n");
    if (getMode(target, &mode)) {
        windowX = (int)get32(mode.bytes + 76);
        windowY = (int)get32(mode.bytes + 80);
        windowWidth = (int)get32(mode.bytes + 172);
        windowHeight = (int)get32(mode.bytes + 176);
    }
}
static LRESULT windowProc(HANDLE window, DWORD message, WPARAM wparam, LPARAM lparam) {
    if (message == 15) {
        PAINT paint;
        RECT area;
        HANDLE dc = BeginPaint(window, &paint);
        if (GetClientRect(window, &area)) {
            const DWORD colors[] = {0x000000ff, 0x0000ff00, 0x00ff0000};
            for (int i = 0; i < 3; ++i) {
                RECT strip = {area.left + (area.right - area.left) * i / 3, area.top,
                              area.left + (area.right - area.left) * (i + 1) / 3, area.bottom};
                HANDLE brush = CreateSolidBrush(colors[i]);
                FillRect(dc, &strip, brush);
                DeleteObject(brush);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == 2) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void mainCRTStartup(void) {
    selectGpu();
    WNDCLASS type = {0};
    type.proc = windowProc;
    type.instance = GetModuleHandleW(0);
    type.name = L"PCEmuVirtioGpuProbe";
    if (!RegisterClassW(&type))
        ExitProcess(1);
    HANDLE window =
        CreateWindowExW(8, type.name, L"PCEmu VirtIO GPU test", 0x90000000u, windowX, windowY,
                        windowWidth ? windowWidth : GetSystemMetrics(0),
                        windowHeight ? windowHeight : GetSystemMetrics(1), 0, 0, type.instance, 0);
    if (!window)
        ExitProcess(2);
    UpdateWindow(window);
    MSG message;
    int result;
    while ((result = GetMessageW(&message, 0, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ExitProcess(result < 0 ? 3 : 0);
}
