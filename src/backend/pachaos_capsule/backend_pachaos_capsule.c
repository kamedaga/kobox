#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "backend/backend_internal.h"
#include "kobox/backend_pachaos_capsule.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    KB_PACHAOS_PCI_BAR_COUNT = 6,
    KB_PACHAOS_DMA_MAPPING_MAX = 128,
    KB_PACHAOS_MMIO_MAPPING_MAX = 32,
    KB_PACHAOS_IRQ_MAX = 32,
    KB_PACHAOS_PCI_CONFIG_SIZE = 256,
};

enum {
    PACHA_SYSCALL_CAPSULE_QUERY = 0x70,
    PACHA_SYSCALL_CAPSULE_DERIVE_MMIO = 0x71,
    PACHA_SYSCALL_CAPSULE_DERIVE_DMA_BUFFER = 0x72,
    PACHA_SYSCALL_CAPSULE_DERIVE_DMA_MAPPING = 0x73,
    PACHA_SYSCALL_CAPSULE_DERIVE_DMA_MAPPING_FROM_BUFFER = 0x74,
    PACHA_SYSCALL_CAPSULE_DERIVE_IRQ = 0x75,
    PACHA_SYSCALL_CAPSULE_CLOSE = 0x78,
    PACHA_SYSCALL_CAPSULE_PCI_CONFIG_READ = 0x79,
    PACHA_SYSCALL_CAPSULE_PCI_CONFIG_WRITE = 0x7a,
    PACHA_SYSCALL_CAPSULE_PCI_BAR_INFO = 0x7b,
};

enum {
    PACHA_CAPSULE_KIND_DEVICE = 2,
    PACHA_CAPSULE_KIND_MMIO = 3,
    PACHA_CAPSULE_KIND_DMA_BUFFER = 4,
    PACHA_CAPSULE_KIND_DMA_MAPPING = 5,
    PACHA_CAPSULE_KIND_IRQ = 6,
};

enum {
    PACHA_DMA_TO_DEVICE = 1,
    PACHA_DMA_FROM_DEVICE = 2,
    PACHA_DMA_BIDIRECTIONAL = 3,
    PACHA_IRQ_AUTO = 0,
    PACHA_BAR_FLAG_IO = 0x01,
    PACHA_BAR_FLAG_MEM = 0x02,
    PACHA_BAR_FLAG_PREFETCHABLE = 0x04,
    PACHA_BAR_FLAG_64BIT = 0x08,
    PACHA_MMIO_MAP_FLAG_REPLACE_EXISTING = 0x01,
    KB_LINUX_IORESOURCE_IO = 0x00000100,
    KB_LINUX_IORESOURCE_MEM = 0x00000200,
    KB_LINUX_IORESOURCE_PREFETCH = 0x00002000,
    KB_LINUX_IORESOURCE_MEM_64 = 0x00100000,
};

enum {
    PACHA_BAR_INFO_START = 0,
    PACHA_BAR_INFO_END = 1,
    PACHA_BAR_INFO_SIZE = 2,
    PACHA_BAR_INFO_FLAGS = 3,
    PACHA_BAR_INFO_WORDS = 4,
};

enum {
    PACHA_SNAPSHOT_TOKEN = 0,
    PACHA_SNAPSHOT_KIND = 3,
    PACHA_SNAPSHOT_RIGHTS = 5,
    PACHA_SNAPSHOT_DEVICE = 9,
    PACHA_SNAPSHOT_USER_VA = 11,
    PACHA_SNAPSHOT_IOVA = 12,
    PACHA_SNAPSHOT_SIZE = 13,
    PACHA_SNAPSHOT_INDEX = 14,
    PACHA_SNAPSHOT_FLAGS = 15,
    PACHA_SNAPSHOT_WORDS = 16,
};

typedef struct kb_pachaos_capsule_backend kb_pachaos_capsule_backend_t;

typedef struct kb_pachaos_mmio_mapping {
    void *map_addr;
    void *region_addr;
    uint64_t map_size;
    uint64_t capsule;
    unsigned bar_index;
} kb_pachaos_mmio_mapping_t;

typedef struct kb_pachaos_dma_mapping {
    void *cpu_addr;
    uint64_t size;
    uint64_t iova;
    uint64_t buffer_capsule;
    uint64_t mapping_capsule;
    int owns_cpu_addr;
} kb_pachaos_dma_mapping_t;

struct kb_device {
    kb_pachaos_capsule_backend_t *backend;
    uint64_t device_capsule;
    uint64_t device_id;
    uint64_t rights;
    kb_pci_id_t pci_id;
    kb_pci_location_t location;
    uint64_t bar_starts[KB_PACHAOS_PCI_BAR_COUNT];
    uint64_t bar_sizes[KB_PACHAOS_PCI_BAR_COUNT];
    uint64_t bar_flags[KB_PACHAOS_PCI_BAR_COUNT];
};

struct kb_irq {
    uint64_t capsule;
    unsigned vector;
    kb_irq_handler_t handler;
    void *ctx;
};

struct kb_pachaos_capsule_backend {
    kb_backend_t base;
    struct kb_device device;
    uint64_t next_iova;
    kb_pachaos_mmio_mapping_t mmio_mappings[KB_PACHAOS_MMIO_MAPPING_MAX];
    kb_pachaos_dma_mapping_t dma_mappings[KB_PACHAOS_DMA_MAPPING_MAX];
    struct kb_irq irqs[KB_PACHAOS_IRQ_MAX];
};

static const uint64_t pacha_native_syscall_tag = 0x50414348ca000000ull;

static long pacha_syscall0(uint64_t nr, uint64_t a0)
{
    errno = 0;
    return syscall((long)(pacha_native_syscall_tag | nr), a0);
}

static long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    errno = 0;
    return syscall((long)(pacha_native_syscall_tag | nr), a0, a1, a2);
}

static long pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    errno = 0;
    return syscall((long)(pacha_native_syscall_tag | nr), a0, a1, a2, a3, a4);
}

static kb_status_t pacha_status_from_return(long result)
{
    if (result == -1 && errno == ENOSYS) {
        return KB_ERR_UNSUPPORTED;
    }
    if (result == -1) {
        if (errno == EPERM || errno == EACCES) {
            return KB_ERR_DENIED;
        }
        if (errno == ENOENT || errno == ENODEV) {
            return KB_ERR_NOT_FOUND;
        }
        return KB_ERR_IO;
    }
    switch (result) {
    case 0:
        return KB_OK;
    case 1:
        return KB_ERR_INVALID;
    case 2:
        return KB_ERR_NOT_FOUND;
    case 3:
        return KB_ERR_DENIED;
    case 4:
        return KB_ERR_NOMEM;
    case 5:
        return KB_ERR_IO;
    case 6:
        return KB_ERR_UNSUPPORTED;
    default:
        return result < 0 ? KB_ERR_IO : KB_OK;
    }
}

static int token_has_kind(uint64_t token, uint64_t kind)
{
    return ((token >> 56) == 0xcau) &&
        (((token >> 52) & 0x0fu) == 1u) &&
        (((token >> 48) & 0x0fu) == kind);
}

static kb_status_t capsule_query(uint64_t token, uint64_t words[PACHA_SNAPSHOT_WORDS])
{
    memset(words, 0, PACHA_SNAPSHOT_WORDS * sizeof(words[0]));
    long result = pacha_syscall3(PACHA_SYSCALL_CAPSULE_QUERY, token, (uint64_t)(uintptr_t)words, PACHA_SNAPSHOT_WORDS);
    if (result != PACHA_SNAPSHOT_WORDS) {
        return pacha_status_from_return(result);
    }
    return KB_OK;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    uint64_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

static uint64_t page_size_u64(void)
{
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (uint64_t)value : 4096u;
}

static kb_pachaos_capsule_backend_t *pacha_from_backend(kb_backend_t *backend)
{
    return (kb_pachaos_capsule_backend_t *)backend;
}

static uint64_t parse_u64_env(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return (uint64_t)parsed;
}

static void configure_pci_id_from_env(struct kb_device *device)
{
    const char *pci_id = getenv("KOBOX_PACHAOS_PCI_ID");
    if (pci_id == NULL || pci_id[0] == '\0') {
        return;
    }

    unsigned vendor = 0;
    unsigned dev = 0;
    unsigned class_code = 0;
    unsigned subclass = 0;
    unsigned prog_if = 0;
    unsigned subsystem_vendor = 0;
    unsigned subsystem_device = 0;
    unsigned revision = 0;
    int fields = sscanf(
        pci_id,
        "%x:%x:%x:%x:%x:%x:%x:%x",
        &vendor,
        &dev,
        &class_code,
        &subclass,
        &prog_if,
        &subsystem_vendor,
        &subsystem_device,
        &revision);
    if (fields < 2) {
        return;
    }
    device->pci_id.vendor_id = (uint16_t)vendor;
    device->pci_id.device_id = (uint16_t)dev;
    if (fields >= 5) {
        device->pci_id.class_code = (uint8_t)class_code;
        device->pci_id.subclass = (uint8_t)subclass;
        device->pci_id.prog_if = (uint8_t)prog_if;
    }
    if (fields >= 7) {
        device->pci_id.subsystem_vendor_id = (uint16_t)subsystem_vendor;
        device->pci_id.subsystem_device_id = (uint16_t)subsystem_device;
    }
    if (fields >= 8) {
        device->pci_id.revision = (uint8_t)revision;
    }
}

static void configure_bar_info_from_env(struct kb_device *device)
{
    for (unsigned i = 0; i < KB_PACHAOS_PCI_BAR_COUNT; i++) {
        char name[64];
        snprintf(name, sizeof(name), "KOBOX_PACHAOS_BAR%u_START", i);
        device->bar_starts[i] = parse_u64_env(name, 0);
        snprintf(name, sizeof(name), "KOBOX_PACHAOS_BAR%u_SIZE", i);
        device->bar_sizes[i] = parse_u64_env(name, 0);
        snprintf(name, sizeof(name), "KOBOX_PACHAOS_BAR%u_FLAGS", i);
        device->bar_flags[i] = parse_u64_env(name, device->bar_sizes[i] != 0 ? KB_LINUX_IORESOURCE_MEM : 0);
    }
}

static void configure_location_from_device_id(struct kb_device *device)
{
    if ((device->device_id & 0xffffffff00000000ull) == 0x5043490000000000ull) {
        device->location.segment = 0;
        device->location.bus = (uint8_t)((device->device_id >> 16) & 0xffu);
        device->location.device = (uint8_t)((device->device_id >> 8) & 0xffu);
        device->location.function = (uint8_t)(device->device_id & 0xffu);
    }
}

static uint64_t dma_dir_to_pacha(kb_dma_dir_t direction)
{
    switch (direction) {
    case KB_DMA_TO_DEVICE:
        return PACHA_DMA_TO_DEVICE;
    case KB_DMA_FROM_DEVICE:
        return PACHA_DMA_FROM_DEVICE;
    case KB_DMA_BIDIRECTIONAL:
    default:
        return PACHA_DMA_BIDIRECTIONAL;
    }
}

static kb_status_t remember_dma_mapping(
    kb_pachaos_capsule_backend_t *backend,
    void *cpu_addr,
    uint64_t size,
    uint64_t iova,
    uint64_t buffer_capsule,
    uint64_t mapping_capsule,
    int owns_cpu_addr)
{
    for (size_t i = 0; i < KB_PACHAOS_DMA_MAPPING_MAX; i++) {
        if (backend->dma_mappings[i].size == 0) {
            backend->dma_mappings[i].cpu_addr = cpu_addr;
            backend->dma_mappings[i].size = size;
            backend->dma_mappings[i].iova = iova;
            backend->dma_mappings[i].buffer_capsule = buffer_capsule;
            backend->dma_mappings[i].mapping_capsule = mapping_capsule;
            backend->dma_mappings[i].owns_cpu_addr = owns_cpu_addr;
            return KB_OK;
        }
    }
    return KB_ERR_NOMEM;
}

static kb_pachaos_dma_mapping_t *find_dma_mapping(kb_pachaos_capsule_backend_t *backend, uint64_t iova)
{
    for (size_t i = 0; i < KB_PACHAOS_DMA_MAPPING_MAX; i++) {
        kb_pachaos_dma_mapping_t *mapping = &backend->dma_mappings[i];
        if (mapping->size != 0 && mapping->iova == iova) {
            return mapping;
        }
    }
    return NULL;
}

static void close_capsule(uint64_t capsule)
{
    if (capsule != 0) {
        (void)pacha_syscall0(PACHA_SYSCALL_CAPSULE_CLOSE, capsule);
    }
}

static void pacha_destroy(kb_backend_t *backend)
{
    kb_pachaos_capsule_backend_t *pacha = pacha_from_backend(backend);
    for (size_t i = 0; i < KB_PACHAOS_DMA_MAPPING_MAX; i++) {
        kb_pachaos_dma_mapping_t *mapping = &pacha->dma_mappings[i];
        if (mapping->size != 0) {
            close_capsule(mapping->mapping_capsule);
            close_capsule(mapping->buffer_capsule);
            if (mapping->owns_cpu_addr) {
                free(mapping->cpu_addr);
            }
        }
    }
    for (size_t i = 0; i < KB_PACHAOS_MMIO_MAPPING_MAX; i++) {
        kb_pachaos_mmio_mapping_t *mapping = &pacha->mmio_mappings[i];
        if (mapping->map_size != 0) {
            close_capsule(mapping->capsule);
            munmap(mapping->map_addr, (size_t)mapping->map_size);
        }
    }
    for (size_t i = 0; i < KB_PACHAOS_IRQ_MAX; i++) {
        close_capsule(pacha->irqs[i].capsule);
    }
    free(pacha);
}

static kb_status_t pacha_device_count(kb_backend_t *backend, size_t *out_count)
{
    (void)backend;
    if (out_count == NULL) {
        return KB_ERR_INVALID;
    }
    *out_count = 1;
    return KB_OK;
}

static kb_status_t pacha_device_at(kb_backend_t *backend, size_t index, kb_device_t **out_device)
{
    if (backend == NULL || out_device == NULL) {
        return KB_ERR_INVALID;
    }
    if (index != 0) {
        return KB_ERR_NOT_FOUND;
    }
    *out_device = &pacha_from_backend(backend)->device;
    return KB_OK;
}

static kb_status_t pacha_device_pci_id(kb_device_t *device, kb_pci_id_t *out_id)
{
    if (device == NULL || out_id == NULL) {
        return KB_ERR_INVALID;
    }
    *out_id = device->pci_id;
    return KB_OK;
}

static kb_status_t pacha_device_pci_location(kb_device_t *device, kb_pci_location_t *out_location)
{
    if (device == NULL || out_location == NULL) {
        return KB_ERR_INVALID;
    }
    *out_location = device->location;
    return KB_OK;
}

static void write_le16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

static uint16_t read_le16(const unsigned char *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint64_t pacha_bar_flags_to_linux(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & PACHA_BAR_FLAG_IO) != 0) {
        out |= KB_LINUX_IORESOURCE_IO;
    }
    if ((flags & PACHA_BAR_FLAG_MEM) != 0) {
        out |= KB_LINUX_IORESOURCE_MEM;
    }
    if ((flags & PACHA_BAR_FLAG_PREFETCHABLE) != 0) {
        out |= KB_LINUX_IORESOURCE_PREFETCH;
    }
    if ((flags & PACHA_BAR_FLAG_64BIT) != 0) {
        out |= KB_LINUX_IORESOURCE_MEM_64;
    }
    return out;
}

static kb_status_t capsule_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    if (device == NULL || dst == NULL || len == 0 || offset >= KB_PACHAOS_PCI_CONFIG_SIZE ||
        len > KB_PACHAOS_PCI_CONFIG_SIZE || offset + len > KB_PACHAOS_PCI_CONFIG_SIZE) {
        return KB_ERR_INVALID;
    }

    long result = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_PCI_CONFIG_READ,
        device->device_capsule,
        offset,
        (uint64_t)(uintptr_t)dst,
        len,
        0);
    return pacha_status_from_return(result);
}

static kb_status_t synthetic_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    unsigned char config[KB_PACHAOS_PCI_CONFIG_SIZE];
    memset(config, 0, sizeof(config));
    write_le16(&config[0x00], device->pci_id.vendor_id);
    write_le16(&config[0x02], device->pci_id.device_id);
    config[0x08] = device->pci_id.revision;
    config[0x09] = device->pci_id.prog_if;
    config[0x0a] = device->pci_id.subclass;
    config[0x0b] = device->pci_id.class_code;
    write_le16(&config[0x2c], device->pci_id.subsystem_vendor_id);
    write_le16(&config[0x2e], device->pci_id.subsystem_device_id);
    memcpy(dst, config + offset, len);
    return KB_OK;
}

static void configure_pci_id_from_config(struct kb_device *device)
{
    unsigned char config[64];
    if (capsule_pci_config_read(device, 0, config, sizeof(config)) != KB_OK) {
        return;
    }
    device->pci_id.vendor_id = read_le16(&config[0x00]);
    device->pci_id.device_id = read_le16(&config[0x02]);
    device->pci_id.revision = config[0x08];
    device->pci_id.prog_if = config[0x09];
    device->pci_id.subclass = config[0x0a];
    device->pci_id.class_code = config[0x0b];
    device->pci_id.subsystem_vendor_id = read_le16(&config[0x2c]);
    device->pci_id.subsystem_device_id = read_le16(&config[0x2e]);
}

static kb_status_t pacha_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len)
{
    kb_status_t status = capsule_pci_config_read(device, offset, dst, len);
    if (status == KB_OK) {
        return KB_OK;
    }
    if (status != KB_ERR_UNSUPPORTED) {
        return status;
    }
    return synthetic_pci_config_read(device, offset, dst, len);
}

static kb_status_t pacha_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len)
{
    if (device == NULL || src == NULL) {
        return KB_ERR_INVALID;
    }
    if (len != 0 && (offset >= KB_PACHAOS_PCI_CONFIG_SIZE || len > KB_PACHAOS_PCI_CONFIG_SIZE ||
        offset + len > KB_PACHAOS_PCI_CONFIG_SIZE)) {
        return KB_ERR_INVALID;
    }
    long result = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_PCI_CONFIG_WRITE,
        device->device_capsule,
        offset,
        (uint64_t)(uintptr_t)src,
        len,
        0);
    kb_status_t status = pacha_status_from_return(result);
    if (status == KB_OK || status != KB_ERR_UNSUPPORTED) {
        return status;
    }
    return KB_OK;
}

static kb_status_t capsule_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= KB_PACHAOS_PCI_BAR_COUNT) {
        return KB_ERR_INVALID;
    }

    uint64_t words[PACHA_BAR_INFO_WORDS];
    long result = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_PCI_BAR_INFO,
        device->device_capsule,
        bar_index,
        (uint64_t)(uintptr_t)words,
        PACHA_BAR_INFO_WORDS,
        0);
    if (result != PACHA_BAR_INFO_WORDS) {
        kb_status_t status = pacha_status_from_return(result);
        return status == KB_ERR_INVALID ? KB_ERR_NOT_FOUND : status;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->start = words[PACHA_BAR_INFO_START];
    out_info->end = words[PACHA_BAR_INFO_END];
    out_info->size = words[PACHA_BAR_INFO_SIZE];
    out_info->flags = pacha_bar_flags_to_linux(words[PACHA_BAR_INFO_FLAGS]);
    if (out_info->flags == 0 && out_info->size != 0) {
        out_info->flags = KB_LINUX_IORESOURCE_MEM;
    }
    out_info->present = out_info->size != 0;
    if (!out_info->present) {
        return KB_ERR_NOT_FOUND;
    }

    device->bar_starts[bar_index] = out_info->start;
    device->bar_sizes[bar_index] = out_info->size;
    device->bar_flags[bar_index] = out_info->flags;
    return KB_OK;
}

static kb_status_t pacha_pci_bar_info(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info)
{
    kb_status_t status = capsule_pci_bar_info(device, bar_index, out_info);
    if (status == KB_OK) {
        return KB_OK;
    }
    if (status != KB_ERR_UNSUPPORTED) {
        return status;
    }
    if (device == NULL || out_info == NULL) {
        return KB_ERR_INVALID;
    }
    if (bar_index >= KB_PACHAOS_PCI_BAR_COUNT || device->bar_sizes[bar_index] == 0) {
        return KB_ERR_NOT_FOUND;
    }
    out_info->start = device->bar_starts[bar_index];
    out_info->end = device->bar_starts[bar_index] + device->bar_sizes[bar_index] - 1u;
    out_info->size = device->bar_sizes[bar_index];
    out_info->flags = device->bar_flags[bar_index];
    out_info->present = 1;
    return KB_OK;
}

static kb_status_t remember_mmio_mapping(
    kb_pachaos_capsule_backend_t *backend,
    void *map_addr,
    void *region_addr,
    uint64_t map_size,
    uint64_t capsule,
    unsigned bar_index)
{
    for (size_t i = 0; i < KB_PACHAOS_MMIO_MAPPING_MAX; i++) {
        if (backend->mmio_mappings[i].map_size == 0) {
            backend->mmio_mappings[i].map_addr = map_addr;
            backend->mmio_mappings[i].region_addr = region_addr;
            backend->mmio_mappings[i].map_size = map_size;
            backend->mmio_mappings[i].capsule = capsule;
            backend->mmio_mappings[i].bar_index = bar_index;
            return KB_OK;
        }
    }
    return KB_ERR_NOMEM;
}

static kb_pachaos_mmio_mapping_t *find_mmio_mapping(kb_pachaos_capsule_backend_t *backend, void *addr)
{
    for (size_t i = 0; i < KB_PACHAOS_MMIO_MAPPING_MAX; i++) {
        if (backend->mmio_mappings[i].map_size != 0 && backend->mmio_mappings[i].region_addr == addr) {
            return &backend->mmio_mappings[i];
        }
    }
    return NULL;
}

static kb_status_t pacha_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region)
{
    if (device == NULL || out_region == NULL || bar_index >= KB_PACHAOS_PCI_BAR_COUNT) {
        return KB_ERR_INVALID;
    }
    kb_pci_bar_info_t info;
    kb_status_t info_status = pacha_pci_bar_info(device, bar_index, &info);
    if (info_status != KB_OK) {
        return info_status;
    }
    if ((info.flags & KB_LINUX_IORESOURCE_MEM) == 0 || info.size == 0) {
        return KB_ERR_INVALID;
    }
    uint64_t page_size = page_size_u64();
    uint64_t page_offset = info.start & (page_size - 1u);
    if (info.size > UINT64_MAX - page_offset) {
        return KB_ERR_INVALID;
    }
    uint64_t map_size = align_up_u64(page_offset + info.size, page_size);
    if (map_size == 0 || map_size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    void *map_addr = mmap(NULL, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_addr == MAP_FAILED) {
        return KB_ERR_NOMEM;
    }
    long child = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_DERIVE_MMIO,
        device->device_capsule,
        bar_index,
        (uint64_t)(uintptr_t)map_addr,
        map_size,
        PACHA_MMIO_MAP_FLAG_REPLACE_EXISTING);
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_MMIO)) {
        munmap(map_addr, (size_t)map_size);
        return pacha_status_from_return(child);
    }

    void *region_addr = (void *)((unsigned char *)map_addr + page_offset);
    kb_status_t status = remember_mmio_mapping(device->backend, map_addr, region_addr, map_size, (uint64_t)child, bar_index);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        munmap(map_addr, (size_t)map_size);
        return status;
    }
    out_region->addr = region_addr;
    out_region->size = info.size;
    out_region->backend_phys = (uint64_t)child;
    out_region->flags = (uint32_t)info.flags;
    return KB_OK;
}

static void pacha_unmap_bar(kb_device_t *device, kb_mmio_region_t *region)
{
    if (device == NULL || region == NULL) {
        return;
    }
    kb_pachaos_mmio_mapping_t *mapping = find_mmio_mapping(device->backend, region->addr);
    if (mapping != NULL) {
        close_capsule(mapping->capsule);
        munmap(mapping->map_addr, (size_t)mapping->map_size);
        memset(mapping, 0, sizeof(*mapping));
    }
    memset(region, 0, sizeof(*region));
}

static kb_status_t pacha_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    (void)direction;
    if (device == NULL || out_buffer == NULL || size == 0 || size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    uint64_t page_size = page_size_u64();
    uint64_t effective_alignment = alignment > page_size ? alignment : page_size;
    uint64_t alloc_size = align_up_u64(size, effective_alignment);
    if (alloc_size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    void *ptr = NULL;
    if (posix_memalign(&ptr, (size_t)effective_alignment, (size_t)alloc_size) != 0) {
        return KB_ERR_NOMEM;
    }
    memset(ptr, 0, (size_t)alloc_size);

    kb_pachaos_capsule_backend_t *backend = device->backend;
    uint64_t iova = align_up_u64(backend->next_iova, effective_alignment);
    backend->next_iova = iova + alloc_size;
    long child = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_DERIVE_DMA_BUFFER,
        device->device_capsule,
        (uint64_t)(uintptr_t)ptr,
        iova,
        alloc_size,
        0);
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_DMA_BUFFER)) {
        free(ptr);
        return pacha_status_from_return(child);
    }
    kb_status_t status = remember_dma_mapping(backend, ptr, alloc_size, iova, (uint64_t)child, 0, 1);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        free(ptr);
        return status;
    }
    out_buffer->cpu_addr = ptr;
    out_buffer->dma_addr = iova;
    out_buffer->size = alloc_size;
    out_buffer->flags = 0;
    return KB_OK;
}

static void pacha_dma_free(kb_device_t *device, kb_dma_buffer_t *buffer)
{
    if (device == NULL || buffer == NULL) {
        return;
    }
    kb_pachaos_dma_mapping_t *mapping = find_dma_mapping(device->backend, buffer->dma_addr);
    if (mapping != NULL) {
        close_capsule(mapping->mapping_capsule);
        close_capsule(mapping->buffer_capsule);
        if (mapping->owns_cpu_addr) {
            free(mapping->cpu_addr);
        }
        memset(mapping, 0, sizeof(*mapping));
    }
    memset(buffer, 0, sizeof(*buffer));
}

static kb_status_t pacha_dma_map(
    kb_device_t *device,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    if (device == NULL || cpu_addr == NULL || size == 0 || out_dma_addr == NULL) {
        return KB_ERR_INVALID;
    }
    uint64_t page_size = page_size_u64();
    uint64_t map_size = align_up_u64(size, page_size);
    kb_pachaos_capsule_backend_t *backend = device->backend;
    uint64_t iova = align_up_u64(backend->next_iova, page_size);
    backend->next_iova = iova + map_size;
    long child = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_DERIVE_DMA_MAPPING,
        device->device_capsule,
        (uint64_t)(uintptr_t)cpu_addr,
        iova,
        map_size,
        dma_dir_to_pacha(direction));
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_DMA_MAPPING)) {
        return pacha_status_from_return(child);
    }
    kb_status_t status = remember_dma_mapping(backend, cpu_addr, map_size, iova, 0, (uint64_t)child, 0);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        return status;
    }
    *out_dma_addr = iova;
    return KB_OK;
}

static void pacha_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)size;
    (void)direction;
    if (device == NULL) {
        return;
    }
    kb_pachaos_dma_mapping_t *mapping = find_dma_mapping(device->backend, dma_addr);
    if (mapping != NULL) {
        close_capsule(mapping->mapping_capsule);
        memset(mapping, 0, sizeof(*mapping));
    }
}

static kb_status_t pacha_irq_register(
    kb_device_t *device,
    unsigned vector,
    kb_irq_handler_t handler,
    void *ctx,
    kb_irq_t **out_irq)
{
    if (device == NULL || handler == NULL || out_irq == NULL) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_capsule_backend_t *backend = device->backend;
    for (size_t i = 0; i < KB_PACHAOS_IRQ_MAX; i++) {
        if (backend->irqs[i].capsule == 0) {
            long child = pacha_syscall5(
                PACHA_SYSCALL_CAPSULE_DERIVE_IRQ,
                device->device_capsule,
                PACHA_IRQ_AUTO,
                vector,
                0,
                0);
            if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_IRQ)) {
                return pacha_status_from_return(child);
            }
            backend->irqs[i].capsule = (uint64_t)child;
            backend->irqs[i].vector = vector;
            backend->irqs[i].handler = handler;
            backend->irqs[i].ctx = ctx;
            *out_irq = &backend->irqs[i];
            return KB_OK;
        }
    }
    return KB_ERR_NOMEM;
}

static void pacha_irq_unregister(kb_device_t *device, kb_irq_t *irq)
{
    (void)device;
    if (irq == NULL) {
        return;
    }
    close_capsule(irq->capsule);
    memset(irq, 0, sizeof(*irq));
}

static kb_status_t pacha_irq_wait(kb_device_t *device, kb_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    (void)irq;
    (void)timeout_ns;
    return KB_ERR_UNSUPPORTED;
}

static uint64_t pacha_monotonic_ns(kb_backend_t *backend)
{
    (void)backend;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void pacha_log(kb_backend_t *backend, int level, const char *message)
{
    (void)backend;
    (void)level;
    if (message != NULL && getenv("KOBOX_PACHAOS_LOG") != NULL) {
        fprintf(stderr, "kobox-pachaos: %s\n", message);
    }
}

static const kb_backend_ops_t pacha_ops = {
    .destroy = pacha_destroy,
    .device_count = pacha_device_count,
    .device_at = pacha_device_at,
    .device_pci_id = pacha_device_pci_id,
    .device_pci_location = pacha_device_pci_location,
    .pci_config_read = pacha_pci_config_read,
    .pci_config_write = pacha_pci_config_write,
    .pci_bar_info = pacha_pci_bar_info,
    .map_bar = pacha_map_bar,
    .unmap_bar = pacha_unmap_bar,
    .dma_alloc = pacha_dma_alloc,
    .dma_free = pacha_dma_free,
    .dma_map = pacha_dma_map,
    .dma_unmap = pacha_dma_unmap,
    .irq_register = pacha_irq_register,
    .irq_unregister = pacha_irq_unregister,
    .irq_wait = pacha_irq_wait,
    .monotonic_ns = pacha_monotonic_ns,
    .log = pacha_log,
};

kb_status_t kb_pachaos_capsule_parse_token(const char *text, uint64_t *out_token)
{
    if (text == NULL || text[0] == '\0' || out_token == NULL) {
        return KB_ERR_INVALID;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long token = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return KB_ERR_INVALID;
    }
    if (!token_has_kind((uint64_t)token, PACHA_CAPSULE_KIND_DEVICE)) {
        return KB_ERR_INVALID;
    }
    *out_token = (uint64_t)token;
    return KB_OK;
}

kb_status_t kb_pachaos_capsule_create(uint64_t device_capsule, kb_backend_t **out_backend)
{
    if (out_backend == NULL || !token_has_kind(device_capsule, PACHA_CAPSULE_KIND_DEVICE)) {
        return KB_ERR_INVALID;
    }
    uint64_t snapshot[PACHA_SNAPSHOT_WORDS];
    kb_status_t status = capsule_query(device_capsule, snapshot);
    if (status != KB_OK) {
        return status;
    }
    if (snapshot[PACHA_SNAPSHOT_KIND] != PACHA_CAPSULE_KIND_DEVICE) {
        return KB_ERR_INVALID;
    }

    kb_pachaos_capsule_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return KB_ERR_NOMEM;
    }
    backend->base.ops = &pacha_ops;
    backend->next_iova = parse_u64_env("KOBOX_PACHAOS_IOVA_BASE", 0x100000000ull);
    backend->device.backend = backend;
    backend->device.device_capsule = snapshot[PACHA_SNAPSHOT_TOKEN] != 0 ? snapshot[PACHA_SNAPSHOT_TOKEN] : device_capsule;
    backend->device.device_id = snapshot[PACHA_SNAPSHOT_DEVICE];
    backend->device.rights = snapshot[PACHA_SNAPSHOT_RIGHTS];
    configure_location_from_device_id(&backend->device);
    configure_pci_id_from_config(&backend->device);
    configure_pci_id_from_env(&backend->device);
    configure_bar_info_from_env(&backend->device);
    *out_backend = &backend->base;
    return KB_OK;
}

kb_status_t kb_pachaos_capsule_create_from_env(kb_backend_t **out_backend)
{
    const char *text = getenv("KOBOX_PACHAOS_DEVICE_CAPSULE");
    uint64_t token = 0;
    kb_status_t status = kb_pachaos_capsule_parse_token(text, &token);
    if (status != KB_OK) {
        return status;
    }
    return kb_pachaos_capsule_create(token, out_backend);
}
