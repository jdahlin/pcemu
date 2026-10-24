// Original host-side WinAPI fixture: the guard must never obtain write access.
#define __declspec(x)
#include "../tools/windows/verify_install_disk.c"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
static jmp_buf escaped;
static int scenario, reads, seeks;
static long long position;
HANDLE CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *security, DWORD mode, DWORD attrs, HANDLE template) {
    (void)name; (void)security; (void)attrs; (void)template;
    assert(access == 0x80000000u && share == 3 && mode == 3);
    return scenario == 1 ? (HANDLE)-1 : (HANDLE)1;
}
int DeviceIoControl(HANDLE handle, DWORD code, void *input, DWORD inputBytes, void *output, DWORD size, DWORD *actual, void *overlapped) {
    (void)input; (void)inputBytes; (void)overlapped;
    assert(handle == (HANDLE)1 && code == 0x7405c && size == 8);
    *(U64 *)output = scenario == 3 ? 1024ull * 1024 * 1024 : 64ull * 1024 * 1024 * 1024;
    *actual = scenario == 4 ? 4 : 8;
    return scenario != 2;
}
int SetFilePointerEx(HANDLE handle, long long offset, long long *result, DWORD method) {
    (void)result; assert(handle == (HANDLE)1 && method == 0);
    position = offset; ++seeks; return scenario != 5;
}
int ReadFile(HANDLE handle, void *output, DWORD count, DWORD *actual, void *overlapped) {
    (void)overlapped; assert(handle == (HANDLE)1 && count == 1024 * 1024);
    assert(position == (reads ? 64ll * 1024 * 1024 * 1024 - count : 0));
    ++reads; memset(output, 0, count); *actual = scenario == 7 ? count - 1 : count;
    if ((scenario == 8 && reads == 1) || (scenario == 9 && reads == 2)) ((unsigned char *)output)[count - 1] = 1;
    if (scenario == 10 && reads == 1) ((unsigned char *)output)[440] = 42;
    if (scenario == 11 && reads == 1) ((unsigned char *)output)[446] = 42;
    return scenario != 6;
}
int CloseHandle(HANDLE handle) { assert(handle == (HANDLE)1); return 1; }
HANDLE GetStdHandle(DWORD which) { assert(which == (DWORD)-11); return (HANDLE)2; }
int WriteFile(HANDLE handle, const void *buffer, DWORD count, DWORD *actual, void *overlapped) {
    (void)buffer; (void)overlapped; assert(handle == (HANDLE)2); *actual = count; return 1;
}
void ExitProcess(DWORD status) { longjmp(escaped, (int)status + 1); }
int main(void) {
    for (scenario = 0; scenario <= 11; ++scenario) {
        reads = seeks = 0;
        int status = setjmp(escaped);
        if (!status) mainCRTStartup();
        assert(status == ((scenario == 0 || scenario == 10) ? 1 : 2));
        if (!scenario) assert(reads == 2 && seeks == 2);
        if (scenario <= 4 && scenario) assert(reads == 0);
    }
    return 0;
}
