#pragma once

#include "kobox/device.h"

#include <stddef.h>
#include <stdint.h>

kb_device_t *kb_subsystem_dma_default_device(kb_device_backend_t *backend);
void *kb_subsystem_dma_alloc(
    kb_device_backend_t *backend,
    kb_device_t *device,
    size_t size,
    uint64_t *dma_handle);
void kb_subsystem_dma_free(
    kb_device_backend_t *backend,
    size_t size,
    void *cpu_addr,
    uint64_t dma_handle);
void *kb_subsystem_dma_cpu_addr(uint64_t dma_addr, size_t *out_available);
uint64_t kb_subsystem_dma_map(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    kb_status_t *out_status);
kb_status_t kb_subsystem_dma_map_pages(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity);
kb_status_t kb_subsystem_dma_map_fixed(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    uint64_t dma_addr,
    kb_dma_dir_t direction);
void kb_subsystem_dma_unmap(
    kb_device_backend_t *backend,
    kb_device_t *device,
    uint64_t dma_addr,
    size_t size,
    kb_dma_dir_t direction);
int kb_subsystem_dma_mapping_error(uint64_t dma_addr);
