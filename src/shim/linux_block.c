#include "kobox/shim.h"
#include "shim/linux_block.h"
#include "shim/linux_nvme.h"
#include "subsystem/block/block.h"
#include "subsystem/dma/dma.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    KB_LINUX_BLOCK_MAX_DRIVER_OPS = 8,
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
    kb_dma_dir_t dma_dir;
    kb_dma_dir_t prp_list_dma_dir;
    const kb_linux_block_driver_ops_t *driver_ops;
    unsigned char reserved_a40[KB_SHIM_REQUEST_SIZE - 0xa40];
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
_Static_assert(offsetof(shim_linux_request_t, driver_ops) == 0xa38, "shim driver_ops offset");
_Static_assert(sizeof(shim_linux_request_t) == KB_SHIM_REQUEST_SIZE, "shim request size");
_Static_assert(offsetof(shim_linux_hctx_t, driver_data) == 0xc8, "hctx.driver_data offset");
_Static_assert(offsetof(shim_linux_queue_data_t, rq) == 0x00, "queue_data.rq offset");
_Static_assert(offsetof(shim_linux_queue_data_t, last) == 0x08, "queue_data.last offset");

static const kb_linux_block_driver_ops_t *registered_driver_ops[KB_LINUX_BLOCK_MAX_DRIVER_OPS];
static size_t registered_driver_ops_count;
static int builtin_drivers_registered;

static void *read_ptr(const void *ptr)
{
    uintptr_t value;
    memcpy(&value, ptr, sizeof(value));
    return (void *)value;
}

static void write_ptr(void *ptr, void *value)
{
    memcpy(ptr, &value, sizeof(value));
}

static int trace_block_enabled(void)
{
    return getenv("KOBOX_TRACE_BLOCK") != NULL || getenv("KOBOX_TRACE_NVME") != NULL;
}

void kb_linux_block_register_driver_ops(const kb_linux_block_driver_ops_t *ops)
{
    if (ops == NULL) {
        return;
    }
    for (size_t i = 0; i < registered_driver_ops_count; i++) {
        if (registered_driver_ops[i] == ops) {
            return;
        }
    }
    if (registered_driver_ops_count < KB_LINUX_BLOCK_MAX_DRIVER_OPS) {
        registered_driver_ops[registered_driver_ops_count++] = ops;
    }
}

static void ensure_builtin_driver_ops(void)
{
    if (builtin_drivers_registered) {
        return;
    }
    builtin_drivers_registered = 1;
    kb_nvme_shim_register_block_driver();
}

static const kb_linux_block_driver_ops_t *block_driver_ops_for_tag_set(void *tag_set)
{
    ensure_builtin_driver_ops();
    for (size_t i = 0; i < registered_driver_ops_count; i++) {
        const kb_linux_block_driver_ops_t *ops = registered_driver_ops[i];
        if (ops->match_tag_set == NULL || ops->match_tag_set(tag_set)) {
            return ops;
        }
    }
    return NULL;
}

void *kb_linux_block_tag_set_driver_data(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }
    return read_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET);
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

static size_t shim_blk_request_queue_index(const shim_linux_request_t *request)
{
    if (request == NULL || request->driver_ops == NULL || request->driver_ops->queue_index == NULL) {
        return 0;
    }
    return request->driver_ops->queue_index(request->hctx_storage.driver_data);
}

static uint32_t shim_blk_tagset_alloc_tag(
    void *tag_set,
    const kb_linux_block_driver_ops_t *driver_ops,
    void *driver_data)
{
    if (tag_set == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return 1;
    }
    size_t queue_index = 0;
    if (driver_ops != NULL && driver_ops->queue_index != NULL) {
        queue_index = driver_ops->queue_index(driver_data);
    }
    return kb_block_subsystem_tagset_alloc_tag(tag_set, queue_index);
}

static void shim_blk_request_init(
    shim_linux_request_t *request,
    void *queue,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue,
    const kb_linux_block_driver_ops_t *driver_ops)
{
    void *tag_set = kb_block_subsystem_queue_tag_set(queue);
    memset(request, 0, sizeof(*request));
    request->hctx = &request->hctx_storage;
    request->queue = queue;
    request->cmd_flags = op;
    request->tag = shim_blk_tagset_alloc_tag(tag_set, driver_ops, driver_data);
    request->special = request->nvme_cmd;
    request->ctrl = ctrl;
    request->owns_queue = owns_queue ? 1u : 0u;
    request->hctx_storage.driver_data = driver_data;
    request->driver_ops = driver_ops;
}

static shim_linux_request_t *shim_blk_request_alloc(
    void *queue,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue,
    const kb_linux_block_driver_ops_t *driver_ops)
{
    if (queue == NULL || kb_block_subsystem_queue_tag_set(queue) == NULL || ctrl == NULL || driver_data == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return NULL;
    }

    shim_blk_request_init(request, queue, ctrl, driver_data, op, owns_queue, driver_ops);
    return request;
}

void *kb_linux_block_alloc_driver_request(
    void *tag_set,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue)
{
    if (tag_set == NULL || ctrl == NULL || driver_data == NULL) {
        return NULL;
    }
    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops == NULL) {
        return NULL;
    }

    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, driver_data, op, owns_queue, driver_ops);
    if (request == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    return request;
}

static void shim_blk_tagset_bind_request(shim_linux_request_t *request, void *tag_set)
{
    if (request == NULL || tag_set == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return;
    }

    size_t queue_index = shim_blk_request_queue_index(request);
    if (kb_block_subsystem_tagset_bind_request(tag_set, queue_index, request->tag, request) != 0) {
        return;
    }
    if (trace_block_enabled()) {
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

    void *tag_set = kb_block_subsystem_queue_tag_set(request->queue);
    if (tag_set == NULL || request->hctx_storage.driver_data == NULL) {
        return;
    }

    kb_block_subsystem_tagset_unbind_request(
        tag_set,
        shim_blk_request_queue_index(request),
        request->tag,
        request);
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

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops != NULL && driver_ops->track_tag_set != NULL) {
        driver_ops->track_tag_set(tag_set);
    }
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: alloc_disk tag_set=%p disk=%p queue=%p\n", tag_set, disk, queue);
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

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops != NULL && driver_ops->track_tag_set != NULL) {
        driver_ops->track_tag_set(tag_set);
    }
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: init_queue tag_set=%p queue=%p\n", tag_set, (void *)queue);
    }
    return queue;
}

void *kb_blk_mq_alloc_request(void *queue, unsigned int op, unsigned int flags)
{
    (void)flags;
    if (queue == NULL) {
        return NULL;
    }

    void *tag_set = kb_block_subsystem_queue_tag_set(queue);
    if (tag_set == NULL) {
        return NULL;
    }

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops == NULL || driver_ops->request_driver_data == NULL || driver_ops->request_ctrl == NULL) {
        return NULL;
    }

    void *driver_data = driver_ops->request_driver_data(queue, tag_set);
    if (driver_data == NULL) {
        if (trace_block_enabled()) {
            fprintf(stderr, "kobox blk-mq: alloc_request missing driver_data tag_set=%p\n", tag_set);
        }
        return NULL;
    }

    void *ctrl = driver_ops->request_ctrl(tag_set, driver_data);
    if (ctrl == NULL) {
        if (trace_block_enabled()) {
            fprintf(stderr, "kobox blk-mq: alloc_request missing ctrl tag_set=%p driver_data=%p\n", tag_set, driver_data);
        }
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, driver_data, op, 0, driver_ops);
    if (request == NULL) {
        return NULL;
    }

    if (trace_block_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: alloc_request queue=%p request=%p hctx=%p driver_data=%p op=0x%x driver=%s\n",
            queue,
            (void *)request,
            (void *)&request->hctx_storage,
            driver_data,
            op,
            driver_ops->name == NULL ? "(unknown)" : driver_ops->name);
    }
    return request;
}

void *kb_linux_block_request_command(void *request)
{
    shim_linux_request_t *rq = request;
    return rq == NULL ? NULL : rq->special;
}

void *kb_linux_block_request_driver_data(void *request)
{
    shim_linux_request_t *rq = request;
    return rq == NULL ? NULL : rq->hctx_storage.driver_data;
}

void *kb_linux_block_request_tag_set(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || rq->queue == NULL) {
        return NULL;
    }
    return kb_block_subsystem_queue_tag_set(rq->queue);
}

void *kb_linux_block_request_tagset_request(void *request)
{
    shim_linux_request_t *rq = request;
    void *tag_set = kb_linux_block_request_tag_set(request);
    if (rq == NULL || tag_set == NULL) {
        return NULL;
    }
    return kb_block_subsystem_tagset_request(tag_set, shim_blk_request_queue_index(rq), rq->tag);
}

uint32_t kb_linux_block_request_tag(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : rq->tag;
}

uint8_t kb_linux_block_request_generation(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : (uint8_t)(rq->reserved_120[0] & 0xfu);
}

int kb_linux_block_request_completed(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq != NULL && rq->completed != 0;
}

unsigned int kb_linux_block_request_end_status(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : rq->end_status;
}

void kb_linux_block_request_set_result_status(void *request, uint64_t result, uint16_t status)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL) {
        return;
    }
    rq->result = result;
    rq->status = status;
}

void kb_linux_block_request_mark_complete(void *request, unsigned int status)
{
    if (request == NULL) {
        return;
    }

    shim_linux_request_t *rq = request;
    rq->completed = 1;
    rq->end_status = status;
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: request complete request=%p status=0x%x\n", request, status);
    }
}

static int map_request_dma(
    shim_linux_request_t *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma,
    int auxiliary)
{
    if (request == NULL || cpu_addr == NULL || length == 0 || out_dma == NULL) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -19;
    }

    kb_status_t dma_status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(backend, device, cpu_addr, length, direction, &dma_status);
    if (dma_status == KB_ERR_UNSUPPORTED) {
        return -95;
    }
    if (dma_status != KB_OK) {
        return -5;
    }

    if (auxiliary) {
        request->prp_list_cpu = cpu_addr;
        request->prp_list_dma = dma_addr;
        request->prp_list_len = length;
        request->prp_list_dma_dir = direction;
    } else {
        request->dma_addr = dma_addr;
        request->dma_len = length;
        request->dma_dir = direction;
    }
    *out_dma = dma_addr;
    return 0;
}

int kb_linux_block_request_map_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma)
{
    kb_linux_block_request_unmap_dma(request);
    return map_request_dma(request, cpu_addr, length, direction, out_dma, 0);
}

void kb_linux_block_request_unmap_dma(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || rq->dma_addr == 0 || rq->dma_len == 0) {
        return;
    }
    kb_backend_t *backend = kb_shim_current_backend();
    kb_subsystem_dma_unmap(
        backend,
        kb_subsystem_dma_default_device(backend),
        rq->dma_addr,
        rq->dma_len,
        rq->dma_dir);
    rq->dma_addr = 0;
    rq->dma_len = 0;
}

int kb_linux_block_request_map_owned_aux_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma)
{
    kb_linux_block_request_unmap_owned_aux_dma(request);
    return map_request_dma(request, cpu_addr, length, direction, out_dma, 1);
}

void kb_linux_block_request_unmap_owned_aux_dma(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || rq->prp_list_cpu == NULL) {
        return;
    }
    if (rq->prp_list_dma != 0 && rq->prp_list_len != 0) {
        kb_backend_t *backend = kb_shim_current_backend();
        kb_subsystem_dma_unmap(
            backend,
            kb_subsystem_dma_default_device(backend),
            rq->prp_list_dma,
            rq->prp_list_len,
            rq->prp_list_dma_dir);
    }
    free(rq->prp_list_cpu);
    rq->prp_list_cpu = NULL;
    rq->prp_list_dma = 0;
    rq->prp_list_len = 0;
}

int kb_blk_rq_map_kern(void *queue, void *request, void *buffer, unsigned int length, unsigned int gfp)
{
    (void)queue;
    if (request == NULL || buffer == NULL || length == 0) {
        return -22;
    }

    shim_linux_request_t *rq = request;
    if (rq->driver_ops == NULL || rq->driver_ops->map_kernel_buffer == NULL) {
        return -95;
    }
    return rq->driver_ops->map_kernel_buffer(request, buffer, length, gfp);
}

void kb_blk_mq_complete_request(void *request)
{
    kb_linux_block_request_mark_complete(request, 0);
}

int kb_blk_mq_complete_request_remote(void *request)
{
    kb_linux_block_request_mark_complete(request, 0);
    return 1;
}

void kb_blk_mq_end_request(void *request, unsigned int status)
{
    kb_linux_block_request_mark_complete(request, status);
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
        kb_linux_block_request_mark_complete(rq, rq->status);
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
    kb_linux_block_request_unmap_owned_aux_dma(rq);
    kb_linux_block_request_unmap_dma(rq);
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
    void *tag_set = kb_block_subsystem_queue_tag_set(rq->queue);
    if (hctx == NULL || tag_set == NULL) {
        return -22;
    }

    unsigned char *ops = read_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET);
    if (ops == NULL) {
        return -22;
    }

    int (*queue_rq)(void *hctx, void *bd) =
        (int (*)(void *, void *))read_ptr(ops + KB_LINUX_BLK_MQ_OPS_QUEUE_RQ_OFFSET);
    if (queue_rq == NULL) {
        return -22;
    }

    if (rq->driver_ops != NULL && rq->driver_ops->before_execute != NULL) {
        int result = rq->driver_ops->before_execute(request);
        if (result != 0) {
            return result;
        }
    }

    shim_linux_queue_data_t *bd = &rq->queue_data;
    bd->rq = rq;
    bd->last = 1;

    shim_blk_tagset_bind_request(rq, tag_set);
    int result = queue_rq(hctx, bd);
    if (trace_block_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: queue_rq request=%p hctx=%p bd=%p result=%d driver_data=%p driver=%s\n",
            request,
            (void *)hctx,
            (void *)bd,
            result,
            hctx->driver_data,
            rq->driver_ops == NULL || rq->driver_ops->name == NULL ? "(unknown)" : rq->driver_ops->name);
    }
    if (result != 0) {
        return result;
    }

    if (rq->driver_ops != NULL && rq->driver_ops->complete_execute != NULL) {
        return rq->driver_ops->complete_execute(request);
    }
    if (rq->completed) {
        return rq->end_status == 0 ? 0 : -5;
    }
    return -95;
}
