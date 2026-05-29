#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/shim.h"

#include <stdint.h>
#include <stdlib.h>

#if !defined(_WIN32) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static kb_backend_t *current_backend;

void kb_shim_set_backend(kb_backend_t *backend)
{
    current_backend = backend;
}

kb_backend_t *kb_shim_current_backend(void)
{
    return current_backend;
}

unsigned long kb_shim_current_kernel_gs(void)
{
#if !defined(_WIN32) && defined(__x86_64__)
    unsigned long gs = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs) != 0) {
        return 0;
    }
    return gs;
#else
    return 0;
#endif
}

int kb_shim_enter_kernel_gs(unsigned long kernel_gs, unsigned long *out_old_gs)
{
    if (out_old_gs == NULL) {
        return -22;
    }
#if !defined(_WIN32) && defined(__x86_64__)
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, out_old_gs) != 0) {
        return -95;
    }
    if (kernel_gs != 0 && syscall(SYS_arch_prctl, ARCH_SET_GS, kernel_gs) != 0) {
        return -95;
    }
#else
    (void)kernel_gs;
    *out_old_gs = 0;
#endif
    return 0;
}

void kb_shim_leave_kernel_gs(unsigned long old_gs)
{
#if !defined(_WIN32) && defined(__x86_64__)
    (void)syscall(SYS_arch_prctl, ARCH_SET_GS, old_gs);
#else
    (void)old_gs;
#endif
}

void kb_stack_chk_fail(void)
{
    abort();
}
