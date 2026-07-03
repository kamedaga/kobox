#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_MMIO_WRITE32_HOOK_MAX = 32,
    KB_VIRTIO_RING_F_INDIRECT_DESC = 28,
    KB_VIRTIO_RING_F_EVENT_IDX = 29,
    KB_VIRTIO_TRACE_BAR_MAX = 8,
    KB_VIRTIO_MODERN_COMMON_BYTES = 0x1000,
    KB_VIRTIO_MODERN_NOTIFY_OFFSET = 0x3000,
};

typedef struct kb_mmio_write32_hook_record {
    void *base;
    size_t size;
    kb_mmio_write32_hook_t hook;
    void *user;
} kb_mmio_write32_hook_record_t;

static kb_mmio_write32_hook_record_t write32_hooks[KB_MMIO_WRITE32_HOOK_MAX];

typedef struct kb_virtio_trace_bar {
    int active;
    unsigned int bar;
    void *base;
    size_t size;
} kb_virtio_trace_bar_t;

static kb_virtio_trace_bar_t virtio_trace_bars[KB_VIRTIO_TRACE_BAR_MAX];

static int trace_mmio_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_MMIO");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int trace_virtio_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_VIRTIO");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void kb_virtio_modern_trace_bar(unsigned int bar, void *base, size_t size)
{
    if (base == NULL || size == 0) {
        return;
    }
    for (size_t i = 0; i < KB_VIRTIO_TRACE_BAR_MAX; i++) {
        if (virtio_trace_bars[i].active && virtio_trace_bars[i].base == base) {
            virtio_trace_bars[i].bar = bar;
            virtio_trace_bars[i].size = size;
            return;
        }
    }
    for (size_t i = 0; i < KB_VIRTIO_TRACE_BAR_MAX; i++) {
        if (!virtio_trace_bars[i].active) {
            virtio_trace_bars[i].active = 1;
            virtio_trace_bars[i].bar = bar;
            virtio_trace_bars[i].base = base;
            virtio_trace_bars[i].size = size;
            return;
        }
    }
}

void kb_virtio_modern_debug_dump_queues(void)
{
    if (!trace_virtio_enabled()) {
        return;
    }
    kb_virtio_trace_bar_t *bar = NULL;
    for (size_t i = 0; i < KB_VIRTIO_TRACE_BAR_MAX; i++) {
        if (virtio_trace_bars[i].active && virtio_trace_bars[i].bar == 4 && virtio_trace_bars[i].size >= 0x1000) {
            bar = &virtio_trace_bars[i];
            break;
        }
    }
    if (bar == NULL) {
        return;
    }

    volatile unsigned char *common = (volatile unsigned char *)bar->base;
    volatile uint16_t *queue_select = (volatile uint16_t *)(common + 0x16);
    volatile uint16_t *queue_size = (volatile uint16_t *)(common + 0x18);
    volatile uint16_t *queue_msix = (volatile uint16_t *)(common + 0x1a);
    volatile uint16_t *queue_enable = (volatile uint16_t *)(common + 0x1c);
    volatile uint16_t *queue_notify_off = (volatile uint16_t *)(common + 0x1e);
    volatile uint32_t *queue_desc_low = (volatile uint32_t *)(common + 0x20);
    volatile uint32_t *queue_desc_high = (volatile uint32_t *)(common + 0x24);
    volatile uint32_t *queue_driver_low = (volatile uint32_t *)(common + 0x28);
    volatile uint32_t *queue_driver_high = (volatile uint32_t *)(common + 0x2c);
    volatile uint32_t *queue_device_low = (volatile uint32_t *)(common + 0x30);
    volatile uint32_t *queue_device_high = (volatile uint32_t *)(common + 0x34);
    uint16_t saved_select = *queue_select;
    for (uint16_t q = 0; q < 3; q++) {
        *queue_select = q;
        uint64_t desc = ((uint64_t)*queue_desc_high << 32) | *queue_desc_low;
        uint64_t driver = ((uint64_t)*queue_driver_high << 32) | *queue_driver_low;
        uint64_t device = ((uint64_t)*queue_device_high << 32) | *queue_device_low;
        fprintf(stderr,
            "kobox virtio: qstate q=%u size=%u enable=%u notify_off=%u msix=0x%04x desc=0x%llx driver=0x%llx device=0x%llx\n",
            q,
            *queue_size,
            *queue_enable,
            *queue_notify_off,
            *queue_msix,
            (unsigned long long)desc,
            (unsigned long long)driver,
            (unsigned long long)device);
    }
    *queue_select = saved_select;
}

static const kb_virtio_trace_bar_t *virtio_trace_bar_for(const void *addr, size_t access_size, size_t *out_offset)
{
    uintptr_t target = (uintptr_t)addr;
    for (size_t i = 0; i < KB_VIRTIO_TRACE_BAR_MAX; i++) {
        const kb_virtio_trace_bar_t *record = &virtio_trace_bars[i];
        if (!record->active || record->base == NULL) {
            continue;
        }
        uintptr_t base = (uintptr_t)record->base;
        if (target >= base && target + access_size <= base + record->size) {
            if (out_offset != NULL) {
                *out_offset = (size_t)(target - base);
            }
            return record;
        }
    }
    return NULL;
}

static const char *virtio_common_name(size_t offset)
{
    switch (offset) {
    case 0x00: return "device_feature_select";
    case 0x04: return "device_feature";
    case 0x08: return "driver_feature_select";
    case 0x0c: return "driver_feature";
    case 0x14: return "device_status";
    case 0x15: return "config_generation";
    case 0x16: return "queue_select";
    case 0x18: return "queue_size";
    case 0x1a: return "queue_msix_vector";
    case 0x1c: return "queue_enable";
    case 0x1e: return "queue_notify_off";
    case 0x20: return "queue_desc_low";
    case 0x24: return "queue_desc_high";
    case 0x28: return "queue_driver_low";
    case 0x2c: return "queue_driver_high";
    case 0x30: return "queue_device_low";
    case 0x34: return "queue_device_high";
    default: return NULL;
    }
}

static void trace_virtio_access(const char *op, const void *addr, size_t access_size, uint64_t value)
{
    if (!trace_virtio_enabled()) {
        return;
    }
    size_t offset = 0;
    const kb_virtio_trace_bar_t *bar = virtio_trace_bar_for(addr, access_size, &offset);
    if (bar == NULL) {
        return;
    }
    const char *name = offset < KB_VIRTIO_MODERN_COMMON_BYTES ? virtio_common_name(offset) : NULL;
    if (name != NULL) {
        fprintf(stderr,
            "kobox virtio: %s bar=%u common+0x%zx %-22s value=0x%llx\n",
            op,
            bar->bar,
            offset,
            name,
            (unsigned long long)value);
        return;
    }
    if (offset >= KB_VIRTIO_MODERN_NOTIFY_OFFSET && offset < KB_VIRTIO_MODERN_NOTIFY_OFFSET + 0x1000) {
        fprintf(stderr,
            "kobox virtio: %s bar=%u notify+0x%zx value=0x%llx\n",
            op,
            bar->bar,
            offset - KB_VIRTIO_MODERN_NOTIFY_OFFSET,
            (unsigned long long)value);
    }
}

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
    uint8_t value = *reg;
    trace_virtio_access("read8", addr, sizeof(value), value);
    return value;
}

void kb_iowrite8(uint8_t value, void *addr)
{
    if (trace_mmio_enabled()) {
        fprintf(stderr, "kobox mmio: write8 addr=%p value=0x%02x\n", addr, value);
    }
    trace_virtio_access("write8", addr, sizeof(value), value);
    volatile uint8_t *reg = addr;
    *reg = value;
}

uint16_t kb_ioread16(const void *addr)
{
    const volatile uint16_t *reg = addr;
    uint16_t value = *reg;
    trace_virtio_access("read16", addr, sizeof(value), value);
    return value;
}

void kb_iowrite16(uint16_t value, void *addr)
{
    if (trace_mmio_enabled()) {
        fprintf(stderr, "kobox mmio: write16 addr=%p value=0x%04x\n", addr, value);
    }
    trace_virtio_access("write16", addr, sizeof(value), value);
    volatile uint16_t *reg = addr;
    *reg = value;
}

uint32_t kb_ioread32(const void *addr)
{
    const volatile uint32_t *reg = addr;
    uint32_t value = *reg;
    const char *no_indirect = getenv("KOBOX_VIRTIO_NO_INDIRECT");
    if (no_indirect != NULL && no_indirect[0] != '\0' && strcmp(no_indirect, "0") != 0) {
        value &= ~(1u << KB_VIRTIO_RING_F_INDIRECT_DESC);
    }
    const char *no_event_idx = getenv("KOBOX_VIRTIO_NO_EVENT_IDX");
    if (no_event_idx != NULL && no_event_idx[0] != '\0' && strcmp(no_event_idx, "0") != 0) {
        value &= ~(1u << KB_VIRTIO_RING_F_EVENT_IDX);
    }
    if (trace_mmio_enabled()) {
        fprintf(stderr, "kobox mmio: read32 addr=%p value=0x%08x\n", addr, value);
    }
    trace_virtio_access("read32", addr, sizeof(value), value);
    return value;
}

void kb_iowrite32(uint32_t value, void *addr)
{
    if (trace_mmio_enabled()) {
        fprintf(stderr, "kobox mmio: write32 addr=%p value=0x%08x\n", addr, value);
    }
    trace_virtio_access("write32", addr, sizeof(value), value);
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

uint16_t ioread16(const void *addr)
{
    return kb_ioread16(addr);
}

void iowrite16(uint16_t value, void *addr)
{
    kb_iowrite16(value, addr);
}

uint32_t ioread32(const void *addr)
{
    return kb_ioread32(addr);
}

void iowrite32(uint32_t value, void *addr)
{
    kb_iowrite32(value, addr);
}
