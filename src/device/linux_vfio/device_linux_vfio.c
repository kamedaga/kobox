#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "device/device_backend_internal.h"
#include "kobox/device_linux_vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

enum {
    KB_VFIO_PATH_MAX = 4096,
    KB_VFIO_PCI_CONFIG_SIZE = 4096,
    KB_VFIO_DMA_IOVA_BASE = 0x01000000,
    KB_PCI_COMMAND_OFFSET = 0x04,
    KB_PCI_COMMAND_MEMORY = 0x0002,
    KB_PCI_STATUS_OFFSET = 0x06,
    KB_PCI_STATUS_CAP_LIST = 0x0010,
    KB_PCI_CAPABILITY_LIST_OFFSET = 0x34,
    KB_PCI_CAP_ID_MSI = 0x05,
    KB_PCI_CAP_ID_MSIX = 0x11,
    KB_PCI_CAP_NEXT_MASK = 0xfc,
    KB_PCI_MSI_CONTROL_ENABLE = 0x0001,
    KB_PCI_MSIX_CONTROL_ENABLE = 0x8000,
    KB_IRQ_BACKEND_KIND_SHIFT = 30,
    KB_IRQ_BACKEND_VECTOR_MASK = 0x3fffffff,
    KB_IRQ_BACKEND_KIND_MSI = 1,
    KB_IRQ_BACKEND_KIND_MSIX = 2,
    KB_PCI_CLASS_STORAGE = 0x01,
    KB_PCI_SUBCLASS_NVME = 0x08,
    KB_PCI_PROGIF_NVME = 0x02,
    KB_NVME_REG_CC = 0x14,
    KB_NVME_REG_CSTS = 0x1c,
};

typedef struct kb_linux_vfio_backend kb_linux_vfio_backend_t;

struct kb_device {
    kb_linux_vfio_backend_t *backend;
    int device_fd;
    char bdf[32];
    char sysfs_path[KB_VFIO_PATH_MAX];
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
};

struct kb_device_irq {
    int event_fd;
    uint32_t index;
    uint32_t start;
    kb_device_irq_handler_t handler;
    void *ctx;
};

typedef struct kb_vfio_dma_mapping {
    uint64_t iova;
    uint64_t vaddr;
    uint64_t size;
    struct kb_vfio_dma_mapping *next;
} kb_vfio_dma_mapping_t;

struct kb_linux_vfio_backend {
    kb_device_backend_t base;
    int container_fd;
    int group_fd;
    int device_fd;
    int group_id;
    uint64_t next_iova;
    kb_vfio_dma_mapping_t *dma_mappings;
    struct kb_device device;
};

static void vfio_quiesce_nvme(kb_linux_vfio_backend_t *vfio);
static void vfio_disable_irq_index(kb_device_t *device, uint32_t index);
static void vfio_unmap_all_dma(kb_linux_vfio_backend_t *vfio);

static kb_linux_vfio_backend_t *vfio_from_backend(kb_device_backend_t *backend)
{
    return (kb_linux_vfio_backend_t *)backend;
}

static int vfio_low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static int path_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    int written = snprintf(dst, dst_size, "%s/%s", a, b);
    return written >= 0 && (size_t)written < dst_size;
}

static uint64_t page_size(void)
{
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (uint64_t)value : 4096;
}

static uint64_t align_down_u64(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1u);
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    const uint64_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

static kb_status_t remember_dma_mapping(kb_linux_vfio_backend_t *backend, uint64_t iova, uint64_t vaddr, uint64_t size)
{
    if (backend == NULL) {
        return KB_ERR_INVALID;
    }
    kb_vfio_dma_mapping_t *mapping = calloc(1, sizeof(*mapping));
    if (mapping == NULL) {
        return KB_ERR_NOMEM;
    }
    mapping->iova = iova;
    mapping->vaddr = vaddr;
    mapping->size = size;
    mapping->next = backend->dma_mappings;
    backend->dma_mappings = mapping;
    return KB_OK;
}

static kb_status_t errno_status(void)
{
    if (errno == EACCES || errno == EPERM) {
        return KB_ERR_DENIED;
    }
    if (errno == ENOENT || errno == ENODEV) {
        return KB_ERR_NOT_FOUND;
    }
    return KB_ERR_IO;
}

static int vfio_trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static kb_status_t read_text_file(const char *path, char *dst, size_t dst_size)
{
    if (path == NULL || dst == NULL || dst_size == 0) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return errno_status();
    }

    size_t read_size = fread(dst, 1, dst_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        return KB_ERR_IO;
    }
    dst[read_size] = '\0';
    fclose(file);
    return KB_OK;
}

static kb_status_t read_hex_file(const char *path, unsigned long *out_value)
{
    char text[64];
    kb_status_t status = read_text_file(path, text, sizeof(text));
    if (status != KB_OK) {
        return status;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text) {
        return KB_ERR_INVALID;
    }
    *out_value = value;
    return KB_OK;
}

static kb_status_t read_device_hex(const char *device_path, const char *file_name, unsigned long *out_value)
{
    char path[KB_VFIO_PATH_MAX];
    if (!path_join(path, sizeof(path), device_path, file_name)) {
        return KB_ERR_INVALID;
    }
    return read_hex_file(path, out_value);
}

static int parse_location(const char *name, kb_pci_location_t *out_location)
{
    unsigned segment = 0;
    unsigned bus = 0;
    unsigned device = 0;
    unsigned function = 0;
    if (sscanf(name, "%x:%x:%x.%x", &segment, &bus, &device, &function) != 4) {
        return 0;
    }
    if (segment > UINT16_MAX || bus > UINT8_MAX || device > UINT8_MAX || function > UINT8_MAX) {
        return 0;
    }

    out_location->segment = (uint16_t)segment;
    out_location->bus = (uint8_t)bus;
    out_location->device = (uint8_t)device;
    out_location->function = (uint8_t)function;
    return 1;
}

static kb_status_t read_device_metadata(struct kb_device *device)
{
    unsigned long value = 0;
    kb_status_t status = read_device_hex(device->sysfs_path, "vendor", &value);
    if (status != KB_OK || value > UINT16_MAX) {
        return status == KB_OK ? KB_ERR_INVALID : status;
    }
    device->pci_id.vendor_id = (uint16_t)value;

    status = read_device_hex(device->sysfs_path, "device", &value);
    if (status != KB_OK || value > UINT16_MAX) {
        return status == KB_OK ? KB_ERR_INVALID : status;
    }
    device->pci_id.device_id = (uint16_t)value;

    status = read_device_hex(device->sysfs_path, "subsystem_vendor", &value);
    if (status == KB_OK && value <= UINT16_MAX) {
        device->pci_id.subsystem_vendor_id = (uint16_t)value;
    }

    status = read_device_hex(device->sysfs_path, "subsystem_device", &value);
    if (status == KB_OK && value <= UINT16_MAX) {
        device->pci_id.subsystem_device_id = (uint16_t)value;
    }

    status = read_device_hex(device->sysfs_path, "class", &value);
    if (status == KB_OK) {
        device->pci_id.class_code = (uint8_t)((value >> 16) & 0xffu);
        device->pci_id.subclass = (uint8_t)((value >> 8) & 0xffu);
        device->pci_id.prog_if = (uint8_t)(value & 0xffu);
    }

    if (!parse_location(device->bdf, &device->location)) {
        return KB_ERR_INVALID;
    }
    return KB_OK;
}

static kb_status_t read_group_id(const char *sysfs_path, int *out_group_id)
{
    char link_path[KB_VFIO_PATH_MAX];
    if (!path_join(link_path, sizeof(link_path), sysfs_path, "iommu_group")) {
        return KB_ERR_INVALID;
    }

    char target[KB_VFIO_PATH_MAX];
    ssize_t len = readlink(link_path, target, sizeof(target) - 1);
    if (len < 0) {
        return errno_status();
    }
    target[len] = '\0';

    const char *name = strrchr(target, '/');
    name = name == NULL ? target : name + 1;
    char *end = NULL;
    long group = strtol(name, &end, 10);
    if (end == name || *end != '\0' || group < 0 || group > INT_MAX) {
        return KB_ERR_INVALID;
    }

    *out_group_id = (int)group;
    return KB_OK;
}

static kb_status_t vfio_region_info(kb_device_t *device, uint32_t index, struct vfio_region_info *out_info)
{
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->argsz = sizeof(*out_info);
    out_info->index = index;
    if (ioctl(device->device_fd, VFIO_DEVICE_GET_REGION_INFO, out_info) != 0) {
        return errno_status();
    }
    return KB_OK;
}

static kb_status_t read_resource_bar(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    char path[KB_VFIO_PATH_MAX];
    if (!path_join(path, sizeof(path), device->sysfs_path, "resource")) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return errno_status();
    }

    char line[256];
    unsigned current = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (current == bar_index) {
            unsigned long long start = 0;
            unsigned long long end = 0;
            unsigned long long flags = 0;
            fclose(file);
            if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3) {
                return KB_ERR_INVALID;
            }
            out_info->start = (uint64_t)start;
            out_info->end = (uint64_t)end;
            out_info->flags = (uint64_t)flags;
            out_info->present = end >= start && start != 0;
            out_info->size = out_info->present ? ((uint64_t)end - (uint64_t)start + 1) : 0;
            return out_info->present ? KB_OK : KB_ERR_NOT_FOUND;
        }
        current++;
    }

    fclose(file);
    return KB_ERR_NOT_FOUND;
}

static void vfio_destroy(kb_device_backend_t *backend)
{
    kb_linux_vfio_backend_t *vfio = vfio_from_backend(backend);
    if (vfio->device_fd >= 0) {
        vfio_disable_irq_index(&vfio->device, VFIO_PCI_INTX_IRQ_INDEX);
        vfio_disable_irq_index(&vfio->device, VFIO_PCI_MSI_IRQ_INDEX);
        vfio_disable_irq_index(&vfio->device, VFIO_PCI_MSIX_IRQ_INDEX);
        vfio_quiesce_nvme(vfio);
        (void)ioctl(vfio->device_fd, VFIO_DEVICE_RESET);
        vfio_unmap_all_dma(vfio);
        close(vfio->device_fd);
    }
    if (vfio->group_fd >= 0) {
        close(vfio->group_fd);
    }
    if (vfio->container_fd >= 0) {
        close(vfio->container_fd);
    }
    free(vfio);
}

static kb_status_t vfio_device_count(kb_device_backend_t *backend, size_t *out_count)
{
    if (backend == NULL || vfio_low_or_err_pointer(out_count)) {
        return KB_ERR_INVALID;
    }
    *out_count = 1;
    return KB_OK;
}

static kb_status_t vfio_device_at(kb_device_backend_t *backend, size_t index, kb_device_t **out_device)
{
    if (backend == NULL || vfio_low_or_err_pointer(out_device)) {
        return KB_ERR_INVALID;
    }
    if (index != 0) {
        return KB_ERR_NOT_FOUND;
    }
    *out_device = &vfio_from_backend(backend)->device;
    return KB_OK;
}

static kb_status_t vfio_device_pci_id(kb_device_t *device, kb_pci_id_t *out_id)
{
    if (device == NULL || vfio_low_or_err_pointer(out_id)) {
        return KB_ERR_INVALID;
    }
    *out_id = device->pci_id;
    return KB_OK;
}

static kb_status_t vfio_device_pci_location(kb_device_t *device, kb_pci_location_t *out_location)
{
    if (device == NULL || vfio_low_or_err_pointer(out_location)) {
        return KB_ERR_INVALID;
    }
    *out_location = device->location;
    return KB_OK;
}

static kb_status_t vfio_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    if (device == NULL || dst == NULL || len == 0 || offset > KB_VFIO_PCI_CONFIG_SIZE ||
        len > KB_VFIO_PCI_CONFIG_SIZE || offset + len > KB_VFIO_PCI_CONFIG_SIZE)
    {
        return KB_ERR_INVALID;
    }

    struct vfio_region_info info;
    kb_status_t status = vfio_region_info(device, VFIO_PCI_CONFIG_REGION_INDEX, &info);
    if (status != KB_OK) {
        return status;
    }
    if ((uint64_t)offset + (uint64_t)len > info.size) {
        return KB_ERR_INVALID;
    }

    ssize_t read_size = pread(device->device_fd, dst, len, (off_t)(info.offset + offset));
    if (read_size < 0) {
        return errno_status();
    }
    return (size_t)read_size == len ? KB_OK : KB_ERR_IO;
}

static kb_status_t vfio_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    if (device == NULL || src == NULL || len == 0 || offset > KB_VFIO_PCI_CONFIG_SIZE ||
        len > KB_VFIO_PCI_CONFIG_SIZE || offset + len > KB_VFIO_PCI_CONFIG_SIZE)
    {
        return KB_ERR_INVALID;
    }

    struct vfio_region_info info;
    kb_status_t status = vfio_region_info(device, VFIO_PCI_CONFIG_REGION_INDEX, &info);
    if (status != KB_OK) {
        return status;
    }
    if ((uint64_t)offset + (uint64_t)len > info.size) {
        return KB_ERR_INVALID;
    }

    ssize_t write_size = pwrite(device->device_fd, src, len, (off_t)(info.offset + offset));
    if (write_size < 0) {
        return errno_status();
    }
    return (size_t)write_size == len ? KB_OK : KB_ERR_IO;
}

static kb_status_t vfio_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= 6) {
        return KB_ERR_NOT_FOUND;
    }

    kb_status_t status = read_resource_bar(device, bar_index, out_info);
    if (status != KB_OK) {
        return status;
    }

    struct vfio_region_info region;
    status = vfio_region_info(device, VFIO_PCI_BAR0_REGION_INDEX + bar_index, &region);
    if (status == KB_OK && region.size != 0) {
        out_info->size = region.size;
    }
    return KB_OK;
}

static kb_status_t vfio_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    if (device == NULL || out_region == NULL) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= 6) {
        return KB_ERR_NOT_FOUND;
    }

    struct vfio_region_info region;
    kb_status_t status = vfio_region_info(device, VFIO_PCI_BAR0_REGION_INDEX + bar_index, &region);
    if (status != KB_OK) {
        return status;
    }
    if (region.size == 0) {
        return KB_ERR_NOT_FOUND;
    }
    if ((region.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0) {
        return KB_ERR_UNSUPPORTED;
    }

    void *addr = mmap(NULL, (size_t)region.size, PROT_READ | PROT_WRITE, MAP_SHARED, device->device_fd, (off_t)region.offset);
    if (addr == MAP_FAILED) {
        return errno_status();
    }

    kb_pci_bar_info_t info;
    memset(&info, 0, sizeof(info));
    (void)read_resource_bar(device, bar_index, &info);

    out_region->addr = addr;
    out_region->size = region.size;
    out_region->host_phys = info.start;
    out_region->flags = region.flags;
    return KB_OK;
}

static void vfio_unmap_bar(kb_device_t *device, kb_mmio_region_t *region)
{
    (void)device;
    if (region != NULL && region->addr != NULL && region->size != 0) {
        munmap(region->addr, (size_t)region->size);
        memset(region, 0, sizeof(*region));
    }
}

static uint32_t vfio_mmio_read32(void *base, size_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    return *reg;
}

static void vfio_mmio_write32(void *base, size_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    *reg = value;
}

static void vfio_quiesce_nvme(kb_linux_vfio_backend_t *vfio)
{
    if (vfio == NULL) {
        return;
    }

    kb_device_t *device = &vfio->device;
    if (device->pci_id.class_code != KB_PCI_CLASS_STORAGE ||
        device->pci_id.subclass != KB_PCI_SUBCLASS_NVME ||
        device->pci_id.prog_if != KB_PCI_PROGIF_NVME)
    {
        return;
    }

    uint16_t command = 0;
    if (vfio_pci_config_read(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command)) != KB_OK ||
        (command & KB_PCI_COMMAND_MEMORY) == 0)
    {
        return;
    }

    kb_mmio_region_t bar;
    memset(&bar, 0, sizeof(bar));
    if (vfio_map_bar(device, 0, &bar) != KB_OK || bar.addr == NULL) {
        return;
    }

    uint32_t cc = vfio_mmio_read32(bar.addr, KB_NVME_REG_CC);
    if ((cc & 1u) != 0) {
        vfio_mmio_write32(bar.addr, KB_NVME_REG_CC, cc & ~1u);
        for (unsigned i = 0; i < 10000000u; i++) {
            if ((vfio_mmio_read32(bar.addr, KB_NVME_REG_CSTS) & 1u) == 0) {
                break;
            }
        }
    }
    vfio_unmap_bar(device, &bar);
}

static kb_status_t vfio_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr);

static void vfio_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction);

static kb_status_t vfio_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    if (device == NULL || out_buffer == NULL || size == 0 || size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    uint64_t effective_alignment = alignment;
    const uint64_t ps = page_size();
    if (effective_alignment < ps) {
        effective_alignment = ps;
    }
    if ((effective_alignment & (effective_alignment - 1u)) != 0 || effective_alignment > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    const uint64_t alloc_size = align_up_u64(size, ps);
    if (alloc_size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    void *ptr = NULL;
    if (posix_memalign(&ptr, (size_t)effective_alignment, (size_t)alloc_size) != 0) {
        return KB_ERR_NOMEM;
    }
    memset(ptr, 0, (size_t)alloc_size);

    uint64_t dma_addr = 0;
    kb_status_t status = vfio_dma_map(device, ptr, alloc_size, direction, &dma_addr);
    if (status != KB_OK) {
        free(ptr);
        return status;
    }

    out_buffer->cpu_addr = ptr;
    out_buffer->dma_addr = dma_addr;
    out_buffer->size = alloc_size;
    out_buffer->flags = 0;
    return KB_OK;
}

static void vfio_dma_free(kb_device_t *device, kb_dma_buffer_t *buffer)
{
    if (device != NULL && buffer != NULL && buffer->cpu_addr != NULL && buffer->size != 0) {
        vfio_dma_unmap(device, buffer->dma_addr, buffer->size, KB_DMA_BIDIRECTIONAL);
        free(buffer->cpu_addr);
        memset(buffer, 0, sizeof(*buffer));
    }
}

static kb_status_t vfio_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    if (device == NULL || device->backend == NULL || cpu_addr == NULL || size == 0 || out_dma_addr == NULL) {
        return KB_ERR_INVALID;
    }

    const uint64_t ps = page_size();
    const uint64_t start = (uint64_t)(uintptr_t)cpu_addr;
    const uint64_t aligned_start = align_down_u64(start, ps);
    const uint64_t offset = start - aligned_start;
    const uint64_t map_size = align_up_u64(offset + size, ps);
    kb_linux_vfio_backend_t *backend = device->backend;
    const uint64_t iova = align_up_u64(backend->next_iova, ps);

    struct vfio_iommu_type1_dma_map map;
    memset(&map, 0, sizeof(map));
    map.argsz = sizeof(map);
    map.vaddr = aligned_start;
    map.iova = iova;
    map.size = map_size;
    if (direction == KB_DMA_TO_DEVICE || direction == KB_DMA_BIDIRECTIONAL) {
        map.flags |= VFIO_DMA_MAP_FLAG_READ;
    }
    if (direction == KB_DMA_FROM_DEVICE || direction == KB_DMA_BIDIRECTIONAL) {
        map.flags |= VFIO_DMA_MAP_FLAG_WRITE;
    }
    if (map.flags == 0) {
        return KB_ERR_INVALID;
    }

    if (ioctl(device->backend->container_fd, VFIO_IOMMU_MAP_DMA, &map) != 0) {
        if (vfio_trace_dma_enabled()) {
            fprintf(stderr,
                "kobox vfio: map_dma failed errno=%d vaddr=0x%llx iova=0x%llx size=0x%llx flags=0x%x cpu=%p len=0x%llx\n",
                errno,
                (unsigned long long)map.vaddr,
                (unsigned long long)map.iova,
                (unsigned long long)map.size,
                map.flags,
                cpu_addr,
                (unsigned long long)size);
        }
        return errno_status();
    }

    kb_status_t status = remember_dma_mapping(backend, iova, aligned_start, map_size);
    if (status != KB_OK) {
        struct vfio_iommu_type1_dma_unmap unmap;
        memset(&unmap, 0, sizeof(unmap));
        unmap.argsz = sizeof(unmap);
        unmap.iova = iova;
        unmap.size = map_size;
        (void)ioctl(device->backend->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
        return status;
    }
    backend->next_iova = iova + map_size + ps;
    *out_dma_addr = iova + offset;
    if (vfio_trace_dma_enabled()) {
        fprintf(stderr,
            "kobox vfio: map_dma ok vaddr=0x%llx iova=0x%llx size=0x%llx flags=0x%x cpu=%p len=0x%llx dma=0x%llx\n",
            (unsigned long long)map.vaddr,
            (unsigned long long)map.iova,
            (unsigned long long)map.size,
            map.flags,
            cpu_addr,
            (unsigned long long)size,
            (unsigned long long)*out_dma_addr);
    }
    return KB_OK;
}

static void vfio_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)direction;
    if (device == NULL || device->backend == NULL || dma_addr == 0 || size == 0) {
        return;
    }

    kb_vfio_dma_mapping_t **cursor = &device->backend->dma_mappings;
    kb_vfio_dma_mapping_t *mapping = NULL;
    while (*cursor != NULL) {
        kb_vfio_dma_mapping_t *candidate = *cursor;
        if (candidate->size != 0 && dma_addr >= candidate->iova && dma_addr < candidate->iova + candidate->size) {
            mapping = candidate;
            *cursor = candidate->next;
            break;
        }
        cursor = &candidate->next;
    }
    if (mapping == NULL) {
        return;
    }

    struct vfio_iommu_type1_dma_unmap unmap;
    memset(&unmap, 0, sizeof(unmap));
    unmap.argsz = sizeof(unmap);
    unmap.iova = mapping->iova;
    unmap.size = mapping->size;
    (void)ioctl(device->backend->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
    free(mapping);
}

static void vfio_unmap_all_dma(kb_linux_vfio_backend_t *vfio)
{
    if (vfio == NULL || vfio->container_fd < 0) {
        return;
    }

    while (vfio->dma_mappings != NULL) {
        kb_vfio_dma_mapping_t *mapping = vfio->dma_mappings;
        vfio->dma_mappings = mapping->next;

        struct vfio_iommu_type1_dma_unmap unmap;
        memset(&unmap, 0, sizeof(unmap));
        unmap.argsz = sizeof(unmap);
        unmap.iova = mapping->iova;
        unmap.size = mapping->size;
        (void)ioctl(vfio->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
        free(mapping);
    }
}

static kb_status_t vfio_irq_info(kb_device_t *device, uint32_t index, struct vfio_irq_info *out_info)
{
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->argsz = sizeof(*out_info);
    out_info->index = index;
    if (ioctl(device->device_fd, VFIO_DEVICE_GET_IRQ_INFO, out_info) != 0) {
        return errno_status();
    }
    return KB_OK;
}

static void vfio_disable_irq_index(kb_device_t *device, uint32_t index)
{
    if (device == NULL || device->device_fd < 0) {
        return;
    }

    struct vfio_irq_set request;
    memset(&request, 0, sizeof(request));
    request.argsz = sizeof(request);
    request.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    request.index = index;
    request.start = 0;
    request.count = 0;
    (void)ioctl(device->device_fd, VFIO_DEVICE_SET_IRQS, &request);
}

static int vfio_trace_irq_enabled(void)
{
    return getenv("KOBOX_TRACE_IRQ") != NULL;
}

static void vfio_unmask_irq(kb_device_t *device, uint32_t index, uint32_t start)
{
    if (device == NULL || device->device_fd < 0) {
        return;
    }

    struct vfio_irq_set request;
    memset(&request, 0, sizeof(request));
    request.argsz = sizeof(request);
    request.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
    request.index = index;
    request.start = start;
    request.count = 1;
    (void)ioctl(device->device_fd, VFIO_DEVICE_SET_IRQS, &request);
}

static int vfio_irq_index_available(kb_device_t *device, uint32_t index, unsigned vector)
{
    struct vfio_irq_info info;
    kb_status_t status = vfio_irq_info(device, index, &info);
    return status == KB_OK && vector < info.count;
}

static int vfio_pci_find_capability(kb_device_t *device, uint8_t cap_id, uint8_t *out_offset)
{
    if (device == NULL || out_offset == NULL) {
        return 0;
    }

    uint16_t status = 0;
    if (vfio_pci_config_read(device, KB_PCI_STATUS_OFFSET, &status, sizeof(status)) != KB_OK ||
        (status & KB_PCI_STATUS_CAP_LIST) == 0)
    {
        return 0;
    }

    uint8_t cap = 0;
    if (vfio_pci_config_read(device, KB_PCI_CAPABILITY_LIST_OFFSET, &cap, sizeof(cap)) != KB_OK) {
        return 0;
    }
    cap &= KB_PCI_CAP_NEXT_MASK;

    for (unsigned depth = 0; depth < 48 && cap >= 0x40; depth++) {
        uint8_t header[2] = {0, 0};
        if (vfio_pci_config_read(device, cap, header, sizeof(header)) != KB_OK) {
            return 0;
        }
        if (header[0] == cap_id) {
            *out_offset = cap;
            return 1;
        }
        cap = header[1] & KB_PCI_CAP_NEXT_MASK;
    }

    return 0;
}

static int vfio_pci_msi_enabled(kb_device_t *device)
{
    uint8_t cap = 0;
    uint16_t control = 0;
    return vfio_pci_find_capability(device, KB_PCI_CAP_ID_MSI, &cap) &&
           vfio_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK &&
           (control & KB_PCI_MSI_CONTROL_ENABLE) != 0;
}

static int vfio_pci_msix_enabled(kb_device_t *device)
{
    uint8_t cap = 0;
    uint16_t control = 0;
    return vfio_pci_find_capability(device, KB_PCI_CAP_ID_MSIX, &cap) &&
           vfio_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK &&
           (control & KB_PCI_MSIX_CONTROL_ENABLE) != 0;
}

static void vfio_enable_irq_mode(kb_device_t *device, unsigned encoded_vector)
{
    const unsigned backend_kind = encoded_vector >> KB_IRQ_BACKEND_KIND_SHIFT;
    if (backend_kind == KB_IRQ_BACKEND_KIND_MSIX) {
        uint8_t cap = 0;
        uint16_t control = 0;
        if (vfio_pci_find_capability(device, KB_PCI_CAP_ID_MSIX, &cap) &&
            vfio_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK)
        {
            control |= KB_PCI_MSIX_CONTROL_ENABLE;
            (void)vfio_pci_config_write(device, (uint16_t)(cap + 2u), &control, sizeof(control));
        }
        return;
    }

    if (backend_kind == KB_IRQ_BACKEND_KIND_MSI) {
        uint8_t cap = 0;
        uint16_t control = 0;
        if (vfio_pci_find_capability(device, KB_PCI_CAP_ID_MSI, &cap) &&
            vfio_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK)
        {
            control |= KB_PCI_MSI_CONTROL_ENABLE;
            (void)vfio_pci_config_write(device, (uint16_t)(cap + 2u), &control, sizeof(control));
        }
    }
}

static kb_status_t vfio_select_irq(kb_device_t *device, unsigned vector, uint32_t *out_index, uint32_t *out_start)
{
    if (device == NULL || out_index == NULL || out_start == NULL) {
        return KB_ERR_INVALID;
    }

    const unsigned backend_kind = vector >> KB_IRQ_BACKEND_KIND_SHIFT;
    const unsigned backend_vector = vector & KB_IRQ_BACKEND_VECTOR_MASK;

    if (backend_kind == KB_IRQ_BACKEND_KIND_MSIX) {
        if (vfio_irq_index_available(device, VFIO_PCI_MSIX_IRQ_INDEX, backend_vector)) {
            *out_index = VFIO_PCI_MSIX_IRQ_INDEX;
            *out_start = backend_vector;
            return KB_OK;
        }
        return KB_ERR_UNSUPPORTED;
    }

    if (backend_kind == KB_IRQ_BACKEND_KIND_MSI) {
        if (vfio_irq_index_available(device, VFIO_PCI_MSI_IRQ_INDEX, backend_vector)) {
            *out_index = VFIO_PCI_MSI_IRQ_INDEX;
            *out_start = backend_vector;
            return KB_OK;
        }
        return KB_ERR_UNSUPPORTED;
    }

    const int msix_enabled = vfio_pci_msix_enabled(device);
    const int msi_enabled = vfio_pci_msi_enabled(device);
    const int intx_available = backend_vector == 0 && vfio_irq_index_available(device, VFIO_PCI_INTX_IRQ_INDEX, 0);

    if (backend_vector == 0 && !msix_enabled && !msi_enabled && intx_available) {
        *out_index = VFIO_PCI_INTX_IRQ_INDEX;
        *out_start = 0;
        if (vfio_trace_irq_enabled()) {
            fprintf(stderr, "kobox vfio: irq vector=%u index=INTx start=0\n", vector);
        }
        return KB_OK;
    }

    if (msix_enabled && vfio_irq_index_available(device, VFIO_PCI_MSIX_IRQ_INDEX, backend_vector)) {
        *out_index = VFIO_PCI_MSIX_IRQ_INDEX;
        *out_start = backend_vector;
        if (vfio_trace_irq_enabled()) {
            fprintf(stderr, "kobox vfio: irq vector=%u index=MSI-X start=%u\n", vector, backend_vector);
        }
        return KB_OK;
    }

    if (msi_enabled && vfio_irq_index_available(device, VFIO_PCI_MSI_IRQ_INDEX, backend_vector)) {
        *out_index = VFIO_PCI_MSI_IRQ_INDEX;
        *out_start = backend_vector;
        if (vfio_trace_irq_enabled()) {
            fprintf(stderr, "kobox vfio: irq vector=%u index=MSI start=%u\n", vector, backend_vector);
        }
        return KB_OK;
    }

    if (intx_available) {
        *out_index = VFIO_PCI_INTX_IRQ_INDEX;
        *out_start = 0;
        if (vfio_trace_irq_enabled()) {
            fprintf(stderr, "kobox vfio: irq vector=%u index=INTx start=0 fallback\n", vector);
        }
        return KB_OK;
    }

    return KB_ERR_UNSUPPORTED;
}

static kb_status_t vfio_irq_register(
    kb_device_t *device,
    unsigned vector,
    kb_device_irq_handler_t handler,
    void *ctx,
    kb_device_irq_t **out_irq)
{
    if (device == NULL || handler == NULL || out_irq == NULL) {
        return KB_ERR_INVALID;
    }

    kb_device_irq_t *irq = calloc(1, sizeof(*irq));
    if (irq == NULL) {
        return KB_ERR_NOMEM;
    }
    irq->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (irq->event_fd < 0) {
        free(irq);
        return errno_status();
    }

    kb_status_t status = vfio_select_irq(device, vector, &irq->index, &irq->start);
    if (status != KB_OK) {
        fprintf(stderr, "kobox vfio: irq vector=%u select failed status=%d\n", vector, status);
        close(irq->event_fd);
        free(irq);
        return status;
    }

    irq->handler = handler;
    irq->ctx = ctx;

    unsigned char request_storage[sizeof(struct vfio_irq_set) + sizeof(int32_t)];
    memset(request_storage, 0, sizeof(request_storage));
    struct vfio_irq_set *request = (struct vfio_irq_set *)request_storage;
    request->argsz = sizeof(request_storage);
    request->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    request->index = irq->index;
    request->start = irq->start;
    request->count = 1;
    memcpy(request->data, &irq->event_fd, sizeof(irq->event_fd));

    if (ioctl(device->device_fd, VFIO_DEVICE_SET_IRQS, request) != 0) {
        status = errno_status();
        fprintf(
            stderr,
            "kobox vfio: irq vector=%u index=%u start=%u SET_IRQS failed errno=%d status=%d\n",
            vector,
            irq->index,
            irq->start,
            errno,
            status);
        close(irq->event_fd);
        free(irq);
        return status;
    }
    vfio_enable_irq_mode(device, vector);
    vfio_unmask_irq(device, irq->index, irq->start);

    *out_irq = irq;
    return KB_OK;
}

static void vfio_irq_drain(kb_device_irq_t *irq)
{
    if (irq == NULL || irq->event_fd < 0) {
        return;
    }

    for (;;) {
        uint64_t count = 0;
        ssize_t read_size = read(irq->event_fd, &count, sizeof(count));
        if (read_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            return;
        }
        if ((size_t)read_size != sizeof(count)) {
            return;
        }
    }
}

static void vfio_irq_unregister(kb_device_t *device, kb_device_irq_t *irq)
{
    if (device == NULL || irq == NULL) {
        return;
    }

    vfio_irq_drain(irq);

    struct vfio_irq_set request;
    memset(&request, 0, sizeof(request));
    request.argsz = sizeof(request);
    request.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    request.index = irq->index;
    request.start = irq->start;
    request.count = 0;
    (void)ioctl(device->device_fd, VFIO_DEVICE_SET_IRQS, &request);
    vfio_irq_drain(irq);
    close(irq->event_fd);
    free(irq);
}

static kb_status_t vfio_irq_wait(kb_device_t *device, kb_device_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    if (irq == NULL || irq->event_fd < 0) {
        return KB_ERR_INVALID;
    }

    int timeout_ms = -1;
    if (timeout_ns != UINT64_MAX) {
        uint64_t rounded_ms = (timeout_ns + 999999ull) / 1000000ull;
        timeout_ms = rounded_ms > (uint64_t)INT_MAX ? INT_MAX : (int)rounded_ms;
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = irq->event_fd;
    pfd.events = POLLIN;
    int result = poll(&pfd, 1, timeout_ms);
    if (result < 0) {
        return errno_status();
    }
    if (result == 0) {
        return KB_ERR_NOT_FOUND;
    }

    uint64_t count = 0;
    ssize_t read_size = read(irq->event_fd, &count, sizeof(count));
    if (read_size < 0) {
        return errno_status();
    }
    if ((size_t)read_size != sizeof(count)) {
        return KB_ERR_IO;
    }
    if (irq->handler != NULL) {
        irq->handler(irq->ctx);
    }
    vfio_unmask_irq(device, irq->index, irq->start);
    return KB_OK;
}

static uint64_t vfio_monotonic_ns(kb_device_backend_t *backend)
{
    (void)backend;
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void vfio_log(kb_device_backend_t *backend, int level, const char *message)
{
    (void)backend;
    (void)level;
    (void)message;
}

static const kb_device_backend_ops_t vfio_ops = {
    .destroy = vfio_destroy,
    .device_count = vfio_device_count,
    .device_at = vfio_device_at,
    .device_pci_id = vfio_device_pci_id,
    .device_pci_location = vfio_device_pci_location,
    .pci_config_read = vfio_pci_config_read,
    .pci_config_write = vfio_pci_config_write,
    .pci_bar_info = vfio_pci_bar_info,
    .map_bar = vfio_map_bar,
    .unmap_bar = vfio_unmap_bar,
    .dma_alloc = vfio_dma_alloc,
    .dma_free = vfio_dma_free,
    .dma_map = vfio_dma_map,
    .dma_unmap = vfio_dma_unmap,
    .irq_register = vfio_irq_register,
    .irq_unregister = vfio_irq_unregister,
    .irq_wait = vfio_irq_wait,
    .monotonic_ns = vfio_monotonic_ns,
    .log = vfio_log,
};

static kb_status_t setup_vfio(kb_linux_vfio_backend_t *backend)
{
    backend->container_fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    if (backend->container_fd < 0) {
        return errno_status();
    }
    if (ioctl(backend->container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        return KB_ERR_UNSUPPORTED;
    }
    if (ioctl(backend->container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) == 0) {
        return KB_ERR_UNSUPPORTED;
    }

    char group_path[64];
    int written = snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", backend->group_id);
    if (written < 0 || (size_t)written >= sizeof(group_path)) {
        return KB_ERR_INVALID;
    }

    backend->group_fd = open(group_path, O_RDWR | O_CLOEXEC);
    if (backend->group_fd < 0) {
        return errno_status();
    }

    struct vfio_group_status group_status;
    memset(&group_status, 0, sizeof(group_status));
    group_status.argsz = sizeof(group_status);
    if (ioctl(backend->group_fd, VFIO_GROUP_GET_STATUS, &group_status) != 0) {
        return errno_status();
    }
    if ((group_status.flags & VFIO_GROUP_FLAGS_VIABLE) == 0) {
        return KB_ERR_DENIED;
    }
    if (ioctl(backend->group_fd, VFIO_GROUP_SET_CONTAINER, &backend->container_fd) != 0) {
        return errno_status();
    }
    if (ioctl(backend->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) != 0) {
        return errno_status();
    }

    backend->device_fd = ioctl(backend->group_fd, VFIO_GROUP_GET_DEVICE_FD, backend->device.bdf);
    if (backend->device_fd < 0) {
        return errno_status();
    }
    backend->device.backend = backend;
    backend->device.device_fd = backend->device_fd;
    return KB_OK;
}

kb_status_t kb_linux_vfio_device_create(const char *pci_bdf, kb_device_backend_t **out_backend)
{
    if (pci_bdf == NULL || out_backend == NULL) {
        return KB_ERR_INVALID;
    }
    *out_backend = NULL;

    kb_linux_vfio_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return KB_ERR_NOMEM;
    }
    backend->base.ops = &vfio_ops;
    backend->container_fd = -1;
    backend->group_fd = -1;
    backend->device_fd = -1;
    backend->next_iova = KB_VFIO_DMA_IOVA_BASE;
    backend->device.device_fd = -1;

    int written = snprintf(backend->device.bdf, sizeof(backend->device.bdf), "%s", pci_bdf);
    if (written < 0 || (size_t)written >= sizeof(backend->device.bdf)) {
        vfio_destroy(&backend->base);
        return KB_ERR_INVALID;
    }
    written = snprintf(backend->device.sysfs_path, sizeof(backend->device.sysfs_path), "/sys/bus/pci/devices/%s", pci_bdf);
    if (written < 0 || (size_t)written >= sizeof(backend->device.sysfs_path)) {
        vfio_destroy(&backend->base);
        return KB_ERR_INVALID;
    }

    kb_status_t status = read_device_metadata(&backend->device);
    if (status != KB_OK) {
        vfio_destroy(&backend->base);
        return status;
    }
    status = read_group_id(backend->device.sysfs_path, &backend->group_id);
    if (status != KB_OK) {
        vfio_destroy(&backend->base);
        return status;
    }
    status = setup_vfio(backend);
    if (status != KB_OK) {
        vfio_destroy(&backend->base);
        return status;
    }

    *out_backend = &backend->base;
    return KB_OK;
}
