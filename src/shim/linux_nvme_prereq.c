#include "kobox/shim.h"
#include "subsystem/block.h"
#include "subsystem/dma.h"
#include "subsystem/nvme.h"
#include "subsystem/pci.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct shim_dma_pool {
    size_t size;
    size_t align;
    struct shim_dma_pool_alloc *allocs;
} shim_dma_pool_t;

typedef struct shim_dma_pool_alloc {
    void *vaddr;
    uint64_t dma_addr;
    struct shim_dma_pool_alloc *next;
} shim_dma_pool_alloc_t;

kb_backend_t *kb_shim_current_backend(void);

typedef struct shim_linux_hctx {
    void *tags;
    unsigned char reserved_008[0xc8 - 0x08];
    void *driver_data;
} shim_linux_hctx_t;

typedef struct shim_linux_queue_data {
    void *rq;
    uint8_t last;
    unsigned char reserved_009[0x100 - 0x09];
} shim_linux_queue_data_t;

enum {
    KB_NVME_DEV_CTRL_OFFSET = 0x1f0,
    KB_NVME_QUEUE_DEV_OFFSET = 0x00,
    KB_NVME_QUEUE_SQ_OFFSET = 0x10,
    KB_NVME_QUEUE_SQ_DB_OFFSET = 0x60,
    KB_NVME_QUEUE_DEPTH_OFFSET = 0x68,
    KB_NVME_QUEUE_SQ_TAIL_OFFSET = 0x6e,
    KB_NVME_QUEUE_CQ_HEAD_OFFSET = 0x72,
    KB_NVME_QUEUE_QID_OFFSET = 0x74,
    KB_NVME_QUEUE_PHASE_OFFSET = 0x76,
    KB_NVME_QUEUE_QES_OFFSET = 0x77,
    KB_NVME_QUEUE_CQ_OFFSET = 0x48,
    KB_NVME_DEV_DB_STRIDE_OFFSET = 0x198,
    KB_NVME_DEV_TAGSET_OFFSET = 0x08,

    KB_NVME_REG_CC = 0x14,
    KB_NVME_REG_CSTS = 0x1c,
    KB_NVME_REG_AQA = 0x24,
    KB_NVME_REG_ASQ = 0x28,
    KB_NVME_REG_ACQ = 0x30,
    KB_NVME_REG_DBS = 0x1000,

    KB_LINUX_BLK_MQ_OPS_QUEUE_RQ_OFFSET = 0x00,
    KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET = 0x00,
    KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET = 0x58,
    KB_LINUX_BLK_MQ_TAG_SET_TAGS_OFFSET = 0x60,
    KB_LINUX_6_8_GENDISK_PART0_OFFSET = 0x40,
    KB_LINUX_6_8_GENDISK_QUEUE_OFFSET = 0x50,

    KB_LINUX_REQUEST_HCTX_OFFSET = 0x00,
    KB_LINUX_REQUEST_QUEUE_OFFSET = 0x10,
    KB_LINUX_REQUEST_CMD_FLAGS_OFFSET = 0x18,
    KB_LINUX_REQUEST_TAG_OFFSET = 0x20,
    KB_LINUX_REQUEST_SPECIAL_OFFSET = 0x110,
    KB_LINUX_REQUEST_RESULT_OFFSET = 0x118,
    KB_LINUX_REQUEST_STATUS_OFFSET = 0x124,
    KB_LINUX_REQUEST_CTRL_OFFSET = 0x130,
    KB_LINUX_REQUEST_NVME_CMD_OFFSET = 0x138,

    KB_SHIM_REQUEST_SIZE = 4096,
    KB_SHIM_REQUEST_HCTX_OFFSET = 0x800,
    KB_SHIM_REQUEST_QUEUE_DATA_OFFSET = 0x900,
    KB_SHIM_REQUEST_DMA_ADDR_OFFSET = 0xa00,
    KB_SHIM_REQUEST_DMA_LEN_OFFSET = 0xa08,
    KB_SHIM_REQUEST_PRP_LIST_CPU_OFFSET = 0xa10,
    KB_SHIM_REQUEST_PRP_LIST_DMA_OFFSET = 0xa18,
    KB_SHIM_REQUEST_PRP_LIST_LEN_OFFSET = 0xa20,
    KB_SHIM_REQUEST_OWNS_QUEUE_OFFSET = 0xa24,

    KB_NVME_PAGE_SIZE = 4096,
    KB_NVME_CMD_READ = 0x02,
    KB_NVME_CMD_WRITE = 0x01,
    KB_NVME_ADMIN_CREATE_SQ = 0x01,
    KB_NVME_ADMIN_CREATE_CQ = 0x05,
    KB_NVME_ADMIN_DBBUF_CONFIG = 0x7c,
    KB_NVME_NSID_FIRST = 1,
    KB_NVME_LBA_SIZE = 512,
    KB_NVME_REQ_OP_DRV_IN = 0x22,
    KB_NVME_REQ_OP_DRV_OUT = 0x23,
    KB_NVME_ADMIN_QUEUE_DEPTH = 64,
    KB_NVME_IO_QUEUE_DEPTH = 64,
};

typedef struct shim_linux_request {
    void *hctx;
    unsigned char reserved_008[0x10 - 0x08];
    void *queue;
    uint32_t cmd_flags;
    unsigned char reserved_01c[0x20 - 0x1c];
    uint32_t tag;
    unsigned char reserved_024[0x48 - 0x24];
    void *batch_next;
    unsigned char reserved_050[0x110 - 0x50];
    void *special;
    uint64_t result;
    unsigned char reserved_120[0x124 - 0x120];
    uint16_t status;
    unsigned char reserved_126[0x130 - 0x126];
    void *ctrl;
    unsigned char nvme_cmd[64];
    unsigned char reserved_178[0x800 - 0x178];
    shim_linux_hctx_t hctx_storage;
    unsigned char reserved_8d0[0x900 - 0x8d0];
    shim_linux_queue_data_t queue_data;
    uint64_t dma_addr;
    uint32_t dma_len;
    unsigned char reserved_a0c[0xa10 - 0xa0c];
    void *prp_list_cpu;
    uint64_t prp_list_dma;
    uint32_t prp_list_len;
    uint32_t owns_queue;
    uint32_t completed;
    uint32_t end_status;
    unsigned char reserved_a30[KB_SHIM_REQUEST_SIZE - 0xa30];
} shim_linux_request_t;

_Static_assert(offsetof(shim_linux_request_t, hctx) == KB_LINUX_REQUEST_HCTX_OFFSET, "request.hctx offset");
_Static_assert(offsetof(shim_linux_request_t, queue) == KB_LINUX_REQUEST_QUEUE_OFFSET, "request.queue offset");
_Static_assert(offsetof(shim_linux_request_t, cmd_flags) == KB_LINUX_REQUEST_CMD_FLAGS_OFFSET, "request.cmd_flags offset");
_Static_assert(offsetof(shim_linux_request_t, tag) == KB_LINUX_REQUEST_TAG_OFFSET, "request.tag offset");
_Static_assert(offsetof(shim_linux_request_t, batch_next) == 0x48, "request.batch_next offset");
_Static_assert(offsetof(shim_linux_request_t, special) == KB_LINUX_REQUEST_SPECIAL_OFFSET, "request.special offset");
_Static_assert(offsetof(shim_linux_request_t, result) == KB_LINUX_REQUEST_RESULT_OFFSET, "request.result offset");
_Static_assert(offsetof(shim_linux_request_t, status) == KB_LINUX_REQUEST_STATUS_OFFSET, "request.status offset");
_Static_assert(offsetof(shim_linux_request_t, ctrl) == KB_LINUX_REQUEST_CTRL_OFFSET, "request.ctrl offset");
_Static_assert(offsetof(shim_linux_request_t, nvme_cmd) == KB_LINUX_REQUEST_NVME_CMD_OFFSET, "request.nvme_cmd offset");
_Static_assert(offsetof(shim_linux_request_t, hctx_storage) == KB_SHIM_REQUEST_HCTX_OFFSET, "shim hctx offset");
_Static_assert(offsetof(shim_linux_request_t, queue_data) == KB_SHIM_REQUEST_QUEUE_DATA_OFFSET, "shim queue_data offset");
_Static_assert(offsetof(shim_linux_request_t, dma_addr) == KB_SHIM_REQUEST_DMA_ADDR_OFFSET, "shim dma_addr offset");
_Static_assert(offsetof(shim_linux_request_t, dma_len) == KB_SHIM_REQUEST_DMA_LEN_OFFSET, "shim dma_len offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_cpu) == KB_SHIM_REQUEST_PRP_LIST_CPU_OFFSET, "shim prp_list_cpu offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_dma) == KB_SHIM_REQUEST_PRP_LIST_DMA_OFFSET, "shim prp_list_dma offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_len) == KB_SHIM_REQUEST_PRP_LIST_LEN_OFFSET, "shim prp_list_len offset");
_Static_assert(offsetof(shim_linux_request_t, owns_queue) == KB_SHIM_REQUEST_OWNS_QUEUE_OFFSET, "shim owns_queue offset");
_Static_assert(sizeof(shim_linux_request_t) == KB_SHIM_REQUEST_SIZE, "shim request size");
_Static_assert(offsetof(shim_linux_hctx_t, driver_data) == 0xc8, "hctx.driver_data offset");
_Static_assert(offsetof(shim_linux_queue_data_t, rq) == 0x00, "queue_data.rq offset");
_Static_assert(offsetof(shim_linux_queue_data_t, last) == 0x08, "queue_data.last offset");

typedef struct nvme_io_smoke_case {
    uint64_t lba;
    unsigned int blocks;
    unsigned int pattern;
} nvme_io_smoke_case_t;

static void *tracked_admin_tag_set;
static unsigned char *tracked_admin_nvmeq;
static unsigned char *tracked_io_nvmeq;
static unsigned int tracked_io_irq_waits;

static uint16_t read_u16(const void *ptr)
{
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint64_t read_u64(const void *ptr)
{
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void write_u16(void *ptr, uint16_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static void write_u32(void *ptr, uint32_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static void write_u64(void *ptr, uint64_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static void write_ptr(void *ptr, void *value)
{
    memcpy(ptr, &value, sizeof(value));
}

static uint32_t mmio_read32(const void *base, size_t offset)
{
    volatile const uint32_t *reg = (volatile const uint32_t *)((const unsigned char *)base + offset);
    return *reg;
}

static void mmio_write32(void *base, size_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    *reg = value;
}

static void mmio_write64(void *base, size_t offset, uint64_t value)
{
    volatile uint64_t *reg = (volatile uint64_t *)((unsigned char *)base + offset);
    *reg = value;
}

static void *read_ptr(const void *ptr)
{
    uintptr_t value;
    memcpy(&value, ptr, sizeof(value));
    return (void *)value;
}

static int trace_work_enabled(void);

static int trace_nvme_enabled(void)
{
    return getenv("KOBOX_TRACE_NVME") != NULL;
}

static int trace_pci_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_PCI");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void fill_io_pattern(unsigned char *buffer, unsigned int length, uint64_t lba, unsigned int blocks, unsigned int pattern)
{
    for (unsigned int i = 0; i < length; i++) {
        uint64_t value = ((uint64_t)i * 37u) ^ (lba * 131u) ^ ((uint64_t)blocks * 17u) ^ (uint64_t)pattern;
        buffer[i] = (unsigned char)(value & 0xffu);
    }
}

static size_t shim_nvme_queue_index(const unsigned char *nvmeq)
{
    if (nvmeq == NULL) {
        return 0;
    }
    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    return qid == 0 ? 0u : (size_t)qid - 1u;
}

static int shim_blk_tagset_prepare(void *tag_set)
{
    void *tag_array = kb_block_subsystem_tagset_array(tag_set);
    if (tag_array == NULL) {
        return -12;
    }
    write_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_TAGS_OFFSET, tag_array);
    return 0;
}

static uint32_t shim_blk_tagset_alloc_tag(void *tag_set, unsigned char *nvmeq)
{
    if (tag_set == NULL || nvmeq == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return 1;
    }
    return kb_block_subsystem_tagset_alloc_tag(tag_set, shim_nvme_queue_index(nvmeq));
}

static void shim_blk_request_init(
    shim_linux_request_t *request,
    void *queue,
    void *ctrl,
    void *nvmeq,
    unsigned int op,
    int owns_queue)
{
    void *tag_set = kb_block_subsystem_queue_tag_set(queue);
    memset(request, 0, sizeof(*request));
    request->hctx = &request->hctx_storage;
    request->queue = queue;
    request->cmd_flags = op;
    request->tag = shim_blk_tagset_alloc_tag(tag_set, nvmeq);
    request->special = request->nvme_cmd;
    request->ctrl = ctrl;
    request->owns_queue = owns_queue ? 1u : 0u;
    request->hctx_storage.driver_data = nvmeq;
}

static shim_linux_request_t *shim_blk_request_alloc(
    void *queue,
    void *ctrl,
    void *nvmeq,
    unsigned int op,
    int owns_queue)
{
    if (queue == NULL || kb_block_subsystem_queue_tag_set(queue) == NULL || ctrl == NULL || nvmeq == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return NULL;
    }

    shim_blk_request_init(request, queue, ctrl, nvmeq, op, owns_queue);
    return request;
}

static void shim_blk_tagset_bind_request(shim_linux_request_t *request, void *tag_set, unsigned char *nvmeq)
{
    if (request == NULL || tag_set == NULL || nvmeq == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return;
    }

    size_t queue_index = shim_nvme_queue_index(nvmeq);
    if (kb_block_subsystem_tagset_bind_request(tag_set, queue_index, request->tag, request) != 0) {
        return;
    }
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: bind tagset=%p queue=%zu tag=%u request=%p\n",
            tag_set,
            queue_index,
            request->tag,
            (void *)request);
    }
}

static void shim_blk_tagset_unbind_request(shim_linux_request_t *request)
{
    if (request == NULL || request->queue == NULL) {
        return;
    }

    shim_linux_hctx_t *hctx = request->hctx;
    void *tag_set = kb_block_subsystem_queue_tag_set(request->queue);
    if (tag_set == NULL || hctx == NULL || hctx->driver_data == NULL) {
        return;
    }

    kb_block_subsystem_tagset_unbind_request(
        tag_set,
        shim_nvme_queue_index(hctx->driver_data),
        request->tag,
        request);
}

static void track_nvme_queue(void *nvmeq_raw)
{
    unsigned char *nvmeq = nvmeq_raw;
    if (nvmeq == NULL) {
        return;
    }

    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    if (qid == 0) {
        tracked_admin_nvmeq = nvmeq;
    } else {
        tracked_io_nvmeq = nvmeq;
    }
    if (trace_nvme_enabled()) {
        fprintf(stderr, "kobox nvme: track queue qid=%u nvmeq=%p\n", (unsigned)qid, (void *)nvmeq);
    }
}

void *kb_kmalloc_node(size_t size, unsigned int flags, int node)
{
    (void)node;
    return kb_kmalloc(size, flags);
}

void *kb_kmalloc_node_trace(void *cache, unsigned int flags, int node, size_t size)
{
    (void)node;
    return kb_kmalloc_trace(cache, flags, size);
}

void *kb_kmemdup(const void *src, size_t len, unsigned int flags)
{
    if (src == NULL) {
        return NULL;
    }
    void *dst = kb_kmalloc(len, flags);
    if (dst != NULL) {
        memcpy(dst, src, len);
    }
    return dst;
}

void kb_kfree_sensitive(const void *ptr)
{
    kb_kfree((void *)ptr);
}

void *kb_dma_pool_create(const char *name, void *dev, size_t size, size_t align, size_t allocation)
{
    (void)name;
    (void)dev;
    (void)allocation;
    if (size == 0) {
        return NULL;
    }
    shim_dma_pool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->size = size;
    pool->align = align == 0 ? 4096 : align;
    return pool;
}

void *kb_dma_pool_alloc(void *pool, unsigned int flags, uint64_t *dma_handle)
{
    (void)flags;
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL || dma_handle == NULL) {
        return NULL;
    }
    void *vaddr = kb_dma_alloc_attrs(NULL, dma_pool->size, dma_handle, 0, 0);
    if (vaddr == NULL) {
        return NULL;
    }

    shim_dma_pool_alloc_t *alloc = calloc(1, sizeof(*alloc));
    if (alloc == NULL) {
        kb_dma_free_attrs(NULL, dma_pool->size, vaddr, *dma_handle, 0);
        return NULL;
    }
    alloc->vaddr = vaddr;
    alloc->dma_addr = *dma_handle;
    alloc->next = dma_pool->allocs;
    dma_pool->allocs = alloc;
    return vaddr;
}

void kb_dma_pool_free(void *pool, void *vaddr, uint64_t dma_addr)
{
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL || vaddr == NULL) {
        return;
    }
    shim_dma_pool_alloc_t **cursor = &dma_pool->allocs;
    while (*cursor != NULL) {
        shim_dma_pool_alloc_t *alloc = *cursor;
        if (alloc->vaddr == vaddr && alloc->dma_addr == dma_addr) {
            *cursor = alloc->next;
            free(alloc);
            break;
        }
        cursor = &alloc->next;
    }
    kb_dma_free_attrs(NULL, dma_pool->size, vaddr, dma_addr, 0);
}

void kb_dma_pool_destroy(void *pool)
{
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL) {
        return;
    }
    while (dma_pool->allocs != NULL) {
        shim_dma_pool_alloc_t *alloc = dma_pool->allocs;
        dma_pool->allocs = alloc->next;
        kb_dma_free_attrs(NULL, dma_pool->size, alloc->vaddr, alloc->dma_addr, 0);
        free(alloc);
    }
    free(dma_pool);
}

int kb_dma_set_mask(void *dev, uint64_t mask)
{
    (void)dev;
    (void)mask;
    return 0;
}

int kb_dma_set_coherent_mask(void *dev, uint64_t mask)
{
    (void)dev;
    (void)mask;
    return 0;
}

int kb_pci_alloc_irq_vectors(void *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags)
{
    enum {
        KB_PCI_IRQ_LEGACY = 1u << 0,
        KB_PCI_IRQ_MSI = 1u << 1,
        KB_PCI_IRQ_MSIX = 1u << 2,
        KB_PCI_CAP_ID_MSI = 0x05,
        KB_PCI_CAP_ID_MSIX = 0x11,
        KB_PCI_MSI_CONTROL_MMC_MASK = 0x000e,
        KB_PCI_MSI_CONTROL_MME_MASK = 0x0070,
        KB_PCI_MSI_CONTROL_ENABLE = 0x0001,
        KB_PCI_MSIX_CONTROL_TABLE_SIZE_MASK = 0x07ff,
        KB_PCI_MSIX_CONTROL_MASKALL = 0x4000,
        KB_PCI_MSIX_CONTROL_ENABLE = 0x8000,
        KB_IRQ_BACKEND_KIND_MSI = 1,
        KB_IRQ_BACKEND_KIND_MSIX = 2,
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
    enum {
        KB_PCI_CAP_ID_MSI = 0x05,
        KB_PCI_CAP_ID_MSIX = 0x11,
        KB_PCI_MSI_CONTROL_ENABLE = 0x0001,
        KB_PCI_MSIX_CONTROL_ENABLE = 0x8000,
    };

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
    track_nvme_queue(dev_id);
    return kb_request_threaded_irq((unsigned int)irq, handler, thread_fn, 0, "kobox-pci", dev_id);
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

void kb_mutex_init(void *lock)
{
    (void)lock;
}

void kb_mutex_lock(void *lock)
{
    (void)lock;
}

void kb_mutex_unlock(void *lock)
{
    (void)lock;
}

int kb_mutex_trylock(void *lock)
{
    (void)lock;
    return 1;
}

void kb_complete(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 1;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete completion=%p done=%u\n", completion, done);
        }
    }
}

void kb_complete_all(void *completion)
{
    if (completion != NULL) {
        uint32_t done = UINT32_MAX / 2u;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete_all completion=%p done=%u\n", completion, done);
        }
    }
}

static int trace_work_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_WORK");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void kb_init_completion(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 0;
        memcpy(completion, &done, sizeof(done));
    }
}

void kb_init_waitqueue_head(void *wq_head)
{
    (void)wq_head;
}

void kb_init_swait_queue_head(void *wq_head)
{
    (void)wq_head;
}

unsigned long kb_wait_for_completion(void *completion)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: wait_for_completion completion=%p\n", completion);
    }
    for (unsigned i = 0; i < 20; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(stderr, "kobox work: wait_for_completion done completion=%p remaining=%u\n", completion, done);
            }
            return 1;
        }
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: wait_for_completion timeout completion=%p\n", completion);
    }
    return 0;
}

unsigned long kb_wait_for_completion_io_timeout(void *completion, unsigned long timeout)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: wait_for_completion_timeout completion=%p timeout=%lu\n", completion, timeout);
    }
    const unsigned long loops = timeout == 0 ? 20 : (timeout > 100 ? 100 : timeout);
    for (unsigned long i = 0; i < loops; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(stderr,
                    "kobox work: wait_for_completion_timeout done completion=%p remaining=%u left=%lu\n",
                    completion,
                    done,
                    timeout == 0 ? 1 : timeout - i);
            }
            return timeout == 0 ? 1 : timeout - i;
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: wait_for_completion_timeout expired completion=%p timeout=%lu\n", completion, timeout);
    }
    return 0;
}

void kb_trace_noop(void)
{
}

int kb_return_zero(void)
{
    return 0;
}

int kb_return_one(void)
{
    return 1;
}

void *kb_alloc_stub(void)
{
    return calloc(1, 4096);
}

void *kb_identity_ptr(void *ptr)
{
    return ptr;
}

const char *kb_empty_string(void)
{
    return "";
}

static void *shim_blk_alloc_disk_with_queue(void *queue)
{
    void *disk = kb_block_subsystem_disk_alloc();
    if (disk == NULL) {
        return NULL;
    }
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (part0 == NULL) {
        kb_block_subsystem_object_free(disk);
        return NULL;
    }

    write_ptr((unsigned char *)disk + KB_LINUX_6_8_GENDISK_PART0_OFFSET, part0);
    write_ptr((unsigned char *)disk + KB_LINUX_6_8_GENDISK_QUEUE_OFFSET, queue);
    return disk;
}

void *kb_blk_alloc_disk(int node, void *lock_class_key)
{
    (void)node;
    (void)lock_class_key;
    void *queue = kb_block_subsystem_queue_alloc(NULL);
    if (queue == NULL) {
        return NULL;
    }
    void *disk = shim_blk_alloc_disk_with_queue(queue);
    if (disk == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    return disk;
}

void *kb_blk_mq_alloc_disk(void *tag_set, void *queuedata, void *lock_class_key)
{
    (void)queuedata;
    (void)lock_class_key;
    if (tag_set == NULL) {
        return NULL;
    }

    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }
    void *disk = shim_blk_alloc_disk_with_queue(queue);
    if (disk == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    tracked_admin_tag_set = tag_set;
    if (trace_nvme_enabled()) {
        fprintf(stderr, "kobox nvme: blk_mq_alloc_disk tag_set=%p disk=%p queue=%p\n", tag_set, disk, queue);
    }
    return disk;
}

void *kb_blk_mq_init_queue(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }
    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }
    tracked_admin_tag_set = tag_set;
    if (trace_nvme_enabled()) {
        fprintf(stderr, "kobox nvme: blk_mq_init_queue tag_set=%p queue=%p\n", tag_set, (void *)queue);
    }
    return queue;
}

void *kb_blk_mq_alloc_request(void *queue, unsigned int op, unsigned int flags)
{
    (void)flags;
    if (queue == NULL) {
        return NULL;
    }

    unsigned char *tag_set = kb_block_subsystem_queue_tag_set(queue);
    if (tag_set == NULL) {
        return NULL;
    }

    unsigned char *ctrl = read_ptr(tag_set + KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET);
    if (ctrl == NULL) {
        if (trace_nvme_enabled()) {
            fprintf(stderr, "kobox nvme: blk_mq_alloc_request missing ctrl tag_set=%p\n", (void *)tag_set);
        }
        return NULL;
    }
    unsigned char *dev = ctrl - KB_NVME_DEV_CTRL_OFFSET;
    unsigned char *nvmeq = read_ptr(dev + KB_NVME_QUEUE_DEV_OFFSET);
    if (nvmeq == NULL) {
        if (trace_nvme_enabled()) {
            fprintf(
                stderr,
                "kobox nvme: blk_mq_alloc_request missing nvmeq ctrl=%p dev=%p\n",
                (void *)ctrl,
                (void *)dev);
        }
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, nvmeq, op, 0);
    if (request == NULL) {
        return NULL;
    }

    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: blk_mq_alloc_request queue=%p request=%p hctx=%p nvmeq=%p op=0x%x\n",
            queue,
            (void *)request,
            (void *)&request->hctx_storage,
            (void *)nvmeq,
            op);
    }
    return request;
}

int kb_blk_rq_map_kern(void *queue, void *request, void *buffer, unsigned int length, unsigned int gfp)
{
    (void)queue;
    (void)gfp;
    if (request == NULL || buffer == NULL || length == 0) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -19;
    }

    kb_status_t dma_status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        backend,
        device,
        buffer,
        length,
        KB_DMA_BIDIRECTIONAL,
        &dma_status);
    if (dma_status == KB_ERR_UNSUPPORTED) {
        return -95;
    }
    if (dma_status != KB_OK) {
        return -5;
    }

    shim_linux_request_t *rq = request;
    unsigned char *cmd = rq->special;
    if (cmd == NULL) {
        return -22;
    }
    size_t first_prp_len = KB_NVME_PAGE_SIZE - (size_t)(dma_addr & (KB_NVME_PAGE_SIZE - 1u));
    if (first_prp_len > length) {
        first_prp_len = length;
    }

    write_u64(cmd + 24, dma_addr);
    if (length > first_prp_len) {
        uint64_t second_prp = dma_addr + first_prp_len;
        size_t remaining = length - first_prp_len;
        if (remaining <= KB_NVME_PAGE_SIZE) {
            write_u64(cmd + 32, second_prp);
        } else {
            size_t prp_count = (remaining + KB_NVME_PAGE_SIZE - 1u) / KB_NVME_PAGE_SIZE;
            size_t list_len = prp_count * sizeof(uint64_t);
            if (list_len > KB_NVME_PAGE_SIZE) {
                kb_subsystem_dma_unmap(backend, device, dma_addr, length, KB_DMA_BIDIRECTIONAL);
                return -95;
            }

            uint64_t *prp_list = calloc(prp_count, sizeof(*prp_list));
            if (prp_list == NULL) {
                kb_subsystem_dma_unmap(backend, device, dma_addr, length, KB_DMA_BIDIRECTIONAL);
                return -12;
            }
            for (size_t i = 0; i < prp_count; i++) {
                prp_list[i] = second_prp + ((uint64_t)i * KB_NVME_PAGE_SIZE);
            }

            uint64_t prp_list_dma = 0;
            dma_status = KB_ERR_INVALID;
            prp_list_dma = kb_subsystem_dma_map(
                backend,
                device,
                prp_list,
                list_len,
                KB_DMA_BIDIRECTIONAL,
                &dma_status);
            if (dma_status != KB_OK) {
                free(prp_list);
                kb_subsystem_dma_unmap(backend, device, dma_addr, length, KB_DMA_BIDIRECTIONAL);
                return -5;
            }

            write_u64(cmd + 32, prp_list_dma);
            rq->prp_list_cpu = prp_list;
            rq->prp_list_dma = prp_list_dma;
            rq->prp_list_len = (uint32_t)list_len;
        }
    }
    rq->dma_addr = dma_addr;
    rq->dma_len = length;
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: blk_rq_map_kern request=%p buffer=%p len=%u dma=0x%llx prp2=0x%llx opcode=0x%x cdw10=0x%x\n",
            request,
            buffer,
            length,
            (unsigned long long)dma_addr,
            (unsigned long long)read_u64(cmd + 32),
            (unsigned)((unsigned char *)cmd)[0],
            (unsigned)read_u64((unsigned char *)cmd + 40));
    }
    return 0;
}

static void *nvme_smoke_alloc_request_for_queue(unsigned char *nvmeq, void *tag_set, unsigned int op)
{
    if (nvmeq == NULL || tag_set == NULL) {
        return NULL;
    }

    unsigned char *dev = read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    unsigned char *ctrl = dev == NULL ? NULL : dev + KB_NVME_DEV_CTRL_OFFSET;
    if (ctrl == NULL) {
        ctrl = read_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET);
    }
    if (ctrl == NULL) {
        return NULL;
    }

    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, nvmeq, op, 1);
    if (request == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    return request;
}

static void nvme_smoke_free_request(void *request)
{
    if (request == NULL) {
        return;
    }
    kb_blk_mq_free_request(request);
}

static int nvme_submit_rw(unsigned char *nvmeq, uint8_t opcode, uint64_t lba, unsigned int blocks, void *buffer, unsigned int length)
{
    unsigned int op = opcode == KB_NVME_CMD_WRITE ? KB_NVME_REQ_OP_DRV_OUT : KB_NVME_REQ_OP_DRV_IN;
    unsigned char *dev = read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    void *tag_set = dev == NULL ? tracked_admin_tag_set : dev + KB_NVME_DEV_TAGSET_OFFSET;
    void *request = nvme_smoke_alloc_request_for_queue(nvmeq, tag_set, op);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = ((shim_linux_request_t *)request)->special;
    memset(cmd, 0, 64);
    cmd[0] = opcode;
    write_u32(cmd + 4, KB_NVME_NSID_FIRST);
    write_u64(cmd + 40, lba);
    write_u32(cmd + 48, blocks - 1u);

    int result = kb_blk_rq_map_kern(NULL, request, buffer, length, 0);
    if (result == 0) {
        result = kb_blk_execute_rq(request, 0);
    }
    nvme_smoke_free_request(request);
    return result;
}

static int nvme_submit_admin_create_queue(unsigned char *adminq, uint8_t opcode, uint16_t qid, uint16_t depth, uint64_t prp1)
{
    void *request = nvme_smoke_alloc_request_for_queue(adminq, tracked_admin_tag_set, KB_NVME_REQ_OP_DRV_OUT);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = ((shim_linux_request_t *)request)->special;
    memset(cmd, 0, 64);
    cmd[0] = opcode;
    write_u64(cmd + 24, prp1);
    write_u32(cmd + 40, ((uint32_t)(depth - 1u) << 16) | qid);
    if (opcode == KB_NVME_ADMIN_CREATE_CQ) {
        unsigned int cq_vector = kb_pci_subsystem_irq_vector_count() > qid ? qid : 0u;
        write_u32(cmd + 44, (cq_vector << 16) | 0x00000003u);
    } else {
        write_u32(cmd + 44, ((uint32_t)qid << 16) | 0x00000001u);
    }

    int result = kb_blk_execute_rq(request, 0);
    nvme_smoke_free_request(request);
    return result;
}

static int nvme_submit_admin_dbbuf_config(unsigned char *adminq)
{
    if (!kb_nvme_subsystem_dbbuf_ready()) {
        return 0;
    }

    void *request = nvme_smoke_alloc_request_for_queue(adminq, tracked_admin_tag_set, KB_NVME_REQ_OP_DRV_OUT);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = ((shim_linux_request_t *)request)->special;
    memset(cmd, 0, 64);
    cmd[0] = KB_NVME_ADMIN_DBBUF_CONFIG;
    write_u64(cmd + 24, kb_nvme_subsystem_dbbuf_dbs_dma());
    write_u64(cmd + 32, kb_nvme_subsystem_dbbuf_eis_dma());

    int result = kb_blk_execute_rq(request, 0);
    nvme_smoke_free_request(request);
    return result;
}

static unsigned char *nvme_bar_from_queue(unsigned char *nvmeq)
{
    volatile uint32_t *sq_tail_db = read_ptr(nvmeq + KB_NVME_QUEUE_SQ_DB_OFFSET);
    if (sq_tail_db == NULL) {
        return NULL;
    }

    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    unsigned char *dev = read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    uint32_t db_stride = 0;
    if (dev != NULL) {
        memcpy(&db_stride, dev + KB_NVME_DEV_DB_STRIDE_OFFSET, sizeof(db_stride));
    }
    return (unsigned char *)sq_tail_db - KB_NVME_REG_DBS - ((size_t)qid * 2u * (size_t)db_stride * sizeof(uint32_t));
}

static void nvme_reset_queue_state(unsigned char *nvmeq)
{
    if (nvmeq == NULL) {
        return;
    }

    unsigned char *sq = read_ptr(nvmeq + KB_NVME_QUEUE_SQ_OFFSET);
    unsigned char *cq = read_ptr(nvmeq + KB_NVME_QUEUE_CQ_OFFSET);
    uint16_t depth = read_u16(nvmeq + KB_NVME_QUEUE_DEPTH_OFFSET);
    if (depth == 0) {
        depth = KB_NVME_IO_QUEUE_DEPTH;
    }
    if (sq != NULL) {
        memset(sq, 0, (size_t)depth * 64u);
    }
    if (cq != NULL) {
        memset(cq, 0, (size_t)depth * 16u);
    }
    write_u16(nvmeq + KB_NVME_QUEUE_SQ_TAIL_OFFSET, 0);
    write_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET, 0);
    *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET) = 1;
}

static void nvme_reset_dbbuf_shadow(void)
{
    kb_nvme_subsystem_reset_dbbuf_shadow();
}

static int nvme_wait_ready(unsigned char *bar, int ready)
{
    for (unsigned int i = 0; i < 10000000u; i++) {
        uint32_t csts = mmio_read32(bar, KB_NVME_REG_CSTS);
        if (((csts & 1u) != 0) == ready) {
            return 0;
        }
    }
    return -110;
}

static int nvme_dma_map_existing(void *cpu_addr, uint64_t size, uint64_t *out_dma)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -19;
    }
    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(
        backend,
        device,
        cpu_addr,
        (size_t)size,
        KB_DMA_BIDIRECTIONAL,
        &status);
    if (status == KB_ERR_UNSUPPORTED) {
        return -95;
    }
    if (status != KB_OK) {
        return -5;
    }
    *out_dma = dma_addr;
    return 0;
}

static void nvme_dma_unmap_existing(uint64_t dma_addr, uint64_t size)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_subsystem_dma_unmap(
        backend,
        kb_subsystem_dma_default_device(backend),
        dma_addr,
        (size_t)size,
        KB_DMA_BIDIRECTIONAL);
}

static int nvme_reset_controller_and_recreate_io_queue(void)
{
    if (tracked_admin_nvmeq == NULL || tracked_io_nvmeq == NULL) {
        return -19;
    }

    unsigned char *bar = nvme_bar_from_queue(tracked_admin_nvmeq);
    if (bar == NULL) {
        return -19;
    }

    uint32_t cc = mmio_read32(bar, KB_NVME_REG_CC);
    if ((cc & 1u) != 0) {
        mmio_write32(bar, KB_NVME_REG_CC, cc & ~1u);
        int result = nvme_wait_ready(bar, 0);
        if (result != 0) {
            return result;
        }
    }

    nvme_reset_queue_state(tracked_admin_nvmeq);
    nvme_reset_queue_state(tracked_io_nvmeq);
    nvme_reset_dbbuf_shadow();

    unsigned char *admin_sq = read_ptr(tracked_admin_nvmeq + KB_NVME_QUEUE_SQ_OFFSET);
    unsigned char *admin_cq = read_ptr(tracked_admin_nvmeq + KB_NVME_QUEUE_CQ_OFFSET);
    unsigned char *io_sq = read_ptr(tracked_io_nvmeq + KB_NVME_QUEUE_SQ_OFFSET);
    unsigned char *io_cq = read_ptr(tracked_io_nvmeq + KB_NVME_QUEUE_CQ_OFFSET);
    if (admin_sq == NULL || admin_cq == NULL || io_sq == NULL || io_cq == NULL) {
        return -19;
    }

    uint64_t admin_sq_dma = 0;
    uint64_t admin_cq_dma = 0;
    uint64_t io_sq_dma = 0;
    uint64_t io_cq_dma = 0;
    int result = nvme_dma_map_existing(admin_sq, KB_NVME_ADMIN_QUEUE_DEPTH * 64u, &admin_sq_dma);
    if (result != 0) {
        return result;
    }
    result = nvme_dma_map_existing(admin_cq, KB_NVME_ADMIN_QUEUE_DEPTH * 16u, &admin_cq_dma);
    if (result != 0) {
        nvme_dma_unmap_existing(admin_sq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 64u);
        return result;
    }
    result = nvme_dma_map_existing(io_sq, KB_NVME_IO_QUEUE_DEPTH * 64u, &io_sq_dma);
    if (result != 0) {
        nvme_dma_unmap_existing(admin_cq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 16u);
        nvme_dma_unmap_existing(admin_sq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 64u);
        return result;
    }
    result = nvme_dma_map_existing(io_cq, KB_NVME_IO_QUEUE_DEPTH * 16u, &io_cq_dma);
    if (result != 0) {
        nvme_dma_unmap_existing(io_sq_dma, KB_NVME_IO_QUEUE_DEPTH * 64u);
        nvme_dma_unmap_existing(admin_cq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 16u);
        nvme_dma_unmap_existing(admin_sq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 64u);
        return result;
    }

    mmio_write32(
        bar,
        KB_NVME_REG_AQA,
        ((uint32_t)(KB_NVME_ADMIN_QUEUE_DEPTH - 1u) << 16) | (KB_NVME_ADMIN_QUEUE_DEPTH - 1u));
    mmio_write64(bar, KB_NVME_REG_ASQ, admin_sq_dma);
    mmio_write64(bar, KB_NVME_REG_ACQ, admin_cq_dma);
    cc = (cc & ~1u) | 1u;
    mmio_write32(bar, KB_NVME_REG_CC, cc);
    result = nvme_wait_ready(bar, 1);
    if (result == 0) {
        result = nvme_submit_admin_create_queue(
            tracked_admin_nvmeq,
            KB_NVME_ADMIN_CREATE_CQ,
            read_u16(tracked_io_nvmeq + KB_NVME_QUEUE_QID_OFFSET),
            KB_NVME_IO_QUEUE_DEPTH,
            io_cq_dma);
    }
    if (result == 0) {
        result = nvme_submit_admin_create_queue(
            tracked_admin_nvmeq,
            KB_NVME_ADMIN_CREATE_SQ,
            read_u16(tracked_io_nvmeq + KB_NVME_QUEUE_QID_OFFSET),
            KB_NVME_IO_QUEUE_DEPTH,
            io_sq_dma);
    }
    if (result == 0) {
        result = nvme_submit_admin_dbbuf_config(tracked_admin_nvmeq);
    }

    if (trace_nvme_enabled()) {
        fprintf(stderr, "kobox nvme: controller reset/recreate io queue result=%d\n", result);
    }
    return result;
}

int kb_nvme_io_smoke(void)
{
    if (tracked_io_nvmeq == NULL) {
        fprintf(stderr, "kobox nvme io smoke: no IO queue captured\n");
        return -19;
    }

    const nvme_io_smoke_case_t cases[] = {
        {17, 1, 0x5a},
        {109, 2, 0xa5},
        {257, 4, 0x3c},
    };
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);
    unsigned int max_length = 0;
    for (size_t i = 0; i < case_count; i++) {
        unsigned int length = cases[i].blocks * KB_NVME_LBA_SIZE;
        if (length > max_length) {
            max_length = length;
        }
    }

    unsigned char *write_buffer = malloc(max_length);
    unsigned char *read_buffer = malloc(max_length);
    if (write_buffer == NULL || read_buffer == NULL) {
        free(write_buffer);
        free(read_buffer);
        return -12;
    }

    unsigned int irq_waits_before = tracked_io_irq_waits;
    for (size_t i = 0; i < case_count; i++) {
        unsigned int length = cases[i].blocks * KB_NVME_LBA_SIZE;
        fill_io_pattern(write_buffer, length, cases[i].lba, cases[i].blocks, cases[i].pattern);
        int result = nvme_submit_rw(
            tracked_io_nvmeq,
            KB_NVME_CMD_WRITE,
            cases[i].lba,
            cases[i].blocks,
            write_buffer,
            length);
        if (result != 0) {
            fprintf(stderr, "kobox nvme io smoke: write failed lba=%llu blocks=%u status=%d\n",
                (unsigned long long)cases[i].lba, cases[i].blocks, result);
            free(write_buffer);
            free(read_buffer);
            return result;
        }
    }

    int result = nvme_reset_controller_and_recreate_io_queue();
    if (result != 0) {
        fprintf(stderr, "kobox nvme io smoke: reset failed %d\n", result);
        free(write_buffer);
        free(read_buffer);
        return result;
    }

    for (size_t i = 0; i < case_count; i++) {
        unsigned int length = cases[i].blocks * KB_NVME_LBA_SIZE;
        memset(read_buffer, 0, length);
        fill_io_pattern(write_buffer, length, cases[i].lba, cases[i].blocks, cases[i].pattern);
        result = nvme_submit_rw(
            tracked_io_nvmeq,
            KB_NVME_CMD_READ,
            cases[i].lba,
            cases[i].blocks,
            read_buffer,
            length);
        if (result != 0) {
            fprintf(stderr, "kobox nvme io smoke: read failed lba=%llu blocks=%u status=%d\n",
                (unsigned long long)cases[i].lba, cases[i].blocks, result);
            free(write_buffer);
            free(read_buffer);
            return result;
        }
        if (memcmp(write_buffer, read_buffer, length) != 0) {
            fprintf(stderr, "kobox nvme io smoke: compare failed lba=%llu blocks=%u\n",
                (unsigned long long)cases[i].lba, cases[i].blocks);
            free(write_buffer);
            free(read_buffer);
            return -5;
        }
        printf(
            "kobox nvme io smoke: read-after-reset lba=%llu blocks=%u bytes=%u compare=ok\n",
            (unsigned long long)cases[i].lba,
            cases[i].blocks,
            length);
    }

    unsigned int irq_waits = tracked_io_irq_waits - irq_waits_before;
    free(write_buffer);
    free(read_buffer);
    if (irq_waits < case_count * 2u) {
        fprintf(stderr, "kobox nvme io smoke: MSI-X waits missing got=%u expected=%u\n", irq_waits, (unsigned)(case_count * 2u));
        return -5;
    }

    printf(
        "kobox nvme io smoke: cases=%u reset=ok msix-waits=%u block-requests=ok\n",
        (unsigned)case_count,
        irq_waits);
    return 0;
}

static void shim_blk_mark_request_complete(void *request, unsigned int status)
{
    if (request == NULL) {
        return;
    }

    shim_linux_request_t *rq = request;
    rq->completed = 1;
    rq->end_status = status;
    if (trace_nvme_enabled()) {
        fprintf(stderr, "kobox blk-mq: request complete request=%p status=0x%x\n", request, status);
    }
}

void kb_blk_mq_complete_request(void *request)
{
    shim_blk_mark_request_complete(request, 0);
}

int kb_blk_mq_complete_request_remote(void *request)
{
    shim_blk_mark_request_complete(request, 0);
    return 1;
}

void kb_blk_mq_end_request(void *request, unsigned int status)
{
    shim_blk_mark_request_complete(request, status);
}

void kb_blk_mq_end_request_batch(void *batch)
{
    if (batch == NULL) {
        return;
    }

    shim_linux_request_t *rq = read_ptr(batch);
    while (rq != NULL) {
        shim_linux_request_t *next = rq->batch_next;
        rq->batch_next = NULL;
        shim_blk_mark_request_complete(rq, rq->status);
        rq = next;
    }
}

void kb_blk_mq_free_request(void *request)
{
    if (request == NULL) {
        return;
    }

    shim_linux_request_t *rq = request;
    shim_blk_tagset_unbind_request(rq);
    uint64_t dma_addr = rq->dma_addr;
    uint32_t dma_len = rq->dma_len;
    void *prp_list = rq->prp_list_cpu;
    uint64_t prp_list_dma = rq->prp_list_dma;
    uint32_t prp_list_len = rq->prp_list_len;
    if (prp_list != NULL && prp_list_dma != 0 && prp_list_len != 0) {
        kb_backend_t *backend = kb_shim_current_backend();
        kb_subsystem_dma_unmap(
            backend,
            kb_subsystem_dma_default_device(backend),
            prp_list_dma,
            prp_list_len,
            KB_DMA_BIDIRECTIONAL);
        free(prp_list);
    }
    if (dma_addr != 0 && dma_len != 0) {
        kb_backend_t *backend = kb_shim_current_backend();
        kb_subsystem_dma_unmap(
            backend,
            kb_subsystem_dma_default_device(backend),
            dma_addr,
            dma_len,
            KB_DMA_BIDIRECTIONAL);
    }
    if (rq->owns_queue) {
        kb_block_subsystem_object_free(rq->queue);
    }
    free(request);
}

int kb_blk_execute_rq(void *request, int at_head)
{
    (void)at_head;
    if (request == NULL) {
        return -22;
    }

    shim_linux_request_t *rq = request;
    shim_linux_hctx_t *hctx = rq->hctx;
    unsigned char *tag_set = kb_block_subsystem_queue_tag_set(rq->queue);
    if (hctx == NULL || tag_set == NULL) {
        return -22;
    }

    unsigned char *ops = read_ptr(tag_set + KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET);
    if (ops == NULL) {
        return -22;
    }

    unsigned char *nvmeq = hctx->driver_data;
    if (nvmeq == NULL) {
        return -22;
    }

    int (*queue_rq)(void *hctx, void *bd) =
        (int (*)(void *, void *))read_ptr(ops + KB_LINUX_BLK_MQ_OPS_QUEUE_RQ_OFFSET);
    if (queue_rq == NULL) {
        return -22;
    }

    shim_linux_queue_data_t *bd = &rq->queue_data;
    bd->rq = rq;
    bd->last = 1;

    unsigned char *cmd = rq->special;
    if (cmd == NULL) {
        cmd = rq->nvme_cmd;
    }
    if (cmd[0] == KB_NVME_ADMIN_DBBUF_CONFIG) {
        kb_nvme_subsystem_track_dbbuf(read_u64(cmd + 24), read_u64(cmd + 32));
        if (trace_nvme_enabled()) {
            size_t dbs_size = 0;
            size_t eis_size = 0;
            void *dbs_cpu = kb_nvme_subsystem_dbbuf_dbs_cpu(&dbs_size);
            void *eis_cpu = kb_nvme_subsystem_dbbuf_eis_cpu(&eis_size);
            fprintf(
                stderr,
                "kobox nvme: track dbbuf dbs=0x%llx/%p+%zu eis=0x%llx/%p+%zu\n",
                (unsigned long long)kb_nvme_subsystem_dbbuf_dbs_dma(),
                dbs_cpu,
                dbs_size,
                (unsigned long long)kb_nvme_subsystem_dbbuf_eis_dma(),
                eis_cpu,
                eis_size);
        }
    }

    uint16_t sq_tail_before = read_u16(nvmeq + KB_NVME_QUEUE_SQ_TAIL_OFFSET);
    shim_blk_tagset_bind_request(rq, tag_set, nvmeq);
    int result = queue_rq(hctx, bd);
    if (trace_nvme_enabled()) {
        unsigned char *sq = read_ptr(nvmeq + KB_NVME_QUEUE_SQ_OFFSET);
        uint8_t qes = *(uint8_t *)(nvmeq + KB_NVME_QUEUE_QES_OFFSET);
        unsigned char *sqe = sq == NULL ? NULL : sq + (((size_t)sq_tail_before) << qes);
        fprintf(
            stderr,
            "kobox nvme: queue_rq request=%p hctx=%p bd=%p result=%d nvmeq=%p tail_before=%u tail_after=%u cmd0=0x%llx prp1=0x%llx prp2=0x%llx cdw10=0x%llx sqe0=0x%llx sqe_prp1=0x%llx sqe_prp2=0x%llx\n",
            request,
            (void *)hctx,
            (void *)bd,
            result,
            (void *)nvmeq,
            (unsigned)sq_tail_before,
            (unsigned)read_u16(nvmeq + KB_NVME_QUEUE_SQ_TAIL_OFFSET),
            (unsigned long long)read_u64(cmd),
            (unsigned long long)read_u64(cmd + 24),
            (unsigned long long)read_u64(cmd + 32),
            (unsigned long long)read_u64(cmd + 40),
            sqe == NULL ? 0ull : (unsigned long long)read_u64(sqe),
            sqe == NULL ? 0ull : (unsigned long long)read_u64(sqe + 24),
            sqe == NULL ? 0ull : (unsigned long long)read_u64(sqe + 32));
    }
    if (result != 0) {
        return result;
    }

    unsigned char *dev = read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    volatile unsigned char *cq = read_ptr(nvmeq + KB_NVME_QUEUE_CQ_OFFSET);
    volatile uint32_t *sq_tail_db = read_ptr(nvmeq + KB_NVME_QUEUE_SQ_DB_OFFSET);
    if (cq == NULL) {
        return -5;
    }
    volatile uint32_t *cq_head_db = NULL;
    if (dev != NULL && sq_tail_db != NULL) {
        uint32_t db_stride = 0;
        memcpy(&db_stride, dev + KB_NVME_DEV_DB_STRIDE_OFFSET, sizeof(db_stride));
        cq_head_db = sq_tail_db + db_stride;
    }

    uint16_t head = read_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET);
    uint16_t depth = read_u16(nvmeq + KB_NVME_QUEUE_DEPTH_OFFSET);
    uint8_t phase = *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET);
    if (depth == 0) {
        return -5;
    }

    volatile unsigned char *completion = cq + ((size_t)head * 16u);
    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    if (qid != 0) {
        if (trace_nvme_enabled()) {
            size_t queue_index = shim_nvme_queue_index(nvmeq);
            void *tag_request = kb_block_subsystem_tagset_request(tag_set, queue_index, rq->tag);
            uint16_t command_id = read_u16((const void *)(completion + 12));
            unsigned int command_gen = (unsigned int)((command_id >> 12) & 0xfu);
            unsigned int request_gen = (unsigned int)(rq->reserved_120[0] & 0xfu);
            fprintf(
                stderr,
                "kobox blk-mq: irq lookup qid=%u queue=%zu tagset=%p tag=%u cid=0x%x gen=%u/%u request=%p tag_request=%p\n",
                (unsigned)qid,
                queue_index,
                (void *)tag_set,
                rq->tag,
                command_id,
                command_gen,
                request_gen,
                (void *)rq,
                tag_request);
        }
        int irq_status = kb_handle_irq_for_dev_id(nvmeq, 1000000000ull);
        if (irq_status == 0) {
            tracked_io_irq_waits++;
            uint16_t irq_head = read_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET);
            uint8_t irq_phase = *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET);
            if (rq->completed || irq_head != head || irq_phase != phase) {
                if (trace_nvme_enabled()) {
                    fprintf(
                        stderr,
                        "kobox nvme: irq handler completed request=%p qid=%u head=%u->%u phase=%u->%u status=0x%x\n",
                        request,
                        (unsigned)qid,
                        (unsigned)head,
                        (unsigned)irq_head,
                        (unsigned)phase,
                        (unsigned)irq_phase,
                        rq->end_status);
                }
                return rq->end_status == 0 ? 0 : -5;
            }
        } else if (trace_nvme_enabled()) {
            fprintf(stderr, "kobox nvme: irq wait failed qid=%u status=%d\n", (unsigned)qid, irq_status);
        }
    }

    int completed = 0;
    for (unsigned i = 0; i < 10000000u; i++) {
        uint16_t status = read_u16((const void *)(completion + 14));
        if ((status & 1u) == phase) {
            completed = 1;
            break;
        }
    }
    if (!completed) {
        if (trace_nvme_enabled()) {
            fprintf(stderr, "kobox nvme: blk_execute_rq timeout request=%p head=%u phase=%u\n", request, head, phase);
        }
        return -110;
    }

    uint64_t result64 = read_u64((const void *)completion);
    uint16_t status = read_u16((const void *)(completion + 14));
    rq->result = result64;
    rq->status = (uint16_t)(status >> 1);

    head++;
    if (head == depth) {
        head = 0;
        phase ^= 1u;
        *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET) = phase;
    }
    write_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET, head);
    if (cq_head_db != NULL) {
        *cq_head_db = head;
    }
    shim_blk_mark_request_complete(rq, (unsigned)(status >> 1));
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: blk_execute_rq complete request=%p status=0x%x result=0x%llx next_head=%u\n",
            request,
            (unsigned)(status >> 1),
            (unsigned long long)result64,
            head);
    }
    return 0;
}

void *kb_hwmon_device_register_with_info(void *dev, const char *name, void *data, const void *chip, const void *groups)
{
    (void)dev;
    (void)name;
    (void)chip;
    (void)groups;
    unsigned char *hwmon = calloc(1, 256);
    if (hwmon == NULL) {
        return NULL;
    }
    memcpy(hwmon + 0x78, &data, sizeof(data));
    return hwmon;
}
