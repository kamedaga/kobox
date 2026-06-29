#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "device/device_backend_internal.h"
#include "kobox/device_pachaos_capsule.h"

#include "pacha/capsule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

typedef struct kb_pachaos_capsule_backend kb_pachaos_capsule_backend_t;
typedef struct kb_pachaos_mmio_mapping kb_pachaos_mmio_mapping_t;
typedef struct kb_pachaos_dma_mapping kb_pachaos_dma_mapping_t;

enum {
    KB_PACHAOS_PCI_BAR_COUNT = 6,
    KB_PACHAOS_MMIO_MAP_ATTEMPTS = 8,
    KB_LINUX_IORESOURCE_IO = 0x00000100,
    KB_LINUX_IORESOURCE_MEM = 0x00000200,
    KB_LINUX_IORESOURCE_PREFETCH = 0x00002000,
    KB_LINUX_IORESOURCE_MEM_64 = 0x00100000,
};

static const uintptr_t KB_PACHAOS_MMIO_HINT_BASE = UINT64_C(0x780000000000);
static const uintptr_t KB_PACHAOS_MMIO_HINT_STRIDE = UINT64_C(0x10000000);

struct kb_pachaos_mmio_mapping {
    struct pacha_capsule_mmio mmio;
    void *region_addr;
    uint64_t map_size;
    unsigned bar_index;
    kb_pachaos_mmio_mapping_t *next;
};

struct kb_pachaos_dma_mapping {
    struct pacha_capsule_dma dma;
    void *cpu_addr;
    void *mapped_cpu_addr;
    void *alloc_addr;
    uint64_t alloc_size;
    uint64_t size;
    kb_dma_dir_t direction;
    int owns_cpu_addr;
    int owns_mapped_cpu_addr;
    kb_pachaos_dma_mapping_t *next;
};

struct kb_device {
    kb_pachaos_capsule_backend_t *backend;
    int device_fd;
    struct pacha_capsule_info info;
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
    uint64_t dma_mask;
    uint64_t coherent_dma_mask;
};

struct kb_device_irq {
    kb_pachaos_capsule_backend_t *backend;
    struct pacha_capsule_irq irq;
    unsigned vector;
    kb_device_irq_handler_t handler;
    void *ctx;
    kb_device_irq_t *next;
};

struct kb_pachaos_capsule_backend {
    kb_device_backend_t base;
    struct kb_device device;
    uintptr_t next_mmio_hint;
    kb_pachaos_mmio_mapping_t *mmio_mappings;
    kb_pachaos_dma_mapping_t *dma_mappings;
    kb_device_irq_t *irqs;
};

static const kb_device_backend_ops_t pachaos_capsule_ops;
static uint64_t pachaos_capsule_monotonic_ns(kb_device_backend_t *backend);

static kb_status_t status_from_pacha(int status)
{
    switch (status) {
    case 0:
        return KB_OK;
    case PACHA_ERR_INVALID:
        return KB_ERR_INVALID;
    case PACHA_ERR_NOT_READY:
        return KB_ERR_NOT_FOUND;
    case PACHA_ERR_ALLOC:
        return KB_ERR_NOMEM;
    case PACHA_ERR_MAP:
        return KB_ERR_IO;
    default:
        return status < 0 ? KB_ERR_IO : KB_ERR_INVALID;
    }
}

static kb_status_t config_read_u8(kb_device_t *device, uint16_t offset, uint8_t *out)
{
    uint32_t value = 0;
    const int status = pacha_capsule_pci_config_read(device->device_fd, offset, 1, &value);
    if (status != 0) {
        return status_from_pacha(status);
    }
    *out = (uint8_t)value;
    return KB_OK;
}

static kb_status_t config_read_le(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    if (device == NULL || dst == NULL) {
        return KB_ERR_INVALID;
    }

    uint8_t *bytes = dst;
    for (size_t i = 0; i < len; i++) {
        kb_status_t status = config_read_u8(device, (uint16_t)(offset + i), &bytes[i]);
        if (status != KB_OK) {
            return status;
        }
    }
    return KB_OK;
}

static kb_status_t config_write_u8(kb_device_t *device, uint16_t offset, uint8_t value)
{
    const int status = pacha_capsule_pci_config_write(device->device_fd, offset, 1, value);
    return status_from_pacha(status);
}

static kb_status_t config_write_le(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    if (device == NULL || src == NULL) {
        return KB_ERR_INVALID;
    }

    const uint8_t *bytes = src;
    for (size_t i = 0; i < len; i++) {
        kb_status_t status = config_write_u8(device, (uint16_t)(offset + i), bytes[i]);
        if (status != KB_OK) {
            return status;
        }
    }
    return KB_OK;
}

static void refresh_pci_identity(kb_device_t *device)
{
    uint8_t config[48];
    memset(config, 0, sizeof(config));
    if (config_read_le(device, 0, config, sizeof(config)) != KB_OK) {
        return;
    }

    device->pci_id.vendor_id = (uint16_t)config[0] | ((uint16_t)config[1] << 8);
    device->pci_id.device_id = (uint16_t)config[2] | ((uint16_t)config[3] << 8);
    device->pci_id.revision = config[8];
    device->pci_id.prog_if = config[9];
    device->pci_id.subclass = config[10];
    device->pci_id.class_code = config[11];
    device->pci_id.subsystem_vendor_id = (uint16_t)config[44] | ((uint16_t)config[45] << 8);
    device->pci_id.subsystem_device_id = (uint16_t)config[46] | ((uint16_t)config[47] << 8);
}

static void configure_location_from_info(kb_device_t *device)
{
    const uint64_t value = device->info.device;
    device->location.segment = (uint16_t)((value >> 16) & 0xffffu);
    device->location.bus = (uint8_t)((value >> 8) & 0xffu);
    device->location.device = (uint8_t)((value >> 3) & 0x1fu);
    device->location.function = (uint8_t)(value & 0x7u);
}

static kb_pachaos_capsule_backend_t *from_backend(kb_device_backend_t *backend)
{
    return (kb_pachaos_capsule_backend_t *)backend;
}

static uint64_t page_size_u64(void)
{
    return 4096;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    const uint64_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

static int is_power_of_two_u64(uint64_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

static void *alloc_aligned_zeroed(uint64_t size, uint64_t alignment, void **out_alloc, uint64_t *out_alloc_size)
{
    if (out_alloc != NULL) {
        *out_alloc = NULL;
    }
    if (out_alloc_size != NULL) {
        *out_alloc_size = 0;
    }
    if (size == 0 || size > (uint64_t)SIZE_MAX || alignment == 0 || !is_power_of_two_u64(alignment)) {
        return NULL;
    }
    const uint64_t page_size = page_size_u64();
    if (alignment < page_size) {
        alignment = page_size;
    }
    if (size > UINT64_MAX - alignment) {
        return NULL;
    }
    const uint64_t raw_size = align_up_u64(size + alignment, page_size);
    if (raw_size == 0 || raw_size > (uint64_t)SIZE_MAX) {
        return NULL;
    }

#if !defined(_WIN32)
    void *raw = mmap(NULL, (size_t)raw_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        raw = NULL;
    }
#else
    void *raw = calloc(1, (size_t)raw_size);
#endif
    if (raw == NULL) {
        return NULL;
    }
    const uintptr_t aligned = (uintptr_t)align_up_u64((uint64_t)(uintptr_t)raw, alignment);
    if ((uintptr_t)raw > aligned || (uint64_t)(aligned - (uintptr_t)raw) > raw_size ||
        size > raw_size - (uint64_t)(aligned - (uintptr_t)raw)) {
#if !defined(_WIN32)
        (void)munmap(raw, (size_t)raw_size);
#else
        free(raw);
#endif
        return NULL;
    }

    if (out_alloc != NULL) {
        *out_alloc = raw;
    }
    if (out_alloc_size != NULL) {
        *out_alloc_size = raw_size;
    }
    return (void *)aligned;
}

static void free_aligned_zeroed(void *alloc_addr, uint64_t alloc_size)
{
    if (alloc_addr == NULL) {
        return;
    }
#if !defined(_WIN32)
    if (alloc_size != 0) {
        (void)munmap(alloc_addr, (size_t)alloc_size);
        return;
    }
#else
    (void)alloc_size;
#endif
    free(alloc_addr);
}

static uint64_t dma_dir_to_pacha(kb_dma_dir_t direction)
{
    switch (direction) {
    case KB_DMA_TO_DEVICE:
        return PACHA_CAPSULE_DMA_TO_DEVICE;
    case KB_DMA_FROM_DEVICE:
        return PACHA_CAPSULE_DMA_FROM_DEVICE;
    case KB_DMA_BIDIRECTIONAL:
    default:
        return PACHA_CAPSULE_DMA_BIDIRECTIONAL;
    }
}

static uint64_t bar_flags_to_linux(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & PACHA_CAPSULE_BAR_IO) != 0) {
        out |= KB_LINUX_IORESOURCE_IO;
    }
    if ((flags & PACHA_CAPSULE_BAR_MEM) != 0) {
        out |= KB_LINUX_IORESOURCE_MEM;
    }
    if ((flags & PACHA_CAPSULE_BAR_PREFETCHABLE) != 0) {
        out |= KB_LINUX_IORESOURCE_PREFETCH;
    }
    if ((flags & PACHA_CAPSULE_BAR_64BIT) != 0) {
        out |= KB_LINUX_IORESOURCE_MEM_64;
    }
    return out;
}

static kb_pachaos_mmio_mapping_t *take_mmio_mapping(kb_pachaos_capsule_backend_t *backend, void *region_addr)
{
    kb_pachaos_mmio_mapping_t **cursor = backend != NULL ? &backend->mmio_mappings : NULL;
    while (cursor != NULL && *cursor != NULL) {
        kb_pachaos_mmio_mapping_t *mapping = *cursor;
        if (mapping->region_addr == region_addr) {
            *cursor = mapping->next;
            mapping->next = NULL;
            return mapping;
        }
        cursor = &mapping->next;
    }
    return NULL;
}

static kb_status_t remember_mmio_mapping(kb_pachaos_capsule_backend_t *backend, const struct pacha_capsule_mmio *mmio, void *region_addr, uint64_t map_size, unsigned bar_index)
{
    if (backend == NULL || mmio == NULL || !pacha_capsule_is_fd(mmio->fd) || mmio->addr == NULL || mmio->len == 0) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_mmio_mapping_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return KB_ERR_NOMEM;
    }
    entry->mmio = *mmio;
    entry->region_addr = region_addr;
    entry->map_size = map_size;
    entry->bar_index = bar_index;
    entry->next = backend->mmio_mappings;
    backend->mmio_mappings = entry;
    return KB_OK;
}

static kb_pachaos_dma_mapping_t *take_dma_mapping(kb_pachaos_capsule_backend_t *backend, uint64_t iova)
{
    kb_pachaos_dma_mapping_t **cursor = backend != NULL ? &backend->dma_mappings : NULL;
    while (cursor != NULL && *cursor != NULL) {
        kb_pachaos_dma_mapping_t *mapping = *cursor;
        if (mapping->dma.iova == iova) {
            *cursor = mapping->next;
            mapping->next = NULL;
            return mapping;
        }
        cursor = &mapping->next;
    }
    return NULL;
}

static kb_status_t remember_dma_mapping(kb_pachaos_capsule_backend_t *backend, const kb_pachaos_dma_mapping_t *mapping)
{
    if (backend == NULL || mapping == NULL || !pacha_capsule_is_fd(mapping->dma.fd) || mapping->dma.iova == 0) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_dma_mapping_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return KB_ERR_NOMEM;
    }
    *entry = *mapping;
    entry->next = backend->dma_mappings;
    backend->dma_mappings = entry;
    return KB_OK;
}

static void release_dma_mapping(kb_pachaos_dma_mapping_t *mapping, int copy_back)
{
    if (mapping == NULL) {
        return;
    }
    if (mapping->owns_mapped_cpu_addr && copy_back && mapping->cpu_addr != NULL && mapping->mapped_cpu_addr != NULL) {
        memcpy(mapping->cpu_addr, mapping->mapped_cpu_addr, (size_t)mapping->size);
    }
    if (pacha_capsule_is_fd(mapping->dma.fd)) {
        (void)pacha_capsule_close(mapping->dma.fd);
    }
    if (mapping->alloc_addr != NULL) {
        free_aligned_zeroed(mapping->alloc_addr, mapping->alloc_size);
    } else if (mapping->owns_cpu_addr) {
        free(mapping->cpu_addr);
    } else if (mapping->owns_mapped_cpu_addr) {
        free(mapping->mapped_cpu_addr);
    }
    free(mapping);
}

static void pachaos_capsule_destroy(kb_device_backend_t *backend)
{
    kb_pachaos_capsule_backend_t *pacha = from_backend(backend);
    if (pacha == NULL) {
        return;
    }
    while (pacha->irqs != NULL) {
        kb_device_irq_t *irq = pacha->irqs;
        pacha->irqs = irq->next;
        if (pacha_capsule_is_fd(irq->irq.fd)) {
            (void)pacha_capsule_close(irq->irq.fd);
        }
        free(irq);
    }
    while (pacha->dma_mappings != NULL) {
        kb_pachaos_dma_mapping_t *mapping = pacha->dma_mappings;
        pacha->dma_mappings = mapping->next;
        mapping->next = NULL;
        release_dma_mapping(mapping, 0);
    }
    while (pacha->mmio_mappings != NULL) {
        kb_pachaos_mmio_mapping_t *mapping = pacha->mmio_mappings;
        pacha->mmio_mappings = mapping->next;
        if (pacha_capsule_is_fd(mapping->mmio.fd)) {
            (void)pacha_capsule_close(mapping->mmio.fd);
        }
        free(mapping);
    }
    if (pacha_capsule_is_fd(pacha->device.device_fd)) {
        (void)pacha_capsule_close(pacha->device.device_fd);
        pacha->device.device_fd = -1;
    }
    free(pacha);
}

static kb_status_t pachaos_capsule_device_count(kb_device_backend_t *backend, size_t *out_count)
{
    if (backend == NULL || out_count == NULL) {
        return KB_ERR_INVALID;
    }
    *out_count = 1;
    return KB_OK;
}

static kb_status_t pachaos_capsule_device_at(kb_device_backend_t *backend, size_t index, kb_device_t **out_device)
{
    if (backend == NULL || out_device == NULL) {
        return KB_ERR_INVALID;
    }
    if (index != 0) {
        return KB_ERR_NOT_FOUND;
    }
    *out_device = &from_backend(backend)->device;
    return KB_OK;
}

static kb_status_t pachaos_capsule_device_pci_id(kb_device_t *device, kb_pci_id_t *out_id)
{
    if (device == NULL || out_id == NULL) {
        return KB_ERR_INVALID;
    }
    *out_id = device->pci_id;
    return KB_OK;
}

static kb_status_t pachaos_capsule_device_pci_location(kb_device_t *device, kb_pci_location_t *out_location)
{
    if (device == NULL || out_location == NULL) {
        return KB_ERR_INVALID;
    }
    *out_location = device->location;
    return KB_OK;
}

static kb_status_t pachaos_capsule_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    return config_read_le(device, offset, dst, len);
}

static kb_status_t pachaos_capsule_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    return config_write_le(device, offset, src, len);
}

static kb_status_t pachaos_capsule_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    if (device == NULL || out_info == NULL || bar_index >= 6) {
        return KB_ERR_INVALID;
    }

    struct pacha_capsule_bar_info bar = {0};
    const int status = pacha_capsule_pci_bar_info(device->device_fd, bar_index, &bar);
    if (status != 0) {
        return status_from_pacha(status);
    }

    out_info->start = bar.start;
    out_info->end = bar.end;
    out_info->size = bar.size;
    out_info->flags = (uint32_t)bar_flags_to_linux(bar.flags);
    out_info->present = bar.size != 0;
    return KB_OK;
}

static void *reserve_mmio_user_va(kb_pachaos_capsule_backend_t *backend, uint64_t map_size)
{
    if (backend == NULL || map_size == 0 || map_size > (uint64_t)SIZE_MAX) {
        return NULL;
    }
    const uint64_t page_size = page_size_u64();
    if (backend->next_mmio_hint < KB_PACHAOS_MMIO_HINT_BASE) {
        backend->next_mmio_hint = KB_PACHAOS_MMIO_HINT_BASE;
    }
    uintptr_t hint = backend->next_mmio_hint;
    if ((hint & (uintptr_t)(page_size - 1u)) != 0) {
        hint = (uintptr_t)align_up_u64((uint64_t)hint, page_size);
    }
    const uint64_t stride = align_up_u64(map_size, KB_PACHAOS_MMIO_HINT_STRIDE);
    const uintptr_t next = hint + (uintptr_t)stride;
    backend->next_mmio_hint = next > hint ? next : KB_PACHAOS_MMIO_HINT_BASE;
    return (void *)hint;
}

static kb_status_t pachaos_capsule_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    if (device == NULL || out_region == NULL || bar_index >= KB_PACHAOS_PCI_BAR_COUNT) {
        return KB_ERR_INVALID;
    }
    memset(out_region, 0, sizeof(*out_region));

    kb_pci_bar_info_t info;
    kb_status_t info_status = pachaos_capsule_pci_bar_info(device, bar_index, &info);
    if (info_status != KB_OK) {
        return info_status;
    }
    if (!info.present || info.size == 0 || (info.flags & KB_LINUX_IORESOURCE_MEM) == 0) {
        return KB_ERR_INVALID;
    }

    const uint64_t page_size = page_size_u64();
    const uint64_t page_offset = info.start & (page_size - 1u);
    if (info.size > UINT64_MAX - page_offset) {
        return KB_ERR_INVALID;
    }
    const uint64_t map_size = align_up_u64(page_offset + info.size, page_size);
    if (map_size == 0 || map_size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_capsule_backend_t *backend = device->backend;
    struct pacha_capsule_mmio mmio = {0};
    kb_status_t last_status = KB_ERR_IO;
    for (unsigned attempt = 0; attempt < KB_PACHAOS_MMIO_MAP_ATTEMPTS; attempt++) {
        void *map_addr = reserve_mmio_user_va(backend, map_size);
        if (map_addr == NULL) {
            return KB_ERR_NOMEM;
        }
        const int status = pacha_capsule_device_derive_mmio(
            device->device_fd,
            bar_index,
            map_addr,
            (size_t)map_size,
            PACHA_CAPSULE_MMIO_REPLACE_EXISTING,
            &mmio);
        if (status == 0) {
            break;
        }
        last_status = status_from_pacha(status);
        memset(&mmio, 0, sizeof(mmio));
        if (last_status != KB_ERR_NOMEM && last_status != KB_ERR_IO) {
            return last_status;
        }
    }
    if (!pacha_capsule_is_fd(mmio.fd) || mmio.addr == NULL || mmio.len < map_size) {
        return last_status;
    }
    void *region_addr = (void *)((unsigned char *)mmio.addr + page_offset);
    kb_status_t status = remember_mmio_mapping(backend, &mmio, region_addr, map_size, bar_index);
    if (status != KB_OK) {
        (void)pacha_capsule_close(mmio.fd);
        return status;
    }
    out_region->addr = region_addr;
    out_region->size = info.size;
    out_region->host_phys = info.start;
    out_region->flags = (uint32_t)info.flags;
    return KB_OK;
}

static void pachaos_capsule_unmap_bar(kb_device_t *device, kb_mmio_region_t *region)
{
    if (device == NULL || region == NULL) {
        return;
    }
    kb_pachaos_mmio_mapping_t *mapping = take_mmio_mapping(device->backend, region->addr);
    if (mapping != NULL) {
        if (pacha_capsule_is_fd(mapping->mmio.fd)) {
            (void)pacha_capsule_close(mapping->mmio.fd);
        }
        free(mapping);
    }
    memset(region, 0, sizeof(*region));
}

static kb_status_t pachaos_capsule_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    if (device == NULL || out_buffer == NULL || size == 0 || size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    memset(out_buffer, 0, sizeof(*out_buffer));

    const uint64_t page_size = page_size_u64();
    uint64_t effective_alignment = alignment > page_size ? alignment : page_size;
    if (!is_power_of_two_u64(effective_alignment)) {
        return KB_ERR_INVALID;
    }
    const uint64_t alloc_size = align_up_u64(size, effective_alignment);
    void *alloc_addr = NULL;
    uint64_t raw_size = 0;
    void *ptr = alloc_aligned_zeroed(alloc_size, effective_alignment, &alloc_addr, &raw_size);
    if (ptr == NULL) {
        return KB_ERR_NOMEM;
    }

    struct pacha_capsule_dma dma = {0};
    const int status = pacha_capsule_device_derive_dma_buffer(device->device_fd, ptr, 0, (size_t)alloc_size, 0, &dma);
    if (status != 0) {
        free_aligned_zeroed(alloc_addr, raw_size);
        return status_from_pacha(status);
    }

    kb_pachaos_dma_mapping_t mapping = {
        .dma = dma,
        .cpu_addr = ptr,
        .mapped_cpu_addr = ptr,
        .alloc_addr = alloc_addr,
        .alloc_size = raw_size,
        .size = alloc_size,
        .direction = direction,
        .owns_cpu_addr = 1,
        .owns_mapped_cpu_addr = 0,
    };
    kb_status_t remember_status = remember_dma_mapping(device->backend, &mapping);
    if (remember_status != KB_OK) {
        (void)pacha_capsule_close(dma.fd);
        free_aligned_zeroed(alloc_addr, raw_size);
        return remember_status;
    }

    out_buffer->cpu_addr = ptr;
    out_buffer->dma_addr = dma.iova;
    out_buffer->size = alloc_size;
    out_buffer->flags = 0;
    return KB_OK;
}

static void pachaos_capsule_dma_free(kb_device_t *device, kb_dma_buffer_t *buffer)
{
    if (device == NULL || buffer == NULL) {
        return;
    }
    kb_pachaos_dma_mapping_t *mapping = take_dma_mapping(device->backend, buffer->dma_addr);
    if (mapping != NULL) {
        release_dma_mapping(mapping, 0);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static int dma_range_crosses_page(const void *ptr, uint64_t size)
{
    if (ptr == NULL || size == 0) {
        return 0;
    }
    const uint64_t page_size = page_size_u64();
    const uint64_t offset = (uint64_t)((uintptr_t)ptr & (uintptr_t)(page_size - 1u));
    return size > UINT64_MAX - offset || offset + size > page_size;
}

static kb_status_t pachaos_capsule_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    if (device == NULL || cpu_addr == NULL || size == 0 || size > (uint64_t)SIZE_MAX || out_dma_addr == NULL) {
        return KB_ERR_INVALID;
    }
    *out_dma_addr = 0;

    const uint64_t page_size = page_size_u64();
    const uint64_t map_size = align_up_u64(size, page_size);
    void *mapped = cpu_addr;
    void *alloc_addr = NULL;
    uint64_t raw_size = 0;
    int owns_mapped = 0;
    if (dma_range_crosses_page(cpu_addr, size)) {
        mapped = alloc_aligned_zeroed(map_size, page_size, &alloc_addr, &raw_size);
        if (mapped == NULL) {
            return KB_ERR_NOMEM;
        }
        if (direction == KB_DMA_TO_DEVICE || direction == KB_DMA_BIDIRECTIONAL) {
            memcpy(mapped, cpu_addr, (size_t)size);
        }
        owns_mapped = 1;
    }

    struct pacha_capsule_dma dma = {0};
    const int status = pacha_capsule_derive_dma_mapping(
        device->device_fd,
        mapped,
        0,
        (size_t)(owns_mapped ? map_size : size),
        (unsigned)dma_dir_to_pacha(direction),
        0);
    if (!pacha_capsule_is_fd(status)) {
        if (alloc_addr != NULL) {
            free_aligned_zeroed(alloc_addr, raw_size);
        }
        return status_from_pacha(status);
    }
    const int read_status = pacha_capsule_dma_from_fd(status, &dma);
    if (read_status != 0) {
        (void)pacha_capsule_close(status);
        if (alloc_addr != NULL) {
            free_aligned_zeroed(alloc_addr, raw_size);
        }
        return status_from_pacha(read_status);
    }

    kb_pachaos_dma_mapping_t mapping = {
        .dma = dma,
        .cpu_addr = cpu_addr,
        .mapped_cpu_addr = mapped,
        .alloc_addr = alloc_addr,
        .alloc_size = raw_size,
        .size = size,
        .direction = direction,
        .owns_cpu_addr = 0,
        .owns_mapped_cpu_addr = owns_mapped,
    };
    kb_status_t remember_status = remember_dma_mapping(device->backend, &mapping);
    if (remember_status != KB_OK) {
        (void)pacha_capsule_close(dma.fd);
        if (alloc_addr != NULL) {
            free_aligned_zeroed(alloc_addr, raw_size);
        }
        return remember_status;
    }
    *out_dma_addr = dma.iova;
    return KB_OK;
}

static void pachaos_capsule_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)size;
    if (device == NULL) {
        return;
    }
    kb_pachaos_dma_mapping_t *mapping = take_dma_mapping(device->backend, dma_addr);
    if (mapping != NULL) {
        const int copy_back = direction == KB_DMA_FROM_DEVICE || direction == KB_DMA_BIDIRECTIONAL ||
            mapping->direction == KB_DMA_FROM_DEVICE || mapping->direction == KB_DMA_BIDIRECTIONAL;
        release_dma_mapping(mapping, copy_back);
    }
}

static kb_status_t pachaos_capsule_dma_set_mask(kb_device_t *device, uint64_t mask, int coherent)
{
    if (device == NULL || mask == 0) {
        return KB_ERR_INVALID;
    }
    if (coherent) {
        device->coherent_dma_mask = mask;
    } else {
        device->dma_mask = mask;
    }
    return KB_OK;
}

static kb_status_t pachaos_capsule_irq_register(
    kb_device_t *device,
    unsigned vector,
    kb_device_irq_handler_t handler,
    void *ctx,
    kb_device_irq_t **out_irq)
{
    if (device == NULL || handler == NULL || out_irq == NULL) {
        return KB_ERR_INVALID;
    }
    *out_irq = NULL;

    struct pacha_capsule_irq irq_fd = {0};
    const int status = pacha_capsule_device_derive_irq(device->device_fd, PACHA_CAPSULE_IRQ_AUTO, vector, 0, &irq_fd);
    if (status != 0) {
        return status_from_pacha(status);
    }

    kb_device_irq_t *irq = calloc(1, sizeof(*irq));
    if (irq == NULL) {
        (void)pacha_capsule_close(irq_fd.fd);
        return KB_ERR_NOMEM;
    }
    irq->backend = device->backend;
    irq->irq = irq_fd;
    irq->vector = vector;
    irq->handler = handler;
    irq->ctx = ctx;
    irq->next = device->backend->irqs;
    device->backend->irqs = irq;
    *out_irq = irq;
    return KB_OK;
}

static void pachaos_capsule_irq_unregister(kb_device_t *device, kb_device_irq_t *irq)
{
    if (irq == NULL) {
        return;
    }
    kb_pachaos_capsule_backend_t *backend = device != NULL && device->backend != NULL ? device->backend : irq->backend;
    kb_device_irq_t **cursor = backend != NULL ? &backend->irqs : NULL;
    while (cursor != NULL && *cursor != NULL) {
        if (*cursor == irq) {
            *cursor = irq->next;
            irq->next = NULL;
            if (pacha_capsule_is_fd(irq->irq.fd)) {
                (void)pacha_capsule_close(irq->irq.fd);
            }
            free(irq);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static kb_status_t pachaos_capsule_irq_wait(kb_device_t *device, kb_device_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    if (irq == NULL || !pacha_capsule_is_fd(irq->irq.fd)) {
        return KB_ERR_INVALID;
    }
    uint64_t attempts = timeout_ns == 0 ? 1 : (timeout_ns + 999999ull) / 1000000ull;
    if (attempts == 0) {
        attempts = 1;
    }
    if (attempts > 4096) {
        attempts = 4096;
    }
    for (uint64_t i = 0; i < attempts; i++) {
        uint64_t next_count = 0;
        const int status = pacha_capsule_irq_poll(irq->irq.fd, irq->irq.count, &next_count);
        if (status == 0) {
            irq->irq.count = next_count;
            irq->handler(irq->ctx);
            return KB_OK;
        }
        if (status != PACHA_ERR_NOT_READY) {
            return status_from_pacha(status);
        }
        if (timeout_ns == 0) {
            return KB_ERR_NOT_FOUND;
        }
        __asm__ volatile("pause");
    }
    return KB_ERR_NOT_FOUND;
}

static uint64_t pachaos_capsule_monotonic_ns(kb_device_backend_t *backend)
{
    (void)backend;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void pachaos_capsule_log(kb_device_backend_t *backend, int level, const char *message)
{
    (void)backend;
    fprintf(stderr, "kobox pachaos_capsule[%d]: %s\n", level, message == NULL ? "" : message);
}

static const kb_device_backend_ops_t pachaos_capsule_ops = {
    .destroy = pachaos_capsule_destroy,
    .device_count = pachaos_capsule_device_count,
    .device_at = pachaos_capsule_device_at,
    .device_pci_id = pachaos_capsule_device_pci_id,
    .device_pci_location = pachaos_capsule_device_pci_location,
    .pci_config_read = pachaos_capsule_pci_config_read,
    .pci_config_write = pachaos_capsule_pci_config_write,
    .pci_bar_info = pachaos_capsule_pci_bar_info,
    .map_bar = pachaos_capsule_map_bar,
    .unmap_bar = pachaos_capsule_unmap_bar,
    .dma_alloc = pachaos_capsule_dma_alloc,
    .dma_free = pachaos_capsule_dma_free,
    .dma_map = pachaos_capsule_dma_map,
    .dma_unmap = pachaos_capsule_dma_unmap,
    .dma_set_mask = pachaos_capsule_dma_set_mask,
    .irq_register = pachaos_capsule_irq_register,
    .irq_unregister = pachaos_capsule_irq_unregister,
    .irq_wait = pachaos_capsule_irq_wait,
    .monotonic_ns = pachaos_capsule_monotonic_ns,
    .log = pachaos_capsule_log,
};

kb_status_t kb_pachaos_capsule_device_create(uint64_t device_capsule, kb_device_backend_t **out_backend)
{
    if (out_backend == NULL || device_capsule > INT32_MAX) {
        return KB_ERR_INVALID;
    }
    *out_backend = NULL;

    const int device_fd = (int)device_capsule;
    if (!pacha_capsule_is_fd(device_fd)) {
        return KB_ERR_INVALID;
    }

    struct pacha_capsule_info info;
    const int status = pacha_capsule_expect_kind(device_fd, PACHA_CAPSULE_KIND_DEVICE, &info);
    if (status != 0) {
        return status_from_pacha(status);
    }

    kb_pachaos_capsule_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return KB_ERR_NOMEM;
    }

    backend->base.ops = &pachaos_capsule_ops;
    backend->device.backend = backend;
    backend->device.device_fd = device_fd;
    backend->device.info = info;
    backend->device.dma_mask = UINT64_MAX;
    backend->device.coherent_dma_mask = UINT64_MAX;
    backend->next_mmio_hint = KB_PACHAOS_MMIO_HINT_BASE;
    configure_location_from_info(&backend->device);
    refresh_pci_identity(&backend->device);

    *out_backend = &backend->base;
    return KB_OK;
}

size_t kb_pachaos_capsule_report_residuals(kb_device_backend_t *backend, FILE *out, const char *label)
{
    (void)backend;
    (void)out;
    (void)label;
    return 0;
}
