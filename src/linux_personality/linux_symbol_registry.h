#pragma once

#include <stddef.h>

typedef struct kb_linux_symbol {
    const char *name;
    void *address;
} kb_linux_symbol_t;
