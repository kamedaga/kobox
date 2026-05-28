#include "kobox/shim.h"

uint32_t kb_ioread32(const void *addr)
{
    const volatile uint32_t *reg = addr;
    return *reg;
}

void kb_iowrite32(uint32_t value, void *addr)
{
    volatile uint32_t *reg = addr;
    *reg = value;
}

uint32_t ioread32(const void *addr)
{
    return kb_ioread32(addr);
}

void iowrite32(uint32_t value, void *addr)
{
    kb_iowrite32(value, addr);
}
