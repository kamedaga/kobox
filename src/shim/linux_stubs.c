#include "kobox/shim.h"

#include <stdlib.h>

void kb_trace_noop(void)
{
}

int kb_return_zero(void)
{
    return 0;
}

int kb_return_one(void)
{
    return 1;
}

void *kb_alloc_stub(void)
{
    return calloc(1, 4096);
}

void *kb_identity_ptr(void *ptr)
{
    return ptr;
}

const char *kb_empty_string(void)
{
    return "";
}
