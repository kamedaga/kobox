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
int kb_kvm_prepare_dma_arena(kb_device_backend_t *backend);
void *kb_kvm_alloc_pages_stub(unsigned int flags, unsigned int order);
