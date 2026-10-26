#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only one session runs at a time. run blocks on the worker; all other calls
// are thread safe. Call prepare before publishing a new session to the UI.
// The caller owns copied ARGB pixels; cursor storage must hold 64*64 pixels.
void pcemu_native_prepare(void);
int pcemu_native_run(const char *machine, const char *disk, const char *snapshot,
                     const char *rom, unsigned memory, const char *bootText, int initializeVga);
int pcemu_native_run_arm(const char *root, const char *disk, const char *snapshot,
                         unsigned memory, unsigned cpus, const char *runDirectory);
void pcemu_native_command(const char *command);
int pcemu_native_error(char *message, int capacity);
void pcemu_native_event(int kind, uint16_t key, int released, int button, int x, int y);
int pcemu_native_frame(uint32_t *pixels, int capacity, int *width, int *height, int *classic);
int pcemu_native_cursor(uint32_t *pixels, int *width, int *height, int *hotX, int *hotY, int *visible);
int pcemu_native_captured(void);
int pcemu_native_absolute(void);
#ifdef __cplusplus
}
#endif
