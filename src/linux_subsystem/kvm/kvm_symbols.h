#pragma once

#include "kobox/device.h"
#include "linux_personality/linux_symbol_registry.h"

#include <stddef.h>
#include <stdint.h>

const kb_linux_symbol_t *kb_linux_kvm_symbols(size_t *out_count);
uintptr_t kb_linux_kvm_page_offset_base(void);
uintptr_t kb_linux_kvm_exported_page_offset_base(void);
uintptr_t kb_linux_kvm_vmemmap_base(void);
uintptr_t kb_linux_kvm_exported_vmemmap_base(void);
uintptr_t kb_linux_kvm_phys_base(void);
int kb_linux_kvm_payload_dma_addr(const void *cpu_addr, size_t size, uint64_t *out_dma_addr);
int kb_linux_kvm_dma_addr_in_payload_arena(uint64_t dma_addr, size_t size);
void *kb_linux_kvm_page_payload(void *page, unsigned long offset, size_t size);
int kb_linux_kvm_page_payload_dma_addr(
    void *page,
    unsigned long offset,
    size_t size,
    void **out_cpu_addr,
    uint64_t *out_dma_addr);
int kb_kvm_prepare_dma_arena(kb_device_backend_t *backend);
void *kb_kvm_alloc_pages_stub(unsigned int flags, unsigned int order);
unsigned long kb_kvm_get_free_pages_stub(unsigned int flags, unsigned int order);
int kb_kvm_release_pages(void *page, unsigned int order);
void kb_kvm_free_pages_stub(void *page, unsigned int order);
void kb_kvm_free_pages_addr_stub(unsigned long addr, unsigned int order);
void kb_kvm_free_pages_exact(void *virt, size_t size);
