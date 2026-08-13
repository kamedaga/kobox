#pragma once

#include "kobox/device.h"

#include <stddef.h>
#include <stdint.h>

typedef struct kb_linux_block_read_request {
    uint64_t sector;
    void *buffer;
    size_t byte_count;
} kb_linux_block_read_request_t;

typedef struct kb_linux_block_write_request {
    uint64_t sector;
    const void *buffer;
    size_t byte_count;
} kb_linux_block_write_request_t;

enum {
    KB_LINUX_BLOCK_WRITE_FUA = 1u << 0,
};

typedef struct kb_linux_block_driver_ops {
    const char *name;
    int (*match_tag_set)(void *tag_set);
    void (*track_tag_set)(void *tag_set);
    void *(*request_driver_data)(void *queue, void *tag_set);
    void *(*request_ctrl)(void *tag_set, void *driver_data);
    size_t (*queue_index)(const void *driver_data);
    int (*map_kernel_buffer)(void *request, void *buffer, unsigned int length, unsigned int gfp);
    int (*before_execute)(void *request);
    int (*complete_execute)(void *request);
    int (*disk_read)(void *queue, uint64_t sector, void *buffer, size_t byte_count);
    int (*disk_read_batch)(
        void *queue,
        const kb_linux_block_read_request_t *requests,
        size_t request_count);
    int (*disk_write)(void *queue, uint64_t sector, const void *buffer, size_t byte_count);
    int (*disk_write_flags)(
        void *queue,
        uint64_t sector,
        const void *buffer,
        size_t byte_count,
        uint32_t flags);
    int (*disk_write_batch)(
        void *queue,
        const kb_linux_block_write_request_t *requests,
        size_t request_count);
    int (*disk_flush)(void *queue);
} kb_linux_block_driver_ops_t;

typedef enum kb_linux_block_profile_stage {
    KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC = 0,
    KB_LINUX_BLOCK_PROFILE_DMA_MAP,
    KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE,
    KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT,
    KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT,
    KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK,
    KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL,
    KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL,
    KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL,
    KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT,
    KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD,
    KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT,
    KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS,
    KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_FALLBACK,
    KB_LINUX_BLOCK_PROFILE_NVME_FLUSH,
    KB_LINUX_BLOCK_PROFILE_STAGE_COUNT,
} kb_linux_block_profile_stage_t;

typedef struct kb_linux_block_profile {
    uint64_t cycles[KB_LINUX_BLOCK_PROFILE_STAGE_COUNT];
    uint64_t calls[KB_LINUX_BLOCK_PROFILE_STAGE_COUNT];
    uint64_t disk_read_bytes;
    uint64_t disk_read_command_calls[8];
    uint64_t disk_read_command_bytes[8];
    uint64_t disk_read_command_count;
    uint64_t disk_read_command_sectors[64];
    uint64_t disk_read_command_lengths[64];
    uint64_t disk_write_command_calls[8];
    uint64_t disk_write_command_bytes[8];
    uint64_t disk_write_command_count;
    uint64_t disk_write_command_sectors[64];
    uint64_t disk_write_command_lengths[64];
    uint32_t disk_write_command_flags[64];
    uint64_t native_fua_commands;
} kb_linux_block_profile_t;

uint64_t kb_linux_block_profile_begin(void);
void kb_linux_block_profile_record(
    kb_linux_block_profile_stage_t stage,
    uint64_t start,
    uint64_t bytes);
void kb_linux_block_profile_snapshot(kb_linux_block_profile_t *out_profile);
void kb_linux_block_profile_record_read_command(
    uint64_t sector,
    size_t byte_count);
void kb_linux_block_profile_record_write_command(
    uint64_t sector,
    size_t byte_count,
    uint32_t flags);
void kb_linux_block_profile_record_native_fua(void);
int kb_linux_block_dma_read_window_begin(void *buffer, size_t length);
void kb_linux_block_dma_read_window_end(void);

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
#define KB_LINUX_BLOCK_PROFILE_BEGIN() kb_linux_block_profile_begin()
#define KB_LINUX_BLOCK_PROFILE_RECORD(stage, start, bytes) \
    kb_linux_block_profile_record((stage), (start), (bytes))
#else
#define KB_LINUX_BLOCK_PROFILE_BEGIN() UINT64_C(0)
#define KB_LINUX_BLOCK_PROFILE_RECORD(stage, start, bytes) \
    do { (void)(stage); (void)(start); (void)(bytes); } while (0)
#endif

void kb_linux_block_register_driver_ops(const kb_linux_block_driver_ops_t *ops);

void *kb_linux_block_tag_set_driver_data(void *tag_set);
void *kb_linux_block_alloc_driver_request(
    void *tag_set,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue);

void *kb_linux_block_request_command(void *request);
void *kb_linux_block_request_driver_data(void *request);
void *kb_linux_block_request_tag_set(void *request);
void *kb_linux_block_request_tagset_request(void *request);
uint32_t kb_linux_block_request_tag(const void *request);
uint8_t kb_linux_block_request_generation(const void *request);
int kb_linux_block_request_completed(const void *request);
unsigned int kb_linux_block_request_end_status(const void *request);
void kb_linux_block_request_set_result_status(void *request, uint64_t result, uint16_t status);
void kb_linux_block_request_mark_complete(void *request, unsigned int status);
int kb_linux_block_request_map_dma_pages(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity);
void kb_linux_block_request_unmap_dma(void *request);
int kb_linux_block_request_map_owned_aux_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma);
int kb_linux_block_request_map_cached_aux_dma(
    void *request,
    const void *data,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma);
void kb_linux_block_request_unmap_owned_aux_dma(void *request);
