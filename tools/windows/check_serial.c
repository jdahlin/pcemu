// Original ARM64 probe for the signed Windows vioserial driver and host stream.
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef int BOOL;
typedef __WCHAR_TYPE__ WCHAR;
#define DLL __declspec(dllimport)
DLL HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DLL BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
DLL BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DLL BOOL CloseHandle(HANDLE);
DLL HANDLE GetStdHandle(DWORD);
DLL DWORD GetLastError(void);
DLL void Sleep(DWORD);
DLL void ExitProcess(DWORD);
static void text(const char *s) {
    DWORD count=0, written;
    while (s[count]) ++count;
    WriteFile(GetStdHandle((DWORD)-11),s,count,&written,0);
}
static void fail(void) {
    DWORD error=GetLastError(); char digits[11]; unsigned n=10; digits[n]=0;
    do {digits[--n]='0'+error%10;error/=10;} while (error);
    text("PCEMU_SERIAL_FAIL win32=");text(digits+n);text("\r\n");ExitProcess(1);
}
void mainCRTStartup(void) {
    HANDLE port=(HANDLE)-1;
    for (unsigned i=0;i<300 && port==(HANDLE)-1;++i) {
        port=CreateFileW(L"\\\\.\\Global\\org.qemu.guest_agent.0",0xc0000000u,0,0,3,0x80,0);
        if (port==(HANDLE)-1) Sleep(100);
    }
    if (port==(HANDLE)-1) fail();
    DWORD written=0;
    const char ready[]="PCEMU_SERIAL_READY\n";
    if (!WriteFile(port,ready,sizeof(ready)-1,&written,0) || written!=sizeof(ready)-1) fail();
    const unsigned char expected[]={0,1,2,3,0xff,0xfe,0x80,0x7f,10,13,42,99,100,0,255,17};
    unsigned char buffer[sizeof(expected)]; DWORD count=0;
    while (count<sizeof(buffer)) {
        DWORD got=0;
        if (!ReadFile(port,buffer+count,sizeof(buffer)-count,&got,0) || !got) fail();
        count+=got;
    }
    for (unsigned i=0;i<sizeof(buffer);++i) if (buffer[i]!=expected[i]) {
        text("PCEMU_SERIAL_DATA_MISMATCH\r\n");ExitProcess(1);
    }
    const char done[]="PCEMU_SERIAL_ROUNDTRIP_OK\n";
    if (!WriteFile(port,done,sizeof(done)-1,&written,0) || written!=sizeof(done)-1) fail();
    if (!CloseHandle(port)) fail();
    text("PCEMU_SERIAL_ROUNDTRIP_OK bytes=16\r\n");ExitProcess(0);
}
