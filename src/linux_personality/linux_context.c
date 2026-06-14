#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_WIN32) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static kb_device_backend_t *current_backend;

uintptr_t kb_ref_stack_chk_guard = 0x6b6f626f785f7370ull;
uint32_t kb_preempt_count;
uint32_t kb_num_online_cpus = 1;
uint64_t kb_cpu_possible_mask = 1;
uint64_t kb_cpu_present_mask = 1;
uint64_t kb_user_ptr_max = UINTPTR_MAX;
uint32_t kb_cpu_number;

void kb_shim_set_device_backend(kb_device_backend_t *backend)
{
    current_backend = backend;
}

kb_device_backend_t *kb_shim_current_device_backend(void)
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
    fprintf(stderr,
        "kobox-shim: stack check failure caller=%p external_target=%p caller_gs=0x%lx callee_gs=0x%lx\n",
        __builtin_return_address(0),
        kb_module_current_external_call_target(),
        kb_module_current_external_call_caller_gs(),
        kb_module_current_external_call_callee_gs());
    abort();
}
