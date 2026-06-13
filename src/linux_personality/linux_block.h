#pragma once

#include "kobox/device.h"

#include <stddef.h>
#include <stdint.h>

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
} kb_linux_block_driver_ops_t;

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

int kb_linux_block_request_map_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma);
void kb_linux_block_request_unmap_dma(void *request);
int kb_linux_block_request_map_owned_aux_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma);
void kb_linux_block_request_unmap_owned_aux_dma(void *request);
