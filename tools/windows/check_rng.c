// Original ARM64 guest check: explicitly use the installed VirtIO CNG provider.
// Entropy is never printed. Success requires two completed provider requests.
typedef unsigned int DWORD;
typedef __WCHAR_TYPE__ WCHAR;
typedef void *HANDLE;
#define DLL __declspec(dllimport)
DLL int BCryptOpenAlgorithmProvider(HANDLE *, const WCHAR *, const WCHAR *, DWORD);
DLL int BCryptGenRandom(HANDLE, unsigned char *, DWORD, DWORD);
DLL int BCryptCloseAlgorithmProvider(HANDLE, DWORD);
DLL HANDLE GetStdHandle(DWORD);
DLL int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DLL void ExitProcess(DWORD);
static unsigned char first[64], second[64];
static void finish(int ok) {
    const char *s = ok ? "PCEMU_VIRTIO_RNG_PROVIDER_OK bytes=128\r\n" : "PCEMU_VIRTIO_RNG_PROVIDER_FAILED\r\n";
    DWORD n = 0, written; while (s[n]) ++n;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
    ExitProcess(ok ? 0 : 1);
}
void mainCRTStartup(void) {
    HANDLE provider = 0;
    if (BCryptOpenAlgorithmProvider(&provider, L"RNG", L"QEMU VirtIO RNG Provider", 0) < 0) finish(0);
    int ok = BCryptGenRandom(provider, first, sizeof(first), 0) >= 0 &&
             BCryptGenRandom(provider, second, sizeof(second), 0) >= 0;
    if (BCryptCloseAlgorithmProvider(provider, 0) < 0) ok = 0;
    unsigned different = 0;
    for (unsigned i = 0; i < sizeof(first); ++i) different |= first[i] ^ second[i];
    finish(ok && different != 0);
}
