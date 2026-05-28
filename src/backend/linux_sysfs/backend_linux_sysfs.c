#include "backend/backend_internal.h"
#include "kobox/backend_linux_sysfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    KB_SYSFS_PATH_MAX = 4096,
};

struct kb_device {
    char path[KB_SYSFS_PATH_MAX];
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
};

struct kb_irq {
    int unused;
};

typedef struct kb_linux_sysfs_backend {
    kb_backend_t base;
    struct kb_device *devices;
    size_t device_count;
} kb_linux_sysfs_backend_t;

static kb_linux_sysfs_backend_t *sysfs_from_backend(kb_backend_t *backend)
{
    return (kb_linux_sysfs_backend_t *)backend;
}

static int path_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    int written = snprintf(dst, dst_size, "%s/%s", a, b);
    return written >= 0 && (size_t)written < dst_size;
}

static kb_status_t read_text_file(const char *path, char *dst, size_t dst_size)
{
    if (path == NULL || dst == NULL || dst_size == 0) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return KB_ERR_IO;
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
    char path[KB_SYSFS_PATH_MAX];
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

static kb_status_t read_device_metadata(struct kb_device *device, const char *entry_name)
{
    unsigned long value = 0;
    kb_status_t status = read_device_hex(device->path, "vendor", &value);
    if (status != KB_OK || value > UINT16_MAX) {
        return status == KB_OK ? KB_ERR_INVALID : status;
    }
    device->pci_id.vendor_id = (uint16_t)value;

    status = read_device_hex(device->path, "device", &value);
    if (status != KB_OK || value > UINT16_MAX) {
        return status == KB_OK ? KB_ERR_INVALID : status;
    }
    device->pci_id.device_id = (uint16_t)value;

    status = read_device_hex(device->path, "subsystem_vendor", &value);
    if (status == KB_OK && value <= UINT16_MAX) {
        device->pci_id.subsystem_vendor_id = (uint16_t)value;
    }

    status = read_device_hex(device->path, "subsystem_device", &value);
    if (status == KB_OK && value <= UINT16_MAX) {
        device->pci_id.subsystem_device_id = (uint16_t)value;
    }

    status = read_device_hex(device->path, "class", &value);
    if (status == KB_OK) {
        device->pci_id.class_code = (uint8_t)((value >> 16) & 0xffu);
        device->pci_id.subclass = (uint8_t)((value >> 8) & 0xffu);
        device->pci_id.prog_if = (uint8_t)(value & 0xffu);
    }

    if (!parse_location(entry_name, &device->location)) {
        return KB_ERR_INVALID;
    }
    return KB_OK;
}

static kb_status_t append_device(kb_linux_sysfs_backend_t *backend, const char *sysfs_root, const char *entry_name)
{
    struct kb_device device;
    memset(&device, 0, sizeof(device));
    if (!path_join(device.path, sizeof(device.path), sysfs_root, entry_name)) {
        return KB_ERR_INVALID;
    }

    kb_status_t status = read_device_metadata(&device, entry_name);
    if (status != KB_OK) {
        return KB_OK;
    }

    struct kb_device *new_devices = realloc(
        backend->devices,
        (backend->device_count + 1) * sizeof(backend->devices[0]));
    if (new_devices == NULL) {
        return KB_ERR_NOMEM;
    }

    backend->devices = new_devices;
    backend->devices[backend->device_count] = device;
    backend->device_count++;
    return KB_OK;
}

static kb_status_t scan_devices(kb_linux_sysfs_backend_t *backend)
{
    const char *sysfs_root = "/sys/bus/pci/devices";
    DIR *dir = opendir(sysfs_root);
    if (dir == NULL) {
        return KB_ERR_IO;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        kb_status_t status = append_device(backend, sysfs_root, entry->d_name);
        if (status != KB_OK) {
            closedir(dir);
            return status;
        }
    }

    closedir(dir);
    return KB_OK;
}

static void sysfs_destroy(kb_backend_t *backend)
{
    kb_linux_sysfs_backend_t *sysfs = sysfs_from_backend(backend);
    free(sysfs->devices);
    free(sysfs);
}

static kb_status_t sysfs_device_count(kb_backend_t *backend, size_t *out_count)
{
    if (backend == NULL || out_count == NULL) {
        return KB_ERR_INVALID;
    }
    *out_count = sysfs_from_backend(backend)->device_count;
    return KB_OK;
}

static kb_status_t sysfs_device_at(kb_backend_t *backend, size_t index, kb_device_t **out_device)
{
    if (backend == NULL || out_device == NULL) {
        return KB_ERR_INVALID;
    }
    kb_linux_sysfs_backend_t *sysfs = sysfs_from_backend(backend);
    if (index >= sysfs->device_count) {
        return KB_ERR_NOT_FOUND;
    }
    *out_device = &sysfs->devices[index];
    return KB_OK;
}

static kb_status_t sysfs_device_pci_id(kb_device_t *device, kb_pci_id_t *out_id)
{
    if (device == NULL || out_id == NULL) {
        return KB_ERR_INVALID;
    }
    *out_id = device->pci_id;
    return KB_OK;
}

static kb_status_t sysfs_device_pci_location(kb_device_t *device, kb_pci_location_t *out_location)
{
    if (device == NULL || out_location == NULL) {
        return KB_ERR_INVALID;
    }
    *out_location = device->location;
    return KB_OK;
}

static kb_status_t sysfs_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    if (device == NULL || dst == NULL || len == 0) {
        return KB_ERR_INVALID;
    }

    char path[KB_SYSFS_PATH_MAX];
    if (!path_join(path, sizeof(path), device->path, "config")) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return KB_ERR_IO;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return KB_ERR_IO;
    }
    if (fread(dst, 1, len, file) != len) {
        fclose(file);
        return KB_ERR_IO;
    }
    fclose(file);
    return KB_OK;
}

static kb_status_t sysfs_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    (void)device;
    (void)offset;
    (void)src;
    (void)len;
    return KB_ERR_UNSUPPORTED;
}

static kb_status_t sysfs_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= 6) {
        return KB_ERR_NOT_FOUND;
    }

    char path[KB_SYSFS_PATH_MAX];
    if (!path_join(path, sizeof(path), device->path, "resource")) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return KB_ERR_IO;
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

static kb_status_t sysfs_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    (void)device;
    (void)bar_index;
    (void)out_region;
    return KB_ERR_UNSUPPORTED;
}

static void sysfs_unmap_bar(kb_device_t *device, kb_mmio_region_t *region)
{
    (void)device;
    if (region != NULL) {
        memset(region, 0, sizeof(*region));
    }
}

static kb_status_t sysfs_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    (void)device;
    (void)size;
    (void)alignment;
    (void)direction;
    (void)out_buffer;
    return KB_ERR_UNSUPPORTED;
}

static void sysfs_dma_free(kb_device_t *device, kb_dma_buffer_t *buffer)
{
    (void)device;
    (void)buffer;
}

static kb_status_t sysfs_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    (void)device;
    (void)cpu_addr;
    (void)size;
    (void)direction;
    (void)out_dma_addr;
    return KB_ERR_UNSUPPORTED;
}

static void sysfs_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)device;
    (void)dma_addr;
    (void)size;
    (void)direction;
}

static kb_status_t sysfs_irq_register(
    kb_device_t *device,
    unsigned vector,
    kb_irq_handler_t handler,
    void *ctx,
    kb_irq_t **out_irq)
{
    (void)device;
    (void)vector;
    (void)handler;
    (void)ctx;
    (void)out_irq;
    return KB_ERR_UNSUPPORTED;
}

static void sysfs_irq_unregister(kb_device_t *device, kb_irq_t *irq)
{
    (void)device;
    (void)irq;
}

static kb_status_t sysfs_irq_wait(kb_device_t *device, kb_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    (void)irq;
    (void)timeout_ns;
    return KB_ERR_UNSUPPORTED;
}

static uint64_t sysfs_monotonic_ns(kb_backend_t *backend)
{
    (void)backend;
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void sysfs_log(kb_backend_t *backend, int level, const char *message)
{
    (void)backend;
    (void)level;
    (void)message;
}

static const kb_backend_ops_t sysfs_ops = {
    .destroy = sysfs_destroy,
    .device_count = sysfs_device_count,
    .device_at = sysfs_device_at,
    .device_pci_id = sysfs_device_pci_id,
    .device_pci_location = sysfs_device_pci_location,
    .pci_config_read = sysfs_pci_config_read,
    .pci_config_write = sysfs_pci_config_write,
    .pci_bar_info = sysfs_pci_bar_info,
    .map_bar = sysfs_map_bar,
    .unmap_bar = sysfs_unmap_bar,
    .dma_alloc = sysfs_dma_alloc,
    .dma_free = sysfs_dma_free,
    .dma_map = sysfs_dma_map,
    .dma_unmap = sysfs_dma_unmap,
    .irq_register = sysfs_irq_register,
    .irq_unregister = sysfs_irq_unregister,
    .irq_wait = sysfs_irq_wait,
    .monotonic_ns = sysfs_monotonic_ns,
    .log = sysfs_log,
};

kb_status_t kb_linux_sysfs_create(kb_backend_t **out_backend)
{
    if (out_backend == NULL) {
        return KB_ERR_INVALID;
    }
    *out_backend = NULL;

    kb_linux_sysfs_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return KB_ERR_NOMEM;
    }
    backend->base.ops = &sysfs_ops;

    kb_status_t status = scan_devices(backend);
    if (status != KB_OK) {
        sysfs_destroy(&backend->base);
        return status;
    }

    *out_backend = &backend->base;
    return KB_OK;
}
