#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "kobox/shim.h"
#include "linux_personality/linux_block.h"
#include "linux_personality/linux_nvme.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/nvme/nvme.h"
#include "linux_subsystem/pci/pci.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#endif

kb_device_backend_t *kb_shim_current_device_backend(void);

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

#define KB_NVME_EXECUTE_ADMIN_TIMEOUT_NS UINT64_C(5000000000)
#define KB_NVME_EXECUTE_IO_POLL_TIMEOUT_NS UINT64_C(1000000000)
#define KB_NVME_EXECUTE_POLL_PAUSE_ITERS 64u
#define KB_NVME_EXECUTE_SPIN_BEFORE_SLEEP 2048u

typedef struct nvme_io_smoke_case {
    uint64_t lba;
    unsigned int blocks;
    unsigned int pattern;
} nvme_io_smoke_case_t;

static void *tracked_admin_tag_set;
static void *tracked_io_tag_set;
static unsigned char *tracked_admin_nvmeq;
static unsigned char *tracked_io_nvmeq;
static unsigned int tracked_io_irq_waits;
static uint64_t recreated_admin_sq_dma;
static uint64_t recreated_admin_cq_dma;
static uint64_t recreated_io_sq_dma;
static uint64_t recreated_io_cq_dma;

static uint16_t read_u16(const void *ptr)
{
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint32_t read_u32(const void *ptr)
{
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint16_t read_volatile_u16(const volatile void *ptr)
{
    const volatile uint8_t *p = ptr;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t read_u64(const void *ptr)
{
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint64_t read_volatile_u64(const volatile void *ptr)
{
    const volatile uint8_t *p = ptr;
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8u; i++) {
        value |= (uint64_t)p[i] << (i * 8u);
    }
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

static int trace_nvme_enabled(void)
{
#if defined(__pachaos__)
    return 0;
#else
    return getenv("KOBOX_TRACE_NVME") != NULL;
#endif
}

static uint64_t nvme_monotonic_ns(void)
{
    return kb_ktime_get_mono_fast_ns();
}

static int nvme_deadline_expired(uint64_t start_ns, uint64_t timeout_ns)
{
    uint64_t now = nvme_monotonic_ns();
    if (now == 0 || start_ns == 0 || now < start_ns) {
        return 0;
    }
    return now - start_ns >= timeout_ns;
}

static void nvme_completion_poll_pause(void)
{
    for (volatile unsigned int i = 0; i < KB_NVME_EXECUTE_POLL_PAUSE_ITERS; i++) {
    }
}

static void nvme_completion_poll_yield(void)
{
#if !defined(_WIN32)
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50000,
    };
    (void)nanosleep(&delay, NULL);
#endif
}

static int nvme_completion_matches_request(
    const volatile unsigned char *completion,
    uint8_t phase,
    uint32_t request_tag,
    uint8_t request_gen,
    uint16_t *out_status)
{
    uint16_t status = read_volatile_u16(completion + 14);
    if ((status & 1u) != phase) {
        return 0;
    }

    uint16_t command_id = read_volatile_u16(completion + 12);
    if ((command_id & 0x0fffu) != (request_tag & 0x0fffu)) {
        return 0;
    }
    if (((command_id >> 12) & 0xfu) != (request_gen & 0xfu)) {
        return 0;
    }

    if (out_status != NULL) {
        *out_status = status;
    }
    return 1;
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

static void *nvme_ctrl_from_queue(const unsigned char *nvmeq)
{
    unsigned char *dev = nvmeq == NULL ? NULL : read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    return dev == NULL ? NULL : dev + KB_NVME_DEV_CTRL_OFFSET;
}

void kb_nvme_shim_track_queue(void *nvmeq_raw)
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

static int nvme_block_match_tag_set(void *tag_set)
{
    if (tag_set == NULL) {
        return 0;
    }
    if (tag_set == tracked_admin_tag_set || tag_set == tracked_io_tag_set) {
        return 1;
    }

    void *ctrl = kb_linux_block_tag_set_driver_data(tag_set);
    return ctrl != NULL &&
        (ctrl == nvme_ctrl_from_queue(tracked_admin_nvmeq) || ctrl == nvme_ctrl_from_queue(tracked_io_nvmeq));
}

static void nvme_block_track_tag_set(void *tag_set)
{
    if (tag_set == NULL) {
        return;
    }
    if (tracked_admin_tag_set == NULL) {
        tracked_admin_tag_set = tag_set;
    } else if (tag_set != tracked_admin_tag_set) {
        tracked_io_tag_set = tag_set;
    }
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: track tag_set=%p admin=%p io=%p\n",
            tag_set,
            tracked_admin_tag_set,
            tracked_io_tag_set);
    }
}

static void *nvme_block_request_driver_data(void *queue, void *tag_set)
{
    (void)queue;
    if (tag_set == tracked_io_tag_set && tracked_io_nvmeq != NULL) {
        return tracked_io_nvmeq;
    }
    if (tag_set == tracked_admin_tag_set && tracked_admin_nvmeq != NULL) {
        return tracked_admin_nvmeq;
    }

    void *ctrl = kb_linux_block_tag_set_driver_data(tag_set);
    if (ctrl != NULL && ctrl == nvme_ctrl_from_queue(tracked_admin_nvmeq)) {
        return tracked_admin_nvmeq;
    }
    if (ctrl != NULL && ctrl == nvme_ctrl_from_queue(tracked_io_nvmeq)) {
        return tracked_io_nvmeq;
    }
    return NULL;
}

static void *nvme_block_request_ctrl(void *tag_set, void *driver_data)
{
    unsigned char *nvmeq = driver_data;
    void *ctrl = nvme_ctrl_from_queue(nvmeq);
    if (ctrl != NULL) {
        return ctrl;
    }
    return kb_linux_block_tag_set_driver_data(tag_set);
}

static size_t nvme_block_queue_index(const void *driver_data)
{
    return shim_nvme_queue_index(driver_data);
}

static int nvme_block_map_kernel_buffer(void *request, void *buffer, unsigned int length, unsigned int gfp)
{
    (void)gfp;
    if (request == NULL || buffer == NULL || length == 0) {
        return -22;
    }

    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        return -22;
    }

    kb_dma_dir_t direction = KB_DMA_BIDIRECTIONAL;
    if (cmd[0] == KB_NVME_CMD_WRITE) {
        direction = KB_DMA_TO_DEVICE;
    } else if (cmd[0] == KB_NVME_CMD_READ) {
        direction = KB_DMA_FROM_DEVICE;
    }

    uint64_t dma_addr = 0;
    int result = kb_linux_block_request_map_dma(request, buffer, length, direction, &dma_addr);
    if (result != 0) {
        return result;
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
                kb_linux_block_request_unmap_dma(request);
                return -95;
            }

            uint64_t *prp_list = calloc(prp_count, sizeof(*prp_list));
            if (prp_list == NULL) {
                kb_linux_block_request_unmap_dma(request);
                return -12;
            }
            for (size_t i = 0; i < prp_count; i++) {
                prp_list[i] = second_prp + ((uint64_t)i * KB_NVME_PAGE_SIZE);
            }

            uint64_t prp_list_dma = 0;
            result = kb_linux_block_request_map_owned_aux_dma(
                request,
                prp_list,
                (uint32_t)list_len,
                KB_DMA_BIDIRECTIONAL,
                &prp_list_dma);
            if (result != 0) {
                free(prp_list);
                kb_linux_block_request_unmap_dma(request);
                return result;
            }

            write_u64(cmd + 32, prp_list_dma);
        }
    }
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: blk_rq_map_kern request=%p buffer=%p len=%u dma=0x%llx prp2=0x%llx opcode=0x%x cdw10=0x%x\n",
                request,
                buffer,
                length,
                (unsigned long long)dma_addr,
                (unsigned long long)read_u64(cmd + 32),
                (unsigned)cmd[0],
                (unsigned)read_u64(cmd + 40));
    }
    return 0;
}

static int nvme_block_before_execute(void *request)
{
    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        return -22;
    }
    if (cmd[0] == KB_NVME_ADMIN_CREATE_CQ) {
        uint32_t cdw11 = read_u32(cmd + 44);
        cdw11 &= 0x0000ffffu;
        write_u32(cmd + 44, cdw11);
    }
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: execute opcode=0x%x nsid=0x%x prp1=0x%llx prp2=0x%llx cdw10=0x%x cdw11=0x%x\n",
            (unsigned)cmd[0],
            (unsigned)read_u32(cmd + 4),
            (unsigned long long)read_u64(cmd + 24),
            (unsigned long long)read_u64(cmd + 32),
            (unsigned)read_u32(cmd + 40),
            (unsigned)read_u32(cmd + 44));
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
        ctrl = kb_linux_block_tag_set_driver_data(tag_set);
    }
    if (ctrl == NULL) {
        return NULL;
    }

    return kb_linux_block_alloc_driver_request(tag_set, ctrl, nvmeq, op, 1);
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
    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    void *tag_set = qid == 0 ? tracked_admin_tag_set : tracked_io_tag_set;
    if (tag_set == NULL && dev != NULL) {
        tag_set = dev + KB_NVME_DEV_TAGSET_OFFSET;
    }
    void *request = nvme_smoke_alloc_request_for_queue(nvmeq, tag_set, op);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        nvme_smoke_free_request(request);
        return -22;
    }
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

static int nvme_block_disk_io(void *queue, uint8_t opcode, uint64_t sector, void *buffer, size_t byte_count)
{
    if (queue == NULL || buffer == NULL || byte_count == 0 || (byte_count % KB_NVME_LBA_SIZE) != 0) {
        return -22;
    }
    if (byte_count > UINT32_MAX) {
        return -34;
    }

    unsigned int blocks = (unsigned int)(byte_count / KB_NVME_LBA_SIZE);
    unsigned int op = opcode == KB_NVME_CMD_WRITE ? KB_NVME_REQ_OP_DRV_OUT : KB_NVME_REQ_OP_DRV_IN;
    void *request = kb_blk_mq_alloc_request(queue, op, 0);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        kb_blk_mq_free_request(request);
        return -22;
    }
    memset(cmd, 0, 64);
    cmd[0] = opcode;
    write_u32(cmd + 4, KB_NVME_NSID_FIRST);
    write_u64(cmd + 40, sector);
    write_u32(cmd + 48, blocks - 1u);

    int result = kb_blk_rq_map_kern(queue, request, buffer, (unsigned int)byte_count, 0);
    if (result == 0) {
        result = kb_blk_execute_rq(request, 0);
    }
    kb_blk_mq_free_request(request);
    return result;
}

static int nvme_block_disk_read(void *queue, uint64_t sector, void *buffer, size_t byte_count)
{
    return nvme_block_disk_io(queue, KB_NVME_CMD_READ, sector, buffer, byte_count);
}

static int nvme_block_disk_write(void *queue, uint64_t sector, const void *buffer, size_t byte_count)
{
    return nvme_block_disk_io(queue, KB_NVME_CMD_WRITE, sector, (void *)(uintptr_t)buffer, byte_count);
}

static int nvme_submit_admin_create_queue(unsigned char *adminq, uint8_t opcode, uint16_t qid, uint16_t depth, uint64_t prp1)
{
    void *request = nvme_smoke_alloc_request_for_queue(adminq, tracked_admin_tag_set, KB_NVME_REQ_OP_DRV_OUT);
    if (request == NULL) {
        return -12;
    }

    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        nvme_smoke_free_request(request);
        return -22;
    }
    memset(cmd, 0, 64);
    cmd[0] = opcode;
    write_u64(cmd + 24, prp1);
    write_u32(cmd + 40, ((uint32_t)(depth - 1u) << 16) | qid);
    if (opcode == KB_NVME_ADMIN_CREATE_CQ) {
        unsigned int cq_vector = 0;
        write_u32(cmd + 44, (cq_vector << 16) | 0x00000003u);
    } else {
        write_u32(cmd + 44, ((uint32_t)qid << 16) | 0x00000001u);
    }
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: create queue opcode=0x%x qid=%u depth=%u prp1=0x%llx cdw10=0x%x cdw11=0x%x\n",
            (unsigned)opcode,
            (unsigned)qid,
            (unsigned)depth,
            (unsigned long long)prp1,
            (unsigned)read_u32(cmd + 40),
            (unsigned)read_u32(cmd + 44));
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

    unsigned char *cmd = kb_linux_block_request_command(request);
    if (cmd == NULL) {
        nvme_smoke_free_request(request);
        return -22;
    }
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

static void nvme_set_queue_depth(unsigned char *nvmeq, uint16_t depth)
{
    if (nvmeq != NULL && depth != 0) {
        write_u16(nvmeq + KB_NVME_QUEUE_DEPTH_OFFSET, depth);
    }
}

static void nvme_reset_dbbuf_shadow(void)
{
    kb_nvme_subsystem_reset_dbbuf_shadow();
}

static void nvme_write_dbbuf_shadow(unsigned char *nvmeq, unsigned int doorbell, uint32_t value)
{
    unsigned char *dev = nvmeq == NULL ? NULL : read_ptr(nvmeq + KB_NVME_QUEUE_DEV_OFFSET);
    if (dev == NULL || !kb_nvme_subsystem_dbbuf_ready()) {
        return;
    }

    uint32_t db_stride = 0;
    memcpy(&db_stride, dev + KB_NVME_DEV_DB_STRIDE_OFFSET, sizeof(db_stride));
    if (db_stride == 0) {
        db_stride = 1;
    }

    uint16_t qid = read_u16(nvmeq + KB_NVME_QUEUE_QID_OFFSET);
    const size_t db_index = ((size_t)qid * 2u * (size_t)db_stride) + doorbell;
    size_t dbs_size = 0;
    uint32_t *dbs = kb_nvme_subsystem_dbbuf_dbs_cpu(&dbs_size);
    if (dbs == NULL || (db_index + 1u) * sizeof(*dbs) > dbs_size) {
        return;
    }
    dbs[db_index] = value;
    atomic_thread_fence(memory_order_release);
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
    kb_device_backend_t *backend = kb_shim_current_device_backend();
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
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_subsystem_dma_unmap(
        backend,
        kb_subsystem_dma_default_device(backend),
        dma_addr,
        (size_t)size,
        KB_DMA_BIDIRECTIONAL);
}

static void nvme_release_recreated_queue_mappings(void)
{
    if (recreated_io_cq_dma != 0) {
        nvme_dma_unmap_existing(recreated_io_cq_dma, KB_NVME_IO_QUEUE_DEPTH * 16u);
        recreated_io_cq_dma = 0;
    }
    if (recreated_io_sq_dma != 0) {
        nvme_dma_unmap_existing(recreated_io_sq_dma, KB_NVME_IO_QUEUE_DEPTH * 64u);
        recreated_io_sq_dma = 0;
    }
    if (recreated_admin_cq_dma != 0) {
        nvme_dma_unmap_existing(recreated_admin_cq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 16u);
        recreated_admin_cq_dma = 0;
    }
    if (recreated_admin_sq_dma != 0) {
        nvme_dma_unmap_existing(recreated_admin_sq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 64u);
        recreated_admin_sq_dma = 0;
    }
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

    nvme_set_queue_depth(tracked_admin_nvmeq, KB_NVME_ADMIN_QUEUE_DEPTH);
    nvme_set_queue_depth(tracked_io_nvmeq, KB_NVME_IO_QUEUE_DEPTH);
    nvme_reset_queue_state(tracked_admin_nvmeq);
    nvme_reset_queue_state(tracked_io_nvmeq);
    nvme_reset_dbbuf_shadow();
    nvme_release_recreated_queue_mappings();

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

    if (result == 0) {
        recreated_admin_sq_dma = admin_sq_dma;
        recreated_admin_cq_dma = admin_cq_dma;
        recreated_io_sq_dma = io_sq_dma;
        recreated_io_cq_dma = io_cq_dma;
    } else {
        nvme_dma_unmap_existing(io_cq_dma, KB_NVME_IO_QUEUE_DEPTH * 16u);
        nvme_dma_unmap_existing(io_sq_dma, KB_NVME_IO_QUEUE_DEPTH * 64u);
        nvme_dma_unmap_existing(admin_cq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 16u);
        nvme_dma_unmap_existing(admin_sq_dma, KB_NVME_ADMIN_QUEUE_DEPTH * 64u);
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
            nvme_release_recreated_queue_mappings();
            free(write_buffer);
            free(read_buffer);
            return result;
        }
        if (memcmp(write_buffer, read_buffer, length) != 0) {
            fprintf(stderr, "kobox nvme io smoke: compare failed lba=%llu blocks=%u\n",
                (unsigned long long)cases[i].lba, cases[i].blocks);
            nvme_release_recreated_queue_mappings();
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
    nvme_release_recreated_queue_mappings();
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

int kb_nvme_recreate_io_queue(void)
{
    return nvme_reset_controller_and_recreate_io_queue();
}

static int nvme_block_complete_execute(void *request)
{
    if (request == NULL) {
        return -22;
    }

    atomic_thread_fence(memory_order_seq_cst);
    nvme_completion_poll_yield();

    unsigned char *nvmeq = kb_linux_block_request_driver_data(request);
    if (nvmeq == NULL) {
        return -22;
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
    unsigned char *poll_bar = qid == 0 ? nvme_bar_from_queue(nvmeq) : NULL;
    if (qid != 0) {
        if (trace_nvme_enabled()) {
            size_t queue_index = shim_nvme_queue_index(nvmeq);
            void *tag_set = kb_linux_block_request_tag_set(request);
            void *tag_request = kb_linux_block_request_tagset_request(request);
            uint32_t request_tag = kb_linux_block_request_tag(request);
            uint16_t command_id = read_u16((const void *)(completion + 12));
            unsigned int command_gen = (unsigned int)((command_id >> 12) & 0xfu);
            unsigned int request_gen = kb_linux_block_request_generation(request);
            fprintf(
                stderr,
                "kobox blk-mq: irq lookup qid=%u queue=%zu tagset=%p tag=%u cid=0x%x gen=%u/%u request=%p tag_request=%p\n",
                (unsigned)qid,
                queue_index,
                tag_set,
                request_tag,
                command_id,
                command_gen,
                request_gen,
                request,
                tag_request);
        }
        int irq_status = kb_wait_irq_signal_for_dev_id(nvmeq, 1000000000ull);
        if (irq_status == 0) {
            tracked_io_irq_waits++;
            uint16_t irq_head = read_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET);
            uint8_t irq_phase = *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET);
            if (kb_linux_block_request_completed(request) || irq_head != head || irq_phase != phase) {
                unsigned int end_status = kb_linux_block_request_end_status(request);
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
                        end_status);
                }
                return end_status == 0 ? 0 : -5;
            }
        } else if (trace_nvme_enabled()) {
            fprintf(stderr, "kobox nvme: irq wait failed qid=%u status=%d\n", (unsigned)qid, irq_status);
        }
    }

    int completed = 0;
    uint32_t request_tag = kb_linux_block_request_tag(request);
    uint8_t request_gen = kb_linux_block_request_generation(request);
    uint16_t completion_status = 0;
    uint64_t wait_start_ns = nvme_monotonic_ns();
    uint64_t wait_timeout_ns = qid == 0 ? KB_NVME_EXECUTE_ADMIN_TIMEOUT_NS : KB_NVME_EXECUTE_IO_POLL_TIMEOUT_NS;
    unsigned int spins = 0;
    for (;;) {
        if (nvme_completion_matches_request(completion, phase, request_tag, request_gen, &completion_status)) {
            completed = 1;
            break;
        }
        if (++spins < KB_NVME_EXECUTE_SPIN_BEFORE_SLEEP) {
            continue;
        }
        spins = 0;
        if (nvme_deadline_expired(wait_start_ns, wait_timeout_ns)) {
            break;
        }
        if (poll_bar != NULL) {
            (void)mmio_read32(poll_bar, KB_NVME_REG_CSTS);
            nvme_completion_poll_yield();
        }
        nvme_completion_poll_pause();
    }
    if (!completed) {
        fprintf(
            stderr,
            "kobox nvme: request timeout qid=%u head=%u phase=%u tag=%u gen=%u cid=0x%x status=0x%x\n",
            (unsigned)qid,
            head,
            phase,
            request_tag,
            request_gen,
            (unsigned)read_volatile_u16(completion + 12),
            (unsigned)read_volatile_u16(completion + 14));
        return -110;
    }

    atomic_thread_fence(memory_order_acquire);
    uint64_t result64 = read_volatile_u64(completion);
    kb_linux_block_request_set_result_status(request, result64, (uint16_t)(completion_status >> 1));

    head++;
    if (head == depth) {
        head = 0;
        phase ^= 1u;
        *(uint8_t *)(nvmeq + KB_NVME_QUEUE_PHASE_OFFSET) = phase;
    }
    write_u16(nvmeq + KB_NVME_QUEUE_CQ_HEAD_OFFSET, head);
    nvme_write_dbbuf_shadow(nvmeq, 1u, head);
    if (cq_head_db != NULL) {
        *cq_head_db = head;
    }
    kb_linux_block_request_mark_complete(request, (unsigned)(completion_status >> 1));
    if (trace_nvme_enabled()) {
        fprintf(
            stderr,
            "kobox nvme: blk_execute_rq complete request=%p status=0x%x result=0x%llx next_head=%u\n",
            request,
            (unsigned)(completion_status >> 1),
            (unsigned long long)result64,
            head);
    }
    return (completion_status >> 1) == 0 ? 0 : -5;
}

static const kb_linux_block_driver_ops_t nvme_block_ops = {
    .name = "nvme",
    .match_tag_set = nvme_block_match_tag_set,
    .track_tag_set = nvme_block_track_tag_set,
    .request_driver_data = nvme_block_request_driver_data,
    .request_ctrl = nvme_block_request_ctrl,
    .queue_index = nvme_block_queue_index,
    .map_kernel_buffer = nvme_block_map_kernel_buffer,
    .before_execute = nvme_block_before_execute,
    .complete_execute = nvme_block_complete_execute,
    .disk_read = nvme_block_disk_read,
    .disk_write = nvme_block_disk_write,
};

void kb_nvme_shim_register_block_driver(void)
{
    kb_linux_block_register_driver_ops(&nvme_block_ops);
}
