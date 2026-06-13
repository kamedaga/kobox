#pragma once

#include "kobox/module.h"

#include <stdint.h>

void *kb_loader_symbol_registry_lookup_export(const char *name);
kb_status_t kb_loader_symbol_registry_add_export(const char *name, uint64_t address, const kb_module_t *owner);
void kb_loader_symbol_registry_remove_owner(const kb_module_t *owner);
