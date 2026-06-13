#pragma once

#include "linux_personality/linux_symbol_registry.h"

#include <stddef.h>

const kb_linux_symbol_t *kb_linux_security_symbols(size_t *out_count);
