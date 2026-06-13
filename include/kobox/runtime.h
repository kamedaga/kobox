#pragma once

#include "kobox/device.h"
#include "kobox/interface.h"
#include "kobox/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_runtime kb_runtime_t;

typedef struct kb_runtime_desc {
    kb_platform_t *platform;
} kb_runtime_desc_t;

#ifdef __cplusplus
}
#endif
