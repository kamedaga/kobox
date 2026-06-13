#pragma once

#include "kobox/device.h"
#include "kobox/interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_platform kb_platform_t;

typedef struct kb_platform_memory_ops {
    void *(*alloc)(kb_platform_t *platform, size_t size, size_t alignment);
    void (*free)(kb_platform_t *platform, void *ptr, size_t size);
} kb_platform_memory_ops_t;

typedef struct kb_platform_time_ops {
    uint64_t (*monotonic_ns)(kb_platform_t *platform);
    void (*sleep_ns)(kb_platform_t *platform, uint64_t duration_ns);
} kb_platform_time_ops_t;

typedef struct kb_platform_log_ops {
    void (*log)(kb_platform_t *platform, int level, const char *message);
} kb_platform_log_ops_t;

typedef struct kb_platform_event_ops {
    kb_status_t (*poll_once)(kb_platform_t *platform, uint64_t timeout_ns);
} kb_platform_event_ops_t;

typedef struct kb_platform_desc {
    const char *name;
    kb_device_backend_t *device_backend;
    kb_interface_t **interfaces;
    size_t interface_count;
} kb_platform_desc_t;

typedef struct kb_platform_ops {
    void (*destroy)(kb_platform_t *platform);
    const char *(*name)(const kb_platform_t *platform);
    kb_device_backend_t *(*device_backend)(kb_platform_t *platform);
    const kb_platform_memory_ops_t *(*memory)(kb_platform_t *platform);
    const kb_platform_time_ops_t *(*time)(kb_platform_t *platform);
    const kb_platform_log_ops_t *(*log)(kb_platform_t *platform);
    const kb_platform_event_ops_t *(*event)(kb_platform_t *platform);
    kb_status_t (*interface_count)(kb_platform_t *platform, size_t *out_count);
    kb_status_t (*interface_at)(kb_platform_t *platform, size_t index, kb_interface_t **out_interface);
} kb_platform_ops_t;

kb_status_t kb_platform_create(const kb_platform_desc_t *desc, kb_platform_t **out_platform);
const kb_platform_ops_t *kb_platform_get_ops(const kb_platform_t *platform);
const char *kb_platform_name(const kb_platform_t *platform);
kb_device_backend_t *kb_platform_device_backend(kb_platform_t *platform);
const kb_platform_memory_ops_t *kb_platform_memory(const kb_platform_t *platform);
const kb_platform_time_ops_t *kb_platform_time(const kb_platform_t *platform);
const kb_platform_log_ops_t *kb_platform_log(const kb_platform_t *platform);
const kb_platform_event_ops_t *kb_platform_event(const kb_platform_t *platform);
kb_status_t kb_platform_interface_count(kb_platform_t *platform, size_t *out_count);
kb_status_t kb_platform_interface_at(kb_platform_t *platform, size_t index, kb_interface_t **out_interface);
void kb_platform_destroy(kb_platform_t *platform);

#ifdef __cplusplus
}
#endif
