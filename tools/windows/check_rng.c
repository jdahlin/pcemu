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
static const char *failedStage = "open";
static int lastStatus;
static void text(const char *s) {
    DWORD n = 0, written; while (s[n]) ++n;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void finish(int ok) {
    if (ok) text("PCEMU_VIRTIO_RNG_PROVIDER_OK bytes=128\r\n");
    else {
        text("PCEMU_VIRTIO_RNG_PROVIDER_FAILED stage="); text(failedStage);
        text(" status=0x");
        char hex[9]; const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 8; ++i) hex[i] = digits[((DWORD)lastStatus >> ((7 - i) * 4)) & 15];
        hex[8] = 0; text(hex); text("\r\n");
    }
    ExitProcess(ok ? 0 : 1);
}
void mainCRTStartup(void) {
    HANDLE provider = 0;
    lastStatus = BCryptOpenAlgorithmProvider(&provider, L"RNG", L"QEMU VirtIO RNG Provider", 0);
    if (lastStatus < 0) finish(0);
    failedStage = "first-read";
    lastStatus = BCryptGenRandom(provider, first, sizeof(first), 0);
    if (lastStatus >= 0) {
        failedStage = "second-read";
        lastStatus = BCryptGenRandom(provider, second, sizeof(second), 0);
    }
    int ok = lastStatus >= 0;
    int closed = BCryptCloseAlgorithmProvider(provider, 0);
    if (ok && closed < 0) { failedStage = "close"; lastStatus = closed; ok = 0; }
    unsigned different = 0;
    for (unsigned i = 0; i < sizeof(first); ++i) different |= first[i] ^ second[i];
    if (ok && !different) { failedStage = "unchanged-output"; lastStatus = (int)0xc0000001u; ok = 0; }
    finish(ok);
}
