#include "kobox/shim.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

enum {
    KB_LINUX_DMA_PAGE_SIZE = 4096,
    KB_LINUX_DMA_STRUCT_PAGE_SIZE = 64,
};

static const uintptr_t KB_LINUX_DMA_ENCODED_PAGE_TAG = (uintptr_t)1ull << 63;
static const uint64_t KB_LINUX_DMA_MAPPING_ERROR = UINT64_MAX;

kb_device_backend_t *kb_shim_current_device_backend(void);

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
    kb_device_t *device = kb_pci_backend_device_for_linux_dev(dev);
    if (device != NULL) {
        return device;
    }
    return kb_subsystem_dma_default_device(kb_shim_current_device_backend());
}

static int trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static int trace_dma_small_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA_SMALL");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void trace_dma_small_bytes(const char *op, const void *cpu_addr, size_t size, int dir, uint64_t dma_addr)
{
    if (!trace_dma_small_enabled() || cpu_addr == NULL || size == 0 || size > 16) {
        return;
    }
    const uint8_t *bytes = (const uint8_t *)cpu_addr;
    fprintf(stderr,
        "kobox dma: %s small cpu=%p size=0x%zx dir=%d dma=0x%llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
        op,
        cpu_addr,
        size,
        dir,
        (unsigned long long)dma_addr,
        size > 0 ? bytes[0] : 0,
        size > 1 ? bytes[1] : 0,
        size > 2 ? bytes[2] : 0,
        size > 3 ? bytes[3] : 0,
        size > 4 ? bytes[4] : 0,
        size > 5 ? bytes[5] : 0,
        size > 6 ? bytes[6] : 0,
        size > 7 ? bytes[7] : 0);
}

static void trace_dma_from_device_head(const char *op, uint64_t dma_addr, size_t size, int dir)
{
    if (!trace_dma_small_enabled() || dir != 2 || size < 8) {
        return;
    }
    size_t available = 0;
    void *cpu_addr = kb_subsystem_dma_cpu_addr(dma_addr, &available);
    if (cpu_addr == NULL || available < 8) {
        fprintf(stderr,
            "kobox dma: %s fromdev cpu-missing size=0x%zx dir=%d dma=0x%llx available=0x%zx\n",
            op,
            size,
            dir,
            (unsigned long long)dma_addr,
            available);
        return;
    }
    const uint8_t *bytes = (const uint8_t *)cpu_addr;
    fprintf(stderr,
        "kobox dma: %s fromdev cpu=%p size=0x%zx dir=%d dma=0x%llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
        op,
        cpu_addr,
        size,
        dir,
        (unsigned long long)dma_addr,
        bytes[0],
        bytes[1],
        bytes[2],
        bytes[3],
        bytes[4],
        bytes[5],
        bytes[6],
        bytes[7]);
}

static int linux_page_to_cpu_addr(void *page, unsigned long offset, void **out_addr)
{
    if (page == NULL || out_addr == NULL) {
        return 0;
    }
    if (offset > 0xffful) {
        return 0;
    }

    /*
     * The 6.8 modules inline virt_to_page() for non-KVM-arena allocations.
     * The SG shim records those userspace pointers as:
     *   page = TAG | ((virt >> 12) << 6)
     * Reverse that representation before handing memory to the backend.
     *
     * Real KVM page-record addresses are handled before this helper by
     * kb_linux_kvm_page_payload_dma_addr(); the tag avoids address-range
     * collisions between encoded userspace pointers and the KVM page arena.
     */
    uintptr_t encoded = (uintptr_t)page;
    if ((encoded & KB_LINUX_DMA_ENCODED_PAGE_TAG) == 0) {
        return 0;
    }
    encoded &= ~KB_LINUX_DMA_ENCODED_PAGE_TAG;
    if ((encoded & 0x3fu) != 0) {
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

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    void *ptr = kb_subsystem_dma_alloc(backend, dma_device(dev), size, dma_handle);
    if (trace_dma_enabled()) {
        fprintf(stderr,
            "kobox dma: alloc size=0x%zx ptr=%p dma=0x%llx\n",
            size,
            ptr,
            (unsigned long long)(ptr == NULL ? 0 : *dma_handle));
    }
    return ptr;
}

void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs)
{
    (void)dev;
    (void)attrs;
    kb_subsystem_dma_free(kb_shim_current_device_backend(), size, cpu_addr, dma_handle);
}

void *kb_dma_cpu_addr(uint64_t dma_addr, size_t *out_available)
{
    return kb_subsystem_dma_cpu_addr(dma_addr, out_available);
}

uint64_t kb_dma_map_page_attrs(void *dev, void *page, unsigned long offset, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (page == NULL || size == 0) {
        return KB_LINUX_DMA_MAPPING_ERROR;
    }

    void *cpu_addr = NULL;
    uint64_t direct_dma_addr = 0;
    if (kb_linux_kvm_page_payload_dma_addr(page, offset, size, &cpu_addr, &direct_dma_addr)) {
        trace_dma_small_bytes("map_page", cpu_addr, size, dir, direct_dma_addr);
        if (trace_dma_small_enabled() && dir == 2 && size == KB_LINUX_DMA_PAGE_SIZE) {
            fprintf(stderr,
                "kobox dma: map_page fromdev page=%p offset=0x%lx cpu=%p size=0x%zx dir=%d dma=0x%llx vmemmap=0x%llx page_offset=0x%llx phys=0x%llx\n",
                page,
                offset,
                cpu_addr,
                size,
                dir,
                (unsigned long long)direct_dma_addr,
                (unsigned long long)kb_linux_kvm_vmemmap_base(),
                (unsigned long long)kb_linux_kvm_page_offset_base(),
                (unsigned long long)kb_linux_kvm_phys_base());
        }
        return direct_dma_addr;
    }

    kb_device_t *device = dma_device(dev);
    if (device == NULL) {
        return KB_LINUX_DMA_MAPPING_ERROR;
    }

    if (!linux_page_to_cpu_addr(page, offset, &cpu_addr)) {
        if (trace_dma_enabled()) {
            fprintf(stderr,
                "kobox dma: map_page invalid page=%p offset=0x%lx size=0x%zx dir=%d\n",
                page,
                offset,
                size,
                dir);
        }
        return KB_LINUX_DMA_MAPPING_ERROR;
    }
    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        kb_shim_current_device_backend(),
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
    if (status != KB_OK || dma_addr == 0) {
        fprintf(stderr,
            "kobox dma: map_page failed-or-zero page=%p offset=0x%lx cpu=%p size=0x%zx dir=%d status=%d dma=0x%llx\n",
            page,
            offset,
            cpu_addr,
            size,
            dir,
            (int)status,
            (unsigned long long)dma_addr);
    }
    if (status == KB_OK) {
        trace_dma_small_bytes("map_page", cpu_addr, size, dir, dma_addr);
        if (trace_dma_small_enabled() && dir == 2 && size == KB_LINUX_DMA_PAGE_SIZE) {
            fprintf(stderr,
                "kobox dma: map_page fromdev page=%p offset=0x%lx cpu=%p size=0x%zx dir=%d dma=0x%llx vmemmap=0x%llx page_offset=0x%llx phys=0x%llx\n",
                page,
                offset,
                cpu_addr,
                size,
                dir,
                (unsigned long long)dma_addr,
                (unsigned long long)kb_linux_kvm_vmemmap_base(),
                (unsigned long long)kb_linux_kvm_page_offset_base(),
                (unsigned long long)kb_linux_kvm_phys_base());
        }
    }
    return status == KB_OK && dma_addr != 0 ? dma_addr : KB_LINUX_DMA_MAPPING_ERROR;
}

uint64_t kb_dma_map_single_attrs(void *dev, void *ptr, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (ptr == NULL || size == 0) {
        return KB_LINUX_DMA_MAPPING_ERROR;
    }

    uint64_t direct_dma_addr = 0;
    if (kb_linux_kvm_payload_dma_addr(ptr, size, &direct_dma_addr)) {
        trace_dma_small_bytes("map_single", ptr, size, dir, direct_dma_addr);
        if (trace_dma_small_enabled() && dir == 2 && size == KB_LINUX_DMA_PAGE_SIZE) {
            fprintf(stderr,
                "kobox dma: map_single fromdev cpu=%p size=0x%zx dir=%d dma=0x%llx\n",
                ptr,
                size,
                dir,
                (unsigned long long)direct_dma_addr);
        }
        return direct_dma_addr;
    }

    kb_device_t *device = dma_device(dev);
    if (device == NULL) {
        return KB_LINUX_DMA_MAPPING_ERROR;
    }

    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        kb_shim_current_device_backend(),
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
    if (status == KB_OK) {
        trace_dma_small_bytes("map_single", ptr, size, dir, dma_addr);
        if (trace_dma_small_enabled() && dir == 2 && size == KB_LINUX_DMA_PAGE_SIZE) {
            fprintf(stderr,
                "kobox dma: map_single fromdev cpu=%p size=0x%zx dir=%d dma=0x%llx\n",
                ptr,
                size,
                dir,
                (unsigned long long)dma_addr);
        }
    }
    if (status != KB_OK || dma_addr == 0) {
        fprintf(stderr,
            "kobox dma: map_single failed-or-zero cpu=%p size=0x%zx dir=%d status=%d dma=0x%llx\n",
            ptr,
            size,
            dir,
            (int)status,
            (unsigned long long)dma_addr);
    }
    return status == KB_OK && dma_addr != 0 ? dma_addr : KB_LINUX_DMA_MAPPING_ERROR;
}

void kb_dma_unmap_page_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    (void)attrs;
    if (dma_addr == 0 || size == 0) {
        return;
    }

    trace_dma_from_device_head("unmap_page", dma_addr, size, dir);
    if (kb_linux_kvm_dma_addr_in_payload_arena(dma_addr, size)) {
        return;
    }
    kb_subsystem_dma_unmap(kb_shim_current_device_backend(), dma_device(dev), dma_addr, size, linux_dma_dir(dir));
}

void kb_dma_unmap_single_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs)
{
    kb_dma_unmap_page_attrs(dev, dma_addr, size, dir, attrs);
}

int kb_dma_mapping_error(void *dev, uint64_t dma_addr)
{
    (void)dev;
    if (dma_addr == KB_LINUX_DMA_MAPPING_ERROR) {
        fprintf(stderr,
            "kobox dma: mapping_error sentinel dma=0x%llx\n",
            (unsigned long long)dma_addr);
        return 1;
    }
    if (kb_linux_kvm_dma_addr_in_payload_arena(dma_addr, 1)) {
        return 0;
    }
    const int failed = kb_subsystem_dma_mapping_error(dma_addr);
    if (failed) {
        fprintf(stderr,
            "kobox dma: mapping_error untracked dma=0x%llx\n",
            (unsigned long long)dma_addr);
    }
    return failed;
}

int kb_dma_need_sync(void *dev, uint64_t dma_addr)
{
    (void)dev;
    (void)dma_addr;
    return 0;
}

size_t kb_dma_max_mapping_size(void *dev)
{
    (void)dev;
    return (size_t)1 << 30;
}

int kb_arch_dma_alloc_attrs(void *dev_ptr, void *gfp_ptr)
{
    (void)dev_ptr;
    (void)gfp_ptr;
    return 1;
}

static uintptr_t kb_linux_dma_map_ops[13] = {
    [0] = (uintptr_t)&kb_dma_alloc_attrs,
    [1] = (uintptr_t)&kb_dma_free_attrs,
    [4] = (uintptr_t)&kb_dma_map_page_attrs,
    [5] = (uintptr_t)&kb_dma_unmap_page_attrs,
    [12] = (uintptr_t)&kb_dma_mapping_error,
};

static void *kb_linux_dma_ops = kb_linux_dma_map_ops;

void *kb_linux_dma_ops_symbol(void)
{
    return &kb_linux_dma_ops;
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
