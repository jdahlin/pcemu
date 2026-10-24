#pragma once
#include <stddef.h>
#define HAS_FLOAT 0
#define HAS_TIME_H 0
#define HAS_STDIO 0
#define HAS_PRINTF 0
#define MAIN_HAS_NOARGC 1
#define MAIN_HAS_NORETURN 0
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD MEM_STATIC
#define MULTITHREAD 1
#define PERFORMANCE_RUN 1
#define COMPILER_VERSION "Clang " __clang_version__
#define PCEMU_STRING_INNER(x) #x
#define PCEMU_STRING(x) PCEMU_STRING_INNER(x)
#define COMPILER_FLAGS "--target=i386-none-elf -march=i386 -mno-sse -mno-mmx -msoft-float -O2 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -DITERATIONS=" PCEMU_STRING(ITERATIONS)
#define MEM_LOCATION "STATIC, 32-bit protected mode, paging disabled"
typedef signed short ee_s16;
typedef unsigned short ee_u16;
typedef signed int ee_s32;
typedef unsigned int ee_u32;
typedef unsigned char ee_u8;
typedef unsigned int ee_ptr_int;
typedef size_t ee_size_t;
typedef ee_u32 CORE_TICKS;
#define CORETIMETYPE ee_u32
#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))
typedef struct { ee_u8 portable_id; } core_portable;
extern ee_u32 default_num_contexts;
void portable_init(core_portable *, int *, char *[]);
void portable_fini(core_portable *);
int ee_printf(const char *, ...);
