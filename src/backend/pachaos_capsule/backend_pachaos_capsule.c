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
    KB_PACHAOS_PCI_CONFIG_SIZE = 256,
    KB_PACHAOS_MMIO_MAP_ATTEMPTS = 8,
};

static const uintptr_t KB_PACHAOS_MIN_MMIO_USER_VA = UINT64_C(0x100000000);

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
    PACHA_SYSCALL_CAPSULE_IRQ_POLL = 0x7c,
    PACHA_SYSCALL_MAP_VM_OBJECT = 0x28,
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
    PACHA_IRQ_INTX = 1,
    PACHA_IRQ_MSI = 2,
    PACHA_IRQ_MSIX = 3,
    PACHA_BAR_FLAG_IO = 0x01,
    PACHA_BAR_FLAG_MEM = 0x02,
    PACHA_BAR_FLAG_PREFETCHABLE = 0x04,
    PACHA_BAR_FLAG_64BIT = 0x08,
    PACHA_MMIO_MAP_FLAG_REPLACE_EXISTING = 0x01,
    KB_LINUX_IORESOURCE_IO = 0x00000100,
    KB_LINUX_IORESOURCE_MEM = 0x00000200,
    KB_LINUX_IORESOURCE_PREFETCH = 0x00002000,
    KB_LINUX_IORESOURCE_MEM_64 = 0x00100000,
    KB_PCI_COMMAND_OFFSET = 0x04,
    KB_PCI_STATUS_OFFSET = 0x06,
    KB_PCI_CAPABILITY_LIST_OFFSET = 0x34,
    KB_PCI_STATUS_CAP_LIST = 0x0010,
    KB_PCI_CAP_NEXT_MASK = 0xfc,
    KB_PCI_CAP_ID_MSI = 0x05,
    KB_PCI_CAP_ID_MSIX = 0x11,
    KB_PCI_COMMAND_MEMORY_SPACE = 1u << 1,
    KB_PCI_COMMAND_BUS_MASTER = 1u << 2,
    KB_PCI_MSI_CONTROL_ENABLE = 1u << 0,
    KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK = 0x07ff,
    KB_PCI_MSIX_CONTROL_FUNCTION_MASK = 1u << 14,
    KB_PCI_MSIX_CONTROL_ENABLE = 1u << 15,
    KB_PCI_MSIX_TABLE_BIR_MASK = 0x00000007,
    KB_PCI_MSIX_ENTRY_SIZE = 16,
    KB_PCI_MSIX_ENTRY_VECTOR_CTRL = 12,
    KB_PCI_MSIX_ENTRY_CTRL_MASKED = 0x00000001,
    PACHA_DEVICE_INTERRUPT_VECTOR = 0x41,
    PACHA_NVME_REG_CC = 0x14,
    PACHA_NVME_REG_CSTS = 0x1c,
    KB_PACHAOS_DEVICE_CATALOG_MAGIC = 0x44455643,
    KB_PACHAOS_DEVICE_CATALOG_VERSION = 1,
    KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES = 23,
};

static const uint32_t PACHA_X86_MSI_ADDRESS_LOW = UINT32_C(0xFEE00000);
static const uintptr_t KB_PACHAOS_DEVICE_CATALOG_VA = UINT64_C(0x26700000);

enum {
    KB_IRQ_BACKEND_KIND_SHIFT = 30,
    KB_IRQ_BACKEND_VECTOR_MASK = 0x3fffffff,
    KB_IRQ_BACKEND_KIND_MSI = 1,
    KB_IRQ_BACKEND_KIND_MSIX = 2,
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

typedef struct kb_pachaos_device_catalog_entry {
    uint64_t present;
    uint64_t kind;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t subsystem_id;
    uint64_t resource_id;
    uint64_t common_page_paddr;
    uint64_t notify_page_paddr;
    uint64_t isr_page_paddr;
    uint64_t device_page_paddr;
    uint64_t common_page_offset;
    uint64_t notify_page_offset;
    uint64_t isr_page_offset;
    uint64_t device_page_offset;
    uint64_t notify_off_multiplier;
    uint64_t iommu_token;
    uint64_t queue0_submit_token;
    uint64_t queue0_notify_token;
    uint64_t queue1_submit_token;
    uint64_t queue1_notify_token;
    uint64_t command_token;
    uint64_t device_capsule_token;
} kb_pachaos_device_catalog_entry_t;

typedef struct kb_pachaos_device_catalog_page {
    uint64_t magic;
    uint64_t version;
    uint64_t entry_count;
    uint64_t reserved0;
    kb_pachaos_device_catalog_entry_t entries[KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES];
} kb_pachaos_device_catalog_page_t;

typedef struct kb_pachaos_mmio_mapping {
    void *map_addr;
    void *region_addr;
    uint64_t map_size;
    uint64_t capsule;
    unsigned bar_index;
    struct kb_pachaos_mmio_mapping *next;
} kb_pachaos_mmio_mapping_t;

typedef struct kb_pachaos_dma_mapping {
    void *cpu_addr;
    void *mapped_cpu_addr;
    uint64_t size;
    uint64_t mapped_size;
    uint64_t iova;
    uint64_t buffer_capsule;
    uint64_t mapping_capsule;
    kb_dma_dir_t direction;
    int owns_cpu_addr;
    int owns_mapped_cpu_addr;
    struct kb_pachaos_dma_mapping *next;
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
    kb_pachaos_capsule_backend_t *backend;
    uint64_t capsule;
    uint64_t last_interrupt_count;
    unsigned vector;
    kb_irq_handler_t handler;
    void *ctx;
    struct kb_irq *next;
};

struct kb_pachaos_capsule_backend {
    kb_backend_t base;
    struct kb_device device;
    uint64_t next_iova;
    kb_pachaos_mmio_mapping_t *mmio_mappings;
    kb_pachaos_dma_mapping_t *dma_mappings;
    kb_irq_t *irqs;
};

static uint64_t pacha_now_ns(void);
static kb_status_t pacha_pci_config_read(kb_device_t *device, uint16_t offset, void *dst, size_t len);
static kb_status_t pacha_pci_config_write(kb_device_t *device, uint16_t offset, const void *src, size_t len);
static int pacha_pci_find_capability(kb_device_t *device, uint8_t cap_id, uint8_t *out_offset);
static kb_status_t pacha_map_bar(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region);
static void pacha_unmap_bar(kb_device_t *device, kb_mmio_region_t *region);
static void close_capsule(uint64_t capsule);
static const kb_backend_ops_t pacha_ops;

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

static long pacha_syscall6(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5)
{
    errno = 0;
    return syscall((long)(pacha_native_syscall_tag | nr), a0, a1, a2, a3, a4, a5);
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

static int is_vm_object_token(uint64_t token)
{
    return (token & (1ull << 62)) != 0 && (token & ~(1ull << 62)) != 0;
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

static kb_status_t capsule_snapshot_iova(uint64_t token, uint64_t expected_kind, uint64_t *out_iova)
{
    if (out_iova == NULL) {
        return KB_ERR_INVALID;
    }
    uint64_t words[PACHA_SNAPSHOT_WORDS];
    kb_status_t status = capsule_query(token, words);
    if (status != KB_OK) {
        return status;
    }
    if (words[PACHA_SNAPSHOT_KIND] != expected_kind || words[PACHA_SNAPSHOT_SIZE] == 0 ||
        words[PACHA_SNAPSHOT_IOVA] == 0) {
        return KB_ERR_INVALID;
    }
    *out_iova = words[PACHA_SNAPSHOT_IOVA];
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

static int is_power_of_two_u64(uint64_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

static uint64_t page_size_u64(void)
{
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (uint64_t)value : 4096u;
}

static int pacha_trace_enabled(void)
{
    return getenv("KOBOX_PACHAOS_LOG") != NULL;
}

static int pacha_user_mmio_va_valid(const void *addr, uint64_t size)
{
    if (addr == NULL || addr == MAP_FAILED || size == 0 || size > (uint64_t)SIZE_MAX) {
        return 0;
    }
    const uintptr_t value = (uintptr_t)addr;
    const uint64_t page_size = page_size_u64();
    if ((value & (uintptr_t)(page_size - 1u)) != 0) {
        return 0;
    }
    if (value < KB_PACHAOS_MIN_MMIO_USER_VA) {
        return 0;
    }
    if ((uint64_t)value > UINT64_MAX - size) {
        return 0;
    }
    return 1;
}

static void pacha_trace_bad_mmio_va(const char *label, const void *addr, uint64_t size)
{
    if (!pacha_trace_enabled()) {
        return;
    }
    fprintf(
        stderr,
        "kobox-pachaos: %s rejected mmio va=%p size=0x%" PRIx64 "\n",
        label,
        addr,
        size);
}

static int dma_range_crosses_page(const void *ptr, uint64_t size)
{
    if (ptr == NULL || size == 0) {
        return 0;
    }
    uint64_t page_size = page_size_u64();
    uint64_t offset = (uint64_t)((uintptr_t)ptr & (uintptr_t)(page_size - 1u));
    if (size > UINT64_MAX - offset) {
        return 1;
    }
    return offset + size > page_size;
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

static const char *kb_status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "KB_OK";
    case KB_ERR_INVALID:
        return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND:
        return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED:
        return "KB_ERR_DENIED";
    case KB_ERR_NOMEM:
        return "KB_ERR_NOMEM";
    case KB_ERR_IO:
        return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED:
        return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG:
        return "KB_ERR_PCI_CONFIG";
    default:
        return "KB_ERR_UNKNOWN";
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
    const kb_pachaos_dma_mapping_t *mapping)
{
    if (backend == NULL || mapping == NULL || mapping->size == 0 || mapping->iova == 0) {
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

static kb_pachaos_dma_mapping_t *take_dma_mapping(kb_pachaos_capsule_backend_t *backend, uint64_t iova)
{
    kb_pachaos_dma_mapping_t **cursor = backend != NULL ? &backend->dma_mappings : NULL;
    while (cursor != NULL && *cursor != NULL) {
        kb_pachaos_dma_mapping_t *mapping = *cursor;
        if (mapping->size != 0 && mapping->iova == iova) {
            *cursor = mapping->next;
            mapping->next = NULL;
            return mapping;
        }
        cursor = &mapping->next;
    }
    return NULL;
}

static void release_dma_mapping(kb_pachaos_dma_mapping_t *mapping, int copy_back)
{
    if (mapping == NULL) {
        return;
    }
    close_capsule(mapping->mapping_capsule);
    close_capsule(mapping->buffer_capsule);
    if (mapping->owns_mapped_cpu_addr && copy_back) {
        memcpy(mapping->cpu_addr, mapping->mapped_cpu_addr, (size_t)mapping->size);
    }
    if (mapping->owns_mapped_cpu_addr) {
        free(mapping->mapped_cpu_addr);
    }
    if (mapping->owns_cpu_addr) {
        free(mapping->cpu_addr);
    }
    free(mapping);
}

static void close_capsule(uint64_t capsule)
{
    if (capsule != 0) {
        (void)pacha_syscall0(PACHA_SYSCALL_CAPSULE_CLOSE, capsule);
    }
}

static int pacha_is_backend(kb_backend_t *backend)
{
    return backend != NULL && backend->ops == &pacha_ops;
}

static size_t count_pacha_residual_irqs(const kb_pachaos_capsule_backend_t *backend)
{
    size_t count = 0;
    for (const kb_irq_t *irq = backend->irqs; irq != NULL; irq = irq->next) {
        if (irq->capsule != 0) {
            count++;
        }
    }
    return count;
}

static size_t count_pacha_residual_dma_mappings(const kb_pachaos_capsule_backend_t *backend)
{
    size_t count = 0;
    for (const kb_pachaos_dma_mapping_t *mapping = backend->dma_mappings; mapping != NULL; mapping = mapping->next) {
        if (mapping->size != 0) {
            count++;
        }
    }
    return count;
}

static size_t count_pacha_residual_mmio_mappings(const kb_pachaos_capsule_backend_t *backend)
{
    size_t count = 0;
    for (const kb_pachaos_mmio_mapping_t *mapping = backend->mmio_mappings; mapping != NULL; mapping = mapping->next) {
        if (mapping->map_size != 0) {
            count++;
        }
    }
    return count;
}

static void print_pacha_residual_summary(
    FILE *out,
    const char *label,
    size_t irqs,
    size_t dma_mappings,
    size_t mmio_mappings)
{
    if (out == NULL || (irqs == 0 && dma_mappings == 0 && mmio_mappings == 0)) {
        return;
    }
    fprintf(
        out,
        "kobox-pachaos-cleanup: label=%s residual irqs=%zu dma_mappings=%zu mmio_mappings=%zu\n",
        label != NULL ? label : "cleanup",
        irqs,
        dma_mappings,
        mmio_mappings);
}

size_t kb_pachaos_capsule_report_residuals(kb_backend_t *backend, FILE *out, const char *label)
{
    if (!pacha_is_backend(backend)) {
        return 0;
    }

    const kb_pachaos_capsule_backend_t *pacha = pacha_from_backend(backend);
    const size_t irqs = count_pacha_residual_irqs(pacha);
    const size_t dma_mappings = count_pacha_residual_dma_mappings(pacha);
    const size_t mmio_mappings = count_pacha_residual_mmio_mappings(pacha);
    print_pacha_residual_summary(out, label, irqs, dma_mappings, mmio_mappings);

    if (out != NULL) {
        size_t i = 0;
        for (const kb_irq_t *irq = pacha->irqs; irq != NULL; irq = irq->next, i++) {
            if (irq->capsule == 0) {
                continue;
            }
            fprintf(
                out,
                "kobox-pachaos-cleanup: residual irq index=%zu capsule=0x%016" PRIx64 " vector=%u last_count=%" PRIu64 "\n",
                i,
                irq->capsule,
                irq->vector,
                irq->last_interrupt_count);
        }
        i = 0;
        for (const kb_pachaos_dma_mapping_t *mapping = pacha->dma_mappings; mapping != NULL; mapping = mapping->next, i++) {
            if (mapping->size == 0) {
                continue;
            }
            fprintf(
                out,
                "kobox-pachaos-cleanup: residual dma index=%zu iova=0x%016" PRIx64 " size=%" PRIu64 " mapped_size=%" PRIu64 " buffer=0x%016" PRIx64 " mapping=0x%016" PRIx64 "\n",
                i,
                mapping->iova,
                mapping->size,
                mapping->mapped_size,
                mapping->buffer_capsule,
                mapping->mapping_capsule);
        }
        i = 0;
        for (const kb_pachaos_mmio_mapping_t *mapping = pacha->mmio_mappings; mapping != NULL; mapping = mapping->next, i++) {
            if (mapping->map_size == 0) {
                continue;
            }
            fprintf(
                out,
                "kobox-pachaos-cleanup: residual mmio index=%zu bar=%u capsule=0x%016" PRIx64 " addr=%p map=%p size=%" PRIu64 "\n",
                i,
                mapping->bar_index,
                mapping->capsule,
                mapping->region_addr,
                mapping->map_addr,
                mapping->map_size);
        }
    }

    return irqs + dma_mappings + mmio_mappings;
}

static int pacha_device_is_nvme(kb_device_t *device)
{
    uint8_t class_bytes[3] = {0};
    if (device == NULL ||
        pacha_pci_config_read(device, 0x09, class_bytes, sizeof(class_bytes)) != KB_OK) {
        return 0;
    }

    return class_bytes[2] == 0x01 && class_bytes[1] == 0x08;
}

static void pacha_disable_pci_interrupts(kb_device_t *device)
{
    if (device == NULL) {
        return;
    }

    uint8_t cap = 0;
    if (pacha_pci_find_capability(device, KB_PCI_CAP_ID_MSIX, &cap)) {
        uint16_t control = 0;
        if (pacha_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK) {
            control |= KB_PCI_MSIX_CONTROL_FUNCTION_MASK;
            control &= (uint16_t)~KB_PCI_MSIX_CONTROL_ENABLE;
            (void)pacha_pci_config_write(device, (uint16_t)(cap + 2u), &control, sizeof(control));
        }
    }

    if (pacha_pci_find_capability(device, KB_PCI_CAP_ID_MSI, &cap)) {
        uint16_t control = 0;
        if (pacha_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) == KB_OK) {
            control &= (uint16_t)~KB_PCI_MSI_CONTROL_ENABLE;
            (void)pacha_pci_config_write(device, (uint16_t)(cap + 2u), &control, sizeof(control));
        }
    }
}

static void pacha_quiesce_nvme_controller(kb_device_t *device)
{
    if (!pacha_device_is_nvme(device)) {
        return;
    }

    pacha_disable_pci_interrupts(device);

    kb_mmio_region_t region;
    memset(&region, 0, sizeof(region));
    if (pacha_map_bar(device, 0, &region) != KB_OK || region.addr == NULL || region.size <= PACHA_NVME_REG_CSTS) {
        return;
    }

    volatile uint32_t *cc = (volatile uint32_t *)((unsigned char *)region.addr + PACHA_NVME_REG_CC);
    volatile uint32_t *csts = (volatile uint32_t *)((unsigned char *)region.addr + PACHA_NVME_REG_CSTS);
    if ((*cc & 1u) != 0) {
        *cc &= ~1u;
        const uint64_t start = pacha_now_ns();
        for (;;) {
            if ((*csts & 1u) == 0) {
                break;
            }
            const uint64_t now = pacha_now_ns();
            if (start != 0 && now >= start && now - start >= 500000000ull) {
                break;
            }
            const struct timespec delay = {
                .tv_sec = 0,
                .tv_nsec = 1000000,
            };
            nanosleep(&delay, NULL);
        }
    }

    pacha_unmap_bar(device, &region);
}

static void pacha_destroy(kb_backend_t *backend)
{
    kb_pachaos_capsule_backend_t *pacha = pacha_from_backend(backend);
    pacha_disable_pci_interrupts(&pacha->device);
    pacha_quiesce_nvme_controller(&pacha->device);
    while (pacha->irqs != NULL) {
        kb_irq_t *irq = pacha->irqs;
        pacha->irqs = irq->next;
        close_capsule(irq->capsule);
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
        close_capsule(mapping->capsule);
        munmap(mapping->map_addr, (size_t)mapping->map_size);
        free(mapping);
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

static int pacha_pci_find_capability(kb_device_t *device, uint8_t cap_id, uint8_t *out_offset)
{
    if (device == NULL || out_offset == NULL) {
        return 0;
    }

    uint16_t status = 0;
    if (pacha_pci_config_read(device, KB_PCI_STATUS_OFFSET, &status, sizeof(status)) != KB_OK ||
        (status & KB_PCI_STATUS_CAP_LIST) == 0) {
        return 0;
    }

    uint8_t offset = 0;
    if (pacha_pci_config_read(device, KB_PCI_CAPABILITY_LIST_OFFSET, &offset, sizeof(offset)) != KB_OK) {
        return 0;
    }
    offset &= KB_PCI_CAP_NEXT_MASK;

    for (unsigned depth = 0; depth < 48 && offset >= 0x40; depth++) {
        uint8_t header[2] = {0};
        if (pacha_pci_config_read(device, offset, header, sizeof(header)) != KB_OK) {
            return 0;
        }
        if (header[0] == cap_id) {
            *out_offset = offset;
            return 1;
        }
        offset = header[1] & KB_PCI_CAP_NEXT_MASK;
    }

    return 0;
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
    if (backend == NULL || map_addr == NULL || region_addr == NULL || map_size == 0 || capsule == 0) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_mmio_mapping_t *mapping = calloc(1, sizeof(*mapping));
    if (mapping == NULL) {
        return KB_ERR_NOMEM;
    }
    mapping->map_addr = map_addr;
    mapping->region_addr = region_addr;
    mapping->map_size = map_size;
    mapping->capsule = capsule;
    mapping->bar_index = bar_index;
    mapping->next = backend->mmio_mappings;
    backend->mmio_mappings = mapping;
    return KB_OK;
}

static kb_pachaos_mmio_mapping_t *take_mmio_mapping(kb_pachaos_capsule_backend_t *backend, void *addr)
{
    kb_pachaos_mmio_mapping_t **cursor = backend != NULL ? &backend->mmio_mappings : NULL;
    while (cursor != NULL && *cursor != NULL) {
        kb_pachaos_mmio_mapping_t *mapping = *cursor;
        if (mapping->map_size != 0 && mapping->region_addr == addr) {
            *cursor = mapping->next;
            mapping->next = NULL;
            return mapping;
        }
        cursor = &mapping->next;
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

    void *map_addr = MAP_FAILED;
    kb_status_t map_status = KB_ERR_NOMEM;
    for (unsigned attempt = 0; attempt < KB_PACHAOS_MMIO_MAP_ATTEMPTS; attempt++) {
        map_addr = mmap(NULL, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map_addr == MAP_FAILED) {
            map_status = KB_ERR_NOMEM;
            break;
        }
        if (pacha_user_mmio_va_valid(map_addr, map_size)) {
            map_status = KB_OK;
            break;
        }
        pacha_trace_bad_mmio_va("mmap", map_addr, map_size);
        munmap(map_addr, (size_t)map_size);
        map_addr = MAP_FAILED;
        map_status = KB_ERR_IO;
    }
    if (map_status != KB_OK) {
        return map_status;
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

    uint64_t snapshot[PACHA_SNAPSHOT_WORDS];
    kb_status_t snapshot_status = capsule_query((uint64_t)child, snapshot);
    if (snapshot_status != KB_OK ||
        snapshot[PACHA_SNAPSHOT_KIND] != PACHA_CAPSULE_KIND_MMIO ||
        snapshot[PACHA_SNAPSHOT_USER_VA] != (uint64_t)(uintptr_t)map_addr ||
        snapshot[PACHA_SNAPSHOT_SIZE] < map_size) {
        close_capsule((uint64_t)child);
        munmap(map_addr, (size_t)map_size);
        return snapshot_status == KB_OK ? KB_ERR_IO : snapshot_status;
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
    kb_pachaos_mmio_mapping_t *mapping = take_mmio_mapping(device->backend, region->addr);
    if (mapping != NULL) {
        close_capsule(mapping->capsule);
        munmap(mapping->map_addr, (size_t)mapping->map_size);
        free(mapping);
    }
    memset(region, 0, sizeof(*region));
}

static kb_status_t pacha_configure_msix_entry(kb_device_t *device, unsigned entry)
{
    if (device == NULL) {
        return KB_ERR_INVALID;
    }

    uint8_t cap = 0;
    if (!pacha_pci_find_capability(device, KB_PCI_CAP_ID_MSIX, &cap)) {
        return KB_ERR_UNSUPPORTED;
    }

    uint16_t control = 0;
    uint32_t table = 0;
    if (pacha_pci_config_read(device, (uint16_t)(cap + 2u), &control, sizeof(control)) != KB_OK ||
        pacha_pci_config_read(device, (uint16_t)(cap + 4u), &table, sizeof(table)) != KB_OK) {
        return KB_ERR_IO;
    }

    unsigned table_size = (unsigned)((control & KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1u);
    if (entry >= table_size) {
        return KB_ERR_INVALID;
    }

    unsigned bar = table & KB_PCI_MSIX_TABLE_BIR_MASK;
    uint64_t table_offset = (uint64_t)(table & ~((uint32_t)KB_PCI_MSIX_TABLE_BIR_MASK));
    uint64_t entry_offset = table_offset + ((uint64_t)entry * KB_PCI_MSIX_ENTRY_SIZE);
    if (entry_offset < table_offset) {
        return KB_ERR_INVALID;
    }

    kb_mmio_region_t region;
    kb_status_t status = pacha_map_bar(device, bar, &region);
    if (status != KB_OK) {
        return status;
    }

    if (entry_offset > UINT64_MAX - KB_PCI_MSIX_ENTRY_SIZE ||
        entry_offset + KB_PCI_MSIX_ENTRY_SIZE > region.size ||
        (uintptr_t)region.addr < KB_PACHAOS_MIN_MMIO_USER_VA ||
        (uint64_t)(uintptr_t)region.addr > UINT64_MAX - entry_offset - KB_PCI_MSIX_ENTRY_SIZE) {
        pacha_trace_bad_mmio_va("msix", region.addr, region.size);
        pacha_unmap_bar(device, &region);
        return KB_ERR_INVALID;
    }

    volatile uint32_t *slot = (volatile uint32_t *)((unsigned char *)region.addr + entry_offset);
    slot[KB_PCI_MSIX_ENTRY_VECTOR_CTRL / sizeof(uint32_t)] = KB_PCI_MSIX_ENTRY_CTRL_MASKED;
    slot[0] = PACHA_X86_MSI_ADDRESS_LOW;
    slot[1] = 0;
    slot[2] = PACHA_DEVICE_INTERRUPT_VECTOR;
    slot[KB_PCI_MSIX_ENTRY_VECTOR_CTRL / sizeof(uint32_t)] = 0;
    pacha_unmap_bar(device, &region);

    uint16_t command = 0;
    if (pacha_pci_config_read(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command)) == KB_OK) {
        command |= KB_PCI_COMMAND_MEMORY_SPACE | KB_PCI_COMMAND_BUS_MASTER;
        (void)pacha_pci_config_write(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command));
    }

    control |= KB_PCI_MSIX_CONTROL_ENABLE;
    control &= (uint16_t)~KB_PCI_MSIX_CONTROL_FUNCTION_MASK;
    return pacha_pci_config_write(device, (uint16_t)(cap + 2u), &control, sizeof(control));
}

static kb_status_t pacha_dma_alloc(
    kb_device_t *device,
    uint64_t size,
    uint64_t alignment,
    kb_dma_dir_t direction,
    kb_dma_buffer_t *out_buffer)
{
    if (device == NULL || out_buffer == NULL || size == 0 || size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    uint64_t page_size = page_size_u64();
    uint64_t effective_alignment = alignment > page_size ? alignment : page_size;
    if (!is_power_of_two_u64(effective_alignment) || effective_alignment > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    if (size > UINT64_MAX - (effective_alignment - 1u)) {
        return KB_ERR_INVALID;
    }
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
    uint64_t iova_hint = align_up_u64(backend->next_iova, effective_alignment);
    if (iova_hint == 0 || iova_hint > UINT64_MAX - alloc_size - page_size) {
        free(ptr);
        return KB_ERR_INVALID;
    }
    backend->next_iova = iova_hint + alloc_size + page_size;
    long child = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_DERIVE_DMA_BUFFER,
        device->device_capsule,
        (uint64_t)(uintptr_t)ptr,
        iova_hint,
        alloc_size,
        0);
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_DMA_BUFFER)) {
        free(ptr);
        return pacha_status_from_return(child);
    }
    uint64_t iova = 0;
    kb_status_t status = capsule_snapshot_iova((uint64_t)child, PACHA_CAPSULE_KIND_DMA_BUFFER, &iova);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        free(ptr);
        return status;
    }
    kb_pachaos_dma_mapping_t mapping = {
        .cpu_addr = ptr,
        .mapped_cpu_addr = ptr,
        .size = alloc_size,
        .mapped_size = alloc_size,
        .iova = iova,
        .buffer_capsule = (uint64_t)child,
        .mapping_capsule = 0,
        .direction = direction,
        .owns_cpu_addr = 1,
        .owns_mapped_cpu_addr = 0,
    };
    status = remember_dma_mapping(backend, &mapping);
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
    kb_pachaos_dma_mapping_t *mapping = take_dma_mapping(device->backend, buffer->dma_addr);
    if (mapping != NULL) {
        release_dma_mapping(mapping, 0);
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
    if (device == NULL || cpu_addr == NULL || size == 0 || size > (uint64_t)SIZE_MAX || out_dma_addr == NULL) {
        return KB_ERR_INVALID;
    }
    uint64_t page_size = page_size_u64();
    if (size > UINT64_MAX - (page_size - 1u)) {
        return KB_ERR_INVALID;
    }
    uint64_t bounce_size = align_up_u64(size, page_size);
    if (bounce_size == 0 || bounce_size > (uint64_t)SIZE_MAX) {
        return KB_ERR_INVALID;
    }
    kb_pachaos_capsule_backend_t *backend = device->backend;
    void *mapped_cpu_addr = cpu_addr;
    uint64_t mapped_size = size;
    int owns_mapped_cpu_addr = 0;
    if (dma_range_crosses_page(cpu_addr, size)) {
        void *bounce = NULL;
        if (posix_memalign(&bounce, (size_t)page_size, (size_t)bounce_size) != 0) {
            return KB_ERR_NOMEM;
        }
        if (direction == KB_DMA_TO_DEVICE || direction == KB_DMA_BIDIRECTIONAL) {
            memcpy(bounce, cpu_addr, (size_t)size);
        } else {
            memset(bounce, 0, (size_t)bounce_size);
        }
        mapped_cpu_addr = bounce;
        mapped_size = size;
        owns_mapped_cpu_addr = 1;
    }
    uint64_t iova_hint = align_up_u64(backend->next_iova, page_size);
    if (iova_hint == 0 || iova_hint > UINT64_MAX - bounce_size - page_size) {
        if (owns_mapped_cpu_addr) {
            free(mapped_cpu_addr);
        }
        return KB_ERR_INVALID;
    }
    backend->next_iova = iova_hint + bounce_size + page_size;
    long child = pacha_syscall6(
        PACHA_SYSCALL_CAPSULE_DERIVE_DMA_MAPPING,
        device->device_capsule,
        (uint64_t)(uintptr_t)mapped_cpu_addr,
        iova_hint,
        mapped_size,
        dma_dir_to_pacha(direction),
        0);
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_DMA_MAPPING)) {
        if (owns_mapped_cpu_addr) {
            free(mapped_cpu_addr);
        }
        return pacha_status_from_return(child);
    }
    uint64_t iova = 0;
    kb_status_t status = capsule_snapshot_iova((uint64_t)child, PACHA_CAPSULE_KIND_DMA_MAPPING, &iova);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        if (owns_mapped_cpu_addr) {
            free(mapped_cpu_addr);
        }
        return status;
    }
    kb_pachaos_dma_mapping_t mapping = {
        .cpu_addr = cpu_addr,
        .mapped_cpu_addr = mapped_cpu_addr,
        .size = size,
        .mapped_size = mapped_size,
        .iova = iova,
        .buffer_capsule = 0,
        .mapping_capsule = (uint64_t)child,
        .direction = direction,
        .owns_cpu_addr = 0,
        .owns_mapped_cpu_addr = owns_mapped_cpu_addr,
    };
    status = remember_dma_mapping(backend, &mapping);
    if (status != KB_OK) {
        close_capsule((uint64_t)child);
        if (owns_mapped_cpu_addr) {
            free(mapped_cpu_addr);
        }
        return status;
    }
    *out_dma_addr = iova;
    return KB_OK;
}

static void pacha_dma_unmap(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction)
{
    (void)size;
    if (device == NULL) {
        return;
    }
    kb_pachaos_dma_mapping_t *mapping = take_dma_mapping(device->backend, dma_addr);
    if (mapping != NULL) {
        int copy_back = direction == KB_DMA_FROM_DEVICE || direction == KB_DMA_BIDIRECTIONAL ||
            mapping->direction == KB_DMA_FROM_DEVICE || mapping->direction == KB_DMA_BIDIRECTIONAL;
        release_dma_mapping(mapping, copy_back);
    }
}

static uint64_t pacha_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static kb_status_t pacha_irq_poll_count(uint64_t irq_capsule, uint64_t observed_count, uint64_t *out_count)
{
    if (irq_capsule == 0 || out_count == NULL) {
        return KB_ERR_INVALID;
    }

    uint64_t words[1] = {0};
    long result = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_IRQ_POLL,
        irq_capsule,
        observed_count,
        (uint64_t)(uintptr_t)words,
        1,
        0);
    if (result == 1) {
        *out_count = words[0];
        return KB_OK;
    }
    if (result == 2) {
        return KB_ERR_NOT_FOUND;
    }
    return pacha_status_from_return(result);
}

static void pacha_decode_irq_vector(unsigned encoded_vector, uint64_t *out_kind, uint64_t *out_vector)
{
    unsigned backend_kind = encoded_vector >> KB_IRQ_BACKEND_KIND_SHIFT;
    unsigned backend_vector = encoded_vector & KB_IRQ_BACKEND_VECTOR_MASK;

    switch (backend_kind) {
    case KB_IRQ_BACKEND_KIND_MSIX:
        *out_kind = PACHA_IRQ_MSIX;
        *out_vector = backend_vector;
        return;
    case KB_IRQ_BACKEND_KIND_MSI:
        *out_kind = PACHA_IRQ_MSI;
        *out_vector = backend_vector;
        return;
    default:
        *out_kind = backend_vector == 0 ? PACHA_IRQ_INTX : PACHA_IRQ_AUTO;
        *out_vector = backend_vector;
        return;
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
    kb_irq_t *irq = calloc(1, sizeof(*irq));
    if (irq == NULL) {
        return KB_ERR_NOMEM;
    }

    uint64_t irq_kind = PACHA_IRQ_AUTO;
    uint64_t irq_vector = 0;
    pacha_decode_irq_vector(vector, &irq_kind, &irq_vector);
    long child = pacha_syscall5(
        PACHA_SYSCALL_CAPSULE_DERIVE_IRQ,
        device->device_capsule,
        irq_kind,
        irq_vector,
        0,
        0);
    if (!token_has_kind((uint64_t)child, PACHA_CAPSULE_KIND_IRQ)) {
        free(irq);
        return pacha_status_from_return(child);
    }
    if (irq_kind == PACHA_IRQ_MSIX) {
        kb_status_t config_status = pacha_configure_msix_entry(device, (unsigned)irq_vector);
        if (config_status != KB_OK) {
            close_capsule((uint64_t)child);
            free(irq);
            return config_status;
        }
    } else if (irq_kind == PACHA_IRQ_MSI) {
        close_capsule((uint64_t)child);
        free(irq);
        return KB_ERR_UNSUPPORTED;
    }
    irq->capsule = (uint64_t)child;
    irq->backend = backend;
    irq->vector = vector;
    irq->handler = handler;
    irq->ctx = ctx;
    uint64_t count = 0;
    if (pacha_irq_poll_count((uint64_t)child, UINT64_MAX, &count) == KB_OK) {
        irq->last_interrupt_count = count;
    }
    irq->next = backend->irqs;
    backend->irqs = irq;
    *out_irq = irq;
    return KB_OK;
}

static void pacha_irq_unregister(kb_device_t *device, kb_irq_t *irq)
{
    if (irq == NULL) {
        return;
    }
    kb_pachaos_capsule_backend_t *backend =
        device != NULL && device->backend != NULL ? device->backend : irq->backend;
    kb_irq_t **cursor = backend != NULL ? &backend->irqs : NULL;
    int found = 0;
    while (cursor != NULL && *cursor != NULL) {
        if (*cursor == irq) {
            *cursor = irq->next;
            irq->next = NULL;
            found = 1;
            break;
        }
        cursor = &(*cursor)->next;
    }
    if (!found) {
        return;
    }
    close_capsule(irq->capsule);
    free(irq);
}

static kb_status_t pacha_irq_wait(kb_device_t *device, kb_irq_t *irq, uint64_t timeout_ns)
{
    (void)device;
    if (irq == NULL || irq->capsule == 0) {
        return KB_ERR_INVALID;
    }

    const uint64_t start = pacha_now_ns();
    for (;;) {
        uint64_t count = 0;
        kb_status_t status = pacha_irq_poll_count(irq->capsule, irq->last_interrupt_count, &count);
        if (status == KB_OK) {
            irq->last_interrupt_count = count;
            if (irq->handler != NULL) {
                irq->handler(irq->ctx);
            }
            return KB_OK;
        }
        if (status != KB_ERR_NOT_FOUND) {
            return status;
        }
        if (timeout_ns == 0) {
            return KB_ERR_NOT_FOUND;
        }
        uint64_t now = pacha_now_ns();
        if (start != 0 && now >= start && now - start >= timeout_ns) {
            return KB_ERR_NOT_FOUND;
        }
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 1000000,
        };
        (void)nanosleep(&delay, NULL);
    }
}

static uint64_t pacha_monotonic_ns(kb_backend_t *backend)
{
    (void)backend;
    return pacha_now_ns();
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

static kb_status_t parse_u64_text(const char *text, uint64_t *out_value)
{
    if (text == NULL || text[0] == '\0' || out_value == NULL) {
        return KB_ERR_INVALID;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return KB_ERR_INVALID;
    }
    *out_value = (uint64_t)value;
    return KB_OK;
}

static int preferred_pci_class(uint8_t *out_class, uint8_t *out_subclass, uint8_t *out_prog_if)
{
    const char *text = getenv("KOBOX_PACHAOS_PREFERRED_CLASS");
    uint64_t value = 0;
    if (parse_u64_text(text, &value) != KB_OK) {
        return 0;
    }
    if (value > 0xffffu && value <= 0xffffffu) {
        *out_class = (uint8_t)((value >> 16) & 0xffu);
        *out_subclass = (uint8_t)((value >> 8) & 0xffu);
        *out_prog_if = (uint8_t)(value & 0xffu);
        return 1;
    }
    return 0;
}

static kb_status_t backend_matches_preferred_class(kb_backend_t *backend, int *out_match)
{
    if (out_match == NULL) {
        return KB_ERR_INVALID;
    }
    *out_match = 0;
    uint8_t class_code = 0;
    uint8_t subclass = 0;
    uint8_t prog_if = 0;
    if (!preferred_pci_class(&class_code, &subclass, &prog_if)) {
        *out_match = 1;
        return KB_OK;
    }

    kb_pachaos_capsule_backend_t *pacha = pacha_from_backend(backend);
    uint8_t class_bytes[3] = {0};
    kb_status_t status = pacha_pci_config_read(&pacha->device, 0x09, class_bytes, sizeof(class_bytes));
    if (status != KB_OK) {
        return status == KB_ERR_DENIED ? KB_ERR_DENIED : KB_ERR_PCI_CONFIG;
    }
    *out_match = class_bytes[2] == class_code && class_bytes[1] == subclass && class_bytes[0] == prog_if;
    return KB_OK;
}

static int preferred_pci_class_is_configured(void)
{
    uint8_t class_code = 0;
    uint8_t subclass = 0;
    uint8_t prog_if = 0;
    return preferred_pci_class(&class_code, &subclass, &prog_if);
}

static kb_status_t catalog_token_from_env(uint64_t *out_catalog_token)
{
    if (out_catalog_token == NULL) {
        return KB_ERR_INVALID;
    }
    const char *catalog_text = getenv("PACHA_EXEC_DEVICE_CATALOG");
    if (catalog_text == NULL || catalog_text[0] == '\0') {
        catalog_text = getenv("KOBOX_PACHAOS_DEVICE_CATALOG");
    }
    if (catalog_text == NULL || catalog_text[0] == '\0') {
        return KB_ERR_NOT_FOUND;
    }
    return parse_u64_text(catalog_text, out_catalog_token);
}

static kb_status_t map_catalog_page(uint64_t catalog_token, const kb_pachaos_device_catalog_page_t **out_page)
{
    if (out_page == NULL || !is_vm_object_token(catalog_token)) {
        return KB_ERR_INVALID;
    }

    long map_status = pacha_syscall3(
        PACHA_SYSCALL_MAP_VM_OBJECT,
        catalog_token,
        (uint64_t)KB_PACHAOS_DEVICE_CATALOG_VA,
        0);
    if (map_status != 0) {
        return pacha_status_from_return(map_status);
    }

    const kb_pachaos_device_catalog_page_t *page =
        (const kb_pachaos_device_catalog_page_t *)KB_PACHAOS_DEVICE_CATALOG_VA;
    if (page->magic != KB_PACHAOS_DEVICE_CATALOG_MAGIC ||
        page->version != KB_PACHAOS_DEVICE_CATALOG_VERSION) {
        return KB_ERR_INVALID;
    }
    *out_page = page;
    return KB_OK;
}

static kb_status_t create_from_catalog_token(uint64_t catalog_token, kb_backend_t **out_backend)
{
    if (out_backend == NULL) {
        return KB_ERR_INVALID;
    }

    const kb_pachaos_device_catalog_page_t *page = NULL;
    kb_status_t status = map_catalog_page(catalog_token, &page);
    if (status != KB_OK) {
        return status;
    }

    kb_status_t last_status = KB_ERR_NOT_FOUND;
    size_t count = page->entry_count;
    if (count > KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES) {
        count = KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES;
    }
    for (size_t i = 0; i < count; i++) {
        const kb_pachaos_device_catalog_entry_t *entry = &page->entries[i];
        if (entry->present == 0 || !token_has_kind(entry->device_capsule_token, PACHA_CAPSULE_KIND_DEVICE)) {
            continue;
        }

        kb_backend_t *candidate = NULL;
        status = kb_pachaos_capsule_create(entry->device_capsule_token, &candidate);
        if (status != KB_OK) {
            if (last_status == KB_ERR_NOT_FOUND || status == KB_ERR_DENIED) {
                last_status = status;
            }
            continue;
        }
        int matched = 0;
        status = backend_matches_preferred_class(candidate, &matched);
        if (status == KB_OK && matched) {
            *out_backend = candidate;
            return KB_OK;
        }
        if (status != KB_OK &&
            (last_status == KB_ERR_NOT_FOUND || status == KB_ERR_DENIED || status == KB_ERR_PCI_CONFIG))
        {
            last_status = status;
        }
        kb_backend_destroy(candidate);
    }
    return last_status;
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
    uint64_t catalog_token = 0;
    kb_status_t catalog_status = catalog_token_from_env(&catalog_token);
    if (catalog_status == KB_OK) {
        catalog_status = create_from_catalog_token(catalog_token, out_backend);
        if (catalog_status == KB_OK) {
            return KB_OK;
        }
        if (preferred_pci_class_is_configured()) {
            return catalog_status;
        }
    }

    const char *text = getenv("KOBOX_PACHAOS_DEVICE_CAPSULE");
    uint64_t token = 0;
    kb_status_t status = kb_pachaos_capsule_parse_token(text, &token);
    if (status != KB_OK) {
        return status;
    }
    kb_backend_t *backend = NULL;
    status = kb_pachaos_capsule_create(token, &backend);
    if (status != KB_OK) {
        return status;
    }
    int matched = 0;
    status = backend_matches_preferred_class(backend, &matched);
    if (status != KB_OK) {
        kb_backend_destroy(backend);
        return status;
    }
    if (!matched) {
        kb_backend_destroy(backend);
        return KB_ERR_NOT_FOUND;
    }
    *out_backend = backend;
    return KB_OK;
}

kb_status_t kb_pachaos_capsule_dump_catalog(FILE *out)
{
    if (out == NULL) {
        return KB_ERR_INVALID;
    }
    uint64_t catalog_token = 0;
    kb_status_t status = catalog_token_from_env(&catalog_token);
    if (status != KB_OK) {
        return status;
    }

    const kb_pachaos_device_catalog_page_t *page = NULL;
    status = map_catalog_page(catalog_token, &page);
    if (status != KB_OK) {
        return status;
    }

    uint8_t preferred_class = 0;
    uint8_t preferred_subclass = 0;
    uint8_t preferred_prog_if = 0;
    const int has_preferred = preferred_pci_class(&preferred_class, &preferred_subclass, &preferred_prog_if);
    fprintf(out, "PachaOS device catalog: entries=%llu", (unsigned long long)page->entry_count);
    if (has_preferred) {
        fprintf(out, " preferred=%02x:%02x:%02x", preferred_class, preferred_subclass, preferred_prog_if);
    }
    fputc('\n', out);

    size_t count = page->entry_count;
    if (count > KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES) {
        count = KB_PACHAOS_DEVICE_CATALOG_MAX_ENTRIES;
    }
    for (size_t i = 0; i < count; i++) {
        const kb_pachaos_device_catalog_entry_t *entry = &page->entries[i];
        if (entry->present == 0) {
            continue;
        }
        fprintf(
            out,
            "[%zu] kind=%llu resource=0x%llx vendor=0x%04llx device=0x%04llx token=0x%016llx",
            i,
            (unsigned long long)entry->kind,
            (unsigned long long)entry->resource_id,
            (unsigned long long)entry->vendor_id,
            (unsigned long long)entry->device_id,
            (unsigned long long)entry->device_capsule_token);
        if (!token_has_kind(entry->device_capsule_token, PACHA_CAPSULE_KIND_DEVICE)) {
            fprintf(out, " status=invalid-token\n");
            continue;
        }
        kb_backend_t *candidate = NULL;
        status = kb_pachaos_capsule_create(entry->device_capsule_token, &candidate);
        if (status != KB_OK) {
            fprintf(out, " status=%s\n", kb_status_name(status));
            continue;
        }
        kb_pachaos_capsule_backend_t *pacha = pacha_from_backend(candidate);
        uint8_t class_bytes[3] = {0};
        kb_status_t config_status = pacha_pci_config_read(&pacha->device, 0x09, class_bytes, sizeof(class_bytes));
        int matched = 0;
        kb_status_t match_status = backend_matches_preferred_class(candidate, &matched);
        if (config_status == KB_OK) {
            fprintf(
                out,
                " bdf=%04x:%02x:%02x.%u pci=%04x:%04x class=%02x:%02x:%02x",
                pacha->device.location.segment,
                pacha->device.location.bus,
                pacha->device.location.device,
                pacha->device.location.function,
                pacha->device.pci_id.vendor_id,
                pacha->device.pci_id.device_id,
                class_bytes[2],
                class_bytes[1],
                class_bytes[0]);
        } else {
            fprintf(out, " config=%s", kb_status_name(config_status));
        }
        if (has_preferred) {
            fprintf(out, " match=%s", match_status == KB_OK ? (matched ? "yes" : "no") : kb_status_name(match_status));
        }
        fputc('\n', out);
        kb_backend_destroy(candidate);
    }
    return KB_OK;
}
