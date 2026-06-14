#pragma once

#include "linux_personality/linux_symbol_registry.h"

#include <stddef.h>
#include <stdint.h>

const kb_linux_symbol_t *kb_linux_kvm_symbols(size_t *out_count);
uintptr_t kb_linux_kvm_page_offset_base(void);
uintptr_t kb_linux_kvm_vmemmap_base(void);
uintptr_t kb_linux_kvm_phys_base(void);
