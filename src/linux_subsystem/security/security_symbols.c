#include "linux_subsystem/security/security_symbols.h"
#include "kobox/shim.h"

#include <stdint.h>

static const kb_linux_symbol_t security_symbols[] = {
    {"security_inode_init_security", (void *)(uintptr_t)&kb_return_zero},
};

const kb_linux_symbol_t *kb_linux_security_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(security_symbols) / sizeof(security_symbols[0]);
    }
    return security_symbols;
}
