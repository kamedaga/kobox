#include "kobox/shim.h"

enum {
    KB_MMIO_WRITE32_HOOK_MAX = 32,
};

typedef struct kb_mmio_write32_hook_record {
    void *base;
    size_t size;
    kb_mmio_write32_hook_t hook;
    void *user;
} kb_mmio_write32_hook_record_t;

static kb_mmio_write32_hook_record_t write32_hooks[KB_MMIO_WRITE32_HOOK_MAX];

int kb_mmio_register_write32_hook(void *base, size_t size, kb_mmio_write32_hook_t hook, void *user)
{
    if (base == NULL || size == 0 || hook == NULL) {
        return -22;
    }
    for (size_t i = 0; i < KB_MMIO_WRITE32_HOOK_MAX; i++) {
        if (write32_hooks[i].base == base && write32_hooks[i].user == user) {
            write32_hooks[i].size = size;
            write32_hooks[i].hook = hook;
            return 0;
        }
    }
    for (size_t i = 0; i < KB_MMIO_WRITE32_HOOK_MAX; i++) {
        if (write32_hooks[i].hook == NULL) {
            write32_hooks[i].base = base;
            write32_hooks[i].size = size;
            write32_hooks[i].hook = hook;
            write32_hooks[i].user = user;
            return 0;
        }
    }
    return -12;
}

void kb_mmio_unregister_write32_hook(void *base, void *user)
{
    for (size_t i = 0; i < KB_MMIO_WRITE32_HOOK_MAX; i++) {
        if (write32_hooks[i].base == base && write32_hooks[i].user == user) {
            write32_hooks[i] = (kb_mmio_write32_hook_record_t){0};
            return;
        }
    }
}

static int run_write32_hooks(void *addr, uint32_t value)
{
    uintptr_t target = (uintptr_t)addr;
    int handled = 0;
    for (size_t i = 0; i < KB_MMIO_WRITE32_HOOK_MAX; i++) {
        const kb_mmio_write32_hook_record_t *record = &write32_hooks[i];
        if (record->hook == NULL) {
            continue;
        }
        uintptr_t base = (uintptr_t)record->base;
        if (target >= base && target + sizeof(uint32_t) <= base + record->size) {
            int result = record->hook(record->user, addr, value);
            if (result > 0) {
                handled = 1;
            }
        }
    }
    return handled;
}

uint8_t kb_ioread8(const void *addr)
{
    const volatile uint8_t *reg = addr;
    return *reg;
}

void kb_iowrite8(uint8_t value, void *addr)
{
    volatile uint8_t *reg = addr;
    *reg = value;
}

uint32_t kb_ioread32(const void *addr)
{
    const volatile uint32_t *reg = addr;
    return *reg;
}

void kb_iowrite32(uint32_t value, void *addr)
{
    if (run_write32_hooks(addr, value)) {
        return;
    }
    volatile uint32_t *reg = addr;
    *reg = value;
}

uint8_t ioread8(const void *addr)
{
    return kb_ioread8(addr);
}

void iowrite8(uint8_t value, void *addr)
{
    kb_iowrite8(value, addr);
}

uint32_t ioread32(const void *addr)
{
    return kb_ioread32(addr);
}

void iowrite32(uint32_t value, void *addr)
{
    kb_iowrite32(value, addr);
}
