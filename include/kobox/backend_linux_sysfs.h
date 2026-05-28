#pragma once

#include "kobox/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

kb_status_t kb_linux_sysfs_create(kb_backend_t **out_backend);

#ifdef __cplusplus
}
#endif
