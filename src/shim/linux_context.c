#include "kobox/shim.h"

#include <stdlib.h>

static kb_backend_t *current_backend;

void kb_shim_set_backend(kb_backend_t *backend)
{
    current_backend = backend;
}

kb_backend_t *kb_shim_current_backend(void)
{
    return current_backend;
}

void kb_stack_chk_fail(void)
{
    abort();
}
