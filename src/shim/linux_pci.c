#include "kobox/shim.h"
#include "shim/linux_nvme.h"
#include "subsystem/pci/pci.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_backend_t *kb_shim_current_backend(void);

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device);
static int trace_pci_enabled(void);

typedef struct shim_pci_driver {
    const char *name;
    const void *id_table;
    int (*probe)(void *dev, const void *id);
    void (*remove)(void *dev);
} shim_pci_driver_t;

typedef struct shim_pci_driver_view {
    const char *name;
    const void *id_table;
    int (*probe)(void *dev, const void *id);
    void (*remove)(void *dev);
} shim_pci_driver_view_t;

typedef struct linux_pci_device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class;
    uint32_t class_mask;
    uintptr_t driver_data;
} linux_pci_device_id_t;

#define KB_PCI_ANY_ID UINT32_C(0xffffffff)

typedef struct shim_pci_binding {
    void *driver;
    void (*remove)(void *dev);
    kb_device_t *device;
    unsigned char pci_dev_storage[4096];
    unsigned char pci_bus_storage[512];
    char pci_dev_name[32];
    uint64_t dma_mask_storage;
    uint32_t pci_domain_storage;
    uint16_t pci_command;
    int probed;
} shim_pci_binding_t;

static shim_pci_binding_t binding;
static void *mapped_bar0_addr;
static size_t mapped_bar0_size;

static int trace_xhci_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_XHCI");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

enum {
    KB_PCI_COMMAND_OFFSET = 0x04,
    KB_PCI_COMMAND_MEMORY = 0x0002,
    KB_PCI_COMMAND_MASTER = 0x0004,
    KB_PCI_COMMAND_INTX_DISABLE = 0x0400,
    KB_PCI_STATUS_OFFSET = 0x06,
    KB_PCI_STATUS_CAP_LIST = 0x0010,
    KB_PCI_CAPABILITY_LIST_OFFSET = 0x34,
    KB_PCI_CAP_NEXT_MASK = 0xfc,
    KB_PCI_CAP_ID_MSI = 0x05,
    KB_PCI_CAP_ID_MSIX = 0x11,
    KB_PCI_MSI_CONTROL_ENABLE = 0x0001,
    KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK = 0x07ff,
    KB_PCI_MSIX_CONTROL_MASKALL = 0x4000,
    KB_PCI_MSIX_CONTROL_ENABLE = 0x8000,
    KB_PCI_MSIX_TABLE_BIR_MASK = 0x00000007,
    KB_PCI_MSIX_ENTRY_SIZE = 16,
    KB_PCI_MSIX_ENTRY_VECTOR_CTRL = 12,
    KB_PCI_MSIX_ENTRY_CTRL_MASKED = 0x00000001,
    KB_PCI_CLASS_STORAGE = 0x01,
    KB_PCI_SUBCLASS_NVME = 0x08,
    KB_PCI_PROGIF_NVME = 0x02,
    KB_NVME_REG_CC = 0x14,
    KB_NVME_REG_CSTS = 0x1c,

    KB_LINUX_6_8_PCI_DEV_BUS_OFFSET = 0x010,
    KB_LINUX_6_8_PCI_DEV_DEVFN_OFFSET = 0x038,
    KB_LINUX_6_8_PCI_DEV_VENDOR_OFFSET = 0x03c,
    KB_LINUX_6_8_PCI_DEV_DEVICE_ID_OFFSET = 0x03e,
    KB_LINUX_6_8_PCI_DEV_SUBSYSTEM_VENDOR_OFFSET = 0x040,
    KB_LINUX_6_8_PCI_DEV_SUBSYSTEM_DEVICE_OFFSET = 0x042,
    KB_LINUX_6_8_PCI_DEV_CLASS_OFFSET = 0x044,
    KB_LINUX_6_8_PCI_DEV_REVISION_OFFSET = 0x048,
    KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET = 0x0c8,
    KB_LINUX_6_6_PCI_DEV_DEVICE_OFFSET = 0x0c0,
    KB_LINUX_6_6_DEVICE_DRIVER_OFFSET = 0x068,
    KB_LINUX_6_6_PCI_DRIVER_DEVICE_DRIVER_OFFSET = 0x078,
    KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET = 0x85c,
    KB_LINUX_6_8_PCI_DEV_DMA_MASK_OFFSET = 0x328,
    KB_LINUX_6_8_PCI_DEV_IRQ_OFFSET = 0x3cc,
    KB_LINUX_6_8_PCI_RESOURCE0_START_OFFSET = 0x3d0,
    KB_LINUX_6_8_PCI_RESOURCE0_END_OFFSET = 0x3d8,
    KB_LINUX_6_8_PCI_RESOURCE0_FLAGS_OFFSET = 0x3e8,
    KB_LINUX_6_6_PCI_RESOURCE0_START_OFFSET = 0x3a8,
    KB_LINUX_6_6_PCI_RESOURCE0_END_OFFSET = 0x3b0,
    KB_LINUX_6_6_PCI_RESOURCE0_FLAGS_OFFSET = 0x3c0,
    KB_LINUX_6_8_PCI_RESOURCE_SIZE = 0x40,
    KB_LINUX_6_8_PCI_BUS_DOMAIN_PTR_OFFSET = 0x0c8,
    KB_LINUX_6_8_PCI_BUS_NUMBER_OFFSET = 0x0d8,
    KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET = 0x1b0,
    KB_LINUX_IORESOURCE_MEM = 0x00000200,
    KB_IRQ_BACKEND_KIND_MSI = 1,
    KB_IRQ_BACKEND_KIND_MSIX = 2,
};

typedef struct shim_msix_entry {
    uint32_t vector;
    uint16_t entry;
    uint16_t reserved;
} shim_msix_entry_t;

int kb_pci_msix_unmask_entries(void *dev, const uint16_t *entries, unsigned int count)
{
    if (entries == NULL || count == 0) {
        return -22;
    }

    int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSIX);
    if (cap == 0) {
        return -28;
    }

    uint32_t table = 0;
    if (kb_pci_read_config_dword(dev, cap + 4, &table) != 0) {
        return -5;
    }
    unsigned int bar = table & KB_PCI_MSIX_TABLE_BIR_MASK;
    uint32_t table_offset = table & ~UINT32_C(0x7);

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return -19;
    }
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->map_bar == NULL || ops->unmap_bar == NULL) {
        return -95;
    }

    kb_mmio_region_t region;
    if (ops->map_bar(device, bar, &region) != KB_OK || region.addr == NULL) {
        return -5;
    }

    int result = 0;
    for (unsigned int i = 0; i < count; i++) {
        uint64_t ctrl_offset =
            (uint64_t)table_offset + ((uint64_t)entries[i] * KB_PCI_MSIX_ENTRY_SIZE) + KB_PCI_MSIX_ENTRY_VECTOR_CTRL;
        if (ctrl_offset + sizeof(uint32_t) > region.size) {
            result = -22;
            break;
        }
        volatile uint32_t *ctrl = (volatile uint32_t *)((unsigned char *)region.addr + ctrl_offset);
        *ctrl &= ~((uint32_t)KB_PCI_MSIX_ENTRY_CTRL_MASKED);
    }

    ops->unmap_bar(device, &region);
    return result;
}

int kb_pci_alloc_irq_vectors(void *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags)
{
    enum {
        KB_PCI_IRQ_LEGACY = 1u << 0,
        KB_PCI_IRQ_MSI = 1u << 1,
        KB_PCI_IRQ_MSIX = 1u << 2,
        KB_PCI_MSI_CONTROL_MMC_MASK = 0x000e,
        KB_PCI_MSI_CONTROL_MME_MASK = 0x0070,
    };

    if (min_vecs == 0) {
        min_vecs = 1;
    }
    if (max_vecs == 0 || max_vecs < min_vecs) {
        return -22;
    }
    if (min_vecs > KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX) {
        return -28;
    }
    if (max_vecs > KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX) {
        max_vecs = KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX;
    }

    kb_pci_subsystem_irq_vectors_clear();
    kb_irq_clear_mappings();

    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: alloc_irq_vectors min=%u max=%u flags=0x%x\n",
            min_vecs,
            max_vecs,
            flags);
    }

    if ((flags & KB_PCI_IRQ_MSIX) != 0) {
        int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSIX);
        if (cap != 0) {
            uint16_t control = 0;
            if (kb_pci_read_config_word(dev, cap + 2, &control) == 0) {
                unsigned int table_size = (unsigned int)((control & KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1u);
                unsigned int vectors = max_vecs < table_size ? max_vecs : table_size;
                if (vectors >= min_vecs) {
                    control |= KB_PCI_MSIX_CONTROL_ENABLE;
                    control &= (uint16_t)~KB_PCI_MSIX_CONTROL_MASKALL;
                    if (kb_pci_write_config_word(dev, cap + 2, control) != 0) {
                        return -5;
                    }
                    unsigned int linux_vectors[KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX];
                    uint16_t entries[KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX];
                    for (unsigned int i = 0; i < vectors; i++) {
                        entries[i] = (uint16_t)i;
                        if (kb_irq_allocate_mapping(KB_IRQ_BACKEND_KIND_MSIX, i, &linux_vectors[i]) != 0) {
                            kb_irq_clear_mappings();
                            kb_pci_subsystem_irq_vectors_clear();
                            return -12;
                        }
                    }
                    if (kb_pci_msix_unmask_entries(dev, entries, vectors) != 0) {
                        kb_irq_clear_mappings();
                        kb_pci_subsystem_irq_vectors_clear();
                        return -5;
                    }
                    if (kb_pci_subsystem_irq_vectors_set(vectors, linux_vectors) != 0) {
                        kb_irq_clear_mappings();
                        return -22;
                    }
                    if (trace_pci_enabled()) {
                        fprintf(stderr, "kobox pci: alloc_irq_vectors msix=%u\n", vectors);
                    }
                    return (int)vectors;
                } else if (trace_pci_enabled()) {
                    fprintf(
                        stderr,
                        "kobox pci: alloc_irq_vectors msix insufficient table=%u min=%u\n",
                        table_size,
                        min_vecs);
                }
            }
        }
    }
    if ((flags & KB_PCI_IRQ_MSI) != 0) {
        int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSI);
        if (cap != 0) {
            uint16_t control = 0;
            if (kb_pci_read_config_word(dev, cap + 2, &control) == 0) {
                unsigned int max_supported = 1u << ((control & KB_PCI_MSI_CONTROL_MMC_MASK) >> 1);
                unsigned int vectors = max_vecs < max_supported ? max_vecs : max_supported;
                if (vectors >= min_vecs) {
                    unsigned int mme = 0;
                    while ((1u << mme) < vectors) {
                        mme++;
                    }
                    if ((1u << mme) > vectors) {
                        mme--;
                        vectors = 1u << mme;
                    }
                    if (vectors < min_vecs) {
                        if (trace_pci_enabled()) {
                            fprintf(
                                stderr,
                                "kobox pci: alloc_irq_vectors msi rounded insufficient vectors=%u min=%u\n",
                                vectors,
                                min_vecs);
                        }
                    } else {
                        control |= KB_PCI_MSI_CONTROL_ENABLE;
                        control &= (uint16_t)~KB_PCI_MSI_CONTROL_MME_MASK;
                        control |= (uint16_t)((mme << 4) & KB_PCI_MSI_CONTROL_MME_MASK);
                        if (kb_pci_write_config_word(dev, cap + 2, control) != 0) {
                            return -5;
                        }
                        unsigned int linux_vectors[KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX];
                        for (unsigned int i = 0; i < vectors; i++) {
                            if (kb_irq_allocate_mapping(KB_IRQ_BACKEND_KIND_MSI, i, &linux_vectors[i]) != 0) {
                                kb_irq_clear_mappings();
                                kb_pci_subsystem_irq_vectors_clear();
                                return -12;
                            }
                        }
                        if (kb_pci_subsystem_irq_vectors_set(vectors, linux_vectors) != 0) {
                            kb_irq_clear_mappings();
                            return -22;
                        }
                        if (trace_pci_enabled()) {
                            fprintf(stderr, "kobox pci: alloc_irq_vectors msi=%u\n", vectors);
                        }
                        return (int)vectors;
                    }
                } else if (trace_pci_enabled()) {
                    fprintf(
                        stderr,
                        "kobox pci: alloc_irq_vectors msi insufficient max_supported=%u min=%u\n",
                        max_supported,
                        min_vecs);
                }
            }
        }
    }
    if ((flags & KB_PCI_IRQ_LEGACY) != 0 || flags == 0) {
        uint8_t interrupt_pin = 0;
        if (kb_pci_read_config_byte(dev, 0x3d, &interrupt_pin) == 0 && interrupt_pin != 0) {
            if (min_vecs <= 1) {
                unsigned int legacy_vector = 0;
                if (kb_pci_subsystem_irq_vectors_set(1, &legacy_vector) != 0) {
                    return -22;
                }
                if (trace_pci_enabled()) {
                    fprintf(stderr, "kobox pci: alloc_irq_vectors legacy=1\n");
                }
                return 1;
            }
            if (trace_pci_enabled()) {
                fprintf(
                    stderr,
                    "kobox pci: alloc_irq_vectors legacy insufficient min=%u\n",
                    min_vecs);
            }
        }
    }
    return -28;
}

int kb_pci_alloc_irq_vectors_affinity(void *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags, void *affd)
{
    (void)affd;
    return kb_pci_alloc_irq_vectors(dev, min_vecs, max_vecs, flags);
}

void kb_pci_free_irq_vectors(void *dev)
{
    int msix_cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSIX);
    if (msix_cap != 0) {
        uint16_t control = 0;
        if (kb_pci_read_config_word(dev, msix_cap + 2, &control) == 0) {
            control &= (uint16_t)~KB_PCI_MSIX_CONTROL_ENABLE;
            (void)kb_pci_write_config_word(dev, msix_cap + 2, control);
        }
    }
    int msi_cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSI);
    if (msi_cap != 0) {
        uint16_t control = 0;
        if (kb_pci_read_config_word(dev, msi_cap + 2, &control) == 0) {
            control &= (uint16_t)~KB_PCI_MSI_CONTROL_ENABLE;
            (void)kb_pci_write_config_word(dev, msi_cap + 2, control);
        }
    }
    kb_free_all_irqs();
    kb_pci_subsystem_irq_vectors_clear();
    kb_irq_clear_mappings();
}

int kb_pci_irq_vector(void *dev, unsigned int nr)
{
    (void)dev;
    unsigned int vector = 0;
    if (kb_pci_subsystem_irq_vector(nr, &vector) == 0) {
        return (int)vector;
    }
    if (kb_pci_subsystem_irq_vector_count() != 0) {
        return -22;
    }
    return (int)nr;
}

int kb_pci_request_irq(
    void *dev,
    unsigned int nr,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    void *dev_id,
    const char *fmt,
    ...)
{
    (void)fmt;
    int irq = kb_pci_irq_vector(dev, nr);
    if (irq < 0) {
        return irq;
    }
    kb_nvme_shim_track_queue(dev_id);
    int result = kb_request_threaded_irq((unsigned int)irq, handler, thread_fn, 0, "kobox-pci", dev_id);
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: request_irq nr=%u irq=%d dev_id=%p result=%d\n", nr, irq, dev_id, result);
    }
    return result;
}

void kb_pci_free_irq(void *dev, unsigned int nr, void *dev_id)
{
    int irq = kb_pci_irq_vector(dev, nr);
    kb_free_irq(irq < 0 ? nr : (unsigned int)irq, dev_id);
}

int kb_pci_enable_device_mem(void *dev)
{
    return kb_pci_enable_device(dev);
}

int kb_pci_request_selected_regions(void *dev, int bars, const char *name)
{
    (void)dev;
    (void)bars;
    (void)name;
    return 0;
}

void kb_pci_release_selected_regions(void *dev, int bars)
{
    (void)dev;
    (void)bars;
}

int kb_pci_select_bars(void *dev, unsigned long flags)
{
    (void)dev;
    (void)flags;
    return 1;
}

int kb_pci_device_is_present(void *dev)
{
    (void)dev;
    return 1;
}

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL) {
        return KB_ERR_INVALID;
    }
    return ops->device_at(backend, 0, out_device);
}

static kb_status_t update_pci_command(uint16_t set_bits, uint16_t clear_bits)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return KB_ERR_NOT_FOUND;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->pci_config_read == NULL || ops->pci_config_write == NULL) {
        return KB_ERR_UNSUPPORTED;
    }

    uint16_t command = 0;
    kb_status_t status = ops->pci_config_read(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command));
    if (status != KB_OK) {
        return status;
    }
    command = (uint16_t)((command | set_bits) & (uint16_t)~clear_bits);
    return ops->pci_config_write(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command));
}

static int trace_pci_enabled(void)
{
    return getenv("KOBOX_TRACE_PCI") != NULL;
}

static void write_u32(void *ptr, uint32_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static void write_u16(void *ptr, uint16_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static uint32_t read_u32_field(const void *ptr)
{
    uint32_t value = 0;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uintptr_t read_ulong_field(const void *ptr)
{
    uintptr_t value = 0;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void write_pci_resource(
    size_t resource0_start_offset,
    size_t resource0_end_offset,
    size_t resource0_flags_offset,
    unsigned bar_index,
    uint64_t start,
    uint64_t end,
    uint64_t flags)
{
    const size_t resource_offset = resource0_start_offset + ((size_t)bar_index * KB_LINUX_6_8_PCI_RESOURCE_SIZE);
    memcpy(binding.pci_dev_storage + resource_offset, &start, sizeof(start));
    memcpy(
        binding.pci_dev_storage + resource_offset + (resource0_end_offset - resource0_start_offset),
        &end,
        sizeof(end));
    memcpy(
        binding.pci_dev_storage + resource_offset + (resource0_flags_offset - resource0_start_offset),
        &flags,
        sizeof(flags));
}

static void config_write_u16(unsigned char *config, size_t offset, uint16_t value)
{
    memcpy(config + offset, &value, sizeof(value));
}

static void config_write_u32(unsigned char *config, size_t offset, uint32_t value)
{
    memcpy(config + offset, &value, sizeof(value));
}

static int pci_id_field_matches(uint32_t expected, uint32_t actual)
{
    return expected == KB_PCI_ANY_ID || expected == actual;
}

static const linux_pci_device_id_t *match_pci_id_table_stride(
    const void *id_table,
    const kb_pci_id_t *device_id,
    size_t stride)
{
    if (id_table == NULL || device_id == NULL) {
        return NULL;
    }

    const uint32_t class_value =
        ((uint32_t)device_id->class_code << 16) |
        ((uint32_t)device_id->subclass << 8) |
        (uint32_t)device_id->prog_if;
    const unsigned char *entry = id_table;
    for (size_t i = 0; i < 4096; i++, entry += stride) {
        const uint32_t vendor = read_u32_field(entry + 0);
        const uint32_t device = read_u32_field(entry + 4);
        const uint32_t subvendor = read_u32_field(entry + 8);
        const uint32_t subdevice = read_u32_field(entry + 12);
        const uint32_t class = read_u32_field(entry + 16);
        const uint32_t class_mask = read_u32_field(entry + 20);
        const uintptr_t driver_data = read_ulong_field(entry + 24);
        if (vendor == 0 && device == 0 && subvendor == 0 && subdevice == 0 && class == 0 && class_mask == 0 &&
            driver_data == 0)
        {
            return NULL;
        }
        if (!pci_id_field_matches(vendor, device_id->vendor_id) ||
            !pci_id_field_matches(device, device_id->device_id) ||
            !pci_id_field_matches(subvendor, device_id->subsystem_vendor_id) ||
            !pci_id_field_matches(subdevice, device_id->subsystem_device_id))
        {
            continue;
        }
        if (((class_value ^ class) & class_mask) != 0) {
            continue;
        }
        return (const linux_pci_device_id_t *)entry;
    }
    return NULL;
}

static const linux_pci_device_id_t *match_pci_id_table(const void *id_table, const kb_pci_id_t *device_id)
{
    const linux_pci_device_id_t *match = match_pci_id_table_stride(id_table, device_id, sizeof(linux_pci_device_id_t));
    if (match != NULL) {
        return match;
    }
    return match_pci_id_table_stride(id_table, device_id, 0x28);
}

static uintptr_t read_pointer_field(const void *base, size_t offset)
{
    uintptr_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static shim_pci_driver_view_t read_pci_driver_view(const void *driver, size_t base_offset)
{
    shim_pci_driver_view_t view;
    memset(&view, 0, sizeof(view));
    view.name = (const char *)read_pointer_field(driver, base_offset + 0);
    view.id_table = (const void *)read_pointer_field(driver, base_offset + 8);
    view.probe = (int (*)(void *, const void *))read_pointer_field(driver, base_offset + 16);
    view.remove = (void (*)(void *))read_pointer_field(driver, base_offset + 24);
    return view;
}

static shim_pci_driver_view_t select_pci_driver_view(
    const void *driver,
    const kb_pci_id_t *device_id,
    const linux_pci_device_id_t **out_matched_id)
{
    shim_pci_driver_view_t compact = read_pci_driver_view(driver, 0);
    shim_pci_driver_view_t linux_list_head = read_pci_driver_view(driver, 16);
    const linux_pci_device_id_t *compact_match = match_pci_id_table(compact.id_table, device_id);
    const linux_pci_device_id_t *linux_match = match_pci_id_table(linux_list_head.id_table, device_id);

    if (compact_match != NULL && compact.probe != NULL) {
        *out_matched_id = compact_match;
        return compact;
    }
    if (linux_match != NULL && linux_list_head.probe != NULL) {
        *out_matched_id = linux_match;
        return linux_list_head;
    }
    if (compact_match != NULL) {
        *out_matched_id = compact_match;
        return compact;
    }
    if (linux_match != NULL) {
        *out_matched_id = linux_match;
        return linux_list_head;
    }

    *out_matched_id = NULL;
    return compact;
}

static int is_nvidia_driver_name(const char *name)
{
    return name != NULL && strncmp(name, "nvidia", 6) == 0;
}

static uint32_t mmio_read32(void *base, size_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    return *reg;
}

static void mmio_write32(void *base, size_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    *reg = value;
}

static void quiesce_nvme_controller(void)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_pci_id == NULL || ops->map_bar == NULL || ops->unmap_bar == NULL) {
        return;
    }

    kb_pci_id_t id;
    memset(&id, 0, sizeof(id));
    if (ops->device_pci_id(device, &id) != KB_OK ||
        id.class_code != KB_PCI_CLASS_STORAGE ||
        id.subclass != KB_PCI_SUBCLASS_NVME ||
        id.prog_if != KB_PCI_PROGIF_NVME)
    {
        return;
    }

    kb_mmio_region_t bar;
    memset(&bar, 0, sizeof(bar));
    if (ops->map_bar(device, 0, &bar) != KB_OK || bar.addr == NULL) {
        return;
    }

    uint32_t cc = mmio_read32(bar.addr, KB_NVME_REG_CC);
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: nvme quiesce cc=0x%x csts=0x%x\n", cc, mmio_read32(bar.addr, KB_NVME_REG_CSTS));
    }
    if ((cc & 1u) != 0) {
        mmio_write32(bar.addr, KB_NVME_REG_CC, cc & ~1u);
        for (unsigned i = 0; i < 10000000u; i++) {
            if ((mmio_read32(bar.addr, KB_NVME_REG_CSTS) & 1u) == 0) {
                break;
            }
        }
    }
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: nvme quiesced cc=0x%x csts=0x%x\n", mmio_read32(bar.addr, KB_NVME_REG_CC), mmio_read32(bar.addr, KB_NVME_REG_CSTS));
    }
    ops->unmap_bar(device, &bar);
}

int kb_pci_register_driver(void *driver, void *owner, const char *mod_name)
{
    (void)owner;
    (void)mod_name;
    if (driver == NULL) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = NULL;
    if (first_device(backend, &device) != KB_OK) {
        return -19;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_pci_id == NULL) {
        return -19;
    }
    kb_pci_id_t device_id;
    memset(&device_id, 0, sizeof(device_id));
    if (ops->device_pci_id(device, &device_id) != KB_OK) {
        return -19;
    }

    const linux_pci_device_id_t *matched_id = NULL;
    shim_pci_driver_view_t pci_driver = select_pci_driver_view(driver, &device_id, &matched_id);
    if (matched_id == NULL) {
        if (trace_pci_enabled()) {
            fprintf(
                stderr,
                "kobox pci: pci_register_driver no match driver=%s vendor=0x%x device=0x%x class=0x%x%02x%02x\n",
                pci_driver.name == NULL ? "" : pci_driver.name,
                device_id.vendor_id,
                device_id.device_id,
                device_id.class_code,
                device_id.subclass,
                device_id.prog_if);
        }
        if (is_nvidia_driver_name(pci_driver.name)) {
            return 0;
        }
        if (pci_driver.id_table != NULL) {
            return 0;
        }
    }
    binding.driver = driver;
    binding.remove = pci_driver.remove;
    binding.device = device;
    binding.probed = 0;
    binding.pci_command = 0;
    memset(binding.pci_dev_storage, 0, sizeof(binding.pci_dev_storage));
    memset(binding.pci_bus_storage, 0, sizeof(binding.pci_bus_storage));
    memset(binding.pci_dev_name, 0, sizeof(binding.pci_dev_name));
    binding.dma_mask_storage = UINT64_MAX;
    uintptr_t dma_mask_ptr = (uintptr_t)&binding.dma_mask_storage;
    memcpy(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DMA_MASK_OFFSET,
        &dma_mask_ptr,
        sizeof(dma_mask_ptr));
    kb_pci_location_t location;
    memset(&location, 0, sizeof(location));
    if (ops != NULL && ops->device_pci_location != NULL) {
        (void)ops->device_pci_location(device, &location);
    }
    uintptr_t bus_ptr = (uintptr_t)binding.pci_bus_storage;
    memcpy(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_BUS_OFFSET, &bus_ptr, sizeof(bus_ptr));
    uintptr_t device_driver_ptr = (uintptr_t)((unsigned char *)driver + KB_LINUX_6_6_PCI_DRIVER_DEVICE_DRIVER_OFFSET);
    memcpy(
        binding.pci_dev_storage + KB_LINUX_6_6_PCI_DEV_DEVICE_OFFSET + KB_LINUX_6_6_DEVICE_DRIVER_OFFSET,
        &device_driver_ptr,
        sizeof(device_driver_ptr));
    uint32_t devfn = ((uint32_t)location.device << 3) | (uint32_t)(location.function & 0x7u);
    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVFN_OFFSET, devfn);
    uintptr_t domain_ptr = (uintptr_t)&binding.pci_domain_storage;
    memcpy(binding.pci_bus_storage + KB_LINUX_6_8_PCI_BUS_DOMAIN_PTR_OFFSET, &domain_ptr, sizeof(domain_ptr));
    binding.pci_bus_storage[KB_LINUX_6_8_PCI_BUS_NUMBER_OFFSET] = location.bus;
    (void)snprintf(
        binding.pci_dev_name,
        sizeof(binding.pci_dev_name),
        "%04x:%02x:%02x.%u",
        location.segment,
        location.bus,
        location.device,
        location.function & 0x7u);
    uintptr_t pci_dev_name_ptr = (uintptr_t)binding.pci_dev_name;
    memcpy(binding.pci_dev_storage + KB_LINUX_6_6_PCI_DEV_DEVICE_OFFSET, &pci_dev_name_ptr, sizeof(pci_dev_name_ptr));
    memcpy(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET, &pci_dev_name_ptr, sizeof(pci_dev_name_ptr));
    write_u16(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_VENDOR_OFFSET, device_id.vendor_id);
    write_u16(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_ID_OFFSET, device_id.device_id);
    write_u16(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_SUBSYSTEM_VENDOR_OFFSET, device_id.subsystem_vendor_id);
    write_u16(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_SUBSYSTEM_DEVICE_OFFSET, device_id.subsystem_device_id);
    write_u32(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_CLASS_OFFSET,
        ((uint32_t)device_id.class_code << 16) | ((uint32_t)device_id.subclass << 8) | device_id.prog_if);
    binding.pci_dev_storage[KB_LINUX_6_8_PCI_DEV_REVISION_OFFSET] = device_id.revision;

    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_IRQ_OFFSET, 16);
    if (ops != NULL && ops->pci_bar_info != NULL) {
        for (unsigned bar_index = 0; bar_index < 6; bar_index++) {
            kb_pci_bar_info_t bar;
            if (ops->pci_bar_info(device, bar_index, &bar) != KB_OK || !bar.present) {
                continue;
            }
            uint64_t flags = bar.flags == 0 ? KB_LINUX_IORESOURCE_MEM : bar.flags;
            write_pci_resource(
                KB_LINUX_6_8_PCI_RESOURCE0_START_OFFSET,
                KB_LINUX_6_8_PCI_RESOURCE0_END_OFFSET,
                KB_LINUX_6_8_PCI_RESOURCE0_FLAGS_OFFSET,
                bar_index,
                bar.start,
                bar.end,
                flags);
            write_pci_resource(
                KB_LINUX_6_6_PCI_RESOURCE0_START_OFFSET,
                KB_LINUX_6_6_PCI_RESOURCE0_END_OFFSET,
                KB_LINUX_6_6_PCI_RESOURCE0_FLAGS_OFFSET,
                bar_index,
                bar.start,
                bar.end,
                flags);
        }
    }

    if (pci_driver.probe == NULL) {
        return 0;
    }

    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: pci_register_driver driver=%s vendor=0x%x device=0x%x class=0x%x%02x%02x probe=%p\n",
            pci_driver.name == NULL ? "" : pci_driver.name,
            device_id.vendor_id,
            device_id.device_id,
            device_id.class_code,
            device_id.subclass,
            device_id.prog_if,
            (void *)pci_driver.probe);
    }
    int result = pci_driver.probe(binding.pci_dev_storage, matched_id);
    if (result != 0) {
        binding.driver = NULL;
        binding.device = NULL;
        return result;
    }
    binding.probed = 1;
    return 0;
}

void kb_pci_unregister_driver(void *driver)
{
    if (binding.driver == NULL || binding.driver != driver) {
        return;
    }
    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: pci_unregister_driver probed=%d remove=%p\n",
            binding.probed,
            (void *)binding.remove);
    }
    if (binding.probed && binding.remove != NULL) {
        binding.remove(binding.pci_dev_storage);
        if (trace_pci_enabled()) {
            fprintf(stderr, "kobox pci: pci_unregister_driver remove returned\n");
        }
    }
    binding.driver = NULL;
    binding.remove = NULL;
    binding.device = NULL;
    binding.probed = 0;
}

int kb_pci_enable_device(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_enable_device\n");
    }
    kb_status_t status = update_pci_command(KB_PCI_COMMAND_MEMORY, KB_PCI_COMMAND_INTX_DISABLE);
    binding.pci_command = (uint16_t)(binding.pci_command | KB_PCI_COMMAND_MEMORY);
    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET, 1);
    write_u32(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET + KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET,
        1);
    return status == KB_OK ? 0 : -5;
}

int kb_pcim_enable_device(void *dev)
{
    return kb_pci_enable_device(dev);
}

void kb_pci_disable_device(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_disable_device\n");
    }
    quiesce_nvme_controller();
    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET, 0);
    write_u32(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET + KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET,
        0);
    (void)update_pci_command(0, (uint16_t)(KB_PCI_COMMAND_MEMORY | KB_PCI_COMMAND_MASTER));
    binding.pci_command = (uint16_t)(binding.pci_command & (uint16_t)~(KB_PCI_COMMAND_MEMORY | KB_PCI_COMMAND_MASTER));
}

void kb_pci_set_master(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_set_master\n");
    }
    (void)update_pci_command(KB_PCI_COMMAND_MASTER, 0);
    binding.pci_command = (uint16_t)(binding.pci_command | KB_PCI_COMMAND_MASTER);
}

void *kb_pci_dev_get(void *dev)
{
    return dev;
}

void kb_pci_dev_put(void *dev)
{
    (void)dev;
}

void kb_pci_d3cold_disable(void *dev)
{
    (void)dev;
}

int kb_pci_choose_state(void *dev, int state)
{
    (void)dev;
    return state;
}

int kb_pci_set_power_state(void *dev, int state)
{
    (void)dev;
    (void)state;
    return 0;
}

int kb_pci_set_mwi(void *dev)
{
    (void)dev;
    return 0;
}

const void *kb_pci_match_id(const void *id_table, void *dev)
{
    (void)dev;
    if (id_table == NULL || binding.device == NULL) {
        return NULL;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    kb_pci_id_t id;
    memset(&id, 0, sizeof(id));
    if (ops == NULL || ops->device_pci_id == NULL || ops->device_pci_id(binding.device, &id) != KB_OK) {
        return NULL;
    }
    return match_pci_id_table(id_table, &id);
}

static int read_config_bytes(void *dev, int where, void *dst, size_t len)
{
    (void)dev;
    if (where < 0 || dst == NULL || len == 0 || (size_t)where + len > 4096u) {
        return -22;
    }

    unsigned char config[256];
    memset(config, 0, sizeof(config));

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return -19;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops != NULL && ops->pci_config_read != NULL) {
        uint16_t real_vendor = 0;
        if (ops->pci_config_read(device, 0, &real_vendor, sizeof(real_vendor)) == KB_OK &&
            real_vendor != 0 && real_vendor != UINT16_MAX &&
            ops->pci_config_read(device, (uint16_t)where, dst, len) == KB_OK)
        {
            if (trace_pci_enabled()) {
                uint32_t value = 0;
                memcpy(&value, dst, len < sizeof(value) ? len : sizeof(value));
                fprintf(stderr, "kobox pci: pci_read_config where=0x%x len=%zu value=0x%x backend\n", where, len, value);
            }
            return 0;
        }
    }

    kb_pci_id_t id;
    memset(&id, 0, sizeof(id));
    if (ops != NULL && ops->device_pci_id != NULL) {
        (void)ops->device_pci_id(device, &id);
    }

    config_write_u16(config, 0x00, id.vendor_id);
    config_write_u16(config, 0x02, id.device_id);
    config_write_u16(config, 0x04, binding.pci_command);
    config[0x08] = id.revision;
    config[0x09] = id.prog_if;
    config[0x0a] = id.subclass;
    config[0x0b] = id.class_code;
    config[0x0e] = 0x00;
    config_write_u16(config, 0x2c, id.subsystem_vendor_id);
    config_write_u16(config, 0x2e, id.subsystem_device_id);
    config[0x3c] = 16;

    if (ops != NULL && ops->pci_bar_info != NULL) {
        for (unsigned bar_index = 0; bar_index < 6; bar_index++) {
            kb_pci_bar_info_t bar;
            if (ops->pci_bar_info(device, bar_index, &bar) != KB_OK || !bar.present) {
                continue;
            }
            uint32_t raw_bar = (uint32_t)(bar.start & UINT64_C(0xfffffff0));
            if ((bar.flags & KB_LINUX_IORESOURCE_MEM) == 0) {
                raw_bar = (uint32_t)((bar.start & UINT64_C(0xfffffffc)) | 1u);
            }
            config_write_u32(config, 0x10 + ((size_t)bar_index * sizeof(uint32_t)), raw_bar);
        }
    }

    if ((size_t)where >= sizeof(config)) {
        memset(dst, 0, len);
        if (trace_pci_enabled()) {
            fprintf(stderr, "kobox pci: pci_read_config where=0x%x len=%zu zero\n", where, len);
        }
        return 0;
    }
    const size_t available = sizeof(config) - (size_t)where;
    const size_t copy_len = len < available ? len : available;
    memcpy(dst, config + where, copy_len);
    if (copy_len < len) {
        memset((unsigned char *)dst + copy_len, 0, len - copy_len);
    }
    if (trace_pci_enabled()) {
        uint32_t value = 0;
        memcpy(&value, dst, len < sizeof(value) ? len : sizeof(value));
        fprintf(stderr, "kobox pci: pci_read_config where=0x%x len=%zu value=0x%x\n", where, len, value);
    }
    return 0;
}

int kb_pci_read_config_byte(void *dev, int where, uint8_t *value)
{
    return read_config_bytes(dev, where, value, sizeof(*value));
}

int kb_pci_read_config_word(void *dev, int where, uint16_t *value)
{
    return read_config_bytes(dev, where, value, sizeof(*value));
}

int kb_pci_read_config_dword(void *dev, int where, uint32_t *value)
{
    return read_config_bytes(dev, where, value, sizeof(*value));
}

static int write_config_bytes(void *dev, int where, const void *src, size_t len)
{
    (void)dev;
    if (where < 0 || src == NULL || len == 0 || (size_t)where + len > 4096u) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return -19;
    }

    if ((size_t)where <= KB_PCI_COMMAND_OFFSET &&
        (size_t)where + len >= KB_PCI_COMMAND_OFFSET + sizeof(binding.pci_command))
    {
        memcpy(&binding.pci_command, (const unsigned char *)src + (KB_PCI_COMMAND_OFFSET - (size_t)where), sizeof(binding.pci_command));
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops != NULL && ops->pci_config_write != NULL) {
        kb_status_t status = ops->pci_config_write(device, (uint16_t)where, src, len);
        if (trace_pci_enabled()) {
            uint32_t value = 0;
            memcpy(&value, src, len < sizeof(value) ? len : sizeof(value));
            fprintf(stderr, "kobox pci: pci_write_config where=0x%x len=%zu value=0x%x status=%d\n", where, len, value, status);
        }
        return status == KB_OK ? 0 : -5;
    }
    return 0;
}

int kb_pci_write_config_byte(void *dev, int where, uint8_t value)
{
    return write_config_bytes(dev, where, &value, sizeof(value));
}

int kb_pci_write_config_word(void *dev, int where, uint16_t value)
{
    return write_config_bytes(dev, where, &value, sizeof(value));
}

int kb_pci_write_config_dword(void *dev, int where, uint32_t value)
{
    return write_config_bytes(dev, where, &value, sizeof(value));
}

int kb_pci_find_capability(void *dev, int cap)
{
    uint16_t status = 0;
    if (kb_pci_read_config_word(dev, KB_PCI_STATUS_OFFSET, &status) != 0 ||
        (status & KB_PCI_STATUS_CAP_LIST) == 0)
    {
        return 0;
    }

    uint8_t offset = 0;
    if (kb_pci_read_config_byte(dev, KB_PCI_CAPABILITY_LIST_OFFSET, &offset) != 0) {
        return 0;
    }
    offset &= KB_PCI_CAP_NEXT_MASK;

    for (unsigned depth = 0; depth < 48 && offset >= 0x40; depth++) {
        uint8_t id = 0;
        uint8_t next = 0;
        if (kb_pci_read_config_byte(dev, offset, &id) != 0 ||
            kb_pci_read_config_byte(dev, offset + 1, &next) != 0)
        {
            return 0;
        }
        if (id == (uint8_t)cap) {
            return offset;
        }
        offset = next & KB_PCI_CAP_NEXT_MASK;
    }
    return 0;
}

int kb_pci_enable_msi(void *dev)
{
    int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSI);
    if (cap == 0) {
        return -28;
    }

    uint16_t control = 0;
    if (kb_pci_read_config_word(dev, cap + 2, &control) != 0) {
        return -5;
    }
    control |= KB_PCI_MSI_CONTROL_ENABLE;
    return kb_pci_write_config_word(dev, cap + 2, control) == 0 ? 0 : -5;
}

int kb_pci_enable_msix_range(void *dev, void *entries, int minvec, int maxvec)
{
    if (entries == NULL || minvec <= 0 || maxvec < minvec) {
        return -22;
    }

    int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSIX);
    if (cap == 0) {
        return -28;
    }

    uint16_t control = 0;
    if (kb_pci_read_config_word(dev, cap + 2, &control) != 0) {
        return -5;
    }

    int table_size = (int)((control & KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1u);
    int vectors = maxvec < table_size ? maxvec : table_size;
    if (vectors > KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX) {
        vectors = KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX;
    }
    if (vectors < minvec) {
        return -28;
    }

    shim_msix_entry_t *msix_entries = entries;
    for (int i = 0; i < vectors; i++) {
        if (msix_entries[i].entry >= (uint16_t)table_size) {
            return -22;
        }
    }
    uint16_t *table_entries = calloc((size_t)vectors, sizeof(*table_entries));
    if (table_entries == NULL) {
        return -12;
    }

    unsigned int linux_vectors[KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX];
    for (int i = 0; i < vectors; i++) {
        unsigned int linux_irq = 0;
        int status = kb_irq_allocate_mapping(KB_IRQ_BACKEND_KIND_MSIX, (unsigned int)msix_entries[i].entry, &linux_irq);
        if (status != 0) {
            kb_irq_clear_mappings();
            kb_pci_subsystem_irq_vectors_clear();
            free(table_entries);
            return status;
        }
        linux_vectors[i] = linux_irq;
        table_entries[i] = msix_entries[i].entry;
        msix_entries[i].vector = linux_irq;
    }
    if (kb_pci_msix_unmask_entries(dev, table_entries, (unsigned int)vectors) != 0) {
        kb_irq_clear_mappings();
        kb_pci_subsystem_irq_vectors_clear();
        free(table_entries);
        return -5;
    }
    free(table_entries);

    control |= KB_PCI_MSIX_CONTROL_ENABLE;
    control &= (uint16_t)~KB_PCI_MSIX_CONTROL_MASKALL;
    if (kb_pci_write_config_word(dev, cap + 2, control) != 0) {
        kb_irq_clear_mappings();
        kb_pci_subsystem_irq_vectors_clear();
        return -5;
    }
    if (kb_pci_subsystem_irq_vectors_set((unsigned int)vectors, linux_vectors) != 0) {
        kb_irq_clear_mappings();
        return -22;
    }
    return vectors;
}

void kb_pci_disable_msi(void *dev)
{
    int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSI);
    if (cap == 0) {
        return;
    }

    uint16_t control = 0;
    if (kb_pci_read_config_word(dev, cap + 2, &control) == 0) {
        control &= (uint16_t)~KB_PCI_MSI_CONTROL_ENABLE;
        (void)kb_pci_write_config_word(dev, cap + 2, control);
    }
    kb_pci_subsystem_irq_vectors_clear();
    kb_irq_clear_mappings();
}

void kb_pci_disable_msix(void *dev)
{
    int cap = kb_pci_find_capability(dev, KB_PCI_CAP_ID_MSIX);
    if (cap == 0) {
        return;
    }

    uint16_t control = 0;
    if (kb_pci_read_config_word(dev, cap + 2, &control) == 0) {
        control &= (uint16_t)~KB_PCI_MSIX_CONTROL_ENABLE;
        (void)kb_pci_write_config_word(dev, cap + 2, control);
    }
    kb_pci_subsystem_irq_vectors_clear();
    kb_irq_clear_mappings();
}

void *kb_pci_get_class(unsigned int class, void *from)
{
    if (from != NULL) {
        return NULL;
    }
    if (binding.device == NULL) {
        return NULL;
    }

    uint32_t actual_class = 0;
    memcpy(&actual_class, binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_CLASS_OFFSET, sizeof(actual_class));
    if (actual_class != class) {
        return NULL;
    }
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_get_class class=0x%x matched\n", class);
    }
    return binding.pci_dev_storage;
}

void *kb_pci_iomap(void *dev, int bar, unsigned long max)
{
    (void)dev;
    (void)max;
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return NULL;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->map_bar == NULL) {
        return NULL;
    }

    kb_mmio_region_t region;
    if (ops->map_bar(device, (unsigned)bar, &region) != KB_OK) {
        return NULL;
    }
    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: pci_iomap bar=%d max=%lu addr=%p size=%llu phys=0x%llx\n",
            bar,
            max,
            region.addr,
            (unsigned long long)region.size,
            (unsigned long long)region.backend_phys);
    }
    if (bar == 0) {
        mapped_bar0_addr = region.addr;
        mapped_bar0_size = (size_t)region.size;
    }
    return region.addr;
}

void *kb_pcim_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pci_iomap(dev, bar, max);
}

void kb_pci_iounmap(void *dev, void *addr)
{
    (void)dev;
    (void)addr;
}

void *kb_ioremap(uint64_t phys_addr, size_t size)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return NULL;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->pci_bar_info == NULL || ops->map_bar == NULL) {
        return NULL;
    }

    kb_pci_bar_info_t bar;
    if (ops->pci_bar_info(device, 0, &bar) != KB_OK || !bar.present) {
        return NULL;
    }
    if (phys_addr < bar.start || size > bar.size || phys_addr + size - 1u > bar.end) {
        return NULL;
    }

    kb_mmio_region_t region;
    if (ops->map_bar(device, 0, &region) != KB_OK) {
        return NULL;
    }
    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: ioremap phys=0x%llx size=%zu addr=%p\n",
            (unsigned long long)phys_addr,
            size,
            (void *)((unsigned char *)region.addr + (phys_addr - bar.start)));
    }
    mapped_bar0_addr = region.addr;
    mapped_bar0_size = (size_t)region.size;
    return (unsigned char *)region.addr + (phys_addr - bar.start);
}

int kb_pci_xhci_irq_pending(void)
{
    enum {
        KB_XHCI_CAPLENGTH_OFFSET = 0x00,
        KB_XHCI_HCSPARAMS1_OFFSET = 0x04,
        KB_XHCI_HCIVERSION_OFFSET = 0x02,
        KB_XHCI_USBSTS_OFFSET = 0x04,
        KB_XHCI_PORT_REGS_OFFSET = 0x400,
        KB_XHCI_PORT_REGS_STRIDE = 0x10,
        KB_XHCI_USBSTS_EINT = 1u << 3,
        KB_XHCI_PORTSC_CHANGE_MASK = 0x00fe0000u,
    };

    if (mapped_bar0_addr == NULL || mapped_bar0_size < 0x40) {
        return 0;
    }

    volatile unsigned char *base = mapped_bar0_addr;
    uint32_t cap_header = *(volatile uint32_t *)(base + KB_XHCI_CAPLENGTH_OFFSET);
    uint8_t caplength = (uint8_t)(cap_header & 0xffu);
    uint16_t raw_version = (uint16_t)((cap_header >> 16) & 0xffffu);
    if (trace_xhci_enabled()) {
        static unsigned trace_count;
        if ((trace_count++ & 0x3fu) == 0) {
            fprintf(stderr, "kobox xhci: bar=%p size=%zu header=0x%08x cap=0x%02x version=0x%04x\n",
                mapped_bar0_addr,
                mapped_bar0_size,
                cap_header,
                caplength,
                raw_version);
        }
    }
    if (caplength < 0x20 || (size_t)caplength + KB_XHCI_USBSTS_OFFSET + sizeof(uint32_t) > mapped_bar0_size) {
        return 0;
    }

    if (raw_version < 0x0090 || raw_version > 0x0120) {
        return 0;
    }

    volatile uint32_t *usbsts = (volatile uint32_t *)(base + caplength + KB_XHCI_USBSTS_OFFSET);
    uint32_t hcsparams1 = *(volatile uint32_t *)(base + KB_XHCI_HCSPARAMS1_OFFSET);
    unsigned max_ports = (hcsparams1 >> 24) & 0xffu;
    int port_change_pending = 0;
    for (unsigned port = 0; port < max_ports; port++) {
        size_t portsc_offset = (size_t)caplength + KB_XHCI_PORT_REGS_OFFSET +
            ((size_t)port * KB_XHCI_PORT_REGS_STRIDE);
        if (portsc_offset + sizeof(uint32_t) > mapped_bar0_size) {
            break;
        }
        volatile uint32_t *portsc = (volatile uint32_t *)(base + portsc_offset);
        if ((*portsc & KB_XHCI_PORTSC_CHANGE_MASK) != 0) {
            port_change_pending = 1;
            break;
        }
    }
    if (trace_xhci_enabled()) {
        static unsigned status_trace_count;
        if ((status_trace_count++ & 0x3fu) == 0) {
            volatile uint32_t *usbcmd = (volatile uint32_t *)(base + caplength);
            fprintf(
                stderr,
                "kobox xhci: usbcmd=0x%08x usbsts=0x%08x port_change=%d\n",
                *usbcmd,
                *usbsts,
                port_change_pending);
            unsigned trace_ports = max_ports;
            if (trace_ports > 8u) {
                trace_ports = 8u;
            }
            for (unsigned port = 0; port < trace_ports; port++) {
                size_t portsc_offset = (size_t)caplength + KB_XHCI_PORT_REGS_OFFSET +
                    ((size_t)port * KB_XHCI_PORT_REGS_STRIDE);
                if (portsc_offset + sizeof(uint32_t) > mapped_bar0_size) {
                    break;
                }
                volatile uint32_t *portsc = (volatile uint32_t *)(base + portsc_offset);
                fprintf(stderr, "kobox xhci: port%u portsc=0x%08x\n", port + 1u, *portsc);
            }
        }
    }
    return ((*usbsts & KB_XHCI_USBSTS_EINT) != 0 || port_change_pending) ? 1 : 0;
}

void kb_iounmap(void *addr)
{
    (void)addr;
}

int __pci_register_driver(void *driver, void *owner, const char *mod_name)
{
    return kb_pci_register_driver(driver, owner, mod_name);
}

void pci_unregister_driver(void *driver)
{
    kb_pci_unregister_driver(driver);
}

int pci_enable_device(void *dev)
{
    return kb_pci_enable_device(dev);
}

int pcim_enable_device(void *dev)
{
    return kb_pcim_enable_device(dev);
}

void pci_disable_device(void *dev)
{
    kb_pci_disable_device(dev);
}

void pci_set_master(void *dev)
{
    kb_pci_set_master(dev);
}

void *pci_dev_get(void *dev)
{
    return kb_pci_dev_get(dev);
}

void pci_dev_put(void *dev)
{
    kb_pci_dev_put(dev);
}

void pci_d3cold_disable(void *dev)
{
    kb_pci_d3cold_disable(dev);
}

int pci_choose_state(void *dev, int state)
{
    return kb_pci_choose_state(dev, state);
}

int pci_set_power_state(void *dev, int state)
{
    return kb_pci_set_power_state(dev, state);
}

int pci_set_mwi(void *dev)
{
    return kb_pci_set_mwi(dev);
}

const void *pci_match_id(const void *id_table, void *dev)
{
    return kb_pci_match_id(id_table, dev);
}

int pci_read_config_byte(void *dev, int where, uint8_t *value)
{
    return kb_pci_read_config_byte(dev, where, value);
}

int pci_read_config_word(void *dev, int where, uint16_t *value)
{
    return kb_pci_read_config_word(dev, where, value);
}

int pci_read_config_dword(void *dev, int where, uint32_t *value)
{
    return kb_pci_read_config_dword(dev, where, value);
}

int pci_write_config_byte(void *dev, int where, uint8_t value)
{
    return kb_pci_write_config_byte(dev, where, value);
}

int pci_write_config_word(void *dev, int where, uint16_t value)
{
    return kb_pci_write_config_word(dev, where, value);
}

int pci_write_config_dword(void *dev, int where, uint32_t value)
{
    return kb_pci_write_config_dword(dev, where, value);
}

int pci_find_capability(void *dev, int cap)
{
    return kb_pci_find_capability(dev, cap);
}

int pci_enable_msi(void *dev)
{
    return kb_pci_enable_msi(dev);
}

int pci_enable_msix_range(void *dev, void *entries, int minvec, int maxvec)
{
    return kb_pci_enable_msix_range(dev, entries, minvec, maxvec);
}

void pci_disable_msi(void *dev)
{
    kb_pci_disable_msi(dev);
}

void pci_disable_msix(void *dev)
{
    kb_pci_disable_msix(dev);
}

void *pci_get_class(unsigned int class, void *from)
{
    return kb_pci_get_class(class, from);
}

void *pci_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pci_iomap(dev, bar, max);
}

void *pcim_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pcim_iomap(dev, bar, max);
}

void pci_iounmap(void *dev, void *addr)
{
    kb_pci_iounmap(dev, addr);
}

void *ioremap(uint64_t phys_addr, size_t size)
{
    return kb_ioremap(phys_addr, size);
}

void iounmap(void *addr)
{
    kb_iounmap(addr);
}
