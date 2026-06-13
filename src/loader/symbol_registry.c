#include "loader/symbol_registry.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct kb_loader_exported_symbol {
    const char *name;
    uint64_t address;
    const kb_module_t *owner;
} kb_loader_exported_symbol_t;

enum {
    KB_LOADER_EXPORTED_SYMBOL_MAX = 4096,
};

static kb_loader_exported_symbol_t exported_symbols[KB_LOADER_EXPORTED_SYMBOL_MAX];

static int symbol_pointer_is_valid(const char *name)
{
    uintptr_t value = (uintptr_t)name;
    return value >= 4096u && value < UINTPTR_MAX - 4095u;
}

static int symbol_name_matches(const char *a, const char *b)
{
    if (!symbol_pointer_is_valid(a) || !symbol_pointer_is_valid(b)) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

void *kb_loader_symbol_registry_lookup_export(const char *name)
{
    if (!symbol_pointer_is_valid(name) || name[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; i < KB_LOADER_EXPORTED_SYMBOL_MAX; i++) {
        if (symbol_name_matches(exported_symbols[i].name, name)) {
            return (void *)(uintptr_t)exported_symbols[i].address;
        }
    }
    return 0;
}

kb_status_t kb_loader_symbol_registry_add_export(const char *name, uint64_t address, const kb_module_t *owner)
{
    if (!symbol_pointer_is_valid(name) || name[0] == '\0' || address == 0 || owner == 0) {
        return KB_ERR_INVALID;
    }

    for (size_t i = 0; i < KB_LOADER_EXPORTED_SYMBOL_MAX; i++) {
        if (symbol_name_matches(exported_symbols[i].name, name)) {
            return KB_OK;
        }
    }

    for (size_t i = 0; i < KB_LOADER_EXPORTED_SYMBOL_MAX; i++) {
        if (exported_symbols[i].name == 0) {
            exported_symbols[i].name = name;
            exported_symbols[i].address = address;
            exported_symbols[i].owner = owner;
            return KB_OK;
        }
    }

    return KB_ERR_NOMEM;
}

void kb_loader_symbol_registry_remove_owner(const kb_module_t *owner)
{
    if (owner == 0) {
        return;
    }
    for (size_t i = 0; i < KB_LOADER_EXPORTED_SYMBOL_MAX; i++) {
        if (exported_symbols[i].owner == owner) {
            memset(&exported_symbols[i], 0, sizeof(exported_symbols[i]));
        }
    }
}

void *kb_module_lookup_exported_symbol(const char *name)
{
    return kb_loader_symbol_registry_lookup_export(name);
}
