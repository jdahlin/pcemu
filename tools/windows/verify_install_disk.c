// Original read-only guard for the dedicated Windows installation target.
// The installation VM must attach only the 1 GiB boot disk and new 64 GiB disk.
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __WCHAR_TYPE__ WCHAR;
typedef void *HANDLE;
#define DLL __declspec(dllimport)
DLL HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DLL int ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
DLL int DeviceIoControl(HANDLE, DWORD, void *, DWORD, void *, DWORD, DWORD *, void *);
DLL int SetFilePointerEx(HANDLE, long long, long long *, DWORD);
DLL int CloseHandle(HANDLE);
DLL HANDLE GetStdHandle(DWORD);
DLL int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DLL void ExitProcess(DWORD);
static unsigned char data[1024 * 1024];
static void finish(int ok) {
    const char *message = ok ? "PCEMU_INSTALL_DISK_1_UNPARTITIONED_64G_OK\r\n" : "PCEMU_INSTALL_DISK_GUARD_FAILED\r\n";
    DWORD size = 0, written; while (message[size]) ++size;
    WriteFile(GetStdHandle((DWORD)-11), message, size, &written, 0); ExitProcess(ok ? 0 : 1);
}
void mainCRTStartup(void) {
    HANDLE file = CreateFileW(L"\\\\.\\PhysicalDrive1", 0x80000000u, 3, 0, 3, 0x80, 0);
    if (file == (HANDLE)-1) finish(0);
    U64 size = 0; DWORD count = 0;
    if (!DeviceIoControl(file, 0x7405c, 0, 0, &size, sizeof(size), &count, 0) || count != sizeof(size) || size != 64ull * 1024 * 1024 * 1024) finish(0);
    for (unsigned part = 0; part < 2; ++part) {
        if (!SetFilePointerEx(file, part ? (long long)(size - sizeof(data)) : 0, 0, 0) ||
            !ReadFile(file, data, sizeof(data), &count, 0) || count != sizeof(data)) finish(0);
        // Windows may assign an MBR disk signature even to a new empty disk.
        // Allow only that field; partition entries, boot signature, GPT and all
        // other bytes in both 1 MiB boundaries must still be zero.
        for (unsigned i = 0; i < sizeof(data); ++i)
            if (data[i] && !(part == 0 && i >= 440 && i < 444)) finish(0);
    }
    CloseHandle(file); finish(1);
}
