#pragma once

#include "kobox/platform.h"

struct kb_platform {
    const kb_platform_ops_t *ops;
};
