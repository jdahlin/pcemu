#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
static void fault(int signal, siginfo_t *info, void *context) {
    ucontext_t *u = context;
    dprintf(1, "signal=%d error=%x address=%p\n", signal, u->uc_mcontext->__es.__err, info->si_addr);
    _exit(0);
}
int main(int argc, char **argv) {
    struct sigaction a = {.sa_sigaction=fault, .sa_flags=SA_SIGINFO};
    sigaction(SIGSEGV,&a,0);sigaction(SIGBUS,&a,0);
    void *p=mmap(0,4096,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);
    if (argc<2) return 1;
    if (*argv[1]=='r') __asm__ volatile("movl (%0), %%eax"::"r"(p):"eax","memory");
    else if (*argv[1]=='w') __asm__ volatile("movl $1, (%0)"::"r"(p):"memory");
    else __asm__ volatile("incl (%0)"::"r"(p):"cc","memory");
    return 2;
}
