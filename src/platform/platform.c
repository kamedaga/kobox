#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "platform/platform_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct kb_basic_platform {
    kb_platform_t base;
    const char *name;
    kb_device_backend_t *device_backend;
    kb_interface_t **interfaces;
    size_t interface_count;
} kb_basic_platform_t;

static kb_basic_platform_t *basic_from_platform(kb_platform_t *platform)
{
    return (kb_basic_platform_t *)platform;
}

static const kb_basic_platform_t *basic_from_const_platform(const kb_platform_t *platform)
{
    return (const kb_basic_platform_t *)platform;
}

static char *copy_name(const char *name)
{
    if (name == NULL) {
        name = "platform";
    }
    const size_t len = strlen(name);
    char *copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, name, len + 1u);
    return copy;
}

static void *basic_memory_alloc(kb_platform_t *platform, size_t size, size_t alignment)
{
    (void)platform;
    (void)alignment;
    return malloc(size == 0 ? 1u : size);
}

static void basic_memory_free(kb_platform_t *platform, void *ptr, size_t size)
{
    (void)platform;
    (void)size;
    free(ptr);
}

static uint64_t basic_time_monotonic_ns(kb_platform_t *platform)
{
    kb_device_backend_t *backend = kb_platform_device_backend(platform);
    const kb_device_backend_ops_t *backend_ops = kb_device_backend_get_ops(backend);
    if (backend_ops != NULL && backend_ops->monotonic_ns != NULL) {
        return backend_ops->monotonic_ns(backend);
    }

#if defined(_WIN32)
    return 0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
#endif
}

static void basic_time_sleep_ns(kb_platform_t *platform, uint64_t duration_ns)
{
    (void)platform;
#if !defined(_WIN32)
    struct timespec req;
    req.tv_sec = (time_t)(duration_ns / 1000000000ull);
    req.tv_nsec = (long)(duration_ns % 1000000000ull);
    while (nanosleep(&req, &req) != 0) {
    }
#else
    (void)duration_ns;
#endif
}

static void basic_log(kb_platform_t *platform, int level, const char *message)
{
    kb_device_backend_t *backend = kb_platform_device_backend(platform);
    const kb_device_backend_ops_t *backend_ops = kb_device_backend_get_ops(backend);
    if (backend_ops != NULL && backend_ops->log != NULL) {
        backend_ops->log(backend, level, message);
    }
}

static kb_status_t basic_event_poll_once(kb_platform_t *platform, uint64_t timeout_ns)
{
    const kb_platform_time_ops_t *time_ops = kb_platform_time(platform);
    if (time_ops != NULL && time_ops->sleep_ns != NULL && timeout_ns != 0) {
        time_ops->sleep_ns(platform, timeout_ns);
    }
    return KB_OK;
}

static const kb_platform_memory_ops_t basic_memory_ops = {
    basic_memory_alloc,
    basic_memory_free,
};

static const kb_platform_time_ops_t basic_time_ops = {
    basic_time_monotonic_ns,
    basic_time_sleep_ns,
};

static const kb_platform_log_ops_t basic_log_ops = {
    basic_log,
};

static const kb_platform_event_ops_t basic_event_ops = {
    basic_event_poll_once,
};

static void basic_destroy(kb_platform_t *platform)
{
    kb_basic_platform_t *basic = basic_from_platform(platform);
    if (basic == NULL) {
        return;
    }
    for (size_t i = 0; i < basic->interface_count; i++) {
        kb_interface_destroy(basic->interfaces[i]);
    }
    free(basic->interfaces);
    kb_device_backend_destroy(basic->device_backend);
    free((char *)basic->name);
    free(basic);
}

static const char *basic_name(const kb_platform_t *platform)
{
    const kb_basic_platform_t *basic = basic_from_const_platform(platform);
    return basic == NULL ? NULL : basic->name;
}

static kb_device_backend_t *basic_device_backend(kb_platform_t *platform)
{
    kb_basic_platform_t *basic = basic_from_platform(platform);
    return basic == NULL ? NULL : basic->device_backend;
}

static const kb_platform_memory_ops_t *basic_memory(kb_platform_t *platform)
{
    (void)platform;
    return &basic_memory_ops;
}

static const kb_platform_time_ops_t *basic_time(kb_platform_t *platform)
{
    (void)platform;
    return &basic_time_ops;
}

static const kb_platform_log_ops_t *basic_log_facet(kb_platform_t *platform)
{
    (void)platform;
    return &basic_log_ops;
}

static const kb_platform_event_ops_t *basic_event(kb_platform_t *platform)
{
    (void)platform;
    return &basic_event_ops;
}

static kb_status_t basic_interface_count(kb_platform_t *platform, size_t *out_count)
{
    if (out_count == NULL) {
        return KB_ERR_INVALID;
    }
    kb_basic_platform_t *basic = basic_from_platform(platform);
    *out_count = basic == NULL ? 0 : basic->interface_count;
    return KB_OK;
}

static kb_status_t basic_interface_at(kb_platform_t *platform, size_t index, kb_interface_t **out_interface)
{
    if (out_interface == NULL) {
        return KB_ERR_INVALID;
    }
    kb_basic_platform_t *basic = basic_from_platform(platform);
    if (basic == NULL || index >= basic->interface_count) {
        *out_interface = NULL;
        return KB_ERR_NOT_FOUND;
    }
    *out_interface = basic->interfaces[index];
    return KB_OK;
}

static const kb_platform_ops_t basic_ops = {
    basic_destroy,
    basic_name,
    basic_device_backend,
    basic_memory,
    basic_time,
    basic_log_facet,
    basic_event,
    basic_interface_count,
    basic_interface_at,
};

kb_status_t kb_platform_create(const kb_platform_desc_t *desc, kb_platform_t **out_platform)
{
    if (desc == NULL || out_platform == NULL) {
        return KB_ERR_INVALID;
    }
    *out_platform = NULL;

    kb_basic_platform_t *platform = calloc(1, sizeof(*platform));
    if (platform == NULL) {
        return KB_ERR_NOMEM;
    }
    platform->base.ops = &basic_ops;
    platform->device_backend = desc->device_backend;
    platform->name = copy_name(desc->name);
    if (platform->name == NULL) {
        free(platform);
        return KB_ERR_NOMEM;
    }

    if (desc->interface_count != 0) {
        if (desc->interfaces == NULL) {
            free((char *)platform->name);
            free(platform);
            return KB_ERR_INVALID;
        }
        platform->interfaces = calloc(desc->interface_count, sizeof(platform->interfaces[0]));
        if (platform->interfaces == NULL) {
            free((char *)platform->name);
            free(platform);
            return KB_ERR_NOMEM;
        }
        memcpy(platform->interfaces, desc->interfaces, desc->interface_count * sizeof(platform->interfaces[0]));
        platform->interface_count = desc->interface_count;
    }

    *out_platform = &platform->base;
    return KB_OK;
}

const kb_platform_ops_t *kb_platform_get_ops(const kb_platform_t *platform)
{
    if (platform == 0) {
        return 0;
    }
    return platform->ops;
}

const char *kb_platform_name(const kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->name != NULL ? ops->name(platform) : NULL;
}

kb_device_backend_t *kb_platform_device_backend(kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->device_backend != NULL ? ops->device_backend(platform) : NULL;
}

const kb_platform_memory_ops_t *kb_platform_memory(const kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->memory != NULL ? ops->memory((kb_platform_t *)platform) : NULL;
}

const kb_platform_time_ops_t *kb_platform_time(const kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->time != NULL ? ops->time((kb_platform_t *)platform) : NULL;
}

const kb_platform_log_ops_t *kb_platform_log(const kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->log != NULL ? ops->log((kb_platform_t *)platform) : NULL;
}

const kb_platform_event_ops_t *kb_platform_event(const kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    return ops != NULL && ops->event != NULL ? ops->event((kb_platform_t *)platform) : NULL;
}

kb_status_t kb_platform_interface_count(kb_platform_t *platform, size_t *out_count)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    if (ops == NULL || ops->interface_count == NULL) {
        return KB_ERR_UNSUPPORTED;
    }
    return ops->interface_count(platform, out_count);
}

kb_status_t kb_platform_interface_at(kb_platform_t *platform, size_t index, kb_interface_t **out_interface)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    if (ops == NULL || ops->interface_at == NULL) {
        return KB_ERR_UNSUPPORTED;
    }
    return ops->interface_at(platform, index, out_interface);
}

void kb_platform_destroy(kb_platform_t *platform)
{
    const kb_platform_ops_t *ops = kb_platform_get_ops(platform);
    if (ops != 0 && ops->destroy != 0) {
        ops->destroy(platform);
    }
}
