#include "backend/backend_internal.h"
#include "kobox/backend_linux_mock.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct kb_device {
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
    unsigned char bar0[4096];
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

static void mock_destroy(kb_backend_t *backend)
{
    free(mock_from_backend(backend));
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

static kb_status_t mock_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    if (device == 0 || out_region == 0) {
        return KB_ERR_INVALID;
    }
    if (bar_index != 0) {
        return KB_ERR_NOT_FOUND;
    }
    out_region->addr = device->bar0;
    out_region->size = sizeof(device->bar0);
    out_region->backend_phys = 0;
    out_region->flags = 0;
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

static const kb_backend_ops_t mock_ops = {
    .destroy = mock_destroy,
    .device_count = mock_device_count,
    .device_at = mock_device_at,
    .device_pci_id = mock_device_pci_id,
    .device_pci_location = mock_device_pci_location,
    .pci_config_read = mock_pci_config_read,
    .pci_config_write = mock_pci_config_write,
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
    backend->device.location.segment = 0;
    backend->device.location.bus = 0;
    backend->device.location.device = 1;
    backend->device.location.function = 0;

    *out_backend = &backend->base;
    return KB_OK;
}
