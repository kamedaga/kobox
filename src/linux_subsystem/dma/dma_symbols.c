#include "linux_subsystem/dma/dma_symbols.h"
#include "kobox/shim.h"

#include <stdint.h>

static const kb_linux_symbol_t dma_symbols[] = {
    {"dma_alloc_attrs", (void *)(uintptr_t)&kb_dma_alloc_attrs},
    {"dma_free_attrs", (void *)(uintptr_t)&kb_dma_free_attrs},
    {"dma_map_page_attrs", (void *)(uintptr_t)&kb_dma_map_page_attrs},
    {"dma_map_resource", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sgtable", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sg_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_single_attrs", (void *)(uintptr_t)&kb_dma_map_single_attrs},
    {"dma_mapping_error", (void *)(uintptr_t)&kb_dma_mapping_error},
    {"dma_max_mapping_size", (void *)(uintptr_t)&kb_dma_max_mapping_size},
    {"dma_opt_mapping_size", (void *)(uintptr_t)&kb_return_zero},
    {"dma_pool_alloc", (void *)(uintptr_t)&kb_dma_pool_alloc},
    {"dma_pool_create", (void *)(uintptr_t)&kb_dma_pool_create},
    {"dma_pool_destroy", (void *)(uintptr_t)&kb_dma_pool_destroy},
    {"dma_pool_free", (void *)(uintptr_t)&kb_dma_pool_free},
    {"dma_fence_context_alloc", (void *)(uintptr_t)&kb_return_zero},
    {"dma_fence_default_wait", (void *)(uintptr_t)&kb_return_zero},
    {"dma_fence_signal", (void *)(uintptr_t)&kb_return_zero},
    {"dma_resv_reserve_fences", (void *)(uintptr_t)&kb_return_zero},
    {"dma_set_coherent_mask", (void *)(uintptr_t)&kb_dma_set_coherent_mask},
    {"dma_set_mask", (void *)(uintptr_t)&kb_dma_set_mask},
    {"dma_unmap_page_attrs", (void *)(uintptr_t)&kb_dma_unmap_page_attrs},
    {"dma_unmap_single_attrs", (void *)(uintptr_t)&kb_dma_unmap_single_attrs},
    {"dma_mmap_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dmam_alloc_attrs", (void *)(uintptr_t)&kb_dma_alloc_attrs},
};

const kb_linux_symbol_t *kb_linux_dma_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(dma_symbols) / sizeof(dma_symbols[0]);
    }
    return dma_symbols;
}
