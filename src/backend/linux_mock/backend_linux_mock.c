#include "backend/backend_internal.h"
#include "kobox/backend_linux_mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    KB_MOCK_PCI_BAR_COUNT = 6,
    KB_MOCK_IORESOURCE_MEM = 0x00000200,
};

struct kb_device {
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
    unsigned char *bars[KB_MOCK_PCI_BAR_COUNT];
    size_t bar_sizes[KB_MOCK_PCI_BAR_COUNT];
    uint64_t bar_starts[KB_MOCK_PCI_BAR_COUNT];
    uint64_t bar_flags[KB_MOCK_PCI_BAR_COUNT];
};

struct kb_irq {
    kb_irq_handler_t handler;
    void *ctx;
};

typedef struct kb_linux_mock_backend {
    kb_backend_t base;
    struct kb_device device;
} kb_linux_mock_backend_t;

static kb_linux_mock_backend_t *mock_from_backend(kb_backend_t *backend)
{
    return (kb_linux_mock_backend_t *)backend;
}

static void mock_mmio_write32(unsigned char *bar, size_t bar_size, size_t offset, uint32_t value)
{
    if (bar == 0 || offset + sizeof(value) > bar_size) {
        return;
    }
    memcpy(bar + offset, &value, sizeof(value));
}

static void initialize_mock_gpu_mmio(kb_device_t *device)
{
    if (device->pci_id.vendor_id != 0x10de || device->bars[0] == 0) {
        return;
    }
    mock_mmio_write32(device->bars[0], device->bar_sizes[0], 0x000000, 0xb77000a1u);
    mock_mmio_write32(device->bars[0], device->bar_sizes[0], 0x000a00, 0x17700000u);
}

static void mock_destroy(kb_backend_t *backend)
{
    kb_linux_mock_backend_t *mock = mock_from_backend(backend);
    for (unsigned i = 0; i < KB_MOCK_PCI_BAR_COUNT; i++) {
        free(mock->device.bars[i]);
    }
    free(mock);
}

static kb_status_t mock_device_count(kb_backend_t *backend, size_t *out_count)
{
    (void)backend;
    if (out_count == 0) {
        return KB_ERR_INVALID;
    }
    *out_count = 1;
    return KB_OK;
}

static kb_status_t mock_device_at(kb_backend_t *backend, size_t index, kb_device_t **out_device)
{
    if (backend == 0 || out_device == 0) {
        return KB_ERR_INVALID;
    }
    if (index != 0) {
        return KB_ERR_NOT_FOUND;
    }
    *out_device = &mock_from_backend(backend)->device;
    return KB_OK;
}

static kb_status_t mock_device_pci_id(kb_device_t *device, kb_pci_id_t *out_id)
{
    if (device == 0 || out_id == 0) {
        return KB_ERR_INVALID;
    }
    *out_id = device->pci_id;
    return KB_OK;
}

static kb_status_t mock_device_pci_location(kb_device_t *device, kb_pci_location_t *out_location)
{
    if (device == 0 || out_location == 0) {
        return KB_ERR_INVALID;
    }
    *out_location = device->location;
    return KB_OK;
}

static kb_status_t mock_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    (void)device;
    if (dst == 0 || len == 0 || offset > 4096 || len > 4096 || offset + len > 4096) {
        return KB_ERR_INVALID;
    }
    memset(dst, 0, len);
    return KB_OK;
}

static kb_status_t mock_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    (void)device;
    (void)src;
    if (len != 0 && (offset > 4096 || len > 4096 || offset + len > 4096)) {
        return KB_ERR_INVALID;
    }
    return KB_OK;
}

static kb_status_t mock_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    if (device == 0 || out_info == 0) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= KB_MOCK_PCI_BAR_COUNT || device->bars[bar_index] == 0) {
        return KB_ERR_NOT_FOUND;
    }
    out_info->start = device->bar_starts[bar_index];
    out_info->end = device->bar_starts[bar_index] + device->bar_sizes[bar_index] - 1;
    out_info->size = device->bar_sizes[bar_index];
    out_info->flags = device->bar_flags[bar_index];
    out_info->present = 1;
    return KB_OK;
}

static kb_status_t mock_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    if (device == 0 || out_region == 0) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= KB_MOCK_PCI_BAR_COUNT || device->bars[bar_index] == 0) {
        return KB_ERR_NOT_FOUND;
    }
    out_region->addr = device->bars[bar_index];
    out_region->size = device->bar_sizes[bar_index];
    out_region->backend_phys = device->bar_starts[bar_index];
    out_region->flags = device->bar_flags[bar_index];
    return KB_OK;
}

static void mock_unmap_bar(kb_device_t *device, kb_mmio_region_t *region)
{
    (void)device;
    if (region != 0) {
        memset(region, 0, sizeof(*region));
    }
}

static kb_status_t mock_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    (void)device;
    (void)alignment;
    (void)direction;
    if (size == 0 || out_buffer == 0 || size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    void *ptr = calloc(1, (size_t)size);
    if (ptr == 0) {
        return KB_ERR_NOMEM;
    }
    out_buffer->cpu_addr = ptr;
    out_buffer->dma_addr = (uint64_t)(uintptr_t)ptr;
    out_buffer->size = size;
    out_buffer->flags = 0;
    return KB_OK;
}

static void mock_dma_free(kb_device_t *device, kb_dma_buffer_t *buffer)
{
    (void)device;
    if (buffer != 0) {
        free(buffer->cpu_addr);
        memset(buffer, 0, sizeof(*buffer));
    }
}

static kb_status_t mock_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    (void)device;
    (void)direction;
    if (cpu_addr == 0 || size == 0 || out_dma_addr == 0) {
        return KB_ERR_INVALID;
    }
    *out_dma_addr = (uint64_t)(uintptr_t)cpu_addr;
    return KB_OK;
}

static void mock_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)device;
    (void)dma_addr;
    (void)size;
    (void)direction;
}

static kb_status_t mock_irq_register(
    kb_device_t *device,
    unsigned vector,
    kb_irq_handler_t handler,
    void *ctx,
    kb_irq_t **out_irq)
{
    (void)device;
    (void)vector;
    if (handler == 0 || out_irq == 0) {
        return KB_ERR_INVALID;
    }
    kb_irq_t *irq = calloc(1, sizeof(*irq));
    if (irq == 0) {
        return KB_ERR_NOMEM;
    }
    irq->handler = handler;
    irq->ctx = ctx;
    *out_irq = irq;
    return KB_OK;
}

static void mock_irq_unregister(kb_device_t *device, kb_irq_t *irq)
{
    (void)device;
    free(irq);
}

static kb_status_t mock_irq_wait(kb_device_t *device, kb_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    (void)timeout_ns;
    if (irq == 0 || irq->handler == 0) {
        return KB_ERR_INVALID;
    }
    irq->handler(irq->ctx);
    return KB_OK;
}

static uint64_t mock_monotonic_ns(kb_backend_t *backend)
{
    (void)backend;
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void mock_log(kb_backend_t *backend, int level, const char *message)
{
    (void)backend;
    (void)level;
    (void)message;
}

static void configure_mock_pci_id_from_env(kb_linux_mock_backend_t *backend)
{
    const char *pci_id = getenv("KOBOX_MOCK_PCI_ID");
    if (pci_id == NULL || pci_id[0] == '\0') {
        return;
    }

    unsigned vendor = 0;
    unsigned device = 0;
    unsigned class_code = 0;
    unsigned subclass = 0;
    unsigned prog_if = 0;
    const int fields = sscanf(pci_id, "%x:%x:%x:%x:%x", &vendor, &device, &class_code, &subclass, &prog_if);
    if (fields < 2) {
        return;
    }

    backend->device.pci_id.vendor_id = (uint16_t)vendor;
    backend->device.pci_id.device_id = (uint16_t)device;
    if (fields >= 5) {
        backend->device.pci_id.class_code = (uint8_t)class_code;
        backend->device.pci_id.subclass = (uint8_t)subclass;
        backend->device.pci_id.prog_if = (uint8_t)prog_if;
    }
    if (backend->device.pci_id.vendor_id == 0x10de) {
        if (backend->device.pci_id.subsystem_vendor_id == 0) {
            backend->device.pci_id.subsystem_vendor_id = backend->device.pci_id.vendor_id;
        }
        if (backend->device.pci_id.subsystem_device_id == 0) {
            if (backend->device.pci_id.device_id == 0x25b5 || backend->device.pci_id.device_id == 0x25b6) {
                backend->device.pci_id.subsystem_device_id = 0x14a9;
            } else {
                backend->device.pci_id.subsystem_device_id = backend->device.pci_id.device_id;
            }
        }
        if (backend->device.pci_id.revision == 0) {
            backend->device.pci_id.revision = 0xa1;
        }
        backend->device.bar_starts[0] = 0x80000000ull;
        backend->device.bar_sizes[0] = 16u * 1024u * 1024u;
        backend->device.bar_flags[0] = KB_MOCK_IORESOURCE_MEM;
        backend->device.bar_starts[1] = 0x90000000ull;
        backend->device.bar_sizes[1] = 256u * 1024u * 1024u;
        backend->device.bar_flags[1] = KB_MOCK_IORESOURCE_MEM;
        backend->device.bar_starts[2] = 0xb0000000ull;
        backend->device.bar_sizes[2] = 256u * 1024u * 1024u;
        backend->device.bar_flags[2] = KB_MOCK_IORESOURCE_MEM;
        backend->device.bar_starts[3] = 0xa0000000ull;
        backend->device.bar_sizes[3] = 32u * 1024u * 1024u;
        backend->device.bar_flags[3] = KB_MOCK_IORESOURCE_MEM;
    }
}

static const kb_backend_ops_t mock_ops = {
    .destroy = mock_destroy,
    .device_count = mock_device_count,
    .device_at = mock_device_at,
    .device_pci_id = mock_device_pci_id,
    .device_pci_location = mock_device_pci_location,
    .pci_config_read = mock_pci_config_read,
    .pci_config_write = mock_pci_config_write,
    .pci_bar_info = mock_pci_bar_info,
    .map_bar = mock_map_bar,
    .unmap_bar = mock_unmap_bar,
    .dma_alloc = mock_dma_alloc,
    .dma_free = mock_dma_free,
    .dma_map = mock_dma_map,
    .dma_unmap = mock_dma_unmap,
    .irq_register = mock_irq_register,
    .irq_unregister = mock_irq_unregister,
    .irq_wait = mock_irq_wait,
    .monotonic_ns = mock_monotonic_ns,
    .log = mock_log,
};

kb_status_t kb_linux_mock_create(kb_backend_t **out_backend)
{
    if (out_backend == 0) {
        return KB_ERR_INVALID;
    }

    kb_linux_mock_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == 0) {
        return KB_ERR_NOMEM;
    }

    backend->base.ops = &mock_ops;
    backend->device.pci_id.vendor_id = 0x1d6b;
    backend->device.pci_id.device_id = 0x0001;
    backend->device.pci_id.class_code = 0x00;
    backend->device.pci_id.subclass = 0x00;
    backend->device.pci_id.prog_if = 0x00;
    backend->device.location.segment = 0;
    backend->device.location.bus = 0;
    backend->device.location.device = 1;
    backend->device.location.function = 0;
    backend->device.bar_sizes[0] = 4096;
    backend->device.bar_starts[0] = 0;
    backend->device.bar_flags[0] = 0;
    configure_mock_pci_id_from_env(backend);
    for (unsigned i = 0; i < KB_MOCK_PCI_BAR_COUNT; i++) {
        if (backend->device.bar_sizes[i] == 0) {
            continue;
        }
        backend->device.bars[i] = calloc(1, backend->device.bar_sizes[i]);
        if (backend->device.bars[i] == 0) {
            mock_destroy(&backend->base);
            return KB_ERR_NOMEM;
        }
    }
    initialize_mock_gpu_mmio(&backend->device);

    *out_backend = &backend->base;
    return KB_OK;
}
