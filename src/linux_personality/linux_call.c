#include "kobox/shim.h"

#if defined(__x86_64__) && !defined(_MSC_VER)
#define KB_NAKED __attribute__((naked))

KB_NAKED void kb_linux_call_void_ptr(void (*fn)(void *), void *arg)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED void kb_linux_call_void_ptr_gs(void (*fn)(void *), void *arg, unsigned long kernel_gs)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "ret\n\t"
        "1:\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rdi, %r12\n\t"
        "mov %rsi, %r13\n\t"
        "mov %rdx, %r14\n\t"
        "test %r14, %r14\n\t"
        "jz 2f\n\t"
        "mov $0x1001, %edi\n\t"
        "mov %r14, %rsi\n\t"
        "mov $158, %eax\n\t"
        "syscall\n\t"
        "2:\n\t"
        "mov %r13, %rdi\n\t"
        "mov %r12, %r11\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED void kb_linux_call_void_ulong(void (*fn)(unsigned long), unsigned long arg)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED void kb_linux_call_void_ulong_gs(void (*fn)(unsigned long), unsigned long arg, unsigned long kernel_gs)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "ret\n\t"
        "1:\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rdi, %r12\n\t"
        "mov %rsi, %r13\n\t"
        "mov %rdx, %r14\n\t"
        "test %r14, %r14\n\t"
        "jz 2f\n\t"
        "mov $0x1001, %edi\n\t"
        "mov %r14, %rsi\n\t"
        "mov $158, %eax\n\t"
        "syscall\n\t"
        "2:\n\t"
        "mov %r13, %rdi\n\t"
        "mov %r12, %r11\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr(int (*fn)(void *), void *arg0)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr_uint(int (*fn)(void *, unsigned int), void *arg0, unsigned int arg1)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "mov %edx, %esi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED void *kb_linux_call_ptr_ptr(void *(*fn)(void *), void *arg0)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_int_ptr(int (*fn)(int, void *), int arg0, void *arg1)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %esi, %edi\n\t"
        "mov %rdx, %rsi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr_ptr(int (*fn)(void *, char *), void *arg0, char *arg1)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "mov %rdx, %rsi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr_ptr_raw(int (*fn)(void *, void *), void *arg0, void *arg1)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "mov %rdx, %rsi\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
    int (*fn)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t),
    void *arg0,
    uint16_t arg1,
    uint16_t arg2,
    uint16_t arg3,
    char *arg4,
    uint16_t arg5)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "mov 8(%rsp), %r10d\n\t"
        "mov %rsi, %rdi\n\t"
        "movzwl %dx, %esi\n\t"
        "movzwl %cx, %edx\n\t"
        "movzwl %r8w, %ecx\n\t"
        "mov %r9, %r8\n\t"
        "movzwl %r10w, %r9d\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $8, %rsp\n\t"
        "call *%r11\n\t"
        "add $8, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

KB_NAKED int kb_linux_call_int_ptr_uint_u8_u8_u16_u16_ptr_u16_int(
    int (*fn)(void *, unsigned int, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t, int),
    void *arg0,
    unsigned int arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint16_t arg4,
    uint16_t arg5,
    void *arg6,
    uint16_t arg7,
    int arg8)
{
    __asm__ volatile(
        "test %rdi, %rdi\n\t"
        "jnz 1f\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        "1:\n\t"
        "mov %rdi, %r11\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov 56(%rsp), %r10d\n\t"
        "mov 64(%rsp), %rax\n\t"
        "mov 72(%rsp), %ebx\n\t"
        "mov 80(%rsp), %r12d\n\t"
        "push %r12\n\t"
        "push %rbx\n\t"
        "push %rax\n\t"
        "mov %rsi, %rdi\n\t"
        "mov %edx, %esi\n\t"
        "movzbl %cl, %edx\n\t"
        "movzbl %r8b, %ecx\n\t"
        "movzwl %r9w, %r8d\n\t"
        "movzwl %r10w, %r9d\n\t"
        "call *%r11\n\t"
        "add $24, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

#else

void kb_linux_call_void_ptr(void (*fn)(void *), void *arg)
{
    if (fn != NULL) {
        fn(arg);
    }
}

void kb_linux_call_void_ptr_gs(void (*fn)(void *), void *arg, unsigned long kernel_gs)
{
    (void)kernel_gs;
    if (fn != NULL) {
        fn(arg);
    }
}

void kb_linux_call_void_ulong(void (*fn)(unsigned long), unsigned long arg)
{
    if (fn != NULL) {
        fn(arg);
    }
}

void kb_linux_call_void_ulong_gs(void (*fn)(unsigned long), unsigned long arg, unsigned long kernel_gs)
{
    (void)kernel_gs;
    if (fn != NULL) {
        fn(arg);
    }
}

int kb_linux_call_int_ptr(int (*fn)(void *), void *arg0)
{
    return fn != NULL ? fn(arg0) : 0;
}

int kb_linux_call_int_ptr_uint(int (*fn)(void *, unsigned int), void *arg0, unsigned int arg1)
{
    return fn != NULL ? fn(arg0, arg1) : 0;
}

void *kb_linux_call_ptr_ptr(void *(*fn)(void *), void *arg0)
{
    return fn != NULL ? fn(arg0) : NULL;
}

int kb_linux_call_int_int_ptr(int (*fn)(int, void *), int arg0, void *arg1)
{
    return fn != NULL ? fn(arg0, arg1) : 0;
}

int kb_linux_call_int_ptr_ptr(int (*fn)(void *, char *), void *arg0, char *arg1)
{
    return fn != NULL ? fn(arg0, arg1) : 0;
}

int kb_linux_call_int_ptr_ptr_raw(int (*fn)(void *, void *), void *arg0, void *arg1)
{
    return fn != NULL ? fn(arg0, arg1) : 0;
}

int kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
    int (*fn)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t),
    void *arg0,
    uint16_t arg1,
    uint16_t arg2,
    uint16_t arg3,
    char *arg4,
    uint16_t arg5)
{
    return fn != NULL ? fn(arg0, arg1, arg2, arg3, arg4, arg5) : 0;
}

int kb_linux_call_int_ptr_uint_u8_u8_u16_u16_ptr_u16_int(
    int (*fn)(void *, unsigned int, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t, int),
    void *arg0,
    unsigned int arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint16_t arg4,
    uint16_t arg5,
    void *arg6,
    uint16_t arg7,
    int arg8)
{
    return fn != NULL ? fn(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) : 0;
}

#endif
