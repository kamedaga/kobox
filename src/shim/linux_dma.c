#include "kobox/shim.h"
#include "subsystem/dma/dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

kb_backend_t *kb_shim_current_backend(void);

static kb_dma_dir_t linux_dma_dir(int dir)
{
    switch (dir) {
    case 1:
        return KB_DMA_TO_DEVICE;
    case 2:
        return KB_DMA_FROM_DEVICE;
    case 0:
    default:
        return KB_DMA_BIDIRECTIONAL;
    }
}

static kb_device_t *dma_device(void *dev)
{
    (void)dev;
    return kb_subsystem_dma_default_device(kb_shim_current_backend());
}

static int trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static int linux_page_to_cpu_addr(void *page, unsigned long offset, void **out_addr)
{
    if (page == NULL || out_addr == NULL) {
        return 0;
    }

    /*
     * The 6.8 modules inline virt_to_page(). With the loader's zero-valued
     * page_offset_base/vmemmap_base shim storage, the non-canonical userspace
     * pointers used here encode as:
     *   page = (virt >> 12) << 6
     * Reverse that representation before handing memory to the backend.
     */
    const uintptr_t encoded = (uintptr_t)page;
    if ((encoded & 0x3fu) != 0) {
        return 0;
    }
    if (offset > 0xffful) {
        return 0;
    }
    const uint64_t page_base = ((uint64_t)encoded >> 6) << 12;
    *out_addr = (void *)(uintptr_t)(page_base + (uint64_t)offset);
    return 1;
}

void *kb_dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs)
{
    (void)dev;
    (void)flags;
    (void)attrs;
    if (size == 0 || dma_handle == NULL) {
        return NULL;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    return kb_subsystem_dma_alloc(backend, dma_device(dev), size, dma_handle);
}

void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs)
{
    (void)dev;
    (void)attrs;
    kb_subsystem_dma_free(kb_shim_current_backend(), size, cpu_addr, dma_handle);
}

void *kb_dma_cpu_addr(uint64_t dma_addr, size_t *out_available)
{
    return kb_subsystem_dma_cpu_addr(dma_addr, out_available);
}

uint64_t kb_dma_map_page_attrs(void *dev, void *page, unsigned long offset, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (page == NULL || size == 0) {
        return 0;
    }

    kb_device_t *device = dma_device(dev);
    if (device == NULL) {
        return 0;
    }

    void *cpu_addr = NULL;
    if (!linux_page_to_cpu_addr(page, offset, &cpu_addr)) {
        if (trace_dma_enabled()) {
            fprintf(stderr,
                "kobox dma: map_page invalid page=%p offset=0x%lx size=0x%zx dir=%d\n",
                page,
                offset,
                size,
                dir);
        }
        return 0;
    }
    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        kb_shim_current_backend(),
        device,
        cpu_addr,
        size,
        linux_dma_dir(dir),
        &status);
    if (trace_dma_enabled()) {
        fprintf(stderr,
            "kobox dma: map_page page=%p offset=0x%lx cpu=%p size=0x%zx dir=%d status=%d dma=0x%llx\n",
            page,
            offset,
            cpu_addr,
            size,
            dir,
            (int)status,
            (unsigned long long)dma_addr);
    }
    return status == KB_OK ? dma_addr : 0;
}

uint64_t kb_dma_map_single_attrs(void *dev, void *ptr, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (ptr == NULL || size == 0) {
        return 0;
    }

    kb_device_t *device = dma_device(dev);
    if (device == NULL) {
        return 0;
    }

    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        kb_shim_current_backend(),
        device,
        ptr,
        size,
        linux_dma_dir(dir),
        &status);
    if (trace_dma_enabled()) {
        fprintf(stderr,
            "kobox dma: map_single cpu=%p size=0x%zx dir=%d status=%d dma=0x%llx\n",
            ptr,
            size,
            dir,
            (int)status,
            (unsigned long long)dma_addr);
    }
    return status == KB_OK ? dma_addr : 0;
}

void kb_dma_unmap_page_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (dma_addr == 0 || size == 0) {
        return;
    }

    kb_subsystem_dma_unmap(kb_shim_current_backend(), dma_device(dev), dma_addr, size, linux_dma_dir(dir));
}

void kb_dma_unmap_single_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    kb_dma_unmap_page_attrs(dev, dma_addr, size, dir, attrs);
}

int kb_dma_mapping_error(void *dev, uint64_t dma_addr)
{
    (void)dev;
    return kb_subsystem_dma_mapping_error(dma_addr);
}

void *dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs)
{
    return kb_dma_alloc_attrs(dev, size, dma_handle, flags, attrs);
}

void dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs)
{
    kb_dma_free_attrs(dev, size, cpu_addr, dma_handle, attrs);
}

uint64_t dma_map_page_attrs(void *dev, void *page, unsigned long offset, size_t size, int dir, unsigned long attrs)
{
    return kb_dma_map_page_attrs(dev, page, offset, size, dir, attrs);
}

uint64_t dma_map_single_attrs(void *dev, void *ptr, size_t size, int dir, unsigned long attrs)
{
    return kb_dma_map_single_attrs(dev, ptr, size, dir, attrs);
}

void dma_unmap_page_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    kb_dma_unmap_page_attrs(dev, dma_addr, size, dir, attrs);
}

void dma_unmap_single_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    kb_dma_unmap_single_attrs(dev, dma_addr, size, dir, attrs);
}

int dma_mapping_error(void *dev, uint64_t dma_addr)
{
    return kb_dma_mapping_error(dev, dma_addr);
}
