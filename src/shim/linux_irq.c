#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_backend_t *kb_shim_current_backend(void);

static int trace_irq_enabled(void)
{
    return getenv("KOBOX_TRACE_IRQ") != NULL;
}

typedef struct shim_irq {
    unsigned int irq;
    void *dev_id;
    int (*handler)(int, void *);
    int (*thread_fn)(int, void *);
    kb_device_t *device;
    kb_irq_t *backend_irq;
    struct shim_irq *next;
} shim_irq_t;

static shim_irq_t *irq_list;

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL) {
        return KB_ERR_INVALID;
    }
    return ops->device_at(backend, 0, out_device);
}

static void irq_trampoline(void *ctx)
{
    shim_irq_t *irq = ctx;
    if (irq->handler != NULL) {
        (void)irq->handler((int)irq->irq, irq->dev_id);
    }
    if (irq->thread_fn != NULL) {
        (void)irq->thread_fn((int)irq->irq, irq->dev_id);
    }
}

int kb_request_threaded_irq(
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id)
{
    (void)flags;
    (void)name;
    if (handler == NULL && thread_fn == NULL) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = NULL;
    kb_status_t status = first_device(backend, &device);
    if (status != KB_OK) {
        return -19;
    }

    shim_irq_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return -12;
    }
    entry->irq = irq;
    entry->dev_id = dev_id;
    entry->handler = handler;
    entry->thread_fn = thread_fn;
    entry->device = device;

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    status = ops->irq_register(device, irq, irq_trampoline, entry, &entry->backend_irq);
    if (status != KB_OK) {
        free(entry);
        return -5;
    }

    entry->next = irq_list;
    irq_list = entry;

    if (trace_irq_enabled()) {
        fprintf(stderr, "kobox irq: request irq=%u dev_id=%p\n", irq, dev_id);
    }
    (void)ops->irq_wait(device, entry->backend_irq, 0);
    return 0;
}

void kb_free_irq(unsigned int irq, void *dev_id)
{
    shim_irq_t **cursor = &irq_list;
    while (*cursor != NULL) {
        shim_irq_t *entry = *cursor;
        if (entry->irq == irq && entry->dev_id == dev_id) {
            *cursor = entry->next;
            if (trace_irq_enabled()) {
                fprintf(stderr, "kobox irq: free irq=%u dev_id=%p\n", irq, dev_id);
            }
            const kb_backend_ops_t *ops = kb_backend_get_ops(kb_shim_current_backend());
            if (ops != NULL && ops->irq_unregister != NULL) {
                ops->irq_unregister(entry->device, entry->backend_irq);
            }
            free(entry);
            return;
        }
        cursor = &entry->next;
    }
}

void kb_free_all_irqs(void)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(kb_shim_current_backend());
    while (irq_list != NULL) {
        shim_irq_t *entry = irq_list;
        irq_list = entry->next;
        if (trace_irq_enabled()) {
            fprintf(stderr, "kobox irq: free-all irq=%u dev_id=%p\n", entry->irq, entry->dev_id);
        }
        if (ops != NULL && ops->irq_unregister != NULL) {
            ops->irq_unregister(entry->device, entry->backend_irq);
        }
        free(entry);
    }
}

int kb_wait_irq_for_dev_id(void *dev_id, uint64_t timeout_ns)
{
    for (shim_irq_t *entry = irq_list; entry != NULL; entry = entry->next) {
        if (entry->dev_id != dev_id) {
            continue;
        }

        const kb_backend_ops_t *ops = kb_backend_get_ops(kb_shim_current_backend());
        if (ops == NULL || ops->irq_wait == NULL) {
            return -95;
        }

        int (*handler)(int, void *) = entry->handler;
        int (*thread_fn)(int, void *) = entry->thread_fn;
        entry->handler = NULL;
        entry->thread_fn = NULL;
        kb_status_t status = ops->irq_wait(entry->device, entry->backend_irq, timeout_ns);
        entry->handler = handler;
        entry->thread_fn = thread_fn;
        if (trace_irq_enabled()) {
            fprintf(stderr, "kobox irq: wait dev_id=%p status=%d\n", dev_id, status);
        }
        return status == KB_OK ? 0 : -110;
    }
    return -19;
}

int request_threaded_irq(
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id)
{
    return kb_request_threaded_irq(irq, handler, thread_fn, flags, name, dev_id);
}

void free_irq(unsigned int irq, void *dev_id)
{
    kb_free_irq(irq, dev_id);
}
