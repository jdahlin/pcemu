// Original minimal ARM64 Windows/WinPE HTTP client. No CRT or SDK headers.
// get /path OUTPUT creates a NEW file; post FILE uploads bounded diagnostic logs.
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned short WORD;
typedef unsigned long long U64;
typedef __WCHAR_TYPE__ WCHAR;
#define DLL __declspec(dllimport)
DLL HANDLE GetStdHandle(DWORD);
DLL BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DLL BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
DLL HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DLL BOOL GetFileSizeEx(HANDLE, long long *);
DLL BOOL CloseHandle(HANDLE);
DLL DWORD GetLastError(void);
DLL WCHAR *GetCommandLineW(void);
DLL void ExitProcess(DWORD);
DLL HANDLE WinHttpOpen(const WCHAR *, DWORD, const WCHAR *, const WCHAR *, DWORD);
DLL BOOL WinHttpSetTimeouts(HANDLE, int, int, int, int);
DLL HANDLE WinHttpConnect(HANDLE, const WCHAR *, WORD, DWORD);
DLL HANDLE WinHttpOpenRequest(HANDLE, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR **, DWORD);
DLL BOOL WinHttpSendRequest(HANDLE, const WCHAR *, DWORD, void *, DWORD, DWORD, unsigned long long);
DLL BOOL WinHttpWriteData(HANDLE, const void *, DWORD, DWORD *);
DLL BOOL WinHttpReceiveResponse(HANDLE, void *);
DLL BOOL WinHttpReadData(HANDLE, void *, DWORD, DWORD *);
DLL BOOL WinHttpQueryHeaders(HANDLE, DWORD, const WCHAR *, void *, DWORD *, DWORD *);
DLL BOOL WinHttpCloseHandle(HANDLE);
static char buffer[65536];
static WCHAR args[4][1024];
static void text(const char *p) {
    DWORD size = 0, written;
    while (p[size]) ++size;
    WriteFile(GetStdHandle((DWORD)-11), p, size, &written, 0);
}
static void number(U64 n) {
    char out[32]; unsigned i = 31; out[i] = 0;
    do { out[--i] = '0' + n % 10; n /= 10; } while (n);
    text(out + i);
}
static void fail(const char *operation) {
    DWORD error = GetLastError(); text("PCEMU_HTTP_FAIL "); text(operation); text(" win32="); number(error); text("\r\n"); ExitProcess(1);
}
static BOOL same(const WCHAR *s, const char *a) {
    while (*s && *a && *s == (unsigned char)*a) { ++s; ++a; }
    return !*s && !*a;
}
void mainCRTStartup(void) {
    WCHAR *line = GetCommandLineW(); unsigned count = 0;
    while (*line && count < 4) {
        while (*line == ' ' || *line == '\t') ++line;
        if (!*line) break;
        BOOL quoted = 0; unsigned n = 0;
        while (*line) {
            if (*line == '"') { quoted = !quoted; ++line; continue; }
            if (!quoted && (*line == ' ' || *line == '\t')) break;
            if (n == 1023) { text("Argument too long\r\n"); ExitProcess(2); }
            args[count][n++] = *line++;
        }
        args[count++][n] = 0;
    }
    BOOL post = count == 3 && same(args[1], "post");
    if (!post && !(count == 4 && same(args[1], "get") && args[2][0] == '/')) {
        text("Usage: guest-http get /path OUTPUT | post FILE\r\n"); ExitProcess(2);
    }
    HANDLE session = WinHttpOpen(L"PCEmuWinPE/1.0", 1, 0, 0, 0);
    if (!session) fail("open");
    WinHttpSetTimeouts(session, 10000, 10000, 60000, 60000);
    HANDLE connection = WinHttpConnect(session, L"10.0.2.2", 8080, 0);
    if (!connection) fail("connect");
    HANDLE request = WinHttpOpenRequest(connection, post ? L"POST" : L"GET", post ? L"/report" : args[2], 0, 0, 0, 0);
    if (!request) fail("request");
    HANDLE file = (HANDLE)-1;
    long long size = 0;
    if (post) {
        file = CreateFileW(args[2], 0x80000000u, 3, 0, 3, 0x80, 0);
        if (file == (HANDLE)-1 || !GetFileSizeEx(file, &size)) fail("open log");
        if (size < 0 || size > 8 * 1024 * 1024) { text("Log too large\r\n"); ExitProcess(2); }
    }
    if (!WinHttpSendRequest(request, 0, 0, 0, 0, (DWORD)size, 0)) fail("send");
    if (post) {
        U64 left = (U64)size;
        while (left) {
            DWORD got, sent, wanted = left < sizeof(buffer) ? (DWORD)left : sizeof(buffer);
            if (!ReadFile(file, buffer, wanted, &got, 0) || !got) fail("read log snapshot");
            if (!WinHttpWriteData(request, buffer, got, &sent) || sent != got) fail("post log");
            left -= got;
        }
        CloseHandle(file);
    }
    if (!WinHttpReceiveResponse(request, 0)) fail("response");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request, 19 | 0x20000000, 0, &status, &length, 0)) fail("HTTP status");
    if (status != 200) { text("HTTP status "); number(status); text("\r\n"); ExitProcess(1); }
    U64 total = 0, expected = 0;
    if (!post) {
        WCHAR contentLength[32]; length = sizeof(contentLength);
        if (!WinHttpQueryHeaders(request, 5, 0, contentLength, &length, 0)) fail("content length");
        for (unsigned i = 0; contentLength[i]; ++i) {
            if (contentLength[i] < '0' || contentLength[i] > '9') { text("Invalid content length\r\n"); ExitProcess(1); }
            expected = expected * 10 + contentLength[i] - '0';
        }
        if (!expected || expected > 16ull * 1024 * 1024 * 1024) { text("Invalid transfer size\r\n"); ExitProcess(1); }
        file = CreateFileW(args[3], 0x40000000u, 0, 0, 1, 0x80, 0); // CREATE_NEW.
        if (file == (HANDLE)-1) fail("create output");
    }
    for (;;) {
        DWORD got = 0, written = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &got)) fail("read HTTP");
        if (!got) break;
        if (!post) {
            if (!WriteFile(file, buffer, got, &written, 0) || written != got) fail("write output");
            total += got;
            if (total > expected) { text("HTTP exceeds declared size\r\n"); ExitProcess(1); }
        }
    }
    if (!post) {
        CloseHandle(file);
        if (total != expected) { text("Truncated HTTP payload\r\n"); ExitProcess(1); }
    }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    text("PCEMU_HTTP_OK bytes="); number(post ? (U64)size : total); text("\r\n"); ExitProcess(0);
}
