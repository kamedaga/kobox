#include "linux_subsystem/input/input_symbols.h"
#include "kobox/shim.h"

#include <stdint.h>

static const kb_linux_symbol_t input_symbols[] = {
    {"input_allocate_device", (void *)(uintptr_t)&kb_input_allocate_device},
    {"input_free_device", (void *)(uintptr_t)&kb_input_free_device},
    {"input_register_device", (void *)(uintptr_t)&kb_input_register_device},
    {"input_unregister_device", (void *)(uintptr_t)&kb_input_unregister_device},
    {"input_event", (void *)(uintptr_t)&kb_input_event},
    {"input_ff_event", (void *)(uintptr_t)&kb_return_zero},
    {"input_scancode_to_scalar", (void *)(uintptr_t)&kb_return_zero},
    {"input_set_abs_params", (void *)(uintptr_t)&kb_input_set_abs_params},
    {"input_alloc_absinfo", (void *)(uintptr_t)&kb_input_alloc_absinfo},
    {"input_mt_init_slots", (void *)(uintptr_t)&kb_input_mt_init_slots},
};

const kb_linux_symbol_t *kb_linux_input_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(input_symbols) / sizeof(input_symbols[0]);
    }
    return input_symbols;
}
