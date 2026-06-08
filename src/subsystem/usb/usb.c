#include "subsystem/usb/usb.h"
#include "subsystem/usb/storage.h"
#include "kobox/shim.h"

#include <stdio.h>
#include <string.h>

static kb_usb_hcd_record_t *usb_hcd_records;

typedef struct kb_usb_urb_record {
    int active;
    void *hcd;
    void *urb;
    void *transfer_buffer;
    void *setup_packet;
    uint64_t transfer_dma;
    uint64_t setup_dma;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    uint32_t transfer_flags;
    uint32_t dma_map_flags;
    int status;
    uint32_t linked;
    uint32_t mapped;
    uint32_t link_count;
    uint32_t unlink_count;
    uint32_t map_count;
    uint32_t unmap_count;
    uint32_t submit_count;
    uint32_t kill_count;
    uint32_t giveback_count;
    int last_unlink_status;
    int last_submit_status;
    int last_giveback_status;
    unsigned int last_mem_flags;
    unsigned int last_submit_mem_flags;
    struct kb_usb_urb_record *next;
} kb_usb_urb_record_t;

static kb_usb_urb_record_t *usb_urb_records;

typedef struct kb_usb_device_record {
    int active;
    kb_usb_device_snapshot_t snapshot;
    struct kb_usb_device_record *next;
} kb_usb_device_record_t;

typedef struct kb_usb_interface_record {
    int active;
    kb_usb_interface_snapshot_t snapshot;
    struct kb_usb_interface_record *next;
} kb_usb_interface_record_t;

typedef struct kb_usb_endpoint_record {
    int active;
    kb_usb_endpoint_snapshot_t snapshot;
    struct kb_usb_endpoint_record *next;
} kb_usb_endpoint_record_t;

typedef struct kb_usb_driver_record {
    int active;
    kb_usb_driver_snapshot_t snapshot;
    struct kb_usb_driver_record *next;
} kb_usb_driver_record_t;

static kb_usb_device_record_t *usb_device_records;
static kb_usb_interface_record_t *usb_interface_records;
static kb_usb_endpoint_record_t *usb_endpoint_records;
static kb_usb_driver_record_t *usb_driver_records;

static int pointer_is_error_or_low(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static long hcd_record_index(const kb_usb_hcd_record_t *target)
{
    if (target == NULL) {
        return -1;
    }
    long index = 0;
    for (kb_usb_hcd_record_t *record = usb_hcd_records; record != NULL; record = record->next) {
        if (record == target) {
            return index;
        }
        index++;
    }
    return -1;
}

static kb_usb_hcd_record_t *hub_event_record_for_hcd(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL || pointer_is_error_or_low(hcd)) {
        return record;
    }
    return kb_usb_subsystem_hcd_track(hcd);
}

static unsigned long hub_status_bits(const unsigned char *status, size_t status_len)
{
    unsigned long bits = 0;
    size_t limit = status_len;
    if (limit > sizeof(bits)) {
        limit = sizeof(bits);
    }
    for (size_t i = 0; i < limit; i++) {
        bits |= ((unsigned long)status[i]) << (i * 8);
    }
    return bits;
}

static kb_usb_urb_record_t *urb_record_find(const void *urb)
{
    if (pointer_is_error_or_low(urb)) {
        return NULL;
    }
    for (kb_usb_urb_record_t *record = usb_urb_records; record != NULL; record = record->next) {
        if (record->active && record->urb == urb) {
            return record;
        }
    }
    return NULL;
}

static kb_usb_urb_record_t *urb_record_for(void *hcd, void *urb)
{
    if (pointer_is_error_or_low(urb)) {
        return NULL;
    }
    kb_usb_urb_record_t *record = urb_record_find(urb);
    if (record != NULL) {
        if (record->hcd == NULL) {
            record->hcd = hcd;
        }
        return record;
    }
    record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->active = 1;
    record->hcd = hcd;
    record->urb = urb;
    record->next = usb_urb_records;
    usb_urb_records = record;
    return record;
}

static void urb_records_release_for_hcd(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return;
    }
    kb_usb_urb_record_t **cursor = &usb_urb_records;
    while (*cursor != NULL) {
        kb_usb_urb_record_t *record = *cursor;
        if (record->active && record->hcd == hcd) {
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            continue;
        }
        cursor = &record->next;
    }
}

static kb_usb_device_record_t *device_record_find(const void *udev)
{
    if (udev == NULL) {
        return NULL;
    }
    for (kb_usb_device_record_t *record = usb_device_records; record != NULL; record = record->next) {
        if (record->active && record->snapshot.udev == udev) {
            return record;
        }
    }
    return NULL;
}

static kb_usb_device_record_t *device_record_for(const void *udev)
{
    kb_usb_device_record_t *record = device_record_find(udev);
    if (record != NULL || udev == NULL) {
        return record;
    }
    record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->active = 1;
    record->snapshot.udev = (void *)udev;
    record->next = usb_device_records;
    usb_device_records = record;
    return record;
}

static kb_usb_interface_record_t *interface_record_find(const void *interface)
{
    if (interface == NULL) {
        return NULL;
    }
    for (kb_usb_interface_record_t *record = usb_interface_records; record != NULL; record = record->next) {
        if (record->active && record->snapshot.interface == interface) {
            return record;
        }
    }
    return NULL;
}

static kb_usb_interface_record_t *interface_record_for(const void *interface)
{
    kb_usb_interface_record_t *record = interface_record_find(interface);
    if (record != NULL || interface == NULL) {
        return record;
    }
    record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->active = 1;
    record->snapshot.interface = (void *)interface;
    record->next = usb_interface_records;
    usb_interface_records = record;
    return record;
}

static kb_usb_endpoint_record_t *endpoint_record_find(const void *endpoint)
{
    if (endpoint == NULL) {
        return NULL;
    }
    for (kb_usb_endpoint_record_t *record = usb_endpoint_records; record != NULL; record = record->next) {
        if (record->active && record->snapshot.endpoint == endpoint) {
            return record;
        }
    }
    return NULL;
}

static kb_usb_endpoint_record_t *endpoint_record_for(const void *endpoint)
{
    kb_usb_endpoint_record_t *record = endpoint_record_find(endpoint);
    if (record != NULL || endpoint == NULL) {
        return record;
    }
    record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->active = 1;
    record->snapshot.endpoint = (void *)endpoint;
    record->next = usb_endpoint_records;
    usb_endpoint_records = record;
    return record;
}

static kb_usb_driver_record_t *driver_record_find(const void *driver)
{
    if (driver == NULL) {
        return NULL;
    }
    for (kb_usb_driver_record_t *record = usb_driver_records; record != NULL; record = record->next) {
        if (record->active && record->snapshot.driver == driver) {
            return record;
        }
    }
    return NULL;
}

static kb_usb_driver_record_t *driver_record_for(const void *driver)
{
    kb_usb_driver_record_t *record = driver_record_find(driver);
    if (record != NULL || driver == NULL) {
        return record;
    }
    record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->active = 1;
    record->snapshot.driver = (void *)driver;
    record->next = usb_driver_records;
    usb_driver_records = record;
    return record;
}

static void endpoint_records_release_for_interface(void *interface)
{
    if (interface == NULL) {
        return;
    }
    kb_usb_endpoint_record_t **cursor = &usb_endpoint_records;
    while (*cursor != NULL) {
        kb_usb_endpoint_record_t *record = *cursor;
        if (record->active && record->snapshot.interface == interface) {
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            continue;
        }
        cursor = &record->next;
    }
}

static void endpoint_records_release_for_udev(void *udev)
{
    if (udev == NULL) {
        return;
    }
    kb_usb_endpoint_record_t **cursor = &usb_endpoint_records;
    while (*cursor != NULL) {
        kb_usb_endpoint_record_t *record = *cursor;
        if (record->active && record->snapshot.udev == udev) {
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            continue;
        }
        cursor = &record->next;
    }
}

static void interface_records_release_for_udev(void *udev)
{
    if (udev == NULL) {
        return;
    }
    kb_usb_interface_record_t **cursor = &usb_interface_records;
    while (*cursor != NULL) {
        kb_usb_interface_record_t *record = *cursor;
        if (record->active && record->snapshot.udev == udev) {
            void *interface = record->snapshot.interface;
            kb_usb_storage_subsystem_remove_interface(interface);
            endpoint_records_release_for_interface(interface);
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            continue;
        }
        cursor = &record->next;
    }
}

static void device_records_release_for_hcd(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return;
    }
    kb_usb_device_record_t **cursor = &usb_device_records;
    while (*cursor != NULL) {
        kb_usb_device_record_t *record = *cursor;
        if (record->active && record->snapshot.hcd == hcd) {
            void *udev = record->snapshot.udev;
            interface_records_release_for_udev(udev);
            endpoint_records_release_for_udev(udev);
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            continue;
        }
        cursor = &record->next;
    }
}

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_alloc(size_t storage_size)
{
    kb_usb_hcd_record_t *record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->hcd = kb_kzalloc(storage_size, 0);
    if (record->hcd == NULL) {
        kb_kfree(record);
        return NULL;
    }
    record->owns_storage = 1;
    record->active = 1;
    record->next = usb_hcd_records;
    usb_hcd_records = record;
    return record;
}

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_track(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return NULL;
    }
    kb_usb_hcd_record_t *existing = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (existing != NULL) {
        return existing;
    }
    kb_usb_hcd_record_t *record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->hcd = hcd;
    record->active = 1;
    record->next = usb_hcd_records;
    usb_hcd_records = record;
    return record;
}

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_for_hcd(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return NULL;
    }
    for (kb_usb_hcd_record_t *record = usb_hcd_records; record != NULL; record = record->next) {
        if (record->active && record->hcd == hcd) {
            return record;
        }
    }
    return NULL;
}

kb_usb_hcd_record_t *kb_usb_subsystem_primary_hcd_for_owner(void *owner)
{
    if (owner == NULL) {
        return NULL;
    }
    for (kb_usb_hcd_record_t *record = usb_hcd_records; record != NULL; record = record->next) {
        if (record->active && record->owner == owner && record->primary) {
            return record;
        }
    }
    return NULL;
}

void kb_usb_subsystem_hcd_release(kb_usb_hcd_record_t *record)
{
    if (record == NULL || !record->active) {
        return;
    }
    device_records_release_for_hcd(record->hcd);
    urb_records_release_for_hcd(record->hcd);
    if (record->owns_storage) {
        kb_kfree(record->hcd);
    }

    kb_usb_hcd_record_t **cursor = &usb_hcd_records;
    while (*cursor != NULL) {
        if (*cursor == record) {
            *cursor = record->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    memset(record, 0, sizeof(*record));
    kb_kfree(record);
}

int kb_usb_subsystem_for_each_hcd(int (*callback)(kb_usb_hcd_record_t *record, void *ctx), void *ctx)
{
    if (callback == NULL) {
        return 0;
    }

    int visited = 0;
    for (kb_usb_hcd_record_t *record = usb_hcd_records; record != NULL;) {
        kb_usb_hcd_record_t *next = record->next;
        if (!record->active || record->hcd == NULL) {
            record = next;
            continue;
        }
        visited++;
        if (callback(record, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}

void kb_usb_subsystem_hcd_note_irq(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->irq_count++;
    }
}

void kb_usb_subsystem_hcd_note_died(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->died_count++;
    }
}

void kb_usb_subsystem_hcd_note_lost_power(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->lost_power_count++;
    }
}

void kb_usb_subsystem_hcd_note_root_hub_poll(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->root_hub_poll_count++;
    }
}

void kb_usb_subsystem_hcd_note_root_hub_resume(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->root_hub_resume_count++;
    }
}

void kb_usb_subsystem_hcd_note_port_resume_start(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->port_resume_start_count++;
    }
}

void kb_usb_subsystem_hcd_note_port_resume_end(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->port_resume_end_count++;
    }
}

void kb_usb_subsystem_hcd_note_wakeup_notification(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->wakeup_notification_count++;
    }
}

void kb_usb_subsystem_hcd_note_remote_wakeup_quirk(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->remote_wakeup_quirk_count++;
    }
}

int kb_usb_subsystem_hcd_snapshot(const void *hcd, kb_usb_hcd_snapshot_t *out_snapshot)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd((void *)hcd);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    out_snapshot->hcd = record->hcd;
    out_snapshot->owner = record->owner;
    out_snapshot->driver = record->driver;
    out_snapshot->primary_hcd = record->primary_hcd;
    out_snapshot->regs = record->regs;
    out_snapshot->irq = record->irq;
    out_snapshot->primary = record->primary != 0;
    out_snapshot->irq_registered = record->irq_registered != 0;
    out_snapshot->added = record->added != 0;
    out_snapshot->irq_count = record->irq_count;
    out_snapshot->died_count = record->died_count;
    out_snapshot->lost_power_count = record->lost_power_count;
    out_snapshot->root_hub_poll_count = record->root_hub_poll_count;
    out_snapshot->root_hub_resume_count = record->root_hub_resume_count;
    out_snapshot->port_resume_start_count = record->port_resume_start_count;
    out_snapshot->port_resume_end_count = record->port_resume_end_count;
    out_snapshot->wakeup_notification_count = record->wakeup_notification_count;
    out_snapshot->remote_wakeup_quirk_count = record->remote_wakeup_quirk_count;
    return 0;
}

int kb_usb_subsystem_hub_event_prepare(
    void *hcd,
    const unsigned char *status,
    size_t status_len,
    kb_usb_hub_event_update_t *update)
{
    if (update != NULL) {
        memset(update, 0, sizeof(*update));
        update->slot = -1;
    }

    if (hcd == NULL || status == NULL || status_len == 0) {
        return -22;
    }

    kb_usb_hcd_record_t *record = hub_event_record_for_hcd(hcd);
    if (record == NULL) {
        return -12;
    }

    size_t copied = status_len;
    if (copied > KB_USB_HUB_STATUS_MAX) {
        copied = KB_USB_HUB_STATUS_MAX;
    }

    unsigned long bits = hub_status_bits(status, status_len);
    unsigned long injected_before = record->hub_event_bits;
    if (bits == 0) {
        record->hub_event_bits = 0;
    }
    unsigned long new_bits = bits & ~record->hub_event_bits;

    if (update != NULL) {
        memcpy(update->status, status, copied);
        update->status_len = (int)copied;
        update->bits = bits;
        update->new_bits = new_bits;
        update->injected_before = injected_before;
        update->injected_after = record->hub_event_bits;
        update->slot = hcd_record_index(record);
    }
    return 0;
}

int kb_usb_subsystem_hub_event_commit(
    void *hcd,
    unsigned long bits,
    kb_usb_hub_event_update_t *update)
{
    if (hcd == NULL) {
        return -22;
    }

    kb_usb_hcd_record_t *record = hub_event_record_for_hcd(hcd);
    if (record == NULL) {
        return -12;
    }

    unsigned long injected_before = record->hub_event_bits;
    record->hub_event_bits |= bits;

    if (update != NULL) {
        if (update->slot < 0) {
            update->slot = hcd_record_index(record);
        }
        update->injected_before = injected_before;
        update->injected_after = record->hub_event_bits;
    }
    return 0;
}

int kb_usb_subsystem_urb_link(void *hcd, void *urb)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL || hcd == NULL) {
        return -22;
    }
    record->hcd = hcd;
    record->linked = 1;
    record->link_count++;
    return 0;
}

void kb_usb_subsystem_urb_unlink(void *hcd, void *urb, int status)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL) {
        return;
    }
    if (record->hcd == NULL) {
        record->hcd = hcd;
    }
    record->linked = 0;
    record->last_unlink_status = status;
    record->unlink_count++;
}

int kb_usb_subsystem_urb_check_unlink(void *hcd, void *urb, int status)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL || hcd == NULL) {
        return -22;
    }
    record->last_unlink_status = status;
    return 0;
}

int kb_usb_subsystem_urb_map_dma(void *hcd, void *urb, const kb_usb_urb_dma_update_t *update)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL || hcd == NULL) {
        return -22;
    }
    record->hcd = hcd;
    record->mapped = 1;
    if (update != NULL) {
        record->transfer_buffer = update->transfer_buffer;
        record->setup_packet = update->setup_packet;
        record->transfer_dma = update->transfer_dma;
        record->setup_dma = update->setup_dma;
        record->transfer_buffer_length = update->transfer_buffer_length;
        record->actual_length = update->actual_length;
        record->transfer_flags = update->transfer_flags;
        record->dma_map_flags = update->dma_map_flags;
        record->status = 0;
        record->last_mem_flags = update->mem_flags;
    }
    record->map_count++;
    return 0;
}

void kb_usb_subsystem_urb_unmap_dma(void *hcd, void *urb)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL) {
        return;
    }
    if (record->hcd == NULL) {
        record->hcd = hcd;
    }
    record->mapped = 0;
    record->dma_map_flags = 0;
    record->unmap_count++;
}

void kb_usb_subsystem_urb_submit(void *hcd, void *urb, unsigned int mem_flags, int status)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL) {
        return;
    }
    if (record->hcd == NULL) {
        record->hcd = hcd;
    }
    record->linked = status == 0;
    record->last_submit_status = status;
    record->last_submit_mem_flags = mem_flags;
    record->submit_count++;
}

void kb_usb_subsystem_urb_kill(void *hcd, void *urb)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL) {
        return;
    }
    if (record->hcd == NULL) {
        record->hcd = hcd;
    }
    record->linked = 0;
    record->kill_count++;
}

void kb_usb_subsystem_urb_giveback(void *hcd, void *urb, int status)
{
    kb_usb_urb_record_t *record = urb_record_for(hcd, urb);
    if (record == NULL) {
        return;
    }
    if (record->hcd == NULL) {
        record->hcd = hcd;
    }
    record->linked = 0;
    record->last_giveback_status = status;
    record->status = status;
    record->giveback_count++;
}

int kb_usb_subsystem_urb_snapshot(const void *urb, kb_usb_urb_snapshot_t *out_snapshot)
{
    kb_usb_urb_record_t *record = urb_record_find(urb);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    out_snapshot->hcd = record->hcd;
    out_snapshot->urb = record->urb;
    out_snapshot->transfer_buffer = record->transfer_buffer;
    out_snapshot->setup_packet = record->setup_packet;
    out_snapshot->transfer_dma = record->transfer_dma;
    out_snapshot->setup_dma = record->setup_dma;
    out_snapshot->transfer_buffer_length = record->transfer_buffer_length;
    out_snapshot->actual_length = record->actual_length;
    out_snapshot->transfer_flags = record->transfer_flags;
    out_snapshot->dma_map_flags = record->dma_map_flags;
    out_snapshot->status = record->status;
    out_snapshot->linked = record->linked;
    out_snapshot->mapped = record->mapped;
    out_snapshot->link_count = record->link_count;
    out_snapshot->unlink_count = record->unlink_count;
    out_snapshot->map_count = record->map_count;
    out_snapshot->unmap_count = record->unmap_count;
    out_snapshot->submit_count = record->submit_count;
    out_snapshot->kill_count = record->kill_count;
    out_snapshot->giveback_count = record->giveback_count;
    out_snapshot->last_unlink_status = record->last_unlink_status;
    out_snapshot->last_submit_status = record->last_submit_status;
    out_snapshot->last_giveback_status = record->last_giveback_status;
    out_snapshot->last_mem_flags = record->last_mem_flags;
    out_snapshot->last_submit_mem_flags = record->last_submit_mem_flags;
    return 0;
}

size_t kb_usb_subsystem_urb_count(void)
{
    size_t count = 0;
    for (kb_usb_urb_record_t *record = usb_urb_records; record != NULL; record = record->next) {
        if (record->active) {
            count++;
        }
    }
    return count;
}

int kb_usb_subsystem_for_each_urb(
    int (*callback)(const kb_usb_urb_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (kb_usb_urb_record_t *record = usb_urb_records; record != NULL;) {
        kb_usb_urb_record_t *next = record->next;
        if (!record->active) {
            record = next;
            continue;
        }
        kb_usb_urb_snapshot_t snapshot = {
            .hcd = record->hcd,
            .urb = record->urb,
            .transfer_buffer = record->transfer_buffer,
            .setup_packet = record->setup_packet,
            .transfer_dma = record->transfer_dma,
            .setup_dma = record->setup_dma,
            .transfer_buffer_length = record->transfer_buffer_length,
            .actual_length = record->actual_length,
            .transfer_flags = record->transfer_flags,
            .dma_map_flags = record->dma_map_flags,
            .status = record->status,
            .linked = record->linked,
            .mapped = record->mapped,
            .link_count = record->link_count,
            .unlink_count = record->unlink_count,
            .map_count = record->map_count,
            .unmap_count = record->unmap_count,
            .submit_count = record->submit_count,
            .kill_count = record->kill_count,
            .giveback_count = record->giveback_count,
            .last_unlink_status = record->last_unlink_status,
            .last_submit_status = record->last_submit_status,
            .last_giveback_status = record->last_giveback_status,
            .last_mem_flags = record->last_mem_flags,
            .last_submit_mem_flags = record->last_submit_mem_flags,
        };
        visited++;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}

int kb_usb_subsystem_device_observe(const kb_usb_device_update_t *update)
{
    if (update == NULL || update->udev == NULL) {
        return -22;
    }
    kb_usb_device_record_t *record = device_record_for(update->udev);
    if (record == NULL) {
        return -12;
    }

    kb_usb_device_snapshot_t snapshot = {
        .hcd = update->hcd,
        .udev = update->udev,
        .linux_device = update->linux_device,
        .parent_linux_device = update->parent_linux_device,
        .bus = update->bus,
        .active_config = update->active_config,
        .devnum = update->devnum,
        .portnum = update->portnum,
        .speed = update->speed,
        .state = update->state,
        .vendor_id = update->vendor_id,
        .product_id = update->product_id,
        .bcd_device = update->bcd_device,
        .device_class = update->device_class,
        .device_subclass = update->device_subclass,
        .device_protocol = update->device_protocol,
        .max_packet_size0 = update->max_packet_size0,
        .configuration_value = update->configuration_value,
        .interface_count = update->interface_count,
    };
    snprintf(snapshot.devpath, sizeof(snapshot.devpath), "%s", update->devpath);
    record->snapshot = snapshot;
    return 0;
}

void kb_usb_subsystem_device_remove(void *udev)
{
    kb_usb_device_record_t **cursor = &usb_device_records;
    while (*cursor != NULL) {
        kb_usb_device_record_t *record = *cursor;
        if (record->active && record->snapshot.udev == udev) {
            interface_records_release_for_udev(udev);
            endpoint_records_release_for_udev(udev);
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

int kb_usb_subsystem_device_snapshot(const void *udev, kb_usb_device_snapshot_t *out_snapshot)
{
    kb_usb_device_record_t *record = device_record_find(udev);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

size_t kb_usb_subsystem_device_count(void)
{
    size_t count = 0;
    for (kb_usb_device_record_t *record = usb_device_records; record != NULL; record = record->next) {
        if (record->active) {
            count++;
        }
    }
    return count;
}

int kb_usb_subsystem_for_each_device(
    int (*callback)(const kb_usb_device_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (kb_usb_device_record_t *record = usb_device_records; record != NULL;) {
        kb_usb_device_record_t *next = record->next;
        if (!record->active) {
            record = next;
            continue;
        }
        visited++;
        kb_usb_device_snapshot_t snapshot = record->snapshot;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}

int kb_usb_subsystem_interface_observe(const kb_usb_interface_update_t *update)
{
    if (update == NULL || update->interface == NULL) {
        return -22;
    }
    kb_usb_interface_record_t *record = interface_record_for(update->interface);
    if (record == NULL) {
        return -12;
    }
    record->snapshot.udev = update->udev;
    record->snapshot.interface = update->interface;
    record->snapshot.linux_device = update->linux_device;
    record->snapshot.parent_linux_device = update->parent_linux_device;
    record->snapshot.driver = update->driver;
    record->snapshot.driver_data = update->driver_data;
    record->snapshot.cur_altsetting = update->cur_altsetting;
    record->snapshot.interface_number = update->interface_number;
    record->snapshot.alternate_setting = update->alternate_setting;
    record->snapshot.endpoint_count = update->endpoint_count;
    record->snapshot.interface_class = update->interface_class;
    record->snapshot.interface_subclass = update->interface_subclass;
    record->snapshot.interface_protocol = update->interface_protocol;
    kb_usb_interface_snapshot_t snapshot = record->snapshot;
    kb_usb_storage_subsystem_observe_interface(&snapshot);
    return 0;
}

void kb_usb_subsystem_interface_remove(void *interface)
{
    kb_usb_interface_record_t **cursor = &usb_interface_records;
    while (*cursor != NULL) {
        kb_usb_interface_record_t *record = *cursor;
        if (record->active && record->snapshot.interface == interface) {
            kb_usb_storage_subsystem_remove_interface(interface);
            endpoint_records_release_for_interface(interface);
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

int kb_usb_subsystem_interface_snapshot(const void *interface, kb_usb_interface_snapshot_t *out_snapshot)
{
    kb_usb_interface_record_t *record = interface_record_find(interface);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

size_t kb_usb_subsystem_interface_count(void)
{
    size_t count = 0;
    for (kb_usb_interface_record_t *record = usb_interface_records; record != NULL; record = record->next) {
        if (record->active) {
            count++;
        }
    }
    return count;
}

int kb_usb_subsystem_for_each_interface(
    int (*callback)(const kb_usb_interface_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (kb_usb_interface_record_t *record = usb_interface_records; record != NULL;) {
        kb_usb_interface_record_t *next = record->next;
        if (!record->active) {
            record = next;
            continue;
        }
        visited++;
        kb_usb_interface_snapshot_t snapshot = record->snapshot;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}

int kb_usb_subsystem_endpoint_observe(const kb_usb_endpoint_update_t *update)
{
    if (update == NULL || update->endpoint == NULL) {
        return -22;
    }
    kb_usb_endpoint_record_t *record = endpoint_record_for(update->endpoint);
    if (record == NULL) {
        return -12;
    }
    record->snapshot.udev = update->udev;
    record->snapshot.interface = update->interface;
    record->snapshot.endpoint = update->endpoint;
    record->snapshot.address = update->address;
    record->snapshot.attributes = update->attributes;
    record->snapshot.interval = update->interval;
    record->snapshot.type = update->type;
    record->snapshot.direction_in = update->direction_in;
    record->snapshot.max_packet_size = update->max_packet_size;
    kb_usb_endpoint_snapshot_t snapshot = record->snapshot;
    kb_usb_storage_subsystem_observe_endpoint(&snapshot);
    return 0;
}

void kb_usb_subsystem_endpoint_remove(void *endpoint)
{
    kb_usb_endpoint_record_t **cursor = &usb_endpoint_records;
    while (*cursor != NULL) {
        kb_usb_endpoint_record_t *record = *cursor;
        if (record->active && record->snapshot.endpoint == endpoint) {
            *cursor = record->next;
            memset(record, 0, sizeof(*record));
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

int kb_usb_subsystem_endpoint_snapshot(const void *endpoint, kb_usb_endpoint_snapshot_t *out_snapshot)
{
    kb_usb_endpoint_record_t *record = endpoint_record_find(endpoint);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

size_t kb_usb_subsystem_endpoint_count(void)
{
    size_t count = 0;
    for (kb_usb_endpoint_record_t *record = usb_endpoint_records; record != NULL; record = record->next) {
        if (record->active) {
            count++;
        }
    }
    return count;
}

int kb_usb_subsystem_for_each_endpoint(
    int (*callback)(const kb_usb_endpoint_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (kb_usb_endpoint_record_t *record = usb_endpoint_records; record != NULL;) {
        kb_usb_endpoint_record_t *next = record->next;
        if (!record->active) {
            record = next;
            continue;
        }
        visited++;
        kb_usb_endpoint_snapshot_t snapshot = record->snapshot;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}

void kb_usb_subsystem_driver_register(void *driver, void *owner, const char *module_name, int status)
{
    kb_usb_driver_record_t *record = driver_record_for(driver);
    if (record == NULL) {
        return;
    }
    record->snapshot.driver = driver;
    record->snapshot.owner = owner;
    record->snapshot.registered = status == 0;
    record->snapshot.last_register_status = status;
    record->snapshot.register_count++;
    if (module_name != NULL) {
        snprintf(record->snapshot.module_name, sizeof(record->snapshot.module_name), "%s", module_name);
    }
    kb_usb_storage_subsystem_note_driver_registered(driver, module_name);
}

void kb_usb_subsystem_driver_deregister(void *driver)
{
    kb_usb_driver_record_t *record = driver_record_for(driver);
    if (record == NULL) {
        return;
    }
    record->snapshot.registered = 0;
    record->snapshot.deregister_count++;
    kb_usb_storage_subsystem_note_driver_deregistered(driver);
}

int kb_usb_subsystem_driver_snapshot(const void *driver, kb_usb_driver_snapshot_t *out_snapshot)
{
    kb_usb_driver_record_t *record = driver_record_find(driver);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

size_t kb_usb_subsystem_driver_count(void)
{
    size_t count = 0;
    for (kb_usb_driver_record_t *record = usb_driver_records; record != NULL; record = record->next) {
        if (record->active) {
            count++;
        }
    }
    return count;
}

int kb_usb_subsystem_for_each_driver(
    int (*callback)(const kb_usb_driver_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (kb_usb_driver_record_t *record = usb_driver_records; record != NULL;) {
        kb_usb_driver_record_t *next = record->next;
        if (!record->active) {
            record = next;
            continue;
        }
        visited++;
        kb_usb_driver_snapshot_t snapshot = record->snapshot;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
        record = next;
    }
    return visited;
}
