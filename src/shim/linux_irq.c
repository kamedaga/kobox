#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_backend_t *kb_shim_current_backend(void);

static int trace_irq_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_IRQ");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

typedef struct shim_irq {
    unsigned int irq;
    void *dev_id;
    int (*handler)(int, void *);
    int (*thread_fn)(int, void *);
    kb_device_t *device;
    kb_irq_t *backend_irq;
    unsigned long kernel_gs;
    struct shim_irq *next;
} shim_irq_t;

static shim_irq_t *irq_list;

enum {
    KB_IRQ_BACKEND_KIND_SHIFT = 30,
    KB_IRQ_BACKEND_VECTOR_MASK = 0x3fffffff,
    KB_IRQ_BACKEND_KIND_LEGACY = 0,
    KB_IRQ_BACKEND_KIND_MSI = 1,
    KB_IRQ_BACKEND_KIND_MSIX = 2,
    KB_IRQ_LINUX_BASE = 32,
    KB_IRQ_MAPPING_MAX = 256,
};

typedef struct shim_irq_mapping {
    unsigned int linux_irq;
    unsigned int backend_vector;
} shim_irq_mapping_t;

static shim_irq_mapping_t irq_mappings[KB_IRQ_MAPPING_MAX];
static unsigned int next_linux_irq = KB_IRQ_LINUX_BASE;

int kb_irq_allocate_mapping(unsigned int backend_kind, unsigned int backend_vector, unsigned int *out_linux_irq)
{
    if (out_linux_irq == NULL || backend_kind > KB_IRQ_BACKEND_KIND_MSIX) {
        return -22;
    }
    for (size_t i = 0; i < KB_IRQ_MAPPING_MAX; i++) {
        if (irq_mappings[i].linux_irq != 0) {
            continue;
        }

        unsigned int linux_irq = next_linux_irq++;
        if (next_linux_irq == 0 || next_linux_irq >= KB_IRQ_BACKEND_VECTOR_MASK) {
            next_linux_irq = KB_IRQ_LINUX_BASE;
        }
        irq_mappings[i].linux_irq = linux_irq;
        irq_mappings[i].backend_vector =
            (backend_kind << KB_IRQ_BACKEND_KIND_SHIFT) | (backend_vector & KB_IRQ_BACKEND_VECTOR_MASK);
        *out_linux_irq = linux_irq;
        return 0;
    }
    return -12;
}

unsigned int kb_irq_backend_vector(unsigned int linux_irq)
{
    for (size_t i = 0; i < KB_IRQ_MAPPING_MAX; i++) {
        if (irq_mappings[i].linux_irq == linux_irq) {
            return irq_mappings[i].backend_vector;
        }
    }
    return linux_irq;
}

void kb_irq_clear_mappings(void)
{
    memset(irq_mappings, 0, sizeof(irq_mappings));
    next_linux_irq = KB_IRQ_LINUX_BASE;
}

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
    unsigned long old_gs = 0;
    int has_gs = kb_shim_enter_kernel_gs(irq->kernel_gs, &old_gs) == 0;
    if (irq->handler != NULL) {
        (void)irq->handler((int)irq->irq, irq->dev_id);
    }
    if (irq->thread_fn != NULL) {
        (void)irq->thread_fn((int)irq->irq, irq->dev_id);
    }
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
}

static void poll_root_hub_for_irq(const shim_irq_t *irq)
{
    unsigned long old_gs = 0;
    int has_gs = kb_shim_enter_kernel_gs(irq->kernel_gs, &old_gs) == 0;
    (void)kb_usb_poll_root_hub(irq->dev_id);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
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
    entry->kernel_gs = kb_shim_current_kernel_gs();

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    unsigned int backend_vector = kb_irq_backend_vector(irq);
    status = ops->irq_register(device, backend_vector, irq_trampoline, entry, &entry->backend_irq);
    if (status != KB_OK) {
        fprintf(
            stderr,
            "kobox irq: request irq=%u backend_vector=0x%x dev_id=%p backend status=%d\n",
            irq,
            backend_vector,
            dev_id,
            status);
        free(entry);
        return -5;
    }

    entry->next = irq_list;
    irq_list = entry;

    if (trace_irq_enabled()) {
        fprintf(stderr, "kobox irq: request irq=%u dev_id=%p\n", irq, dev_id);
    }
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
        int xhci_pending = kb_pci_xhci_irq_pending();
        if (status == KB_OK || xhci_pending) {
            irq_trampoline(entry);
        }
        if (xhci_pending) {
            poll_root_hub_for_irq(entry);
        }
        entry->handler = handler;
        entry->thread_fn = thread_fn;
        kb_run_deferred_work();
        if (trace_irq_enabled()) {
            fprintf(stderr, "kobox irq: wait dev_id=%p status=%d\n", dev_id, status);
        }
        return status == KB_OK ? 0 : -110;
    }
    return -19;
}

int kb_handle_irq_for_dev_id(void *dev_id, uint64_t timeout_ns)
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
        int xhci_pending = kb_pci_xhci_irq_pending();
        entry->handler = handler;
        entry->thread_fn = thread_fn;
        if (status == KB_OK || xhci_pending) {
            irq_trampoline(entry);
        }
        if (xhci_pending) {
            poll_root_hub_for_irq(entry);
        }
        kb_run_deferred_work();
        if (trace_irq_enabled()) {
            fprintf(stderr, "kobox irq: handle dev_id=%p status=%d\n", dev_id, status);
        }
        return status == KB_OK ? 0 : -110;
    }
    return -19;
}

static int handle_any_irq(uint64_t timeout_ns, int run_work)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(kb_shim_current_backend());
    if (ops == NULL || ops->irq_wait == NULL) {
        return -95;
    }
    for (shim_irq_t *entry = irq_list; entry != NULL; entry = entry->next) {
        int (*handler)(int, void *) = entry->handler;
        int (*thread_fn)(int, void *) = entry->thread_fn;
        entry->handler = NULL;
        entry->thread_fn = NULL;
        kb_status_t status = ops->irq_wait(entry->device, entry->backend_irq, timeout_ns);
        int xhci_pending = kb_pci_xhci_irq_pending();
        entry->handler = handler;
        entry->thread_fn = thread_fn;
        if (status == KB_OK || xhci_pending) {
            irq_trampoline(entry);
        }
        if (xhci_pending) {
            poll_root_hub_for_irq(entry);
        }
        if (trace_irq_enabled()) {
            fprintf(stderr, "kobox irq: handle-any irq=%u dev_id=%p status=%d\n", entry->irq, entry->dev_id, status);
        }
        if (status == KB_OK) {
            if (run_work) {
                kb_run_deferred_work();
            } else {
                kb_run_deferred_bottom_halves();
            }
            return 0;
        }
    }
    if (run_work) {
        kb_run_deferred_work();
    } else {
        kb_run_deferred_bottom_halves();
    }
    return -110;
}

int kb_handle_any_irq(uint64_t timeout_ns)
{
    return handle_any_irq(timeout_ns, 1);
}

int kb_handle_any_irq_no_work(uint64_t timeout_ns)
{
    return handle_any_irq(timeout_ns, 0);
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
