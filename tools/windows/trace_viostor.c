// Original ARM64 WinPE controller for the upstream viostor WPP provider.
// Captures up to 4 MiB to X:\PCEMU\VIOSTOR.ETL; never changes driver settings.
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __WCHAR_TYPE__ WCHAR;
typedef struct { DWORD a; unsigned short b, c; unsigned char d[8]; } GUID;
typedef struct {
    DWORD size, provider; U64 context, timestamp; GUID guid; DWORD client, flags;
} WNODE;
typedef struct {
    WNODE wnode;
    DWORD bufferKB, minBuffers, maxBuffers, maxFileMB, mode, flushSeconds, enableFlags;
    int ageLimit;
    DWORD buffers, freeBuffers, eventsLost, buffersWritten, logBuffersLost, realtimeBuffersLost;
    void *loggerThread;
    DWORD filenameOffset, nameOffset;
} PROPERTIES;
_Static_assert(sizeof(PROPERTIES) == 120, "Windows ARM64 ETW properties ABI");
#define DLL __declspec(dllimport)
DLL DWORD StartTraceW(U64 *, const WCHAR *, PROPERTIES *);
DLL DWORD EnableTraceEx2(U64, const GUID *, DWORD, unsigned char, U64, U64, DWORD, void *);
DLL DWORD ControlTraceW(U64, const WCHAR *, PROPERTIES *, DWORD);
DLL WCHAR *GetCommandLineW(void);
DLL void *GetStdHandle(DWORD);
DLL int WriteFile(void *, const void *, DWORD, DWORD *, void *);
DLL void ExitProcess(DWORD);
static struct { PROPERTIES p; WCHAR filename[256], name[128]; } settings;
static const WCHAR session[] = L"PCEMU_VIOSTOR";
static const GUID provider = {0xb17fa150, 0x8c45, 0x482e, {0x9e,0xb8,0x29,0x61,0x1a,0x86,0x2b,0xf3}};
static void copy(WCHAR *out, const WCHAR *in) { while ((*out++ = *in++)) {} }
static void result(DWORD code) {
    char line[64] = "PCEMU_VIOSTOR_TRACE status="; unsigned n = sizeof("PCEMU_VIOSTOR_TRACE status=") - 1, i = 0; char digits[12];
    do { digits[i++] = '0' + code % 10; code /= 10; } while (code);
    while (i) line[n++] = digits[--i];
    line[n++] = '\r'; line[n++] = '\n'; DWORD written;
    WriteFile(GetStdHandle((DWORD)-11), line, n, &written, 0);
}
void mainCRTStartup(void) {
    PROPERTIES *p = &settings.p;
    p->wnode.size = sizeof(settings); p->wnode.flags = 0x20000; p->wnode.client = 1;
    p->bufferKB = 64; p->minBuffers = 2; p->maxBuffers = 8;
    p->maxFileMB = 4; p->mode = 1; p->flushSeconds = 1;
    p->filenameOffset = __builtin_offsetof(__typeof__(settings), filename);
    p->nameOffset = __builtin_offsetof(__typeof__(settings), name);
    copy(settings.filename, L"X:\\PCEMU\\VIOSTOR.ETL"); copy(settings.name, session);
    WCHAR *args = GetCommandLineW(); int stop = 0;
    for (unsigned i = 0; args[i]; ++i)
        if (args[i] == ' ' && args[i+1] == 's' && args[i+2] == 't' && args[i+3] == 'o' && args[i+4] == 'p') { stop = 1; break; }
    DWORD code; U64 handle = 0;
    if (stop) code = ControlTraceW(0, session, p, 1);
    else {
        code = StartTraceW(&handle, session, p);
        if (!code) {
            code = EnableTraceEx2(handle, &provider, 1, 5, 0xffffffffu, 0, 0, 0);
            if (code) ControlTraceW(handle, session, p, 1);
        }
    }
    result(code); ExitProcess(code ? 1 : 0);
}
