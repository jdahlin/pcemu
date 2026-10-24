#include "coremark.h"
#include <stdarg.h>
volatile ee_s32 seed1_volatile = 0, seed2_volatile = 0, seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS, seed5_volatile = 0;
ee_u32 default_num_contexts = 1;
static ee_u32 started, stopped;
static void out(unsigned short p, unsigned char v) { __asm__ volatile("outb %0,%1"::"a"(v),"d"(p)); }
static ee_u32 host_millis(void) {
    ee_u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"d"((unsigned short)0xe8)); return v;
}
void start_time(void) { out(0xe7,1); started=host_millis(); }
void stop_time(void) { stopped=host_millis(); out(0xe7,2); }
CORE_TICKS get_time(void) { return stopped-started; }
secs_ret time_in_secs(CORE_TICKS t) { return t/1000; }
void portable_init(core_portable *p,int *argc,char *argv[]) { (void)argc;(void)argv;p->portable_id=1; }
void portable_fini(core_portable *p) { p->portable_id=0; }
static void character(char c) { out(0xe9,(unsigned char)c); }
static void number(ee_u32 n,unsigned base,unsigned width,char pad) {
    char digits[32];unsigned size=0;
    do {digits[size++]="0123456789abcdef"[n%base];n/=base;}while(n);
    while(width>size){character(pad);--width;}
    while(size)character(digits[--size]);
}
int ee_printf(const char *fmt,...) {
    va_list args;va_start(args,fmt);
    while(*fmt) {
        if(*fmt!='%'){character(*fmt++);continue;}
        ++fmt;char pad=' ';unsigned width=0;
        if(*fmt=='0'){pad='0';++fmt;}
        while(*fmt>='0'&&*fmt<='9')width=width*10+(*fmt++-'0');
        if(*fmt=='l')++fmt; // int and long are both 32 bits in this i386 ABI.
        char code=*fmt++;
        if(code=='s'){const char *p=va_arg(args,const char*);while(*p)character(*p++);}
        else if(code=='d') {int v=va_arg(args,int);if(v<0){character('-');number(0u-(ee_u32)v,10,width,pad);}else number(v,10,width,pad);}
        else if(code=='u'||code=='x')number(va_arg(args,ee_u32),code=='x'?16:10,width,pad);
        else if(code=='c')character((char)va_arg(args,int));
        else if(code=='%')character('%');
    }
    va_end(args);return 0;
}
int main(void);
void bench_entry(void) { out(0xe6,1);main();out(0xe7,3); }
void *memset(void *p,int v,size_t n) {unsigned char *q=p;while(n--)*q++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,size_t n) {unsigned char *q=p;const unsigned char *r=s;while(n--)*q++=*r++;return p;}
