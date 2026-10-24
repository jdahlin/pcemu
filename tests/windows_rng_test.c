#define __declspec(x)
#include "../tools/windows/check_rng.c"
#include <assert.h>
#include <setjmp.h>
#include <wchar.h>
static jmp_buf escaped;
static int scenario, reads, closes;
int BCryptOpenAlgorithmProvider(HANDLE *handle, const WCHAR *algorithm, const WCHAR *implementation, DWORD flags) {
    assert(!wcscmp(algorithm, L"RNG") && !wcscmp(implementation, L"QEMU VirtIO RNG Provider") && flags == 0);
    *handle = (HANDLE)1; return scenario == 1 ? -1 : 0;
}
int BCryptGenRandom(HANDLE handle, unsigned char *buffer, DWORD bytes, DWORD flags) {
    assert(handle == (HANDLE)1 && bytes == 64 && flags == 0);
    ++reads;
    for (DWORD i = 0; i < bytes; ++i) buffer[i] = scenario == 5 ? 42 : reads;
    return ((scenario == 2 && reads == 1) || (scenario == 3 && reads == 2)) ? -1 : 0;
}
int BCryptCloseAlgorithmProvider(HANDLE handle, DWORD flags) {
    assert(handle == (HANDLE)1 && flags == 0); ++closes; return scenario == 4 ? -1 : 0;
}
HANDLE GetStdHandle(DWORD which) { assert(which == (DWORD)-11); return (HANDLE)2; }
int WriteFile(HANDLE handle, const void *buffer, DWORD count, DWORD *actual, void *overlapped) {
    (void)buffer; (void)overlapped; assert(handle == (HANDLE)2); *actual = count; return 1;
}
void ExitProcess(DWORD status) { longjmp(escaped, (int)status + 1); }
int main(void) {
    for (scenario = 0; scenario <= 5; ++scenario) {
        reads = closes = 0;
        int status = setjmp(escaped);
        if (!status) mainCRTStartup();
        assert(status == (scenario ? 2 : 1));
        assert(closes == (scenario == 1 ? 0 : 1));
        if (!scenario || scenario >= 3) assert(reads == 2);
    }
    return 0;
}
